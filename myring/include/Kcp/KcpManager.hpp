#ifndef MYRING_KCP_KCP_MANAGER_HPP
#define MYRING_KCP_KCP_MANAGER_HPP

#include <sys/socket.h>   // sockaddr、socklen_t等socket地址结构

#include <atomic>         // std::atomic，用于running_线程安全状态控制
#include <cstddef>        // std::size_t
#include <cstdint>        // std::uint32_t、std::uint64_t等固定宽度整数
#include <functional>     // std::function，用于回调函数
#include <memory>         // std::unique_ptr
#include <string>         // std::string
#include <unordered_map>  // std::unordered_map，用于管理KCP会话表
#include <vector>         // std::vector，用作定时器小根堆底层容器

#include "Kcp/KcpConv.hpp"      // 256-bit KCP会话标识
#include "Kcp/KcpSession.hpp"   // 单个KCP会话封装
#include "Kcp/UdpDispatcher.h"  // UDP异步收发封装，底层基于Proactor/io_uring
#include "nocopyable.h"         // 禁止拷贝基类

namespace myring {
namespace kcp {

/**
 * KcpManager配置项
 *
 * KcpManager负责：
 * 1.绑定UDP端口；
 * 2.接收底层UDP数据；
 * 3.根据peer地址和conv找到对应KcpSession；
 * 4.把UDP数据交给对应KcpSession处理；
 * 5.管理KCP定时update；
 * 6.清理空闲session。
 */
struct KcpManagerConfig {
    // io_uring队列深度，传给UdpDispatcher/ProactorThread使用
    unsigned entries = 256;

    // 每轮最多处理的完成事件数量
    std::size_t max_events = 64;

    // 每个UDP接收请求的缓冲区大小
    // 对KCP底层UDP包来说，2048通常能覆盖常见MTU场景
    std::size_t recv_buffer_size = 2048;

    // KCP会话空闲超时时间，单位毫秒
    // 如果为0，通常可以理解为不启用空闲回收
    std::uint64_t idle_timeout_ms = 0;

    // 兜底tick间隔，单位毫秒
    // 当无法精确计算下一次KCP更新时间时，可以用这个值做保底调度
    std::uint64_t fallback_tick_ms = 10;

    // 单个KcpSession的配置，例如MTU、窗口大小、nodelay参数等
    KcpSessionConfig session_config;
};

/**
 * KcpSessionKey
 *
 * 用于唯一标识一个KCP会话。
 *
 * 一个KCP连接不能只用conv区分，因为不同peer可能使用相同conv。
 * 所以这里使用：
 *
 * peer IPv4地址 + peer端口 + conv
 *
 * 三者共同作为session key。
 *
 * 注意：
 * 当前这个key结构只保存IPv4地址和端口，所以它主要适用于IPv4 UDP。
 * 如果以后支持IPv6，需要扩展这个结构，例如使用sockaddr_storage作为key内容。
 */
struct KcpSessionKey {
    // 对端IPv4地址，网络字节序
    // 一般来自sockaddr_in.sin_addr.s_addr
    std::uint32_t addr_be;

    // 对端UDP端口，网络字节序
    // 一般来自sockaddr_in.sin_port
    std::uint16_t port_be;

    // 预留字段
    // 主要用于对齐或后续扩展，目前不参与核心逻辑也可以
    std::uint16_t reserved;

    // KCP会话编号
    // 通信双方同一条KCP通道必须使用相同conv
    KcpConv conv;

    /**
     * 默认构造函数
     *
     * 初始化为空key。
     */
    KcpSessionKey() noexcept
        : addr_be(0),
          port_be(0),
          reserved(0),
          conv() {}

    /**
     * 根据conv和peer地址构造session key
     *
     * 参数说明：
     * conv_in：KCP会话编号
     * peer：对端socket地址
     * peer_len_in：对端socket地址长度
     *
     * 构造函数内部通常会判断peer是不是IPv4地址，
     * 然后提取IPv4地址和端口。
     */
    KcpSessionKey(const KcpConv& conv_in,
                  const sockaddr* peer,
                  socklen_t peer_len_in) noexcept;
    KcpSessionKey(std::uint32_t conv_in,
                  const sockaddr* peer,
                  socklen_t peer_len_in) noexcept;

    /**
     * 判断key是否合法
     *
     * 一般需要满足：
     * 1.IP地址有效；
     * 2.端口有效；
     * 3.conv有效。
     *
     * 具体规则取决于.cpp中的实现。
     */
    bool valid() const noexcept;

