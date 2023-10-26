#include <netinet/in.h>
#include <cstdio>
#include <cstdlib>
#include <sys/socket.h>
#include <unistd.h>
#include <iostream>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <cstring>
#include <iostream>
#include <fstream>
#include <csignal>
#include <sys/stat.h>
#include <syslog.h>

#define DEBUG

#ifdef DEBUG
#   define LOG_DEBUG(to_log) std::cout << to_log << std::endl;
#else
#   define LOG_DEBUG(to_log)
#endif

volatile int FD_SERVER;
const size_t PORT = 8080;
const char *HOST = "127.0.0.1";
const int MAX_EVENTS = 10;                // Max count of events got for handling after epoll wait
const size_t BUFFER_SIZE = 1024;
int MAX_CONN_COUNT;
std::ofstream STORE_FILE;

int make_server_fd(int *opt) {
    int fd_server = socket(AF_INET, SOCK_STREAM, 0);
    if (fd_server < 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    if (setsockopt(fd_server, SOL_SOCKET,               // Set options
                   SO_REUSEADDR | SO_REUSEPORT, &opt,
                   sizeof(*opt))) {
        shutdown(fd_server, SHUT_RDWR);
        close(fd_server);
        perror("set socket options");
        exit(EXIT_FAILURE);
    }

    return fd_server;
}

void bind_with_address(int fd_server, sockaddr_in &address) {
    int binding_result = bind(                          // Attaching socket to the address 127.0.0.1:8080
            fd_server,
            (const sockaddr *) &address,
            (socklen_t) sizeof(address)
    );
    if (binding_result == -1) {
        shutdown(fd_server, SHUT_RDWR);                 // Free socket
        close(fd_server);                               // Free socket
        perror("socket bind");
        exit(EXIT_FAILURE);
    }
}

int create_epoll() {
    int fd_epoll = epoll_create(1);

    if (fd_epoll == -1) {
        // errno == 22 is EINVAL
        LOG_DEBUG("Error while epoll creating. errno is " << errno)
        perror("epoll fd creation");
        exit(EXIT_FAILURE);
    }

    return fd_epoll;
}

void make_fd_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags == -1) {
        perror("fcntl failure");
        exit(1);
    }

    // modify flags for file descriptor
    flags |= O_NONBLOCK;
    fcntl(fd, F_SETFL, flags);
}

void sigterm_handler(int signum) {
    shutdown(FD_SERVER, SHUT_RDWR);
    close(FD_SERVER);

    exit(signum);
}

static void daemonize() {
    pid_t pid;

    /* Fork off the parent process */
    pid = fork();

    /* An error occurred */
    if (pid < 0)
        exit(EXIT_FAILURE);

    /* Success: Let the parent terminate */
    if (pid > 0)
        exit(EXIT_SUCCESS);

    /* On success: The child process becomes session leader */
    if (setsid() < 0)
        exit(EXIT_FAILURE);

    /* Catch, ignore and handle signals */
    std::signal(SIGTERM, sigterm_handler);
    std::signal(SIGQUIT, SIG_IGN);
    std::signal(SIGINT, SIG_IGN);
    std::signal(SIGHUP, SIG_IGN);
    std::signal(SIGSTOP, SIG_IGN);
    std::signal(SIGCONT, SIG_IGN);
    std::signal(SIGCHLD, SIG_IGN);
    std::signal(SIGHUP, SIG_IGN);

    /* Fork off for the second time*/
    pid = fork();

    /* An error occurred */
    if (pid < 0)
        exit(EXIT_FAILURE);

    /* Success: Let the parent terminate */
    if (pid > 0)
        exit(EXIT_SUCCESS);
}

