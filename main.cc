#include "io_context.hh"
#include "socket.hh"

#include "lazy.hh"
#include <exception>
#include <unordered_map>
#include <string>
#include <iostream>

// 简单词典示例
std::unordered_map<std::string, std::string> dictionary = {
    {"apple", "A fruit that is round and red or green."},
    {"banana", "A long yellow fruit."},
    {"hello", "A greeting or expression of goodwill."}
};

// 处理普通文本查询
std::lazy<bool> inside_loop(Socket& socket)
{
    char buffer[128] = {0};
    ssize_t nbRecv = co_await socket.recv(buffer, sizeof(buffer));
    if (nbRecv <= 0)
        co_return false;

    std::string word(buffer, nbRecv);
    while (!word.empty() && (word.back() == '\n' || word.back() == '\r' || word.back() == ' '))
        word.pop_back();

    std::string reply;
    auto it = dictionary.find(word);
    if (it != dictionary.end())
        reply = it->second;
    else
        reply = "Word not found.";

    ssize_t nbSend = 0;
    while (nbSend < static_cast<ssize_t>(reply.size())) {
        ssize_t res = co_await socket.send(reply.data() + nbSend, reply.size() - nbSend);
        if (res <= 0)
            co_return false;
        nbSend += res;
    }

    co_return true;
}

// 处理HTTP请求
std::lazy<> echo_socket(std::shared_ptr<Socket> socket)
{
    char buffer[4096];
    ssize_t n = co_await socket->recv(buffer, sizeof(buffer) - 1);
    if (n <= 0) co_return;

    buffer[n] = 0;
    std::string req(buffer);

    // 解析HTTP请求行
    auto pos = req.find(' ');
    auto pos2 = req.find(' ', pos + 1);
    if(pos == std::string::npos || pos2 == std::string::npos) co_return;

    std::string path = req.substr(pos + 1, pos2 - pos - 1);

    // 从 URL 解析 ?word=xxx
    std::string word;
    auto pos_q = path.find("?word=");
    if (pos_q != std::string::npos) {
        word = path.substr(pos_q + 6);
    }

    std::string def = "Word not found.";
    auto it = dictionary.find(word);
    if (it != dictionary.end()) def = it->second;

    // HTTP 响应
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: " + std::to_string(def.size()) + "\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Connection: close\r\n"   // 新增
        "\r\n" +
        def;



    ssize_t sent = 0;
    while(sent < static_cast<ssize_t>(resp.size())) {
        ssize_t res = co_await socket->send(resp.data() + sent, resp.size() - sent);
        if(res <= 0) break;
        sent += res;
    }

    co_return;
}

// 接受连接并启动协程
std::lazy<> accept(Socket& listen)
{
    try {
        while (true) {
            auto task = listen.accept();
            auto socket = co_await task;
            auto t = echo_socket(socket);
            listen.getContext().spawn(std::move(t));
        }
    } catch (const std::exception& e) {
        std::cout << "exception (accept): " << e.what() << '\n';
    }
}

int main()
{
    IOContext io_context{};
    Socket listen{"8080", io_context};

    auto t = accept(listen);
    io_context.spawn(std::move(t));

    io_context.run();
}