    /**
     * 判断两个session key是否相同
     *
     * unordered_map查找时会用到该比较函数。
     */
    bool operator==(const KcpSessionKey& other) const noexcept;
};

/**
 * KcpSessionKey的哈希函数
 *
 * 用于让KcpSessionKey可以作为std::unordered_map的key。
 */
struct KcpSessionKeyHash {
    std::size_t operator()(const KcpSessionKey& key) const noexcept;
};

/**
 * KcpManager上层消息回调
 *
 * 当某个KcpSession从KCP流中解析出完整应用层消息后，
 * KcpManager会通过这个回调把数据交给更上层。
 *
 * 参数说明：
 * key：消息来自哪个KCP会话
 * data：应用层消息数据
 * len：应用层消息长度
 */
using KcpManagerMessageCallback =
    std::function<void(const KcpSessionKey& key,
                       const char* data,
                       std::size_t len)>;

/**
 * KcpManager
 *
 * KcpManager是KCP通道管理器。
 *
 * 它不是单个KCP连接，而是管理多个KcpSession。
 *
 * 核心职责：
 * 1.持有UdpDispatcher，负责底层UDP收发；
 * 2.收到UDP包后，根据peer地址和conv找到或创建KcpSession；
 * 3.把UDP包喂给对应KcpSession；
 * 4.应用层发送数据时，找到对应KcpSession并调用send_app_data；
 * 5.根据ikcp_check结果调度下一次ikcp_update；
 * 6.根据idle_timeout_ms清理长期无活动会话。
 *
 * 这个类继承nocopyable，说明它不允许被拷贝。
 * 原因是它内部持有socket、线程、session对象和回调状态，
 * 这些资源不适合做默认拷贝。
 */
class KcpManager : private nocopyable {
public:
    /**
     * 构造函数
     *
     * 根据配置初始化UdpDispatcher、KCP会话配置和定时参数。
     */
    explicit KcpManager(const KcpManagerConfig& config = {});

    /**
     * 析构函数
     *
     * 通常会调用stop并释放内部资源。
     */
    ~KcpManager();

    /**
     * 绑定本地UDP监听地址
     *
     * 参数说明：
     * host：本地IP，例如"0.0.0.0"、"127.0.0.1"
     * port：本地UDP端口
     *
     * 返回值：
     * true：绑定成功
     * false：绑定失败
     */
    bool bind(const std::string& host, std::uint16_t port);

    /**
     * 设置上层消息回调
     *
     * 当KCP收到完整应用层数据后，会最终回调到这里设置的函数。
     */
    void set_message_callback(KcpManagerMessageCallback cb);

    /**
     * 启动KcpManager
     *
     * 一般流程：
     * 1.启动UdpDispatcher；
     * 2.设置UDP收包回调；
     * 3.启动KCP定时tick。
     */
    void start();

    /**
     * 停止KcpManager
     *
     * 一般流程：
     * 1.running_置为false；
     * 2.停止UdpDispatcher；
     * 3.停止后续tick调度。
     */
    void stop();

    /**
     * 向指定KCP会话发送数据
     *
     * 参数说明：
     * key：目标session key
     * data：应用层数据
     * len：数据长度
     *
     * 如果session不存在，内部可能会尝试创建，
     * 具体行为取决于send_in_loop/find_or_create_session_entry_in_loop实现。
     */
    bool send(const KcpSessionKey& key,
              const char* data,
              std::size_t len);

    /**
     * std::string版本发送接口
     *
     * 只是对上面char*版本的便利封装。
     */
    bool send(const KcpSessionKey& key,
              const std::string& data);
    bool send(const KcpSessionKey& key,
              std::string&& data);

    /**
     * 根据peer地址和conv发送数据
     *
     * 这个接口会先根据peer和conv构造KcpSessionKey，
     * 再发送应用层数据。
     */
    bool send(const sockaddr* peer,
              socklen_t peer_len,
              const KcpConv& conv,
              const char* data,
              std::size_t len);
    bool send(const sockaddr* peer,
              socklen_t peer_len,
              std::uint32_t conv,
              const char* data,
              std::size_t len);

    /**
     * 根据peer地址和conv发送std::string数据
     */
    bool send(const sockaddr* peer,
              socklen_t peer_len,
              const KcpConv& conv,
              const std::string& data);
    bool send(const sockaddr* peer,
              socklen_t peer_len,
              std::uint32_t conv,
              const std::string& data);
    bool send(const sockaddr* peer,
              socklen_t peer_len,
              const KcpConv& conv,
              std::string&& data);
    bool send(const sockaddr* peer,
              socklen_t peer_len,
              std::uint32_t conv,
              std::string&& data);

