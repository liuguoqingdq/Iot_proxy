#ifndef MYRING_KCP_KCP_SESSION_H
#define MYRING_KCP_KCP_SESSION_H

#include <sys/socket.h>  // sockaddr、socklen_t、sockaddr_storage等socket地址结构

#include <cstddef>       // std::size_t
#include <cstdint>       // std::uint32_t
#include <functional>    // std::function

#include "Kcp/KcpConv.hpp"
#include "nocopyable.h"
#include "objectpool/Buffer.hpp"

// 前向声明KCP控制块结构体。
// ikcp.h中通常定义的是struct IKCPCB，这里不直接包含ikcp.h，
// 可以降低头文件依赖，减少编译耦合。
struct IKCPCB;
typedef struct IKCPCB ikcpcb;

namespace myring {
namespace kcp {

/**
 * @brief KCP会话配置参数。
 *
 * 该结构体用于描述一个KcpSession内部KCP实例的运行参数。
 * 每个KcpSession对应一个ikcpcb对象，因此不同会话可以拥有不同的KCP参数。
 */
struct KcpSessionConfig {
    /**
     * @brief KCP层MTU大小。
     *
     * KCP会根据mtu计算mss，即单个KCP分片可以承载的最大应用层数据长度。
     * 一般UDP场景下不建议设置得过大，否则底层IP可能发生分片。
     *
     * 常见取值：1200、1350、1400。
     */
    int mtu = 1400;

    /**
     * @brief KCP发送窗口大小。
     *
     * 表示最多允许多少个尚未被ACK确认的KCPsegment处于发送状态。
     * 该值越大，潜在吞吐越高，但丢包时缓存和重传压力也会更大。
     */
    int sndwnd = 128;

    /**
     * @brief KCP接收窗口大小。
     *
     * 表示本端接收侧最多缓存多少个KCPsegment。
     * 如果应用层迟迟不调用ikcp_recv取走数据，接收窗口会逐渐变小。
     */
    int rcvwnd = 128;

    /**
     * @brief 是否启用KCP低延迟模式。
     *
     * 传给ikcp_nodelay的第一个参数。
     *
     * nodelay=0表示普通模式；
     * nodelay=1表示低延迟模式，KCP会使用更激进的RTO计算和重传策略。
     */
    int nodelay = 1;

    /**
     * @brief KCP内部flush间隔，单位为毫秒。
     *
     * 传给ikcp_nodelay的第二个参数。
     * interval越小，KCP响应越快，但update调用和协议处理频率也更高。
     *
     * 常见低延迟配置为10ms或20ms。
     */
    int interval = 20;

    /**
     * @brief 快速重传触发阈值。
     *
     * 传给ikcp_nodelay的第三个参数。
     * resend=2表示某个包被后续ACK跳过2次后，就可以触发快速重传。
     *
     * 设置为0表示关闭快速重传。
     */
    int resend = 2;

    /**
     * @brief 是否关闭拥塞控制。
     *
     * 传给ikcp_nodelay的第四个参数。
     *
     * nc=0表示启用KCP拥塞控制；
     * nc=1表示关闭拥塞控制，发送策略会更激进。
     *
     * 内网、游戏、实时通信场景可以考虑关闭；
     * 公网复杂网络环境下需要谨慎，否则可能加剧丢包。
     */
    int nc = 1;

