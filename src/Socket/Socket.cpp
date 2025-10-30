#include <HySerial/Socket/Socket.hpp>

#include <cstring>
#include <fcntl.h>
#include <format>
#include <unistd.h>
#include <termios.h>
#include <cerrno>
#include <sys/ioctl.h>

using tl::unexpected, std::format;

namespace HySerial
{
    static speed_t baud_to_speed(uint32_t baud)
    {
        switch (baud)
        {
        case 0: return B0;
        case 50: return B50;
        case 75: return B75;
        case 110: return B110;
        case 134: return B134;
        case 150: return B150;
        case 200: return B200;
        case 300: return B300;
        case 600: return B600;
        case 1200: return B1200;
        case 1800: return B1800;
        case 2400: return B2400;
        case 4800: return B4800;
        case 9600: return B9600;
        case 19200: return B19200;
        case 38400: return B38400;
        case 57600: return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
#ifdef B460800
        case 460800: return B460800;
#endif
#ifdef B921600
        case 921600: return B921600;
#endif
        default: return 0;
        }
    }

    Socket::Socket(const SerialConfig& cfg) : config(cfg)
    {
    }

    Socket::~Socket()
    {
        if (sock_fd > 0)
        {
            close(sock_fd);
            sock_fd = -1;
        }
    }

    tl::expected<void, Error> Socket::ensure_connected() noexcept
    {
        if (sock_fd > 0)
        {
            close(sock_fd);
            sock_fd = -1;
        }

        const std::string dev_path = config.device_path;
        sock_fd = open(dev_path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (sock_fd == -1)
        {
            return unexpected(Error{
                ErrorCode::SocketCreateError, format("Failed to open serial device '{}': {}", dev_path, strerror(errno))
            });
        }

        termios tty{};
        if (tcgetattr(sock_fd, &tty) == -1)
        {
            close(sock_fd);
            sock_fd = -1;
            return unexpected(Error{
                ErrorCode::SocketBindError, format("Failed to get attributes for '{}': {}", dev_path, strerror(errno))
            });
        }

        // Set baud
        const speed_t speed = baud_to_speed(config.baud_rate);
        if (speed == 0)
        {
            close(sock_fd);
            sock_fd = -1;
            return unexpected(Error{
                ErrorCode::SocketBindError, format("Unsupported baud rate {}", config.baud_rate)
            });
        }
        if (cfsetispeed(&tty, speed) == -1 || cfsetospeed(&tty, speed) == -1)
        {
            close(sock_fd);
            sock_fd = -1;
            return unexpected(Error{
                ErrorCode::SocketBindError, format("Failed to set baud rate for '{}': {}", dev_path, strerror(errno))
            });
        }

        // Data bits
        tty.c_cflag &= ~CSIZE;
        switch (config.data_bits)
        {
        case DataBits::BITS_5: tty.c_cflag |= CS5;
            break;
        case DataBits::BITS_6: tty.c_cflag |= CS6;
            break;
        case DataBits::BITS_7: tty.c_cflag |= CS7;
            break;
        case DataBits::BITS_8: tty.c_cflag |= CS8;
            break;
        default: tty.c_cflag |= CS8;
            break;
        }

        // Parity
        if (config.parity == Parity::NONE)
        {
            tty.c_cflag &= ~PARENB;
        }
        else
        {
            tty.c_cflag |= PARENB;
            if (config.parity == Parity::ODD)
            {
                tty.c_cflag |= PARODD;
            }
            else
            {
                tty.c_cflag &= ~PARODD;
            }
        }

        // Stop bits
        if (config.stop_bits == StopBits::TWO)
        {
            tty.c_cflag |= CSTOPB;
        }
        else
        {
            tty.c_cflag &= ~CSTOPB;
        }

        // Flow control
        if (config.flow_control == FlowControl::RTS_CTS)
        {
            tty.c_cflag |= CRTSCTS;
        }
        else
        {
            tty.c_cflag &= ~CRTSCTS;
        }

        // Input flags - disable special handling
        tty.c_iflag &= ~(IGNBRK | BRKINT | ICRNL | INLCR | PARMRK | INPCK | ISTRIP | IXON);

        // Output flags - disable post processing
        tty.c_oflag &= ~OPOST;

        // Local flags - raw mode
        tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);

        // Control characters - block until at least 1 byte is available
        tty.c_cc[VMIN] = 1;
        tty.c_cc[VTIME] = 0;

        // Apply attributes
        if (tcsetattr(sock_fd, TCSANOW, &tty) == -1)
        {
            close(sock_fd);
            sock_fd = -1;
            return unexpected(Error{
                ErrorCode::SocketBindError, format("Failed to set attributes for '{}': {}", dev_path, strerror(errno))
            });
        }

        // Handle RTS/DTR if requested (platform dependent)
        if (config.rts_dtr_on)
        {
            int status;
            if (ioctl(sock_fd, TIOCMGET, &status) == -1)
            {
                // non-fatal; ignore
            }
            else
            {
                status |= TIOCM_RTS | TIOCM_DTR;
                if (ioctl(sock_fd, TIOCMSET, &status) == -1)
                {
                    // non-fatal; ignore
                }
            }
        }

        // Flush and ensure blocking (clear O_NONBLOCK)
        tcflush(sock_fd, TCIOFLUSH);
        const int flags = fcntl(sock_fd, F_GETFL, 0);
        fcntl(sock_fd, F_SETFL, flags & ~O_NONBLOCK);

        return {};
    }

    tl::expected<void, Error> Socket::validate_connection() noexcept
    {
        if (sock_fd <= 0)
        {
            return ensure_connected();
        }
        return {};
    }

    tl::expected<void, Error> Socket::flush() const noexcept
    {
        if (sock_fd < 0)
        {
            return unexpected(Error{ErrorCode::InvalidSocketError, "Cannot flush with invalid socket descriptor"});
        }

        if (tcflush(sock_fd, TCIOFLUSH) == -1)
        {
            return unexpected(Error{
                ErrorCode::SocketFlushError, format("Failed to flush serial device: {}", strerror(errno))
            });
        }

        return {};
    }
}
