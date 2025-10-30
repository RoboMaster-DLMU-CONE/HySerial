#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

#include <HySerial/Builder/Builder.hpp>

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::cerr << "Usage: test_serial_app <devA> <devB>\n";
        return 2;
    }
    const char* devA = argv[1];
    const char* devB = argv[2];

    using namespace HySerial;

    // Builder for A
    Builder bA;
    bA.device(devA).baud_rate(115200);

    // We'll create serial A
    auto sa = bA.build();
    if (!sa)
    {
        std::cerr << "Failed to create serial A: " << sa.error().message << "\n";
        return 1;
    }
    auto serialA = std::move(sa.value());

    // Builder for B
    Builder bB;
    bB.device(devB).baud_rate(115200);
    auto sb = bB.build();
    if (!sb)
    {
        std::cerr << "Failed to create serial B: " << sb.error().message << "\n";
        return 1;
    }
    auto serialB = std::move(sb.value());

    std::atomic<bool> received{false};

    // set read callback on A (now receives data span)
    serialA->set_read_callback([&](std::span<const std::byte> data)
    {
        received.store(true);
        std::cout << "serialA read callback: " << data.size() << " bytes\n";
    });

    // start continuous read on A
    serialA->start_read(1024);

    // prepare message and send from B
    const std::string msg = "hello-test";
    std::vector<std::byte> buf(msg.size());
    for (size_t i = 0; i < msg.size(); ++i) buf[i] = static_cast<std::byte>(msg[i]);

    serialB->send(std::span<const std::byte>(buf.data(), buf.size()));

    // wait up to 2 seconds
    for (int i = 0; i < 200; ++i)
    {
        if (received.load())
        {
            std::cout << "Test passed: data received\n";
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::cerr << "Test failed: timeout waiting for data\n";
    return 1;
}