    /**
     * @brief 应用层接收缓冲区大小。
     *
     * drain_messages中会使用该缓冲区不断调用ikcp_recv，
     * 把KCP已经重组好的应用层消息读取出来。
     */
    std::size_t recv_buffer_size = 64 * 1024;
};

/**
 * @brief KCP底层数据报发送回调。
 *
 * KCP本身不负责UDP发送，它只会在内部flush时生成KCP数据包。
 * 当KCP需要发送数据时，会通过output_bridge转发到handle_output，
 * 最终调用该回调把KCP包交给外部网络层发送。
 *
 * @param peer 对端地址。
 * @param peer_len 对端地址长度。
 * @param data KCP已经编码完成的数据包。
 * @param len 数据包长度。
 * @return 发送结果。通常返回0表示成功，负数表示失败。
 *
 * 注意：
 * data指向的是KCP内部临时输出缓冲区。
 * 如果外部发送是同步sendto，通常可以直接使用；
 * 如果外部发送是io_uring异步发送，必须拷贝data内容，
 * 不能直接保存这个指针。
 */
using KcpDatagramOutput =
    std::function<int(const sockaddr* peer,
                      socklen_t peer_len,
                      const char* data,
                      std::size_t len)>;

/**
 * @brief KCP应用层消息回调。
 *
 * 当KcpSession把完整、有序的应用层数据drain出来时，
 * 会通过该回调把数据交给当前调用方。
 *
 * @param data 应用层消息数据。
 * @param len 应用层消息长度。
 *
 * 注意：
 * data通常指向KcpSession内部recv_buffer_的内存区域，
 * 回调函数如果需要长期保存数据，应当自行拷贝。
 */
using KcpMessageCallback =
    std::function<void(const char* data, std::size_t len)>;

/**
 * @brief 单个KCP连接/会话对象。
 *
 * 一个KcpSession封装一个ikcpcb实例，并维护与该KCP连接相关的：
 *
 * 1.会话IDconv；
 * 2.对端UDP地址；
 * 3.KCP协议配置；
 * 4.底层UDP发送回调；
 * 5.KCP接收缓冲区。
 *
 * 它本身不负责socket创建、事件循环、定时器和线程调度。
 * 外部网络层需要在UDP收到数据时调用input_packet，
 * 在定时器触发时调用update，
 * 在业务要发送数据时调用send_app_data。
 */
class KcpSession : private nocopyable {
public:
    /**
     * @brief 创建KCP会话。
     *
     * 构造函数会保存conv和peer地址，创建ikcpcb对象，
     * 设置KCP输出回调，并根据config配置KCP参数。
     *
     * @param conv KCP会话ID，用于区分不同KCP连接。
     * @param peer 对端socket地址。
     * @param peer_len 对端地址长度。
     * @param output 底层UDP发送回调。
     * @param config KCP配置参数，默认使用KcpSessionConfig默认值。
     */
    KcpSession(const KcpConv& conv,
               const sockaddr* peer,
               socklen_t peer_len,
               KcpDatagramOutput output,
               KcpSessionConfig config = {});

    /**
     * @brief 析构KCP会话。
     *
     * 析构函数中应当释放ikcpcb对象，
     * 同时释放或归还内部缓冲区资源。
     */
    ~KcpSession();

    /**
     * @brief 获取当前KCP会话ID。
     *
     * @return 当前会话的conv值。
     */
    const KcpConv& conv() const noexcept;

    /**
     * @brief 获取当前会话绑定的对端地址。
     *
     * @return 指向peer_addr_的sockaddr指针。
     *
     * 注意：
     * 返回的是内部地址存储，不应由调用方释放。
     */
    const sockaddr* peer() const noexcept;

    /**
     * @brief 获取当前对端地址长度。
     *
     * @return peer_addr_对应的地址长度。
     */
    socklen_t peer_len() const noexcept;

    bool valid() const noexcept;

    /**
     * @brief 输入一个底层UDP数据包。
     *
     * 外部UDP网络层收到数据后，应当根据conv找到对应KcpSession，
     * 然后调用该函数把数据交给KCP协议栈。
     *
     * 函数内部通常会调用：
     *
     * 1.ikcp_input，把UDP数据喂给KCP；
     * 2.必要时调用update，让ACK或重传逻辑及时刷新。
     *
     * @param data UDP收到的KCP数据包。
     * @param len 数据包长度。
     * @param now_ms 当前时间戳，单位毫秒。
     * @return KCP处理结果，通常0表示成功，负数表示失败。
     */
    int input_packet(const char* data, std::size_t len, std::uint32_t now_ms);

    /**
     * @brief 主动提取当前已经重组完成的应用层消息。
     *
     * KcpSession不再持有上层消息回调。
     * 上层编排者在合适的时机显式调用该函数，把已经就绪的应用层消息拉出来。
     *
     * @param cb 每提取到一条完整消息就调用一次。
     */
    void drain_messages(const KcpMessageCallback& cb);

    /**
     * @brief 发送一条应用层数据。
     *
     * 上层业务调用该函数发送可靠有序的数据。
     * 函数内部通常会调用ikcp_send把数据放入KCP发送队列，
     * 然后通过update驱动KCP尽快flush。
     *
     * 注意：
     * ikcp_send不等于立即调用UDP发送。
     * 真正的底层发送通常发生在ikcp_update触发flush之后。
     *
     * @param data 应用层数据。
     * @param len 应用层数据长度。
     * @param now_ms 当前时间戳，单位毫秒。
     * @return KCP发送接口返回值，通常0表示成功，负数表示失败。
     */
    int send_app_data(const char* data, std::size_t len, std::uint32_t now_ms);

