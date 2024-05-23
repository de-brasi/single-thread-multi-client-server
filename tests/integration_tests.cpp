// End to end tests: every test starts a real server process on its own port and
// talks to it over real TCP sockets.
//
// There are no fixed "wait a second and hope" sleeps: the tests poll for the
// condition they need with a timeout, so they are fast when things work and
// still deterministic when they do not.

#include "test_framework.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

const char* g_server_path = "./build/server";

void SleepMilliseconds(int milliseconds) {
    timespec duration{};
    duration.tv_sec = milliseconds / 1000;
    duration.tv_nsec = static_cast<long>(milliseconds % 1000) * 1000000L;
    nanosleep(&duration, nullptr);
}

// Polls a condition instead of sleeping for a fixed amount of time.
template <typename Predicate>
bool WaitFor(Predicate predicate, int timeout_milliseconds = 5000) {
    for (int waited = 0; waited < timeout_milliseconds; waited += 10) {
        if (predicate()) {
            return true;
        }
        SleepMilliseconds(10);
    }
    return predicate();
}

uint16_t NextPort() {
    // Spread ports out so that parallel jobs and sockets left in TIME_WAIT by a
    // previous run do not collide.
    static uint16_t next_port = static_cast<uint16_t>(18000 + (getpid() % 2000) * 3);
    return next_port++;
}

std::string MakeOutputPath() {
    static int counter = 0;
    return "/tmp/epoll_server_test_" + std::to_string(getpid()) + "_" +
           std::to_string(counter++) + ".txt";
}

sockaddr_in MakeLocalAddress(uint16_t port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    return address;
}

int ConnectToServer(uint16_t port) {
    const int client_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (client_fd == -1) {
        return -1;
    }
    const sockaddr_in address = MakeLocalAddress(port);
    if (connect(client_fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == -1) {
        close(client_fd);
        return -1;
    }
    return client_fd;
}

// Writes everything, tolerating partial writes. Returns false if the peer went
// away, which some tests expect.
bool SendAll(int fd, const std::string& data) {
    size_t sent_total = 0;
    while (sent_total < data.size()) {
        const ssize_t sent = send(fd, data.data() + sent_total, data.size() - sent_total, 0);
        if (sent <= 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        sent_total += static_cast<size_t>(sent);
    }
    return true;
}

std::vector<std::string> ReadLines(const std::string& path) {
    std::vector<std::string> lines;
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        lines.push_back(line);
    }
    return lines;
}

bool Contains(const std::vector<std::string>& lines, const std::string& value) {
    return std::find(lines.begin(), lines.end(), value) != lines.end();
}

pid_t ReadPidFile(const std::string& path) {
    std::ifstream input(path);
    pid_t pid = 0;
    if (!(input >> pid)) {
        return -1;
    }
    return pid;
}

// A daemon is orphaned by design, so nobody in this test can wait() for it. It
// counts as stopped once it is gone or left as an unreaped zombie (which is
// what happens when the process that inherits it does not reap children).
bool ProcessStopped(pid_t pid) {
    if (kill(pid, 0) == -1) {
        return errno == ESRCH;
    }
    std::ifstream status("/proc/" + std::to_string(pid) + "/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("State:", 0) == 0) {
            return line.find('Z') != std::string::npos;
        }
    }
    return false;
}

// Starts a server process and guarantees it is stopped again, even if a check
// inside a test fails.
class ServerProcess {
public:
    explicit ServerProcess(int max_connections)
        : port_(NextPort()), output_path_(MakeOutputPath()) {
        const std::string port_argument = std::to_string(port_);
        const std::string max_connections_argument = std::to_string(max_connections);

        pid_ = fork();
        if (pid_ == 0) {
            // Server logs would drown the test output; keep them only when asked.
            if (std::getenv("VERBOSE_SERVER") == nullptr) {
                const int null_fd = open("/dev/null", O_WRONLY);
                if (null_fd != -1) {
                    dup2(null_fd, STDOUT_FILENO);
                    dup2(null_fd, STDERR_FILENO);
                    close(null_fd);
                }
            }
            execl(g_server_path, g_server_path, "-f", "-p", port_argument.c_str(),
                  max_connections_argument.c_str(), output_path_.c_str(),
                  static_cast<char*>(nullptr));
            _exit(127);
        }
    }

