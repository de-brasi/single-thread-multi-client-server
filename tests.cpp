#include <arpa/inet.h>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>
#include <iostream>
#include <vector>
#include <fstream>
#include <cassert>
#include <sys/wait.h>
#include <algorithm>

size_t PORT = 8080;
const char* HOST = "127.0.0.1";

void RunClients(int clients_count, std::string message_template, size_t messages_count) {
    pid_t child_pid, wpid;
    int status = 0;

    for (int i = 0; i < clients_count; ++i) {
        if ((child_pid = fork()) == 0) {
            // Child process.
            // Do client's work:

            int connection_status, client_file_descriptor;
            sockaddr_in serv_address{};

            message_template += std::to_string(i);
            char* message_to_server = new char [message_template.length() + 1];
            std::strcpy(message_to_server, message_template.c_str());

            client_file_descriptor = socket(AF_INET, SOCK_STREAM, 0);
            if (client_file_descriptor < 0) {
                std::cout << std::endl << "Socket creation error" << std::endl;
                return;
            }

            serv_address.sin_family = AF_INET;
            serv_address.sin_port = htons(PORT);

            // Convert IPv4 and IPv6 addresses from text to binary form (write to serv_address.sin_addr)
            if (inet_pton(AF_INET, HOST, &serv_address.sin_addr) <= 0) {
                std::cout << std::endl << "Invalid address/ Address not supported" << std::endl;
                close(client_file_descriptor);
                return;
            }

            connection_status = connect(client_file_descriptor, (const sockaddr *) &serv_address, (socklen_t) sizeof(serv_address));
            if (connection_status < 0) {
                std::cout << std::endl << "Connection Failed" << std::endl;
                close(client_file_descriptor);
                return;
            }

            for (int j = 0; j < messages_count; ++j) {
                // Send message to server
                auto res = send(client_file_descriptor, message_to_server, strlen(message_to_server), 0);
                if (res == -1) {
                    std::cout << "Failure to send" << std::endl;
                }
                sleep(1);
            }

            shutdown(client_file_descriptor, SHUT_RDWR);
            // Closing the connected socket
            close(client_file_descriptor);

            exit(0);
        } else {
            // Parent process.
            sleep(1);
            continue;
        }
    }

    while ((wpid = wait(&status)) > 0);     // father waits for all the child processes
}


std::vector<std::string> GetRowsFromFile(const std::string& file_path) {
    std::vector<std::string> res = {};
    std::ifstream content_source;
    content_source.open(file_path);

    if (content_source.is_open()) {
        std::string line;
        while (std::getline(content_source, line)) {
            res.push_back(line); // Добавляем строку в вектор
        }
    } // else empty vector

    return res;
}


void test_single_send_less_or_equal_then_max_value(int server_max_clients_count, const std::string& destination_file) {
    // Одиночная отправка (с расчётом на то, что сообщение обработается сервером менее чем за 1 секунду
    // до следующей записи) - можно практически точно проверить корректность порядка записи.

    assert(server_max_clients_count > 0);

    // Clear file
    std::ofstream output_file(destination_file, std::ios::trunc);
    output_file.close();

    int clients_count = std::max(server_max_clients_count - 1, 1);
    std::string text_template = "test";
    RunClients(clients_count, text_template, /*single message per client*/ 1);

    std::vector<std::string> content = GetRowsFromFile(destination_file);

    std::vector<std::string> expected_content;
    for (int i = 0; i < clients_count; ++i) {
        expected_content.emplace_back(text_template + std::to_string(i));
    }

    // Order is important
    assert(content == expected_content);
    std::cout << "test_single_send_less_or_equal_then_max_value OK!" << std::endl;
}

void test_multiple_sends_less_or_equal_then_max_value(int server_max_clients_count, const std::string& destination_file) {
    // Множественная отправка - можно проверить только содержимое записи, но не порядок.

    assert(server_max_clients_count > 0);

    // Clear file
    std::ofstream output_file(destination_file, std::ios::trunc);
    output_file.close();

    int clients_count = std::max(server_max_clients_count - 1, 1);
    std::string text_template = "test";
    int send_per_client = 3;
    RunClients(clients_count, text_template, send_per_client);

    std::vector<std::string> content = GetRowsFromFile(destination_file);

    std::vector<std::string> expected_content;
    for (int i = 0; i < clients_count; ++i) {
        for (int j = 0; j < send_per_client; ++j) {
            expected_content.emplace_back(text_template + std::to_string(i));
        }
    }

    // Order is NOT important
    std::sort(expected_content.begin(), expected_content.end());
    std::sort(content.begin(), content.end());

    assert(content == expected_content);
    std::cout << "test_multiple_sends_less_or_equal_then_max_value OK!" << std::endl;
}


int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Not enough arguments" << std::endl;
        exit(0);
    }

    int max_count = std::atoi(argv[1]);
    const std::string file_to_store_path = argv[2];

    test_single_send_less_or_equal_then_max_value(max_count, file_to_store_path);
    test_multiple_sends_less_or_equal_then_max_value(max_count, file_to_store_path);

    return 0;
}