int main(int argc, char* argv[]) {
    std::signal(SIGTERM,    sigterm_handler);
    std::signal(SIGQUIT,    SIG_IGN);
    std::signal(SIGINT,     SIG_IGN);
    std::signal(SIGHUP,     SIG_IGN);
    std::signal(SIGSTOP,    SIG_IGN);
    std::signal(SIGCONT,    SIG_IGN);


    if (argc < 3) {
        std::cerr << "Not enough arguments" << std::endl;
    }

    MAX_CONN_COUNT = std::atoi(argv[1]);
    const std::string file_to_store_path = argv[2];

    STORE_FILE.open(file_to_store_path, std::ios::app);

    if (not STORE_FILE.is_open()) {
        LOG_DEBUG("cant write")
        exit(EXIT_FAILURE);
    }

    daemonize();

    LOG_DEBUG("Current process id is: " << getpid() << std::endl)   // need to have an empty line between next line

    int opt = 1;
    FD_SERVER = make_server_fd(&opt);

    sockaddr_in address{};
    size_t address_length = sizeof(address);
    address.sin_family = AF_INET;                       // IPv4
    address.sin_addr.s_addr = inet_addr(HOST);          // Bind with localhost
    address.sin_port = htons(PORT);

    bind_with_address(FD_SERVER, address);

    make_fd_nonblock(FD_SERVER);

    epoll_event events[MAX_EVENTS];
    epoll_event event{};
    event.events = (EPOLLIN | EPOLLET | EPOLLRDHUP);
    event.data.fd = FD_SERVER;

    int fd_epoll = create_epoll();

    // enqueue the server file descriptor
    epoll_ctl(fd_epoll, EPOLL_CTL_ADD, FD_SERVER, &event);

    if (listen(FD_SERVER, SOMAXCONN) == -1) {           // Socket listening
        perror("listen");
        exit(EXIT_FAILURE);
    }

    char buffer[BUFFER_SIZE] = {0};


    int current_connections = 0;
    LOG_DEBUG("Current connections count: " << current_connections)

    while (true) {
        int events_handled = epoll_wait(fd_epoll, events, MAX_EVENTS, -1);

        if (events_handled == -1) {
            LOG_DEBUG("Error when epoll_wait. Errno value is " << errno)
            close(FD_SERVER);
            break;
        }

        for (int i = 0; i < events_handled; ++i) {
            // for every event in queue
            int got_event_fd = events[i].data.fd;
            uint32_t got_event_value = events[i].events;

            if (got_event_fd == FD_SERVER) {
                int new_connection_fd;
                while (
                        (new_connection_fd = accept(FD_SERVER, (sockaddr *) &address, (socklen_t *) &address_length))
                        != -1
                        ) {
                    // Get new connections
                    if (current_connections < MAX_CONN_COUNT) {
                        ++current_connections;
                        // Remember new connection
                        LOG_DEBUG("new connection")
                        LOG_DEBUG("Current connections count: " << current_connections)
                        make_fd_nonblock(new_connection_fd);
                        event.events = (EPOLLIN | EPOLLRDHUP);
                        event.data.fd = new_connection_fd;

                        // enqueue the new connection's file descriptor
                        epoll_ctl(fd_epoll, EPOLL_CTL_ADD, new_connection_fd, &event);
                    } else {
                        // Decline new connection
                        LOG_DEBUG("Oops! To many connections!")
                        close(new_connection_fd);
                    }
                }

            } else if (got_event_value & EPOLLIN) {
                // Get data
                memset(buffer, 0, BUFFER_SIZE);                 // Clear buffer
                auto read_bytes = read(got_event_fd, buffer, BUFFER_SIZE);

                if (read_bytes <= 0 and errno == EAGAIN) {
                    // Client is disconnected

                    --current_connections;

                    LOG_DEBUG("Some client disconnected")
                    LOG_DEBUG("Current connections count: " << current_connections)

                    shutdown(got_event_fd, SHUT_RDWR);
                    close(got_event_fd);

                } else if (read_bytes == -1) {
                    // Error got
                    perror("read connected failure");
                    exit(1);
                } else {
                    // Success reading
                    LOG_DEBUG("New message received: " << buffer)

                    STORE_FILE << buffer << std::endl;

                    event.events = (EPOLLIN | EPOLLET | EPOLLRDHUP);
                    event.data.fd = got_event_fd;

                    // enqueue the new connection's file descriptor
                    epoll_ctl(fd_epoll, EPOLL_CTL_MOD, got_event_fd, &event);
                }

            } else if (got_event_value & EPOLLRDHUP) {
                // Disconnect
                --current_connections;

                LOG_DEBUG("Some client disconnected")
                LOG_DEBUG("Current connections count: " << current_connections)

                shutdown(got_event_fd, SHUT_RDWR);
                close(got_event_fd);
            }
        }
    }

    shutdown(FD_SERVER, SHUT_RDWR);
    close(FD_SERVER);

    STORE_FILE.close();
    return 0;
}