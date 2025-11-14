// pressure_client_async_legacy.cpp
#include <boost/asio.hpp>
#include <iostream>
#include <vector>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

using namespace boost::asio;
using ip::tcp;

std::atomic<uint64_t> g_success{0}, g_fail{0};

struct Connection : std::enable_shared_from_this<Connection> {
    tcp::socket socket;
    io_context& io;           // 保存 io_context 引用
    std::string host;
    int port;
    int req_per_sec;
    int duration_sec;
    std::chrono::steady_clock::time_point start;
    bool long_conn;

    Connection(io_context& io_ctx, const std::string& h, int p, int rps, int dur, bool is_long)
        : socket(io_ctx), io(io_ctx), host(h), port(p),
          req_per_sec(rps), duration_sec(dur),
          start(std::chrono::steady_clock::now()), long_conn(is_long) {}

    void start_connect() {
        auto self = shared_from_this();
        tcp::resolver resolver(io);
        resolver.async_resolve(host, std::to_string(port),
            [this, self](const boost::system::error_code& ec, tcp::resolver::results_type results){
                if (ec) { g_fail++; return; }
                async_connect(socket, results,
                    [this, self](const boost::system::error_code& ec, const tcp::endpoint&){
                        if (ec) { g_fail++; return; }
                        g_success++;
                        if (long_conn) do_write();
                        else do_short();
                    });
            });
    }

    void do_write() {
        auto self = shared_from_this();
        std::string msg = "ping";
        async_write(socket, buffer(msg),
            [this, self](const boost::system::error_code& ec, std::size_t){
                if (ec) { g_fail++; return; }
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now - start).count() >= duration_sec) {
                    socket.close();
                    return;
                }
                // 控制每秒请求数
                int interval_us = req_per_sec > 0 ? 1000000 / req_per_sec : 1000;
                boost::asio::steady_timer timer(io, std::chrono::microseconds(interval_us));
                timer.async_wait([this, self](const boost::system::error_code&){
                    do_write();
                });
            });
    }

    void do_short() {
        auto self = shared_from_this();
        std::string msg = "ping";
        async_write(socket, buffer(msg),
            [this, self](const boost::system::error_code& ec, std::size_t){
                if (ec) { g_fail++; return; }
                char reply[128];
                socket.async_read_some(buffer(reply, sizeof(reply)),
                    [this, self](const boost::system::error_code& ec, std::size_t){
                        if (ec) { g_fail++; return; }
                        g_success++;
                        socket.close();
                        auto now = std::chrono::steady_clock::now();
                        if (std::chrono::duration_cast<std::chrono::seconds>(now - start).count() < duration_sec) {
                            // 重新建立短连接
                            start_connect();
                        }
                    });
            });
    }
};

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
    bool long_conn = (mode == "long");

    io_context io;

    std::vector<std::shared_ptr<Connection>> conns;
    for (int i = 0; i < connections; i++) {
        auto c = std::make_shared<Connection>(io, host, port, req_per_sec, duration_sec, long_conn);
        conns.push_back(c);
        c->start_connect();
    }

    auto start_time = std::chrono::steady_clock::now();
    std::thread t([&io](){ io.run(); });

    // 主线程监控时间
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count() >= duration_sec) break;
    }

    io.stop();
    t.join();

    std::cout << "connections success = " << g_success.load() << "\n";
    std::cout << "failures = " << g_fail.load() << "\n";
    std::cout << "duration = " << duration_sec << " seconds\n";
    double qps = g_success.load() * (long_conn ? req_per_sec : 1) / (double)duration_sec;
    std::cout << "QPS = " << qps << "\n";

    return 0;
}

