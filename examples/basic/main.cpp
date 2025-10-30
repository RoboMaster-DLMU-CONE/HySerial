#include <chrono>
#include <iostream>
#include <thread>

#include <HySerial/HySerial.hpp>

int main()
{
    // Change this to your serial device
    constexpr const char* DEVICE_PATH = "/dev/ttyUSB0";

    using namespace HySerial;

    Builder builder;
    builder.device(DEVICE_PATH)
           .baud_rate(115200)
           .data_bits(DataBits::BITS_8)
           .parity(Parity::NONE)
           .stop_bits(StopBits::ONE);

    // register simple callbacks
    builder.on_read([](ssize_t n)
    {
        std::cout << "Read callback: " << n << " bytes\n";
    });

    builder.on_write([](ssize_t n)
    {
        std::cout << "Write callback: " << n << " bytes written\n";
    });

    builder.on_error([](ssize_t e)
    {
        std::cerr << "I/O error: " << e << "\n";
    });

    auto serial_or_err = builder.build();
    if (!serial_or_err)
    {
        std::cerr << "Failed to create Serial: " << serial_or_err.error().message << "\n";
        return 1;
    }

    auto serial = std::move(serial_or_err.value());

    // set callbacks again (optional) and start reading
    serial->set_read_callback([](ssize_t n) { std::cout << "(active) Read " << n << " bytes\n"; });
    serial->set_send_callback([](ssize_t n) { std::cout << "(active) Sent " << n << " bytes\n"; });

    serial->start_read(4096);

    // send a test message
    const std::string msg = "Hello, serial\n";
    std::vector<std::byte> buf(msg.size());
    for (size_t i = 0; i < msg.size(); ++i) buf[i] = static_cast<std::byte>(msg[i]);
    serial->send(std::span<const std::byte>(buf.data(), buf.size()));

    // Let it run a little
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // stop reading and exit
    serial->stop_read();

    return 0;
}