    ~ServerProcess() {
        Stop();
        unlink(output_path_.c_str());
    }

    ServerProcess(const ServerProcess&) = delete;
    ServerProcess& operator=(const ServerProcess&) = delete;

    // The server binds before it starts serving, so "the port is taken" is a
    // readiness signal that, unlike a probe connection, does not occupy one of
    // the connection slots under test.
    bool WaitUntilListening() {
        return WaitFor([this] {
            const int probe_fd = socket(AF_INET, SOCK_STREAM, 0);
            if (probe_fd == -1) {
                return false;
            }
            const sockaddr_in address = MakeLocalAddress(port_);
            const bool port_is_taken =
                bind(probe_fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == -1;
            close(probe_fd);
            return port_is_taken;
        });
    }

    // Sends SIGTERM and returns the exit code, or -1 if the process had to be
    // killed because it did not stop on its own.
    int Stop() {
        if (pid_ <= 0) {
            return stop_result_;
        }
        kill(pid_, SIGTERM);

        int status = 0;
        for (int waited = 0; waited < 5000; waited += 10) {
            if (waitpid(pid_, &status, WNOHANG) == pid_) {
                pid_ = -1;
                stop_result_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                return stop_result_;
            }
            SleepMilliseconds(10);
        }

        kill(pid_, SIGKILL);
        waitpid(pid_, &status, 0);
        pid_ = -1;
        stop_result_ = -1;
        return stop_result_;
    }

    bool IsRunning() const { return pid_ > 0 && kill(pid_, 0) == 0; }

    pid_t pid() const { return pid_; }
    uint16_t port() const { return port_; }
    const std::string& output_path() const { return output_path_; }
    std::vector<std::string> StoredLines() const { return ReadLines(output_path_); }

    bool WaitForLineCount(size_t expected) {
        return WaitFor([this, expected] { return StoredLines().size() >= expected; });
    }

private:
    pid_t pid_ = -1;
    uint16_t port_;
    std::string output_path_;
    int stop_result_ = -1;
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void ServerStoresOneMessagePerClient() {
    constexpr int kClients = 5;
    ServerProcess server(kClients);
    CHECK_TRUE(server.WaitUntilListening());

    std::vector<int> clients;
    for (int i = 0; i < kClients; ++i) {
        const int client_fd = ConnectToServer(server.port());
        CHECK_TRUE(client_fd != -1);
        clients.push_back(client_fd);
    }
    for (int i = 0; i < kClients; ++i) {
        CHECK_TRUE(SendAll(clients[i], "client-" + std::to_string(i) + "\n"));
    }

    CHECK_TRUE(server.WaitForLineCount(kClients));

    std::vector<std::string> stored = server.StoredLines();
    std::vector<std::string> expected;
    for (int i = 0; i < kClients; ++i) {
        expected.push_back("client-" + std::to_string(i));
    }
    // Interleaving between connections is up to the network, the contents are not.
    std::sort(stored.begin(), stored.end());
    std::sort(expected.begin(), expected.end());
    CHECK_EQ(stored, expected);

    for (const int client_fd : clients) {
        close(client_fd);
    }
}

void MessageOrderOfOneClientIsPreserved() {
    constexpr int kMessages = 50;
    ServerProcess server(4);
    CHECK_TRUE(server.WaitUntilListening());

    const int client_fd = ConnectToServer(server.port());
    CHECK_TRUE(client_fd != -1);

    std::vector<std::string> expected;
    for (int i = 0; i < kMessages; ++i) {
        const std::string message = "message-" + std::to_string(i);
        CHECK_TRUE(SendAll(client_fd, message + "\n"));
        expected.push_back(message);
    }

    CHECK_TRUE(server.WaitForLineCount(kMessages));
    CHECK_EQ(server.StoredLines(), expected);
    close(client_fd);
}

void MessageSplitAcrossPacketsIsReassembled() {
    ServerProcess server(2);
    CHECK_TRUE(server.WaitUntilListening());

    const int client_fd = ConnectToServer(server.port());
    CHECK_TRUE(client_fd != -1);

    // Deliberately slow, so the fragments cannot be coalesced into one read().
    CHECK_TRUE(SendAll(client_fd, "one message "));
    SleepMilliseconds(60);
    CHECK_TRUE(SendAll(client_fd, "split into three "));
    SleepMilliseconds(60);
    CHECK_TRUE(SendAll(client_fd, "packets\n"));

    CHECK_TRUE(server.WaitForLineCount(1));
    CHECK_EQ(server.StoredLines(),
             std::vector<std::string>{"one message split into three packets"});
    close(client_fd);
}

void SeveralMessagesInOnePacketAreSplit() {
    ServerProcess server(2);
    CHECK_TRUE(server.WaitUntilListening());

    const int client_fd = ConnectToServer(server.port());
    CHECK_TRUE(client_fd != -1);
    CHECK_TRUE(SendAll(client_fd, "alpha\nbeta\ngamma\n"));

    CHECK_TRUE(server.WaitForLineCount(3));
    CHECK_EQ(server.StoredLines(), (std::vector<std::string>{"alpha", "beta", "gamma"}));
    close(client_fd);
}

void LargeMessageIsStoredIntact() {
    ServerProcess server(2);
    CHECK_TRUE(server.WaitUntilListening());

    const int client_fd = ConnectToServer(server.port());
    CHECK_TRUE(client_fd != -1);

    // Much larger than the server's read buffer, so it needs several reads.
    const std::string payload(200 * 1024, 'z');
    CHECK_TRUE(SendAll(client_fd, payload + "\n"));

    CHECK_TRUE(server.WaitForLineCount(1));
    const std::vector<std::string> stored = server.StoredLines();
    CHECK_EQ(stored.size(), size_t{1});
    if (stored.size() == 1) {
        CHECK_EQ(stored[0].size(), payload.size());
        CHECK_TRUE(stored[0] == payload);
    }
    close(client_fd);
}

void UnterminatedTailIsStoredOnDisconnect() {
    ServerProcess server(2);
    CHECK_TRUE(server.WaitUntilListening());

    const int client_fd = ConnectToServer(server.port());
    CHECK_TRUE(client_fd != -1);
    CHECK_TRUE(SendAll(client_fd, "no trailing newline"));
    close(client_fd);

    CHECK_TRUE(server.WaitForLineCount(1));
    CHECK_EQ(server.StoredLines(), std::vector<std::string>{"no trailing newline"});
}

void ConnectionSlotIsReusedAfterDisconnect() {
    // Regression test: a server that fails to notice a disconnect leaks the
    // slot, and with a limit of one client nothing would ever be served again.
    ServerProcess server(1);
    CHECK_TRUE(server.WaitUntilListening());

    const int first_client = ConnectToServer(server.port());
    CHECK_TRUE(first_client != -1);
    CHECK_TRUE(SendAll(first_client, "first\n"));
    CHECK_TRUE(server.WaitForLineCount(1));
    close(first_client);

    // Retry: the server may not have processed the disconnect yet, but it must
    // process it eventually.
    bool second_message_stored = false;
    for (int attempt = 0; attempt < 40 && !second_message_stored; ++attempt) {
        const int second_client = ConnectToServer(server.port());
        if (second_client != -1) {
            SendAll(second_client, "second\n");
            second_message_stored =
                WaitFor([&] { return Contains(server.StoredLines(), "second"); }, 100);
            close(second_client);
        }
        if (!second_message_stored) {
            SleepMilliseconds(25);
        }
    }

    CHECK_TRUE(second_message_stored);
    CHECK_TRUE(Contains(server.StoredLines(), "first"));
}

void ConnectionsOverTheLimitAreRejected() {
    ServerProcess server(2);
    CHECK_TRUE(server.WaitUntilListening());

    const int first_client = ConnectToServer(server.port());
    const int second_client = ConnectToServer(server.port());
    CHECK_TRUE(first_client != -1);
    CHECK_TRUE(second_client != -1);
    CHECK_TRUE(SendAll(first_client, "first\n"));
    CHECK_TRUE(SendAll(second_client, "second\n"));
    CHECK_TRUE(server.WaitForLineCount(2));

    // Both slots are taken now, so this one has to be dropped.
    const int third_client = ConnectToServer(server.port());
    CHECK_TRUE(third_client != -1);
    SendAll(third_client, "third\n");

    // The server closes the extra connection instead of serving it.
    const bool connection_closed = WaitFor([third_client] {
        char buffer[64];
        const ssize_t received = recv(third_client, buffer, sizeof(buffer), MSG_DONTWAIT);
        return received == 0 || (received == -1 && errno != EAGAIN && errno != EWOULDBLOCK);
    });
    CHECK_TRUE(connection_closed);
    CHECK_FALSE(Contains(server.StoredLines(), "third"));

    close(third_client);
    close(second_client);
    close(first_client);
}

void MessageOverTheSizeLimitDropsOnlyThatConnection() {
    ServerProcess server(2);
    CHECK_TRUE(server.WaitUntilListening());

    const int greedy_client = ConnectToServer(server.port());
    CHECK_TRUE(greedy_client != -1);
    // 1.5 MiB without a single newline: over the server's 1 MiB limit.
    SendAll(greedy_client, std::string(1536 * 1024, 'x'));

    const bool connection_closed = WaitFor([greedy_client] {
        char buffer[64];
        const ssize_t received = recv(greedy_client, buffer, sizeof(buffer), MSG_DONTWAIT);
        return received == 0 || (received == -1 && errno != EAGAIN && errno != EWOULDBLOCK);
    });
    CHECK_TRUE(connection_closed);
    close(greedy_client);

    // The server itself must still be alive and serving other clients.
    CHECK_TRUE(server.IsRunning());
    const int well_behaved_client = ConnectToServer(server.port());
    CHECK_TRUE(well_behaved_client != -1);
    CHECK_TRUE(SendAll(well_behaved_client, "still alive\n"));
    CHECK_TRUE(WaitFor([&] { return Contains(server.StoredLines(), "still alive"); }));
    close(well_behaved_client);
}

void IgnoredSignalsDoNotStopTheServer() {
    ServerProcess server(2);
    CHECK_TRUE(server.WaitUntilListening());

    const int client_fd = ConnectToServer(server.port());
    CHECK_TRUE(client_fd != -1);
    CHECK_TRUE(SendAll(client_fd, "before signals\n"));
    CHECK_TRUE(server.WaitForLineCount(1));

    // The task requires these to be ignored: every one of them would terminate
    // or stop a process that left them at their default disposition.
    for (const int signal_number : {SIGINT, SIGQUIT, SIGHUP, SIGCONT}) {
        CHECK_TRUE(kill(server.pid(), signal_number) == 0);
    }
    SleepMilliseconds(200);

    CHECK_TRUE(server.IsRunning());
    CHECK_TRUE(SendAll(client_fd, "after signals\n"));
    CHECK_TRUE(WaitFor([&] { return Contains(server.StoredLines(), "after signals"); }));
    close(client_fd);
}

void DaemonDetachesAndStillHandlesSigterm() {
    // Regression test for a subtle one: a signalfd registered with epoll before
    // the daemonizing fork() reports nothing in the child, which used to leave
    // the daemon deaf to SIGTERM while every foreground test stayed green.
    const uint16_t port = NextPort();
    const std::string output_path = MakeOutputPath();
    const std::string pid_path = output_path + ".pid";
    const std::string port_argument = std::to_string(port);

    const pid_t launcher_pid = fork();
    if (launcher_pid == 0) {
        if (std::getenv("VERBOSE_SERVER") == nullptr) {
            const int null_fd = open("/dev/null", O_WRONLY);
            if (null_fd != -1) {
                dup2(null_fd, STDOUT_FILENO);
                dup2(null_fd, STDERR_FILENO);
                close(null_fd);
            }
        }
        execl(g_server_path, g_server_path, "-p", port_argument.c_str(), "--pidfile",
              pid_path.c_str(), "4", output_path.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    // Daemonizing means the process the shell started returns immediately.
    int status = 0;
    CHECK_TRUE(waitpid(launcher_pid, &status, 0) == launcher_pid);
    CHECK_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    CHECK_TRUE(WaitFor([&] { return ReadPidFile(pid_path) > 0; }));
    const pid_t daemon_pid = ReadPidFile(pid_path);
    CHECK_TRUE(daemon_pid > 0);
    CHECK_TRUE(daemon_pid != launcher_pid);  // it really detached

    const int client_fd = ConnectToServer(port);
    CHECK_TRUE(client_fd != -1);
    CHECK_TRUE(SendAll(client_fd, "from a daemon\n"));
    CHECK_TRUE(WaitFor([&] { return Contains(ReadLines(output_path), "from a daemon"); }));
    close(client_fd);

    CHECK_TRUE(kill(daemon_pid, SIGTERM) == 0);
    CHECK_TRUE(WaitFor([&] { return ProcessStopped(daemon_pid); }));

    unlink(output_path.c_str());
    unlink(pid_path.c_str());
}

void ServerShutsDownCleanlyOnSigterm() {
    ServerProcess server(2);
    CHECK_TRUE(server.WaitUntilListening());

    const int client_fd = ConnectToServer(server.port());
    CHECK_TRUE(client_fd != -1);
    CHECK_TRUE(SendAll(client_fd, "before shutdown\n"));
    CHECK_TRUE(server.WaitForLineCount(1));

    // Exit code 0 means the loop left through the signalfd path rather than
    // being killed.
    CHECK_EQ(server.Stop(), 0);
    CHECK_FALSE(server.IsRunning());
    CHECK_EQ(server.StoredLines(), std::vector<std::string>{"before shutdown"});
    close(client_fd);
}

}  // namespace

int main(int argc, char* argv[]) {
    // Writing to a connection the server has dropped must not kill the tests.
    signal(SIGPIPE, SIG_IGN);

    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--server") == 0) {
            g_server_path = argv[i + 1];
        }
    }

    if (access(g_server_path, X_OK) != 0) {
        std::fprintf(stderr, "server binary '%s' not found, run 'make' first\n", g_server_path);
        return 1;
    }

    RUN_TEST(ServerStoresOneMessagePerClient);
    RUN_TEST(MessageOrderOfOneClientIsPreserved);
    RUN_TEST(MessageSplitAcrossPacketsIsReassembled);
    RUN_TEST(SeveralMessagesInOnePacketAreSplit);
    RUN_TEST(LargeMessageIsStoredIntact);
    RUN_TEST(UnterminatedTailIsStoredOnDisconnect);
    RUN_TEST(ConnectionSlotIsReusedAfterDisconnect);
    RUN_TEST(ConnectionsOverTheLimitAreRejected);
    RUN_TEST(MessageOverTheSizeLimitDropsOnlyThatConnection);
    RUN_TEST(IgnoredSignalsDoNotStopTheServer);
    RUN_TEST(DaemonDetachesAndStillHandlesSigterm);
    RUN_TEST(ServerShutsDownCleanlyOnSigterm);
    return testing::Summary();
}
