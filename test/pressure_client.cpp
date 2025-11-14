// pressure_client_fixed.cpp
#include <boost/asio.hpp>
#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>

using namespace boost::asio;
using ip::tcp;

std::atomic<uint64_t> g_success{0}, g_fail{0};

void session_long(tcp::endpoint endpoint, int id, int req_per_sec, int duration_sec) {
    io_context io;
    tcp::socket socket(io);
    try {
        socket.connect(endpoint);
        g_success++; // 连接成功计数
    } catch (std::exception &e) {
        g_fail++;
        std::cerr << "conn failed id=" << id << " : " << e.what() << "\n";
        return;
    }

    int interval_us = 1000000 / req_per_sec;
    auto start = std::chrono::steady_clock::now();
    while (true) {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - start).count() >= duration_sec) break;

        // 非阻塞发送数据，如果发送失败仅记录，不退出
        boost::system::error_code ec;
        std::string req = "ping";
        write(socket, buffer(req), ec);
        if (ec) g_fail++;

        std::this_thread::sleep_for(std::chrono::microseconds(interval_us));
    }
    socket.close();
}

void session_short(tcp::endpoint endpoint, int id, int duration_sec) {
    auto start = std::chrono::steady_clock::now();
    while (true) {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - start).count() >= duration_sec) break;

        try {
            io_context io;
            tcp::socket socket(io);
            socket.connect(endpoint);
            g_success++; // 每次短连接成功计数

            // 非阻塞发送
            boost::system::error_code ec;
            std::string req = "ping";
            write(socket, buffer(req), ec);
            if (ec) g_fail++;

            socket.close();
        } catch (std::exception &e) {
            g_fail++;
        }
    }
}

int main(int argc, char* argv[]) {
    if (argc < 6) {
        std::cout << "usage: " << argv[0] << " <host> <port> <connections> <req_per_sec_per_conn> <duration_sec> [mode=long|short]\n";
        return 1;
    }

    std::string host = argv[1];
    int port = std::atoi(argv[2]);
    int connections = std::atoi(argv[3]);
    int req_per_sec = std::atoi(argv[4]);
    int duration_sec = std::atoi(argv[5]);
    std::string mode = (argc > 6 ? argv[6] : "long");

    tcp::endpoint endpoint(ip::make_address(host), port);

    std::vector<std::thread> threads;
    for (int i = 0; i < connections; i++) {
        if (mode == "long")
            threads.emplace_back(session_long, endpoint, i, req_per_sec, duration_sec);
        else
            threads.emplace_back(session_short, endpoint, i, duration_sec);
    }

    for (auto &t : threads) t.join();

    std::cout << "connections success = " << g_success.load() << "\n";
    std::cout << "failures = " << g_fail.load() << "\n";
    std::cout << "duration = " << duration_sec << " seconds\n";
    double cps = double(g_success.load()) / duration_sec; // connections per second
    std::cout << "Connections/sec = " << cps << "\n";

    return 0;
}