    /**
     * 返回底层UDP socket fd
     *
     * 主要用于调试或和外部事件系统集成。
     */
    int fd() const noexcept;

private:
    /**
     * SessionEntry
     *
     * sessions_里保存的value类型。
     *
     * 一个SessionEntry对应一个KcpSession及其调度信息。
     */
    struct SessionEntry {
        // 真正的KCP会话对象
        std::unique_ptr<KcpSession> session;

        // 最近一次活跃时间，使用单调毫秒时间
        // 收到对端UDP包、收到KCP消息或发送数据时可以更新它
        // 用于判断session是否空闲超时
        std::uint64_t last_active_mono_ms;

        // 当前session下一次应该update的时间点，使用单调毫秒时间
        // 通常由KcpSession::check，也就是ikcp_check计算出来
        std::uint64_t next_due_mono_ms;

        // 调度序号
        // 每次重新调度session时递增，用于识别timer_heap_里的旧定时节点
        std::uint64_t schedule_seq;
    };

    /**
     * TimerNode
     *
     * 定时堆中的一个节点。
     *
     * 由于一个session可能被多次重新调度，
     * 所以堆里可能存在旧节点。
     * schedule_seq用于判断这个TimerNode是不是当前有效的调度节点。
     */
    struct TimerNode {
        // 到期时间，单调毫秒时间
        std::uint64_t due_mono_ms;

        // 调度序号，用于过滤过期timer节点
        std::uint64_t schedule_seq;

        // 该timer对应哪个session
        KcpSessionKey key;
    };

    /**
     * TimerNode比较器
     *
     * 用于std::make_heap/std::push_heap/std::pop_heap一类堆操作。
     *
     * 这里lhs.due_mono_ms > rhs.due_mono_ms表示构造小根堆：
     * due_mono_ms越小，越靠近堆顶。
     */
    struct TimerNodeGreater {
        bool operator()(const TimerNode& lhs,
                        const TimerNode& rhs) const noexcept {
            return lhs.due_mono_ms > rhs.due_mono_ms;
        }
    };

    /**
     * SessionMap
     *
     * KCP会话表。
     *
     * key：KcpSessionKey，也就是peer地址+端口+conv
     * value：SessionEntry，也就是KcpSession及其调度状态
     */
    using SessionMap =
        std::unordered_map<KcpSessionKey,
                           SessionEntry,
                           KcpSessionKeyHash>;

private:
    /**
     * 处理底层UDP数据包
     *
     * UdpDispatcher收到UDP包后，会调用这个函数。
     *
     * 处理流程通常是：
     * 1.从KCP包中解析conv；
     * 2.根据peer地址和conv构造KcpSessionKey；
     * 3.查找或创建对应KcpSession；
     * 4.调用KcpSession::input_packet；
     * 5.主动drain该session已经完成重组的应用层消息；
     * 6.更新session活跃时间；
     * 7.重新调度该session下一次update时间。
     */
    void handle_udp_packet(const sockaddr* peer,
                           socklen_t peer_len,
                           const char* data,
                           std::size_t len);

    /**
     * 处理单个KcpSession吐出的完整应用层消息
     *
     * KcpManager在驱动KcpSession之后，会主动drain消息并调用这里，
     * 再把消息通过message_callback_交给业务层。
     */
    void handle_session_message(const KcpSessionKey& key,
                                const char* data,
                                std::size_t len);

    /**
     * 在事件循环线程中主动提取某个session已经完成重组的应用层消息。
     */
    void drain_session_messages_in_loop(const KcpSessionKey& key,
                                        SessionEntry& entry);

    /**
     * KCP定时tick处理函数
     *
     * 这是KcpManager定时调度的核心函数。
     *
     * 典型逻辑：
     * 1.获取当前单调时间；
     * 2.从timer_heap_中取出已经到期的session；
     * 3.对到期session调用KcpSession::update；
     * 4.update后重新调用KcpSession::check；
     * 5.把新的next_due重新放入timer_heap_；
     * 6.根据堆顶节点安排下一轮timer；
     * 7.必要时执行空闲session回收。
     */
    void handle_tick();

    /**
     * 在事件循环线程中安排下一次tick
     *
     * 参数说明：
     * due_mono_ms：下一次tick应该触发的单调毫秒时间点
     *
     * 这个函数一般会把绝对时间点转换成delay：
     *
     * delay = due_mono_ms - now
     *
     * 然后调用UdpDispatcher::schedule_timeout。
     */
    void arm_tick_for_due_in_loop(std::uint64_t due_mono_ms);

