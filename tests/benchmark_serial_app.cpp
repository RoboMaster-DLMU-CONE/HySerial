#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#include <HySerial/Builder/Builder.hpp>

using namespace HySerial;
using namespace std::chrono;

static uint64_t now_ns()
{
    return static_cast<uint64_t>(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::cerr << "Usage: benchmark_serial_app <devA> <devB> [count] [payload_size]\n";
        return 2;
    }
    const char* devA = argv[1];
    const char* devB = argv[2];
    const int count = (argc >= 4) ? std::stoi(argv[3]) : 1000;
    const int payload_size = (argc >= 5) ? std::stoi(argv[4]) : 16;

    Builder bA;
    bA.device(devA).baud_rate(115200);
    Builder bB;
    bB.device(devB).baud_rate(115200);

    auto sa = bA.build();
    if (!sa)
    {
        std::cerr << "Failed to create A: " << sa.error().message << "\n";
        return 1;
    }
    auto serialA = std::move(sa.value());

    auto sb = bB.build();
    if (!sb)
    {
        std::cerr << "Failed to create B: " << sb.error().message << "\n";
        return 1;
    }
    auto serialB = std::move(sb.value());

    const size_t frame_size = sizeof(uint64_t) + sizeof(uint64_t) + payload_size; // seq + ts + payload

    std::vector<uint64_t> latencies_ns;
    latencies_ns.reserve(count);
    std::mutex lat_mtx;
    std::vector<std::byte> rx_accum;
    rx_accum.reserve(frame_size * 4);
    std::atomic<int> received{0};

    // read callback for A: accumulate bytes and parse frames
    std::vector<std::byte> local_acc;
    local_acc.reserve(frame_size * 4);
    std::atomic<int> debug_prints{0};
    serialA->set_read_callback([&](std::span<const std::byte> data)
    {
        if (data.empty()) return;
        {
            if (debug_prints.fetch_add(1) < 10)
            {
                std::cerr << "[DBG] read callback: " << data.size() << " bytes\n";
                size_t toshow = std::min<size_t>(data.size(), 8);
                std::cerr << "[DBG] first bytes:";
                for (size_t i = 0; i < toshow; ++i) std::cerr << ' ' << std::hex << (static_cast<int>(data[i]) & 0xFF);
                std::cerr << std::dec << "\n";
            }
            std::lock_guard lock(lat_mtx);
            // append incoming bytes
            local_acc.insert(local_acc.end(), data.begin(), data.end());
            // parse complete frames
            while (local_acc.size() >= frame_size)
            {
                uint64_t seq = 0;
                uint64_t ts = 0;
                std::memcpy(&seq, local_acc.data(), sizeof(uint64_t));
                std::memcpy(&ts, local_acc.data() + sizeof(uint64_t), sizeof(uint64_t));
                uint64_t now = now_ns();
                uint64_t lat = (now > ts) ? (now - ts) : 0;
                latencies_ns.push_back(lat);
                received.fetch_add(1);
                local_acc.erase(local_acc.begin(), local_acc.begin() + frame_size);
            }
        }
    });

    // start continuous read on A to ensure read SQEs are pending
    serialA->start_read(frame_size);
    // give a short moment for the background uring thread to queue the read
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // register write and error callbacks to help debug send/IO problems
    serialB->set_send_callback([&](ssize_t n)
    {
        (void)n; // we don't need to use it here, but logging may be enabled
        // optional: could log per-send, but avoid flooding output
    });
    serialB->set_error_callback([&](ssize_t e)
    {
        std::cerr << "serialB error callback: " << e << ' ' << std::strerror(-e) << '\n';
    });

    // Sender: send frames as fast as possible
    for (uint64_t i = 0; i < static_cast<uint64_t>(count); ++i)
    {
        std::vector<std::byte> frame(frame_size);
        uint64_t seq = i;
        uint64_t ts = now_ns();
        std::memcpy(frame.data(), &seq, sizeof(seq));
        std::memcpy(frame.data() + sizeof(seq), &ts, sizeof(ts));
        // fill payload with pattern
        for (int k = 0; k < payload_size; ++k) frame[sizeof(seq) + sizeof(ts) + k] = static_cast<std::byte>(k & 0xFF);
        serialB->send(std::span<const std::byte>(frame.data(), frame.size()));
        // occasional small yield to help delivery and avoid starving
        if ((i & 0x3FF) == 0) std::this_thread::sleep_for(std::chrono::microseconds(50));
        if ((i & 0x3FF) == 0) std::cerr << "sent " << i << " frames\n";
    }

    // wait for all messages or timeout
    auto start_wait = steady_clock::now();
    while (received.load() < count)
    {
        if (steady_clock::now() - start_wait > seconds(10)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // compute stats
    std::vector<uint64_t> latcopy;
    {
        std::lock_guard lock(lat_mtx);
        latcopy = latencies_ns;
    }

    if (latcopy.empty())
    {
        std::cerr << "No packets received\n";
        return 1;
    }

    std::ranges::sort(latcopy);
    uint64_t sum = std::accumulate(latcopy.begin(), latcopy.end(), static_cast<uint64_t>(0));
    double mean = static_cast<double>(sum) / static_cast<double>(latcopy.size());
    uint64_t p50 = latcopy[latcopy.size() / 2];
    uint64_t p95 = latcopy[std::min<size_t>(latcopy.size() - 1, (size_t)(latcopy.size() * 95 / 100))];
    uint64_t p99 = latcopy[std::min<size_t>(latcopy.size() - 1, (size_t)(latcopy.size() * 99 / 100))];
    uint64_t minv = latcopy.front();
    uint64_t maxv = latcopy.back();

    std::cout << "Messages sent: " << count << " received: " << latcopy.size() << "\n";
    std::cout << "Min(us): " << (minv / 1000.0) << " Mean(us): " << (mean / 1000.0) << " P50(us): " << (p50 / 1000.0)
        << " P95(us): " << (p95 / 1000.0) << " P99(us): " << (p99 / 1000.0) << " Max(us): " << (maxv / 1000.0) << "\n";

    return 0;
}