    /**
     * @brief 驱动KCP内部定时逻辑。
     *
     * KCP是纯算法库，不会自己创建线程，也不会自己读取系统时间。
     * 外部必须周期性调用update，KCP才能处理：
     *
     * 1.ACK发送；
     * 2.新数据flush；
     * 3.超时重传；
     * 4.快速重传；
     * 5.窗口探测；
     * 6.拥塞窗口更新。
     *
     * @param now_ms 当前时间戳，单位毫秒。
     */
    void update(std::uint32_t now_ms);

    /**
     * @brief 查询下一次适合调用update的时间。
     *
     * 该函数通常封装ikcp_check。
     * 外部定时器可以利用该返回值减少无意义的update调用。
     *
     * @param now_ms 当前时间戳，单位毫秒。
     * @return 下一次建议调用update的时间戳，单位毫秒。
     */
    std::uint32_t check(std::uint32_t now_ms) const noexcept;

private:
    /**
     * @brief KCP输出桥接函数。
     *
     * ikcp_setoutput要求的回调是普通C函数指针，
     * 不能直接绑定C++成员函数。
     *
     * 因此这里使用一个static函数作为桥接层：
     *
     * 1.KCP内部调用output_bridge；
     * 2.output_bridge通过user指针还原KcpSession对象；
     * 3.再调用该对象的handle_output成员函数。
     *
     * @param buf KCP编码后的数据包。
     * @param len 数据包长度。
     * @param kcp 当前ikcpcb对象。
     * @param user 用户指针，通常指向KcpSession对象。
     * @return 输出结果。
     */
    static int output_bridge(const char* buf, int len, ikcpcb* kcp, void* user);

    /**
     * @brief 处理KCP底层输出。
     *
     * 该函数由output_bridge调用，最终会触发output_回调，
     * 把KCP生成的数据包交给外部UDP发送层。
     *
     * @param buf KCP编码后的数据包。
     * @param len 数据包长度。
     * @return 输出结果。
     */
    int handle_output(const char* buf, int len);

    /**
     * @brief 保存对端地址。
     *
     * 由于sockaddr只是通用地址指针，真实类型可能是sockaddr_in、
     * sockaddr_in6等，所以这里使用sockaddr_storage存储地址。
     *
     * @param peer 对端地址。
     * @param peer_len 对端地址长度。
     * @return 保存成功返回true，地址非法或长度过大返回false。
     */
    bool assign_peer(const sockaddr* peer, socklen_t peer_len) noexcept;

    /**
     * @brief 配置KCP实例。
     *
     * 该函数通常在构造函数中调用，用于设置：
     *
     * 1.ikcp_setoutput；
     * 2.ikcp_setmtu；
     * 3.ikcp_wndsize；
     * 4.ikcp_nodelay；
     * 5.其他KCP运行参数。
     */
    void configure();

private:
    /**
     * @brief 当前KCP会话ID。
     *
     * conv会写入每个KCP包头，用于区分不同KCP连接。
     */
    KcpConv conv_;

    /**
     * @brief 对端地址存储。
     *
     * 使用sockaddr_storage可以同时兼容IPv4和IPv6地址。
     */
    sockaddr_storage peer_addr_;

    /**
     * @brief 对端地址长度。
     */
    socklen_t peer_len_;

    /**
     * @brief 底层数据报发送回调。
     *
     * KCP生成的数据包最终会通过该回调发送出去。
     * 外部可以在这里使用sendto、sendmsg或者io_uring提交UDP发送。
     */
    KcpDatagramOutput output_;

    /**
     * @brief 应用层接收缓冲区。
     *
     * drain_messages调用ikcp_recv时，会把数据读取到该缓冲区。
     * 读取成功后，再通过本次传入的回调把消息交给上层。
     */
    Buffer recv_buffer_;

    /**
     * @brief KCP控制块指针。
     *
     * 每个KcpSession内部持有一个ikcpcb对象。
     * 该对象负责维护KCP协议状态，包括发送队列、接收队列、
     * ACK列表、窗口、RTT、RTO、重传状态等。
     */
    ikcpcb* kcp_;

    /**
     * @brief 当前会话的KCP配置。
     *
     * 构造时传入，configure函数会根据该配置初始化ikcpcb。
     */
    KcpSessionConfig config_;
};

}  // namespace kcp
}  // namespace myring

#endif
