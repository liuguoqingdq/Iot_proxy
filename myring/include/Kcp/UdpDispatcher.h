#ifndef MYRING_KCP_UDP_DISPATCHER_H
#define MYRING_KCP_UDP_DISPATCHER_H

#include <sys/socket.h>

#include <atomic>      // std::atomic，用于线程间安全地标记运行状态
#include <cstddef>     // std::size_t
#include <cstdint>     // std::uint16_t、std::uint32_t等固定宽度整数类型
#include <functional>  // std::function，用于保存回调函数
#include <memory>      // std::unique_ptr，用于管理ProactorThread生命周期
#include <string>      // std::string，用于bind时传入host地址

#include "Proactor/ProactorThread.hpp"  // Proactor线程封装，负责io_uring事件循环
#include "Proactor/Request.hpp"         // RequestContext，请求上下文，用于区分recv/send等I/O请求
#include "nocopyable.h"                 // 禁止拷贝基类

namespace myring {
namespace kcp {

/**
 * @brief UDP消息接收回调类型。
 *
 * UdpDispatcher收到一个UDP数据报后，会通过该回调把数据交给上层。
 * 对于KCP场景，上层通常会根据data中的KCP包头提取conv，
 * 然后找到对应的KcpSession，并调用ikcp_input处理该数据包。
 *
 * @param peer 对端地址。
 *        该地址通常来自recvmsg/recvfrom返回的源地址。
 *
 * @param peer_len 对端地址长度。
 *        用于区分IPv4、IPv6等不同地址结构。
 *
 * @param data UDP数据报内容。
 *        对于KCP场景，这里通常是一整个KCP包。
 *
 * @param len UDP数据报长度。
 *
 * 注意：
 * data指向的内存一般属于某个接收RequestContext或内部buffer。
 * 如果上层需要在回调结束后继续保存数据，需要自行拷贝。
 */
using UdpMessageCallback =
    std::function<void(const sockaddr* peer,
                       socklen_t peer_len,
                       const char* data,
                       std::size_t len)>;

/**
 * @brief UDP异步收发调度器。
 *
 * UdpDispatcher负责管理一个UDP socket，并通过ProactorThread提交和处理异步I/O。
 * 它本身只关心UDP数据报的收发，不关心KCP协议细节。
 *
 * 典型工作流程：
 *
 * 1.调用bind绑定本地地址和端口；
 * 2.调用set_message_callback设置接收回调；
 * 3.调用start启动Proactor线程和异步接收；
 * 4.底层收到UDP包后触发handle_recv；
 * 5.handle_recv通过message_callback_把数据交给上层；
 * 6.上层需要发送数据时调用send_to；
 * 7.send_to提交异步UDP发送请求；
 * 8.发送完成后触发handle_send。
 *
 * 该类不可拷贝，避免socket_fd_、io_thread_等资源被重复管理。
 */
class UdpDispatcher : private nocopyable {
public:
    /**
     * @brief 构造UDP调度器。
     *
     * 构造函数通常只初始化成员变量和ProactorThread对象，
     * 不一定会立即创建或绑定socket。真正的地址绑定由bind完成。
     *
     * @param entries io_uring队列深度。
     *        该值决定io_uring提交队列和完成队列的大致容量。
     *        值越大，可以同时挂起的I/O请求越多，但也会占用更多资源。
     *
     * @param max_events 每次事件循环最多处理的完成事件数量。
     *        ProactorThread内部可能会批量获取CQE，
     *        max_events用于限制一次批处理的最大事件数。
     *
     * @param recv_buffer_size 每个UDP接收缓冲区大小。
     *        UDP是数据报协议，如果收到的数据报长度超过该缓冲区，
     *        多余部分可能会被截断。因此该值需要结合业务MTU设置。
     */
    explicit UdpDispatcher(unsigned entries = 256,
                           std::size_t max_events = 64,
                           std::size_t recv_buffer_size = 2048);

    /**
     * @brief 析构UDP调度器。
     *
     * 析构时通常需要：
     *
     * 1.停止Proactor线程；
     * 2.关闭socket_fd_；
     * 3.释放尚未完成或已经完成的RequestContext资源。
     *
     * 如果stop已经被显式调用，析构函数应当能够安全处理重复清理问题。
     */
    ~UdpDispatcher();

    /**
     * @brief 绑定本地UDP地址。
     *
     * 该函数通常会完成：
     *
     * 1.创建UDP socket；
     * 2.设置必要socket选项，例如SO_REUSEADDR；
     * 3.根据host和port构造sockaddr_in或sockaddr_in6；
     * 4.调用系统bind绑定地址；
     * 5.保存socket_fd_。
     *
     * @param host 本地监听地址。
     *        例如"0.0.0.0"表示监听所有IPv4网卡，
     *        "127.0.0.1"表示只监听本机回环地址。
     *
     * @param port 本地UDP端口。
     *
     * @return 绑定成功返回true，失败返回false。
     */
    bool bind(const std::string& host, std::uint16_t port);

    /**
     * @brief 设置UDP消息接收回调。
     *
     * 当底层收到一个完整UDP数据报后，UdpDispatcher会调用该回调。
     * 对于KCP层，上层可以在该回调中完成：
     *
     * 1.从data中解析KCP conv；
     * 2.查找或创建KcpSession；
     * 3.调用KcpSession::input_packet；
     * 4.进一步触发ikcp_recv读取应用层消息。
     *
     * @param cb 接收回调函数。
     */
    void set_message_callback(UdpMessageCallback cb);

