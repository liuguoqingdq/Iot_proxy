#include "Kcp/KcpSession.hpp"

#include <cstring>   // std::memset、std::memcpy
#include <limits>    // std::numeric_limits，用于做长度范围检查
#include <string>    // std::string，接收超大KCP消息时临时动态缓冲
#include <utility>   // std::move，用于移动回调对象

// ikcp是C库，所以这里使用extern "C"避免C++名字改编，保证能正确链接ikcp中的函数
extern "C" {
#include "ikcp.h"
}

static_assert(myring::kcp::KcpConv::kSize == IKCP_CONV_BYTES,
              "KcpConv size must match ikcp wire conv size");

namespace myring {
namespace kcp {

/**
 * KcpSession构造函数
 *
 * 一个KcpSession代表一个远端peer对应的一条KCP逻辑连接。
 *
 * 参数说明：
 * conv：KCP会话编号，通信双方必须使用相同conv才能互相识别数据包
 * peer：远端地址，通常来自UDP recvfrom/recvmsg中的对端sockaddr
 * peer_len：远端地址长度
 * output：KCP底层数据输出回调，KCP需要发UDP包时会调用它
 * config：KCP配置，例如MTU、窗口大小、nodelay参数、接收缓冲区大小等
 */
KcpSession::KcpSession(const KcpConv& conv,
                       const sockaddr* peer,
                       socklen_t peer_len,
                       KcpDatagramOutput output,
                       KcpSessionConfig config)
    : conv_(conv),                  // 保存KCP会话编号
      peer_addr_(),                 // 初始化远端地址存储结构，实际内容后面由assign_peer复制
      peer_len_(0),                 // 初始远端地址长度为0，assign_peer成功后才会赋值
      output_(std::move(output)),   // 保存底层输出回调，使用move避免不必要拷贝
      recv_buffer_(config.recv_buffer_size == 0 ? 64 * 1024
                                                : config.recv_buffer_size),
                                      // 接收缓冲区，如果配置为0，则默认使用64KB
      kcp_(nullptr),                // ikcp控制块指针，创建成功后指向ikcpcb
      config_(config) {             // 保存KCP配置
    // 保存远端地址，后续KCP输出数据时，需要知道UDP包要发给谁
    assign_peer(peer, peer_len);

    // 创建KCP控制块。
    // conv_用于标识一条KCP连接；
    // this作为user参数传给ikcp，后续output_bridge里可以通过user拿回当前KcpSession对象。
    IKCPCONV ikcp_conv;
    std::memcpy(ikcp_conv.bytes, conv_.data(), IKCP_CONV_BYTES);
    kcp_ = ikcp_create(&ikcp_conv, this);

    // 创建成功后，设置KCP底层输出函数，并应用配置参数
    if (kcp_ != nullptr) {
        // 设置KCP的底层输出回调。
        // 当ikcp_update/ikcp_send触发底层包发送时，ikcp内部会调用output_bridge。
        ikcp_setoutput(kcp_, &KcpSession::output_bridge);

        // 设置MTU、窗口、nodelay等参数
        configure();
    }
}

/**
 * 析构函数
 *
 * 释放ikcp_create创建出来的KCP控制块。
 * ikcp_release会释放KCP内部缓存、发送队列、接收队列等资源。
 */
KcpSession::~KcpSession() {
    if (kcp_ != nullptr) {
        ikcp_release(kcp_);
        kcp_ = nullptr;
    }
}

/**
 * 返回当前KCP会话编号
 */
const KcpConv& KcpSession::conv() const noexcept {
    return conv_;
}

/**
 * 返回远端地址指针
 *
 * peer_addr_内部使用sockaddr_storage保存，可以兼容IPv4和IPv6。
 * 这里转成const sockaddr*是为了方便sendto/sendmsg等socket接口使用。
 */
const sockaddr* KcpSession::peer() const noexcept {
    return reinterpret_cast<const sockaddr*>(&peer_addr_);
}

/**
 * 返回远端地址长度
 *
 * IPv4一般是sizeof(sockaddr_in)，IPv6一般是sizeof(sockaddr_in6)。
 */
socklen_t KcpSession::peer_len() const noexcept {
    return peer_len_;
}

/**
 * 判断当前KCP会话是否有效
 *
 * 有效条件：
 * 1.kcp_创建成功；
 * 2.远端地址有效；
 * 3.底层输出回调存在。
 */
bool KcpSession::valid() const noexcept {
    return kcp_ != nullptr && peer_len_ != 0 && static_cast<bool>(output_);
}

/**
 * 输入一个底层UDP收到的KCP包
 *
 * 参数说明：
 * data：UDP负载数据，也就是KCP报文
 * len：数据长度
 * now_ms：当前时间，单位毫秒，通常由外层统一传入
 *
 * 流程：
 * 1.参数检查；
 * 2.调用ikcp_input把底层KCP报文喂给KCP协议栈；
 * 3.调用ikcp_update推动KCP状态机。
 */
int KcpSession::input_packet(const char* data,
                             std::size_t len,
                             std::uint32_t now_ms) {
    // kcp_为空说明KCP控制块创建失败；
    // data为空或len为0说明输入非法；
    // ikcp_input的长度参数是long，所以这里要防止size_t超过long范围。
    if (kcp_ == nullptr || data == nullptr || len == 0 ||
        len > static_cast<std::size_t>(std::numeric_limits<long>::max())) {
        return -1;
    }

    // 把UDP收到的KCP数据包交给ikcp处理。
    // ikcp_input内部会解析KCP头部、ACK、数据分片、窗口等信息。
    const int ret = ikcp_input(kcp_, data, static_cast<long>(len));
    if (ret < 0) {
        return ret;
    }

    // 推动KCP状态机。
    // 这一步可能触发ACK发送、重传检测、窗口更新等逻辑。
    ikcp_update(kcp_, now_ms);

    return ret;
}

/**
 * 主动提取KCP已经重组好的完整应用层消息
 *
 * 谁负责驱动KcpSession，谁就显式调用这个函数来拉取消息。
 * KcpSession本身不再保存上层业务回调。
 */
void KcpSession::drain_messages(const KcpMessageCallback& cb) {
    if (kcp_ == nullptr || cb == nullptr) {
        return;
    }

    while (true) {
        const int peek = ikcp_peeksize(kcp_);
        if (peek <= 0) {
            return;
        }

        if (static_cast<std::size_t>(peek) <= recv_buffer_.capacity()) {
            const int n = ikcp_recv(kcp_, recv_buffer_.data(), peek);
            if (n <= 0) {
                return;
            }

            recv_buffer_.set_length(static_cast<std::size_t>(n));
            cb(recv_buffer_.data(), recv_buffer_.length());
            continue;
        }

        std::string dynamic_buffer(static_cast<std::size_t>(peek), '\0');
        const int n = ikcp_recv(kcp_, &dynamic_buffer[0], peek);
        if (n <= 0) {
            return;
        }

        cb(dynamic_buffer.data(), static_cast<std::size_t>(n));
    }
}

/**
 * 发送应用层数据
 *
 * 参数说明：
 * data：应用层要发送的数据
 * len：数据长度
 * now_ms：当前时间，单位毫秒
 *
 * 注意：
 * ikcp_send不是直接发送UDP包，而是把应用层数据交给KCP协议栈。
 * 真正的底层发送通常会在ikcp_update时触发，然后通过output_bridge回调出去。
 */
int KcpSession::send_app_data(const char* data,
                              std::size_t len,
                              std::uint32_t now_ms) {
    // ikcp_send的长度参数是int，所以这里要防止size_t超过int范围。
    if (kcp_ == nullptr || data == nullptr || len == 0 ||
        len > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return -1;
    }

    // 把应用层数据写入KCP发送队列。
    // 如果数据超过MSS，KCP内部会负责分片。
    const int ret = ikcp_send(kcp_, data, static_cast<int>(len));
    if (ret < 0) {
        return ret;
    }

    // 推动KCP发送流程。
    // 这一步可能会立即把待发送数据封装成KCP包，然后调用output回调。
    ikcp_update(kcp_, now_ms);

    return ret;
}

/**
 * 定时更新KCP状态机
 *
 * KCP不是完全事件驱动的协议，它需要外部周期性调用ikcp_update。
 * 该函数负责推动重传、ACK刷新、窗口探测等定时逻辑。
 */
void KcpSession::update(std::uint32_t now_ms) {
    if (kcp_ == nullptr) {
        return;
    }

    ikcp_update(kcp_, now_ms);
}

/**
 * 查询下一次应该调用ikcp_update的时间
 *
 * ikcp_check会根据KCP内部状态计算下一次update的推荐时间点。
 * 外层事件循环可以用它来减少无意义的频繁update。
 */
std::uint32_t KcpSession::check(std::uint32_t now_ms) const noexcept {
    if (kcp_ == nullptr) {
        return now_ms;
    }

    return ikcp_check(kcp_, now_ms);
}

/**
 * KCP底层输出桥接函数
 *
 * 这是一个静态函数，签名必须符合ikcp_setoutput要求：
 *
 * int output(const char* buf, int len, ikcpcb* kcp, void* user)
 *
 * ikcp内部不能直接调用C++成员函数，所以这里用静态函数做桥接。
 * user就是ikcp_create(conv_, this)时传入的this指针。
 */
int KcpSession::output_bridge(const char* buf, int len, ikcpcb* kcp, void* user) {
    // 当前实现不需要直接使用ikcpcb指针，所以显式忽略，避免编译器警告。
    (void)kcp;

    // user为空说明无法找到对应的KcpSession对象。
    if (user == nullptr) {
        return -1;
    }

    // 把user恢复成KcpSession对象，然后转发给成员函数处理。
    return static_cast<KcpSession*>(user)->handle_output(buf, len);
}

/**
 * 处理KCP底层输出数据
 *
 * 当KCP需要发送底层报文时，会调用到这里。
 * 这里不会直接操作socket，而是调用外部注入的output_回调。
 *
 * output_通常会封装UDP sendto/sendmsg/io_uring sendmsg等逻辑。
 */
int KcpSession::handle_output(const char* buf, int len) {
    if (output_ == nullptr || buf == nullptr || len <= 0) {
        return -1;
    }

    // 调用底层输出回调，把KCP报文发给当前peer。
    return output_(
        peer(),                         // 远端地址
        peer_len(),                     // 远端地址长度
        buf,                            // KCP底层报文数据
        static_cast<std::size_t>(len)   // 报文长度
    );
}

/**
 * 保存远端地址
 *
 * peer_addr_是sockaddr_storage类型，能够容纳sockaddr_in和sockaddr_in6。
 * 这样可以同时兼容IPv4和IPv6。
 */
bool KcpSession::assign_peer(const sockaddr* peer, socklen_t peer_len) noexcept {
    // 先清空旧地址，避免残留数据。
    std::memset(&peer_addr_, 0, sizeof(peer_addr_));

    // 地址长度先置0，只有复制成功后才认为peer有效。
    peer_len_ = 0;

    // 参数检查：
    // peer不能为空；
    // peer_len不能为0；
    // peer_len不能超过sockaddr_storage可容纳的大小。
    if (peer == nullptr || peer_len == 0 ||
        peer_len > static_cast<socklen_t>(sizeof(peer_addr_))) {
        return false;
    }

    // 拷贝远端地址内容。
    std::memcpy(&peer_addr_, peer, peer_len);

    // 保存远端地址长度。
    peer_len_ = peer_len;

    return true;
}

/**
 * 配置KCP参数
 *
 * 主要包括：
 * 1.MTU；
 * 2.发送窗口和接收窗口；
 * 3.nodelay相关参数。
 */
void KcpSession::configure() {
    if (kcp_ == nullptr) {
        return;
    }

    // 设置MTU。
    // MTU会影响KCP每个底层包的最大大小。
    // ikcp内部会根据MTU计算MSS，MSS大约等于MTU减去KCP头部长度。
    if (config_.mtu > 0) {
        ikcp_setmtu(kcp_, config_.mtu);
    }

    // 设置发送窗口和接收窗口。
    // sndwnd越大，理论上可同时在途的数据越多；
    // rcvwnd越大，接收端可缓存的未交付数据越多。
    ikcp_wndsize(kcp_, config_.sndwnd, config_.rcvwnd);

    // 设置KCP快速模式参数。
    //
    // nodelay：
    // 0表示普通模式；
    // 1表示启用快速模式，通常能降低延迟。
    //
    // interval：
    // 内部flush间隔，单位毫秒。
    // 值越小响应越快，但CPU和网络开销可能更高。
    //
    // resend：
    // 快速重传参数。
    // 例如设置为2时，收到2次重复ACK可能触发快速重传。
    //
    // nc：
    // 是否关闭拥塞控制。
    // 0表示不关闭；
    // 1表示关闭拥塞控制，可能提高吞吐，但更容易造成网络拥塞。
    ikcp_nodelay(
        kcp_,
        config_.nodelay,
        config_.interval,
        config_.resend,
        config_.nc
    );
}

}  // namespace kcp
}  // namespace myring
