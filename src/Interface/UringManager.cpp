#include <cerrno>
#include <cstring>
#include <format>
#include <HySerial/Interface/UringManager.hpp>
#include <iostream>
#include <stdexcept>

namespace HySerial
{
    // helper to convert between id and user_data pointer-sized storage
    static inline void set_user_data(io_uring_sqe* sqe, uint64_t id)
    {
        io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(static_cast<uintptr_t>(id)));
    }

    static inline uint64_t get_user_data_id(const io_uring_cqe* cqe)
    {
        return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(io_uring_cqe_get_data(cqe)));
    }

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

        // allocate id
        uint64_t id = m_next_request_id.fetch_add(1);

        // Single critical section: protect active_requests and submission with m_submit_lock
        m_submit_lock.lock();
        io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
        if (!sqe)
        {
            m_submit_lock.unlock();
            // No SQE available; remove request record not necessary since not yet inserted
            return;
        }

        RequestRecord rec;
        rec.is_write = false;
        rec.fd = m_fd;
        m_active_requests.emplace(id, std::move(rec));

        io_uring_prep_read(sqe, m_fd, m_read_buffer.data(), m_read_buffer.size(), -1);
        set_user_data(sqe, id);

        if (const int ret = io_uring_submit(&m_ring); ret < 0)
        {
            // cleanup
            m_active_requests.erase(id);
            m_submit_lock.unlock();
            throw std::runtime_error(std::string("io_uring_submit failed with ") + std::to_string(ret));
        }
        m_submit_lock.unlock();
    }

    void UringManager::submit_send(std::span<const std::byte> buffer)
    {
        if (m_fd < 0)
        {
            return;
        }

        auto buf = std::make_shared<std::vector<std::byte>>(buffer.begin(), buffer.end());

        // allocate id
        uint64_t id = m_next_request_id.fetch_add(1);

        // single critical section protecting both active_requests and submission
        m_submit_lock.lock();
        io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
        if (!sqe)
        {
            m_submit_lock.unlock();
            return;
        }

        RequestRecord rec;
        rec.is_write = true;
        rec.fd = m_fd;
        rec.buf = buf;
        rec.offset = 0;
        m_active_requests.emplace(id, std::move(rec));

        // prepare initial write for the whole buffer
        io_uring_prep_write(sqe, m_fd, buf->data(), buf->size(), -1);
        set_user_data(sqe, id);

        if (const int ret = io_uring_submit(&m_ring); ret < 0)
        {
            // cleanup
            m_active_requests.erase(id);
            m_submit_lock.unlock();
            throw std::runtime_error(std::string("io_uring_submit failed with ") + std::to_string(ret));
        }

        m_submit_lock.unlock();
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

            bool need_rearm_read = false;
            io_uring_for_each_cqe(&m_ring, head, cqe)
            {
                cqe_count++;

                uint64_t id = get_user_data_id(cqe);
                if (id == 0)
                {
                    continue;
                }

                // lookup and copy request record atomically, then release lock before any submit calls
                m_submit_lock.lock();
                auto it = m_active_requests.find(id);
                if (it == m_active_requests.end())
                {
                    m_submit_lock.unlock();
                    continue;
                }
                RequestRecord record = it->second; // copy the shared_ptr and small fields
                m_submit_lock.unlock();

                bool is_write = record.is_write;
                int res = cqe->res;
                if (!is_write)
                {
                    // read completion: report bytes read and mark rearm if needed
                    if (res < 0)
                    {
                        m_error_lock.lock();
                        auto ecb = m_error_cb;
                        m_error_lock.unlock();
                        if (ecb)
                        {
                            ecb(res);
                        }
                        else
                        {
                            std::cerr << "[FATAL] UringManager: Read error res=" << res << ' ' << std::strerror(-res) <<
                                '\n';
                        }
                        // cleanup record
                        m_submit_lock.lock();
                        m_active_requests.erase(id);
                        m_submit_lock.unlock();
                        continue;
                    }

                    m_read_lock.lock();
                    auto rcb = m_read_cb;
                    m_read_lock.unlock();
                    if (rcb)
                    {
                        // create a span referencing the internal read buffer of length 'res'
                        std::span<const std::byte> data(reinterpret_cast<const std::byte*>(m_read_buffer.data()),
                                                        static_cast<size_t>(res));
                        rcb(data);
                    }

#ifdef HS_ENABLE_STASIS
                    if (m_stasis) m_stasis->record_receive(static_cast<uint64_t>(res));
#endif

                    // remove record and request rearm after CQ processing
                    m_submit_lock.lock();
                    m_active_requests.erase(id);
                    m_submit_lock.unlock();
                    if (m_continue_read.load(std::memory_order_relaxed))
                    {
                        need_rearm_read = true;
                    }
                    continue;
                }

                // write path
                if (res == -EINTR)
                {
                    // retry same offset; prepare submit without holding m_active_lock
                    m_submit_lock.lock();
                    io_uring_sqe* sqe_retry = io_uring_get_sqe(&m_ring);
                    if (!sqe_retry)
                    {
                        m_submit_lock.unlock();
                        m_error_lock.lock();
                        auto ecb = m_error_cb;
                        m_error_lock.unlock();
                        if (ecb) ecb(-EINTR);
                        // cleanup record
                        m_submit_lock.lock();
                        m_active_requests.erase(id);
                        m_submit_lock.unlock();
                        continue;
                    }
                    size_t remaining = record.buf->size() - record.offset;
                    io_uring_prep_write(sqe_retry, record.fd, record.buf->data() + record.offset, remaining, -1);
                    set_user_data(sqe_retry, id);
                    if (const int ret = io_uring_submit(&m_ring); ret < 0)
                    {
                        m_submit_lock.unlock();
                        m_error_lock.lock();
                        auto ecb = m_error_cb;
                        m_error_lock.unlock();
                        if (ecb) ecb(ret);
                        m_submit_lock.lock();
                        m_active_requests.erase(id);
                        m_submit_lock.unlock();
                    }
                    else
                    {
                        m_submit_lock.unlock();
                    }
                    continue;
                }

                if (res < 0)
                {
                    m_error_lock.lock();
                    auto ecb = m_error_cb;
                    m_error_lock.unlock();
                    if (ecb)
                    {
                        ecb(res);
                    }
                    else
                    {
                        std::cerr << "[FATAL] UringManager: Write error res=" << res << ' ' << std::strerror(-res) <<
                            '\n';
                    }
                    m_submit_lock.lock();
                    m_active_requests.erase(id);
                    m_submit_lock.unlock();
                    continue;
                }

                // success: update offset and check completion
                size_t new_offset = record.offset + static_cast<size_t>(res);
                if (new_offset < record.buf->size())
                {
                    // partial write, resubmit remaining without holding m_active_lock
                    m_submit_lock.lock();
                    io_uring_sqe* sqe_next = io_uring_get_sqe(&m_ring);
                    if (!sqe_next)
                    {
                        m_submit_lock.unlock();
                        m_error_lock.lock();
                        auto ecb = m_error_cb;
                        m_error_lock.unlock();
                        if (ecb) ecb(-EAGAIN);
                        m_submit_lock.lock();
                        m_active_requests.erase(id);
                        m_submit_lock.unlock();
                        continue;
                    }
                    size_t remaining = record.buf->size() - new_offset;
                    io_uring_prep_write(sqe_next, record.fd, record.buf->data() + new_offset, remaining, -1);
                    set_user_data(sqe_next, id);
                    if (const int ret = io_uring_submit(&m_ring); ret < 0)
                    {
                        m_submit_lock.unlock();
                        m_error_lock.lock();
                        auto ecb = m_error_cb;
                        m_error_lock.unlock();
                        if (ecb) ecb(ret);
                        m_submit_lock.lock();
                        m_active_requests.erase(id);
                        m_submit_lock.unlock();
                    }
                    else
                    {
                        m_submit_lock.unlock();
                        // update stored offset now that resubmit succeeded
                        m_submit_lock.lock();
                        auto it2 = m_active_requests.find(id);
                        if (it2 != m_active_requests.end()
                        )
                        {
                            it2->second.offset = new_offset;
                        }
                        m_submit_lock.unlock();
                    }
                    continue;
                }

                // fully written - call write callback and erase record
                m_write_lock.lock();
                auto wcb = m_write_cb;
                m_write_lock.unlock();
                if (wcb)
                {
                    wcb(static_cast<IoResult>(new_offset));
                }

#ifdef HS_ENABLE_STASIS
                if (m_stasis) m_stasis->record_send(static_cast<uint64_t>(new_offset));
#endif

                m_submit_lock.lock();
                m_active_requests.erase(id);
                m_submit_lock.unlock();
            }

            if (cqe_count > 0)
            {
                io_uring_cq_advance(&m_ring, cqe_count);
            }

            // re-arm read if any completion requested it
            if (need_rearm_read)
            {
                submit_read();
            }
        }
    }

    void UringManager::stop()
    {
        m_is_running.store(false);
        m_submit_lock.lock();
        if (io_uring_sqe* sqe = io_uring_get_sqe(&m_ring))
        {
            io_uring_prep_nop(sqe);
            set_user_data(sqe, 0);
            io_uring_submit(&m_ring);
        }
        m_submit_lock.unlock();
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
        uint64_t id = m_next_request_id.fetch_add(1);

        // insert record first
        RequestRecord rec;
        rec.cb = std::move(callback);
        rec.is_write = false;
        rec.fd = m_fd;
        m_submit_lock.lock();
        m_active_requests.emplace(id, std::move(rec));
        m_submit_lock.unlock();

        // submit SQE
        m_submit_lock.lock();
        set_user_data(sqe, id);

        if (const int ret = io_uring_submit(&m_ring); ret < 0)
        {
            m_submit_lock.unlock();
            m_submit_lock.lock();
            m_active_requests.erase(id);
            m_submit_lock.unlock();
            throw std::runtime_error(std::string("io_uring_submit failed with ") + std::to_string(ret));
        }
        m_submit_lock.unlock();
    }

    void UringManager::bind_fd(int fd)
    {
        m_fd = fd;
    }
}
