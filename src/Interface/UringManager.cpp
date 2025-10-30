#include <cerrno>
#include <cstring>
#include <format>
#include <HySerial/Interface/UringManager.hpp>
#include <iostream>
#include <stdexcept>

namespace HySerial
{
    tl::expected<std::unique_ptr<UringManager>, Error> UringManager::create(const unsigned int queue_depth)
    {
        struct enabler : UringManager
        {
        };
        auto manager = std::make_unique<enabler>();
        auto res = manager->initialize(queue_depth);
        if (res)
        {
            // initialize stasis if enabled
#ifdef HS_ENABLE_STASIS
            manager->m_stasis = std::make_unique<Stasis>();
#endif
            return manager;
        }
        return tl::make_unexpected(res.error());
    }

    UringManager::UringManager() = default;

    UringManager::~UringManager()
    {
        if (m_is_running.load())
        {
            stop();
        }
        io_uring_queue_exit(&m_ring);
    }

    void UringManager::register_read_callback(ReadCallback cb)
    {
        m_read_lock.lock();
        m_read_cb = std::move(cb);
        m_read_lock.unlock();
    }

    void UringManager::register_write_callback(WriteCallback cb)
    {
        m_write_lock.lock();
        m_write_cb = std::move(cb);
        m_write_lock.unlock();
    }

    void UringManager::register_error_callback(ErrorCallback cb)
    {
        m_error_lock.lock();
        m_error_cb = std::move(cb);
        m_error_lock.unlock();
    }

    void UringManager::start_read_for_fd(int fd, size_t buf_size)
    {
        m_fd = fd;
        m_read_buffer.assign(buf_size, std::byte{0});
        m_continue_read.store(true, std::memory_order_relaxed);
        submit_read();
    }

    void UringManager::stop_read_for_fd()
    {
        m_continue_read.store(false, std::memory_order_relaxed);
    }

    void UringManager::submit_read()
    {
        if (m_fd < 0)
        {
            std::cerr << "[FATAL] UringManager: submit_read called with invalid fd\n";
            return;
        }

        io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
        if (!sqe)
        {
            // drop if no sqe available; non-fatal
            return;
        }

        io_uring_prep_read(sqe, m_fd, m_read_buffer.data(), m_read_buffer.size(), -1);

        CompletionCallback completion_cb = [this](const io_uring_cqe* cqe)
        {
            if (cqe->res < 0)
            {
                m_error_lock.lock();
                auto ecb = m_error_cb;
                m_error_lock.unlock();
                if (ecb)
                {
                    ecb(cqe->res);
                }
                else
                {
                    std::cerr << "[FATAL] UringManager: Read error res=" << cqe->res << '\n';
                }
            }
            else
            {
                m_read_lock.lock();
                auto cb = m_read_cb;
                m_read_lock.unlock();
                // record receive
#ifdef HS_ENABLE_STASIS
                if (m_stasis)
                {
                    m_stasis->record_receive(static_cast<uint64_t>(cqe->res));
                }
#endif
                if (cb)
                {
                    cb(cqe->res);
                }
            }

            if (m_continue_read.load(std::memory_order_relaxed))
            {
                submit_read();
            }
        };

        submit_request(sqe, std::move(completion_cb));
    }

    void UringManager::submit_send(std::span<const std::byte> buffer)
    {
        if (m_fd < 0)
        {
            return;
        }

        auto buf = std::make_shared<std::vector<std::byte>>(buffer.begin(), buffer.end());

        io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
        if (!sqe)
        {
            // drop if no sqe available; non-fatal
            return;
        }

        io_uring_prep_write(sqe, m_fd, buf->data(), buf->size(), -1);

        CompletionCallback completion_cb = [this, buf](const io_uring_cqe* cqe)
        {
            (void)buf; // keep shared_ptr alive and silence unused-capture warning
            if (cqe->res < 0)
            {
                m_error_lock.lock();
                auto ecb = m_error_cb;
                m_error_lock.unlock();
                if (ecb)
                {
                    ecb(cqe->res);
                }
                else
                {
                    std::cerr << "[FATAL] UringManager: Write error res=" << cqe->res << '\n';
                }
                return;
            }
            m_write_lock.lock();
            auto cb = m_write_cb;
            m_write_lock.unlock();
            if (cb)
            {
                cb(cqe->res);
            }

#ifdef HS_ENABLE_STASIS
            if (m_stasis) m_stasis->record_send(static_cast<uint64_t>(cqe->res));
#endif
        };

        submit_request(sqe, std::move(completion_cb));
    }

    void UringManager::run()
    {
        m_is_running.store(true);

        while (m_is_running.load())
        {
            io_uring_submit_and_wait(&m_ring, 1);

            io_uring_cqe* cqe;
            unsigned head;
            unsigned cqe_count = 0;

            io_uring_for_each_cqe(&m_ring, head, cqe)
            {
                cqe_count++;
                uint64_t request_id = cqe->user_data;

                if (request_id == 0)
                {
                    continue;
                }

                CompletionCallback cb;
                m_active_lock.lock();
                auto it = m_active_requests.find(request_id);
                if (it != m_active_requests.end())
                {
                    cb = it->second;
                    m_active_requests.erase(it);
                }
                m_active_lock.unlock();

                if (cb)
                {
                    cb(cqe);
                }
            }

            if (cqe_count > 0)
            {
                io_uring_cq_advance(&m_ring, cqe_count);
            }
        }
    }

    void UringManager::stop()
    {
        m_is_running.store(false);
        if (io_uring_sqe* sqe = io_uring_get_sqe(&m_ring))
        {
            io_uring_prep_nop(sqe);
            io_uring_sqe_set_data(sqe, nullptr);
            io_uring_submit(&m_ring);
        }
    }

    tl::expected<void, Error> UringManager::initialize(const unsigned int queue_depth)
    {
        if (io_uring_queue_init(queue_depth, &m_ring, 0) < 0)
        {
            return tl::make_unexpected(Error{
                ErrorCode::UringInitError, std::string("UringManager init failed: ") + strerror(errno)
            });
        }
        return {};
    }

    void UringManager::submit_request(io_uring_sqe* sqe, CompletionCallback callback)
    {
        const uint64_t request_id = m_next_request_id++;

        m_active_lock.lock();
        m_active_requests.emplace(request_id, std::move(callback));
        m_active_lock.unlock();

        io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(request_id));

        if (const int ret = io_uring_submit(&m_ring); ret < 0)
        {
            throw std::runtime_error(std::string("io_uring_submit failed with ") + std::to_string(ret));
        }
    }

    void UringManager::bind_fd(int fd)
    {
        m_fd = fd;
    }
}
