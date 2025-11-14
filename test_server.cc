#include "io_context.hh"
#include "socket.hh"
#include "lazy.hh"

#include <exception>
#include <unordered_map>
#include <string>
#include <iostream>
#include <memory>

// 简单词典
std::unordered_map<std::string, std::string> dictionary = {
    {"apple", "A fruit that is round and red or green."},
    {"banana", "A long yellow fruit."},
    {"hello", "A greeting or expression of goodwill."}
};

// 处理单个客户端请求（长连接）
std::lazy<> handle_client_long(std::shared_ptr<Socket> socket)
{
    char buffer[4096];

    while (true) {
        ssize_t n = co_await socket->recv(buffer, sizeof(buffer));
        if (n <= 0) {
            std::cout << "Client disconnected\n";
            co_return;
        }

        std::string req(buffer, n);
        while (!req.empty() && (req.back() == '\n' || req.back() == '\r' || req.back() == ' '))
            req.pop_back();

        std::string def = "Word not found.";
        auto it = dictionary.find(req);
        if (it != dictionary.end()) def = it->second;

        ssize_t sent = 0;
        while (sent < static_cast<ssize_t>(def.size())) {
            ssize_t res = co_await socket->send(def.data() + sent, def.size() - sent);
            if (res <= 0) co_return;
            sent += res;
        }
    }
}

// 接受连接并启动协程
std::lazy<> accept_loop(Socket& listen)
{
    try {
        while (true) {
            auto task = listen.accept();
            auto socket = co_await task;
            std::cout << "New client connected\n";
            auto t = handle_client_long(socket);
            listen.getContext().spawn(std::move(t));
        }
    } catch (const std::exception& e) {
        std::cout << "Exception in accept: " << e.what() << '\n';
    }
}

int main()
{
    try {
        IOContext io_context{};
        Socket listen{"8080", io_context};

        auto t = accept_loop(listen);
        io_context.spawn(std::move(t));

        std::cout << "Long-connection server running on port 8080...\n";
        io_context.run();
    } catch (const std::exception& e) {
        std::cerr << "Fatal exception: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
