#ifndef HYSERIAL_URINGMANAGER_HPP
#define HYSERIAL_URINGMANAGER_HPP

#include <liburing.h>
#include <functional>
#include <memory>
#include <span>
#include <atomic>
#include <vector>
#include <ankerl/unordered_dense.h>

#include <tl/expected.hpp>
#include <HySerial/Util/Error.hpp>

#include <HySerial/Util/SpinLock.hpp>

#ifdef HS_ENABLE_STASIS
#include <HySerial/Stats.hpp>
#endif


namespace HySerial
{
    // 使用ssize_t来表示I/O操作的结果，与read/write的返回值保持一致
    using IoResult = ssize_t;

    // 定义两种回调的类型签名
    using ReadCallback = std::function<void(IoResult bytes_read)>;
    using WriteCallback = std::function<void(IoResult bytes_written)>;
    using ErrorCallback = std::function<void(IoResult error_code)>;

    class UringManager
    {
    public:
        // 工厂函数，推荐的创建方式
        static tl::expected<std::unique_ptr<UringManager>, Error> create(unsigned int queue_depth = 256);

        // 析构函数负责清理io_uring
        ~UringManager();

        // 禁止拷贝和移动，因为它管理着一个唯一的内核资源
        UringManager(const UringManager&) = delete;
        UringManager& operator=(const UringManager&) = delete;

        /**
         * @brief 提交一个异步写请求。函数会将传入的 buffer 复制到内部堆内存，
         * 以保证在 I/O 完成前数据有效性。
         * @param buffer 要发送的数据（会被复制）
         */
        void submit_send(std::span<const std::byte> buffer);

        /**
         * @brief 启动事件循环。此函数将阻塞，直到 stop() 被调用。
         */
        void run();

        /**
         * @brief 停止事件循环。可以从另一个线程调用此函数。
         */
        void stop();

        /**
         * @brief 注册或修改默认的读取回调
         */
        void register_read_callback(ReadCallback cb);

        /**
         * @brief 注册或修改默认的写入回调
         */
        void register_write_callback(WriteCallback cb);

        void register_error_callback(ErrorCallback cb);

        /**
         * @brief 将本 manager 绑定到单个 fd 并为其启用连续读取
         * @param fd 文件描述符
         * @param buf_size 每次读取的缓冲区大小
         */
        void start_read_for_fd(int fd, size_t buf_size = 4096);

        /**
         * @brief 停止对已绑定 fd 的连续读取
         */
        void stop_read_for_fd();

        /**
         * @brief 将管理器绑定到 fd（仅设置 fd，用于 write 或延迟启动 read）
         */
        void bind_fd(int fd);

    private:
        // 私有构造函数，强制使用 create() 工厂
        UringManager();

        // 通用的完成回调类型，它接收整个 CQE 作为参数
        using CompletionCallback = std::function<void(const io_uring_cqe*)>;

        // 初始化 io_uring 实例
        tl::expected<void, Error> initialize(unsigned int queue_depth);

        // 提交一个请求并注册其回调
        void submit_request(struct io_uring_sqe* sqe, CompletionCallback callback);

        // 提交异步读请求（不对外公开）
        void submit_read();

        io_uring m_ring{};
        std::atomic<bool> m_is_running{false};

        uint64_t m_next_request_id{1};
        ankerl::unordered_dense::map<uint64_t, CompletionCallback> m_active_requests;
        // lock protecting m_active_requests for cross-thread usage
        SpinLock m_active_lock;

        // single-fd helpers
        int m_fd{-1};
        std::vector<std::byte> m_read_buffer;
        std::atomic<bool> m_continue_read{false};

        // callbacks protected by spinlocks
        SpinLock m_read_lock;
        SpinLock m_write_lock;
        SpinLock m_error_lock;
        ReadCallback m_read_cb;
        WriteCallback m_write_cb;
        ErrorCallback m_error_cb;

#ifdef HS_ENABLE_STASIS
        // optional stats collector
        std::unique_ptr<Stasis> m_stasis;
#endif
    };
}

#endif //HYSERIAL_URINGMANAGER_HPP