    /**
     * @brief 启动UDP调度器。
     *
     * 该函数通常会启动ProactorThread中的io_uring事件循环，
     * 并提交第一个异步接收请求。
     *
     * 调用start前，一般应当先调用bind成功绑定socket。
     *
     * 注意：
     * start通常只负责启动I/O循环，不负责阻塞等待程序结束。
     */
    void start();

    /**
     * @brief 停止UDP调度器。
     *
     * 该函数用于停止接收新的I/O事件，并关闭或终止ProactorThread。
     * running_会被设置为false，后续handle_recv中不应继续arm_recv。
     *
     * stop需要考虑线程安全和重复调用问题。
     */
    void stop();

    /**
     * @brief 异步发送一个UDP数据报。
     *
     * 该函数会把data指定的数据发送给peer指定的对端地址。
     * 在KCP场景中，KcpSession的output回调最终可以调用该函数，
     * 将KCP生成的包通过UDP发送出去。
     *
     * @param peer 对端地址。
     * @param peer_len 对端地址长度。
     * @param data 待发送数据。
     * @param len 待发送数据长度。
     *
     * @return 提交发送请求成功返回true，失败返回false。
     *
     * 注意：
     * 如果该函数内部使用io_uring异步发送，那么不能直接保存data指针。
     * 因为调用方传入的data可能在函数返回后失效。
     * 正确做法是把data拷贝到发送RequestContext拥有的buffer中，
     * 等发送CQE完成后再释放或归还该buffer。
     */
    bool send_to(const sockaddr* peer,
                 socklen_t peer_len,
                 const char* data,
                 std::size_t len);
    bool schedule_timeout(std::uint64_t timeout_ms,
                          CompleteCallback cb,
                          void* user_data = nullptr);
    bool post(CompleteCallback cb, void* user_data = nullptr);

    /**
     * @brief 获取底层UDP socket文件描述符。
     *
     * @return 当前socket_fd_。
     *
     * 如果尚未bind成功，可能返回-1。
     */
    int fd() const noexcept;

private:
    /**
     * @brief 提交一个异步UDP接收请求。
     *
     * 该函数通常会：
     *
     * 1.分配RequestContext；
     * 2.分配或绑定接收buffer；
     * 3.准备sockaddr_storage用于保存对端地址；
     * 4.通过ProactorThread提交recvmsg/recvfrom请求；
     * 5.设置完成回调为handle_recv。
     *
     * UDP服务端通常需要持续接收数据，所以每次handle_recv处理完成后，
     * 如果running_仍然为true，一般需要再次调用arm_recv，
     * 从而保持接收链路不断开。
     */
    void arm_recv();

    /**
     * @brief 处理UDP接收完成事件。
     *
     * ProactorThread从io_uring CQE中拿到接收结果后，
     * 会调用该函数处理一次recv请求的结果。
     *
     * @param ctx 本次接收请求对应的上下文。
     *        其中通常保存接收buffer、peer地址、msghdr、iovec等信息。
     *
     * @param res CQE返回结果。
     *        对于recv/recvmsg来说：
     *        res>0表示收到的字节数；
     *        res=0在UDP里一般不常见，可视情况处理；
     *        res<0表示错误码的负值。
     *
     * 典型处理逻辑：
     *
     * 1.如果res>0，调用message_callback_；
     * 2.释放或复用ctx中的接收buffer；
     * 3.如果running_为true，继续调用arm_recv提交下一次接收；
     * 4.如果发生错误，根据错误类型决定是否继续接收。
     */
    void handle_recv(RequestContext* ctx, int res);

    /**
     * @brief 处理UDP发送完成事件。
     *
     * ProactorThread从io_uring CQE中拿到发送结果后，
     * 会调用该函数处理一次send请求的结果。
     *
     * @param ctx 本次发送请求对应的上下文。
     *        其中通常保存发送buffer、peer地址、msghdr、iovec等信息。
     *
     * @param res CQE返回结果。
     *        对于send/sendmsg/sendto来说：
     *        res>=0通常表示已经提交给内核发送的字节数；
     *        res<0表示错误码的负值。
     *
     * 发送完成后，需要释放或归还ctx以及其持有的发送buffer。
     */
    void handle_send(RequestContext* ctx, int res);

private:
    /**
     * @brief 底层UDP socket文件描述符。
     *
     * bind成功后保存有效fd。
     * 初始值通常应为-1。
     * 析构或stop时需要关闭。
     */
    int socket_fd_;

    /**
     * @brief 单个接收buffer大小。
     *
     * arm_recv提交接收请求时，会按照该大小准备接收缓冲区。
     * 对于KCP场景，该值通常应不小于KCP的MTU配置。
     */
    std::size_t recv_buffer_size_;

    /**
     * @brief 运行状态标记。
     *
     * start时设置为true，stop时设置为false。
     * handle_recv中可以根据该标记决定是否继续提交下一次接收。
     *
     * 使用std::atomic可以避免I/O线程和调用线程之间的数据竞争。
     */
    std::atomic<bool> running_;

    /**
     * @brief UDP接收回调。
     *
     * 收到完整UDP数据报后，由handle_recv调用该回调交给上层处理。
     */
    UdpMessageCallback message_callback_;

    /**
     * @brief Proactor线程对象。
     *
     * 该对象通常封装io_uring实例和事件循环线程。
     * UdpDispatcher通过它提交异步recv/send请求，并在I/O完成后执行回调。
     */
    std::unique_ptr<ProactorThread> io_thread_;
};

}  // namespace kcp
}  // namespace myring

#endif
