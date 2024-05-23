// Single threaded epoll based TCP server.
//
// Serves up to N simultaneous connections, reassembles the incoming byte streams
// into newline delimited messages and appends every message to a file in the
// order the server received it. Runs as a daemon by default and shuts down
// cleanly on SIGTERM.
//
// Linux only: epoll and signalfd are Linux specific interfaces.

#include "message_stream.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <getopt.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <utility>

namespace {

constexpr uint16_t kDefaultPort = 8080;
constexpr char kDefaultHost[] = "127.0.0.1";
constexpr int kMaxEpollEvents = 64;
constexpr size_t kReadBufferSize = 4096;
constexpr size_t kMaxMessageSize = 1u << 20;  // 1 MiB per single message

// Diagnostics go to stderr while the process runs in the foreground and to
// syslog once it has detached from the terminal.
bool g_use_syslog = false;

void WriteLog(int priority, const char* format, va_list arguments) {
    if (g_use_syslog) {
        vsyslog(priority, format, arguments);
        return;
    }
    std::vfprintf(stderr, format, arguments);
    std::fputc('\n', stderr);
}

void LogInfo(const char* format, ...) __attribute__((format(printf, 1, 2)));
void LogInfo(const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    WriteLog(LOG_INFO, format, arguments);
    va_end(arguments);
}

void LogError(const char* format, ...) __attribute__((format(printf, 1, 2)));
void LogError(const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    WriteLog(LOG_ERR, format, arguments);
    va_end(arguments);
}

// ---------------------------------------------------------------------------
// Command line
// ---------------------------------------------------------------------------

struct Options {
    int max_connections = 0;
    std::string output_path;
    std::string host = kDefaultHost;
    std::string pid_path;
    uint16_t port = kDefaultPort;
    bool foreground = false;
};

enum class ParseResult { kOk, kHelpRequested, kError };

void PrintUsage(const char* program_name) {
    std::fprintf(stderr,
                 "Usage: %s [options] <max-connections> <output-file>\n"
                 "\n"
                 "Arguments:\n"
                 "  max-connections  maximum number of simultaneously served clients\n"
                 "  output-file      file the received messages are appended to\n"
                 "\n"
                 "Options:\n"
                 "  -H, --host ADDR    address to bind to (default: %s)\n"
                 "  -p, --port PORT    TCP port to listen on (default: %u)\n"
                 "  -f, --foreground   stay in the foreground instead of daemonizing\n"
                 "      --pidfile PATH write the pid of the running server to PATH\n"
                 "  -h, --help         show this message and exit\n",
                 program_name, kDefaultHost, static_cast<unsigned>(kDefaultPort));
}

bool ParseLong(const char* text, long* value) {
    errno = 0;
    char* parse_end = nullptr;
    const long parsed = std::strtol(text, &parse_end, 10);
    if (errno != 0 || parse_end == text || *parse_end != '\0') {
        return false;
    }
    *value = parsed;
    return true;
}

ParseResult ParseOptions(int argc, char* argv[], Options* options) {
    static const option long_options[] = {
        {"host", required_argument, nullptr, 'H'},
        {"port", required_argument, nullptr, 'p'},
        {"foreground", no_argument, nullptr, 'f'},
        {"pidfile", required_argument, nullptr, 'P'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };

    int option_character = 0;
    while ((option_character = getopt_long(argc, argv, "H:p:fh", long_options, nullptr)) != -1) {
        switch (option_character) {
            case 'H':
                options->host = optarg;
                break;
            case 'p': {
                long port = 0;
                if (!ParseLong(optarg, &port) || port <= 0 || port > 65535) {
                    LogError("invalid port: '%s'", optarg);
                    return ParseResult::kError;
                }
                options->port = static_cast<uint16_t>(port);
                break;
            }
            case 'f':
                options->foreground = true;
                break;
            case 'P':
                options->pid_path = optarg;
                break;
            case 'h':
                return ParseResult::kHelpRequested;
            default:
                return ParseResult::kError;
        }
    }

    if (argc - optind != 2) {
        LogError("expected 2 positional arguments, got %d", argc - optind);
        return ParseResult::kError;
    }

    long max_connections = 0;
    if (!ParseLong(argv[optind], &max_connections) || max_connections <= 0 ||
        max_connections > INT_MAX) {
        LogError("invalid max-connections: '%s'", argv[optind]);
        return ParseResult::kError;
    }
    options->max_connections = static_cast<int>(max_connections);
    options->output_path = argv[optind + 1];
    return ParseResult::kOk;
}

// ---------------------------------------------------------------------------
// Process setup
// ---------------------------------------------------------------------------

// The task requires the server to terminate on SIGTERM and to ignore SIGQUIT,
// SIGINT, SIGHUP, SIGSTOP and SIGCONT.
//
// SIGTERM is not handled by a classic signal handler. It is blocked and
// delivered through a signalfd instead, which turns it into just another
// readable descriptor in the epoll set. That removes every async signal safety
// concern (a handler may only call a short list of functions, and exit() is not
// on it) and the race a "set a flag and hope epoll_wait sees it" handler has.
//
// SIGSTOP is intentionally absent: POSIX guarantees it can be neither caught,
// blocked nor ignored, so the kernel stops the process no matter what the
// program asks for.
//
// Dispositions and the signal mask are both inherited across fork(), so this
// runs before daemonizing; the signalfd itself must not (see CreateSignalFd).
bool ConfigureSignalDispositions() {
    for (const int signal_number : {SIGQUIT, SIGINT, SIGHUP, SIGCONT, SIGPIPE}) {
        if (std::signal(signal_number, SIG_IGN) == SIG_ERR) {
            LogError("signal(%d, SIG_IGN): %s", signal_number, std::strerror(errno));
            return false;
        }
    }

    sigset_t signal_mask;
    sigemptyset(&signal_mask);
    sigaddset(&signal_mask, SIGTERM);

    // Blocking is what keeps the default "terminate immediately" action from
    // firing before the event loop gets a chance to read the signal.
    if (sigprocmask(SIG_BLOCK, &signal_mask, nullptr) == -1) {
        LogError("sigprocmask: %s", std::strerror(errno));
        return false;
    }
    return true;
}

int CreateSignalFd() {
    sigset_t signal_mask;
    sigemptyset(&signal_mask);
    sigaddset(&signal_mask, SIGTERM);

    const int signal_fd = signalfd(-1, &signal_mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (signal_fd == -1) {
        LogError("signalfd: %s", std::strerror(errno));
    }
    return signal_fd;
}

// Writes the pid of the running server so that an init script, systemd or a
// test can address the daemon after it has detached.
bool WritePidFile(const std::string& path) {
    std::ofstream pid_file(path, std::ios::out | std::ios::trunc);
    pid_file << getpid() << '\n';
    if (!pid_file) {
        LogError("cannot write pid file '%s': %s", path.c_str(), std::strerror(errno));
        return false;
    }
    return true;
}

// Classic SysV daemonization: fork twice so the process can never reacquire a
// controlling terminal, detach from the session, and replace the standard
// streams. The output file and the listening socket are opened *before* this
// point, so configuration errors still reach the user's terminal instead of
// disappearing into syslog.
bool Daemonize() {
    pid_t pid = fork();
    if (pid < 0) {
        LogError("fork: %s", std::strerror(errno));
        return false;
    }
    // _exit() rather than exit(): the parent shares stdio buffers and the open
    // output file with the child and must not flush or close them.
    if (pid > 0) {
        _exit(EXIT_SUCCESS);
    }

    if (setsid() == -1) {
        LogError("setsid: %s", std::strerror(errno));
        return false;
    }

    pid = fork();
    if (pid < 0) {
        LogError("fork: %s", std::strerror(errno));
        return false;
    }
    if (pid > 0) {
        _exit(EXIT_SUCCESS);
    }

    umask(0);
    // Safe even for a relative output path: the file is already open by now.
    if (chdir("/") == -1) {
        LogError("chdir(/): %s", std::strerror(errno));
        return false;
    }

    const int null_fd = open("/dev/null", O_RDWR);
    if (null_fd == -1) {
        LogError("open(/dev/null): %s", std::strerror(errno));
        return false;
    }
    dup2(null_fd, STDIN_FILENO);
    dup2(null_fd, STDOUT_FILENO);
    dup2(null_fd, STDERR_FILENO);
    if (null_fd > STDERR_FILENO) {
        close(null_fd);
    }

    openlog("epoll-server", LOG_PID, LOG_DAEMON);
    g_use_syslog = true;
    return true;
}

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------

class Server {
public:
    explicit Server(Options options) : options_(std::move(options)) {}

    ~Server() {
        for (const auto& connection : connections_) {
            close(connection.first);
        }
        if (listen_fd_ != -1) {
            close(listen_fd_);
        }
        if (epoll_fd_ != -1) {
            close(epoll_fd_);
        }
        if (signal_fd_ != -1) {
            close(signal_fd_);
        }
    }

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Performs every step that can fail for a user visible reason (bad path,
    // busy port, ...) so that it happens before the process detaches and its
    // diagnostics disappear into syslog.
    bool Setup() {
        return OpenOutputFile() && CreateListeningSocket() && ConfigureSignalDispositions();
    }

    int Run() {
        if (!SetUpEventLoop()) {
            return EXIT_FAILURE;
        }

        LogInfo("listening on %s:%u, max connections: %d, output: %s", options_.host.c_str(),
                static_cast<unsigned>(options_.port), options_.max_connections,
                options_.output_path.c_str());

        epoll_event events[kMaxEpollEvents];
        while (running_) {
            const int event_count = epoll_wait(epoll_fd_, events, kMaxEpollEvents, -1);
            if (event_count == -1) {
                if (errno == EINTR) {
                    continue;
                }
                LogError("epoll_wait: %s", std::strerror(errno));
                return EXIT_FAILURE;
            }

            for (int i = 0; i < event_count; ++i) {
                const int fd = events[i].data.fd;
                if (fd == signal_fd_) {
                    HandleSignal();
                } else if (fd == listen_fd_) {
                    AcceptConnections();
                } else {
                    HandleClientEvent(fd, events[i].events);
                }
            }
        }

        LogInfo("shutting down, %llu message(s) stored", stored_messages_);
        return exit_code_;
    }

private:
    bool OpenOutputFile() {
        output_.open(options_.output_path, std::ios::out | std::ios::app);
        if (!output_.is_open()) {
            LogError("cannot open output file '%s': %s", options_.output_path.c_str(),
                     std::strerror(errno));
            return false;
        }
        return true;
    }

    bool CreateListeningSocket() {
        listen_fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (listen_fd_ == -1) {
            LogError("socket: %s", std::strerror(errno));
            return false;
        }

        // SO_REUSEADDR and SO_REUSEPORT are option *names*, not a bit mask:
        // OR-ing them together silently selects a single unrelated option.
        // Only SO_REUSEADDR is wanted here (restart without waiting out
        // TIME_WAIT); SO_REUSEPORT would let another process share the port.
        const int enable = 1;
        if (setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) == -1) {
            LogError("setsockopt(SO_REUSEADDR): %s", std::strerror(errno));
            return false;
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(options_.port);
        if (inet_pton(AF_INET, options_.host.c_str(), &address.sin_addr) != 1) {
            LogError("invalid bind address: '%s'", options_.host.c_str());
            return false;
        }

        if (bind(listen_fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == -1) {
            LogError("bind %s:%u: %s", options_.host.c_str(),
                     static_cast<unsigned>(options_.port), std::strerror(errno));
            return false;
        }

        if (listen(listen_fd_, SOMAXCONN) == -1) {
            LogError("listen: %s", std::strerror(errno));
            return false;
        }
        return true;
    }

    // Called from Run(), i.e. *after* a possible fork into the background, and
    // that placement is load bearing: epoll_ctl() attaches its wait queue entry
    // to the signal state of the calling process, so a signalfd registered
    // before fork() never reports anything in the child. Setting this up before
    // daemonizing produces a daemon that ignores SIGTERM forever, because the
    // signal is blocked and nothing ever reads it.
    bool SetUpEventLoop() {
        signal_fd_ = CreateSignalFd();
        if (signal_fd_ == -1) {
            return false;
        }

        // epoll_create1() replaces the legacy epoll_create(size), whose argument
        // has been a meaningless (but validated) hint since Linux 2.6.8.
        epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd_ == -1) {
            LogError("epoll_create1: %s", std::strerror(errno));
            return false;
        }
        return RegisterFd(listen_fd_, EPOLLIN | EPOLLET) && RegisterFd(signal_fd_, EPOLLIN);
    }

    bool RegisterFd(int fd, uint32_t events) {
        epoll_event event{};
        event.events = events;
        event.data.fd = fd;
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &event) == -1) {
            LogError("epoll_ctl(ADD, fd=%d): %s", fd, std::strerror(errno));
            return false;
        }
        return true;
    }

    void AcceptConnections() {
        // The listening socket is edge triggered, so the backlog has to be
        // drained completely: a connection left behind would never produce
        // another event.
        while (true) {
            const int client_fd = accept4(listen_fd_, nullptr, nullptr,
                                          SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (client_fd == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return;  // backlog drained
                }
                if (errno == EINTR) {
                    continue;
                }
                // A failing accept() must not take the whole server down.
                LogError("accept: %s", std::strerror(errno));
                return;
            }

            if (static_cast<int>(connections_.size()) >= options_.max_connections) {
                LogInfo("connection limit of %d reached, rejecting new client",
                        options_.max_connections);
                close(client_fd);
                continue;
            }

            if (!RegisterFd(client_fd, EPOLLIN | EPOLLET | EPOLLRDHUP)) {
                close(client_fd);
                continue;
            }

            connections_.emplace(client_fd, MessageStream(kMaxMessageSize));
            LogInfo("client connected: fd=%d, active connections: %zu", client_fd,
                    connections_.size());
        }
    }

    void HandleClientEvent(int fd, uint32_t events) {
        // EPOLLRDHUP and EPOLLHUP only mean "the peer will not send any more":
        // bytes it sent before closing may still be sitting in the receive
        // buffer. The socket is therefore always drained first and closed on
        // EOF, never on the flag alone.
        if (events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
            if (!ReadFromClient(fd)) {
                return;  // the connection has already been closed
            }
        }
        if (events & (EPOLLHUP | EPOLLERR)) {
            CloseConnection(fd, "socket error or hangup");
        }
    }

    // Returns false when the connection was closed and must not be touched again.
    bool ReadFromClient(int fd) {
        const auto connection = connections_.find(fd);
        if (connection == connections_.end()) {
            return false;
        }
        MessageStream& stream = connection->second;

        char buffer[kReadBufferSize];
        while (true) {
            const ssize_t read_bytes = read(fd, buffer, sizeof(buffer));

            if (read_bytes > 0) {
                stream.Append(buffer, static_cast<size_t>(read_bytes));

                std::string message;
                while (stream.NextMessage(&message)) {
                    if (!StoreMessage(message)) {
                        return true;  // fatal for the server, not for this socket
                    }
                }

                if (stream.Overflowed()) {
                    CloseConnection(fd, "message exceeds the size limit");
                    return false;
                }
                // Edge triggered: keep reading until the socket is drained.
                continue;
            }

            if (read_bytes == 0) {
                // Orderly shutdown by the peer. Anything it sent without a
                // trailing newline is still payload and is kept.
                std::string tail;
                if (stream.TakePendingTail(&tail)) {
                    StoreMessage(tail);
                }
                CloseConnection(fd, "closed by peer");
                return false;
            }

            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return true;  // drained, wait for the next event
            }

            LogError("read(fd=%d): %s", fd, std::strerror(errno));
            CloseConnection(fd, "read error");
            return false;
        }
    }

    // Returns false if the message could not be persisted, which is fatal: a
    // server that silently stops recording data is worse than one that stops.
    bool StoreMessage(const std::string& message) {
        // Flushed per message: the point of the service is to preserve received
        // data, so it must survive an abrupt kill of the process.
        output_ << message << '\n' << std::flush;
        if (!output_) {
            LogError("cannot write to '%s': %s", options_.output_path.c_str(),
                     std::strerror(errno));
            exit_code_ = EXIT_FAILURE;
            running_ = false;
            return false;
        }
        ++stored_messages_;
        return true;
    }

    void CloseConnection(int fd, const char* reason) {
        // close() alone would drop the descriptor from the epoll set, but the
        // explicit delete keeps the intent obvious and stays correct if the
        // descriptor is ever duplicated.
        epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
        close(fd);
        connections_.erase(fd);
        LogInfo("client disconnected: fd=%d (%s), active connections: %zu", fd, reason,
                connections_.size());
    }

    void HandleSignal() {
        signalfd_siginfo signal_info{};
        const ssize_t read_bytes = read(signal_fd_, &signal_info, sizeof(signal_info));
        if (read_bytes != static_cast<ssize_t>(sizeof(signal_info))) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                LogError("read(signalfd): %s", std::strerror(errno));
            }
            return;
        }
        LogInfo("received signal %u (%s), shutting down", signal_info.ssi_signo,
                strsignal(static_cast<int>(signal_info.ssi_signo)));
        running_ = false;
    }

    Options options_;
    std::ofstream output_;
    int listen_fd_ = -1;
    int epoll_fd_ = -1;
    int signal_fd_ = -1;
    std::unordered_map<int, MessageStream> connections_;
    unsigned long long stored_messages_ = 0;
    bool running_ = true;
    int exit_code_ = EXIT_SUCCESS;
};

}  // namespace

int main(int argc, char* argv[]) {
    Options options;
    switch (ParseOptions(argc, argv, &options)) {
        case ParseResult::kHelpRequested:
            PrintUsage(argv[0]);
            return EXIT_SUCCESS;
        case ParseResult::kError:
            PrintUsage(argv[0]);
            return EXIT_FAILURE;
        case ParseResult::kOk:
            break;
    }

    Server server(options);
    if (!server.Setup()) {
        return EXIT_FAILURE;
    }

    if (!options.foreground) {
        if (!Daemonize()) {
            return EXIT_FAILURE;
        }
        LogInfo("daemon started, pid=%d", getpid());
    }

    // Written once the final pid is known, i.e. after the forks above.
    if (!options.pid_path.empty() && !WritePidFile(options.pid_path)) {
        return EXIT_FAILURE;
    }

    const int exit_code = server.Run();

    if (!options.pid_path.empty()) {
        unlink(options.pid_path.c_str());
    }
    return exit_code;
}