    /**
     * 重新调度某个session的下一次update时间
     *
     * 参数说明：
     * key：要调度的session key
     * entry：session表中的entry
     * kcp_now_ms：传给KCP的uint32毫秒时间
     * mono_now_ms：当前单调毫秒时间，uint64版本
     *
     * 通常流程：
     * 1.调用entry.session->check(kcp_now_ms)得到KCP下一次update时间；
     * 2.换算成mono时间体系；
     * 3.更新entry.next_due_mono_ms；
     * 4.递增entry.schedule_seq；
     * 5.向timer_heap_压入新的TimerNode。
     */
    void schedule_session_in_loop(const KcpSessionKey& key,
                                  SessionEntry& entry,
                                  std::uint32_t kcp_now_ms,
                                  std::uint64_t mono_now_ms);

    /**
     * 回收空闲session
     *
     * 参数说明：
     * mono_now_ms：当前单调毫秒时间
     *
     * 如果idle_timeout_ms_为0，可以不做回收。
     * 否则遍历sessions_，删除长期没有活跃的session。
     *
     * 判断方式通常是：
     *
     * mono_now_ms - entry.last_active_mono_ms > idle_timeout_ms_
     */
    void maybe_collect_idle_sessions_in_loop(std::uint64_t mono_now_ms);

    /**
     * 查找或创建SessionEntry
     *
     * 参数说明：
     * key：session key
     *
     * 返回值：
     * 找到或创建成功，返回SessionEntry指针；
     * key非法、创建失败或资源不足时，返回nullptr。
     *
     * 该函数一般只应该在KcpManager自己的事件循环线程中调用，
     * 所以后缀使用_in_loop。
     */
    SessionEntry* find_or_create_session_entry_in_loop(
        const KcpSessionKey& key);

    /**
     * 在事件循环线程中发送数据
     *
     * 参数说明：
     * key：目标session key
     * data：应用层数据
     * len：数据长度
     *
     * 处理流程通常是：
     * 1.查找或创建对应SessionEntry；
     * 2.调用KcpSession::send_app_data；
     * 3.更新last_active_mono_ms；
     * 4.重新调度该session。
     */
    bool send_in_loop(const KcpSessionKey& key,
                      const char* data,
                      std::size_t len);

private:
    /**
     * 运行状态标志
     *
     * true：KcpManager正在运行
     * false：KcpManager已停止或尚未启动
     *
     * 使用atomic是为了让start/stop/send等函数在多线程调用时更安全。
     */
    std::atomic<bool> running_;

    /**
     * UDP异步收发器
     *
     * 底层负责：
     * 1.创建UDP socket；
     * 2.绑定端口；
     * 3.通过io_uring接收UDP包；
     * 4.通过io_uring发送UDP包；
     * 5.提供timeout/post能力。
     */
    UdpDispatcher dispatcher_;

    /**
     * KCP会话表
     *
     * 保存所有当前活跃的KcpSession。
     *
     * key由peer地址、peer端口和conv组成。
     */
    SessionMap sessions_;

    /**
     * KCP定时调度堆
     *
     * 用于保存每个session下一次需要update的时间。
     *
     * 这是一个小根堆，堆顶是最近要触发update的session。
     *
     * 为什么要用堆：
     * 如果每次tick都遍历所有session，会有较多无意义扫描。
     * 堆可以快速找到最近一个到期的session。
     */
    std::vector<TimerNode> timer_heap_;

    /**
     * 上层消息回调
     *
     * 当KCP收到完整应用层消息后，KcpManager通过这个回调通知业务层。
     */
    KcpManagerMessageCallback message_callback_;

    /**
     * 单个KcpSession的配置
     *
     * 创建新session时会使用这份配置。
     */
    KcpSessionConfig session_config_;

    /**
     * session空闲超时时间，单位毫秒
     *
     * 为0时通常表示不启用空闲回收。
     */
    std::uint64_t idle_timeout_ms_;

    /**
     * 兜底tick时间，单位毫秒
     *
     * 当timer_heap_为空、ikcp_check无法得到合适时间，
     * 或者需要周期性检查idle session时，可以用它兜底。
     */
    std::uint64_t fallback_tick_ms_;

    /**
     * 下一次执行空闲session回收的时间点
     *
     * 避免每次tick都全量扫描sessions_做GC。
     */
    std::uint64_t next_idle_gc_mono_ms_;

    /**
     * 当前已经安排的tick到期时间
     *
     * 用于避免重复安排完全相同或更晚的timer。
     */
    std::uint64_t armed_tick_due_mono_ms_;

    /**
     * 当前已安排tick的序号
     *
     * 和session的schedule_seq类似，可以用于识别旧timer。
     * 比如之前安排了一个100ms后的tick，后来又安排了一个更早的tick，
     * 那么旧tick触发时可以通过seq判断是否应该忽略。
     */
    std::uint64_t armed_tick_seq_;
};

}  // namespace kcp
}  // namespace myring

#endif
