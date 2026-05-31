#include "Kcp/KcpManager.hpp"

#include <algorithm>   // std::push_heap、std::pop_heap，用于维护timer_heap_小根堆
#include <arpa/inet.h> // sockaddr_in、AF_INET、网络字节序地址相关定义
#include <chrono>      // std::chrono::steady_clock，用于获取单调时间
#include <cstdint>     // std::uint32_t、std::uint64_t等固定宽度整数
#include <cstring>     // std::memset
#include <string>      // std::string，用于跨线程/post时保存发送数据副本
#include <utility>     // std::move

namespace myring {
namespace kcp {

namespace {

/**
 * 获取单调递增毫秒时间
 *
 * 这里使用steady_clock，而不是system_clock。
 *
 * steady_clock的特点：
 * 1.单调递增；
 * 2.不会因为系统时间被手动修改、NTP校时而倒退；
 * 3.适合KCP这种依赖时间差进行重传、flush、调度的协议。
 *
 * 返回值：
 * 当前单调时间，单位毫秒，uint64_t类型。
 */
std::uint64_t monotonic_ms() noexcept {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(
            steady_clock::now().time_since_epoch()
        ).count()
    );
}

/**
 * 把uint64_t单调时间转换成KCP需要的uint32_t毫秒时间
 *
 * KCP接口中的current时间通常是IUINT32，也就是32位毫秒时间。
 * 这里把64位单调时间截断成低32位。
 *
 * 为什么可以这样做：
 * KCP内部比较时间时会使用有符号差值方式处理回绕，
 * 所以uint32_t时间溢出回绕是KCP设计中允许的。
 */
std::uint32_t kcp_now_ms(std::uint64_t mono_ms) noexcept {
    return static_cast<std::uint32_t>(mono_ms & 0xffffffffu);
}

/**
 * 判断KCP时间是否已经到达
 *
 * 参数说明：
 * now_ms：当前KCP时间，uint32_t毫秒
 * due_ms：目标到期时间，uint32_t毫秒
 *
 * 不能简单写成now_ms >= due_ms，因为uint32_t时间会回绕。
 * 例如时间从0xffffffff回到0时，普通大小比较会出错。
 *
 * 这里使用KCP常见写法：
 *
 * static_cast<int32_t>(now - due) >= 0
 *
 * 如果结果成立，表示now已经到达或超过due。
 */
bool kcp_time_reached(std::uint32_t now_ms,
                      std::uint32_t due_ms) noexcept {
    return static_cast<std::int32_t>(now_ms - due_ms) >= 0;
}

/**
 * 根据KCP时间计算距离到期还有多久
 *
 * 参数说明：
 * now_ms：当前KCP时间
 * due_ms：KCP建议的下一次更新时间
 *
 * 返回值：
 * 距离due_ms还有多少毫秒。
 *
 * 如果已经到期，这里返回1，而不是0。
 * 这样可以避免提交0ms timeout后出现过于频繁的立即触发。
 */
std::uint64_t kcp_delay_ms(std::uint32_t now_ms,
                           std::uint32_t due_ms) noexcept {
    if (kcp_time_reached(now_ms, due_ms)) {
        return 1;
    }

    return static_cast<std::uint64_t>(
        static_cast<std::uint32_t>(due_ms - now_ms)
    );
}

/**
 * 从KCP底层数据包中提取conv
 *
 * KCP报文前4个字节就是conv字段，并且KCP内部使用小端格式编码。
 *
 * 参数说明：
 * data：UDP收到的数据，也就是KCP报文
 * len：数据长度
 * conv：输出参数，用于保存解析出的conv
 *
 * 返回值：
 * true：解析成功
 * false：参数非法或数据长度不足
 *
 * 注意：
 * 这里只是提取conv，不会校验整个KCP包是否合法。
 */
bool extract_conv(const char* data,
                  std::size_t len,
                  KcpConv* conv) noexcept {
    if (conv == nullptr || data == nullptr || len < KcpConv::kSize) {
        return false;
    }

    const std::uint8_t* bytes =
        reinterpret_cast<const std::uint8_t*>(data);
    std::copy(bytes, bytes + KcpConv::kSize, conv->bytes.begin());
    return true;
}

/**
 * 根据KcpSessionKey重新构造IPv4 sockaddr_in地址
 *
 * KcpSessionKey中保存的是：
 * 1.对端IPv4地址，网络字节序；
 * 2.对端端口，网络字节序；
 * 3.conv。
 *
 * 创建KcpSession时需要sockaddr地址，所以这里把key还原成sockaddr_in。
 */
sockaddr_in make_ipv4_addr(const KcpSessionKey& key) noexcept {
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = key.addr_be;
    addr.sin_port = key.port_be;

    return addr;
}

}  // namespace

/**
 * KcpSessionKey构造函数
 *
 * 作用：
 * 根据conv和peer地址生成一个session唯一标识。
 *
 * 当前版本只支持IPv4。
 * key由以下字段组成：
 *
 * peer IPv4地址 + peer UDP端口 + conv
 *
 * 这样可以避免不同对端使用相同conv时产生冲突。
 */
KcpSessionKey::KcpSessionKey(const KcpConv& conv_in,
                             const sockaddr* peer,
                             socklen_t peer_len_in) noexcept
    : addr_be(0),
      port_be(0),
      reserved(0),
      conv(conv_in) {
    // 参数检查：
    // 1.peer不能为空；
    // 2.peer长度至少能容纳sockaddr_in；
    // 3.peer必须是IPv4地址。
    if (peer == nullptr || peer_len_in < static_cast<socklen_t>(sizeof(sockaddr_in)) ||
        peer->sa_family != AF_INET) {
        return;
    }

    // 将通用sockaddr转换成IPv4 sockaddr_in。
    const sockaddr_in* addr = reinterpret_cast<const sockaddr_in*>(peer);

    // 保存IPv4地址和端口。
    // 这里不做ntohl/ntohs转换，直接保存网络字节序。
    addr_be = addr->sin_addr.s_addr;
    port_be = addr->sin_port;
}

KcpSessionKey::KcpSessionKey(std::uint32_t conv_in,
                             const sockaddr* peer,
                             socklen_t peer_len_in) noexcept
    : KcpSessionKey(KcpConv::from_uint32(conv_in), peer, peer_len_in) {}

/**
 * 判断KcpSessionKey是否合法
 *
 * 当前判断比较宽松：
 * 只要addr_be或者port_be不全为0，就认为有效。
 *
 * 也就是说：
 * 0.0.0.0:0会被认为无效。
 */
bool KcpSessionKey::valid() const noexcept {
    return (addr_be != 0 || port_be != 0) && !conv.empty();
}

/**
 * 判断两个KcpSessionKey是否相等
 *
 * 注意：
 * reserved字段没有参与比较。
 *
 * 真正用于区分session的是：
 * 1.对端IP；
 * 2.对端端口；
 * 3.KCP conv。
 */
bool KcpSessionKey::operator==(const KcpSessionKey& other) const noexcept {
    return addr_be == other.addr_be &&
           port_be == other.port_be &&
           conv == other.conv;
}

/**
 * KcpSessionKey哈希函数
 *
 * 作用：
 * 让KcpSessionKey可以作为unordered_map的key。
 *
 * 哈希组合了：
 * 1.addr_be；
 * 2.port_be；
 * 3.conv。
 */
std::size_t KcpSessionKeyHash::operator()(
    const KcpSessionKey& key) const noexcept {
    std::size_t seed = std::hash<std::uint32_t>()(key.addr_be);

    // 使用类似boost hash_combine的方式混合port。
    seed ^= std::hash<std::uint16_t>()(key.port_be) +
            0x9e3779b9u +
            (seed << 6) +
            (seed >> 2);

    // 继续混合256-bit conv。
    seed ^= KcpConvHash()(key.conv) +
            0x9e3779b9u +
            (seed << 6) +
            (seed >> 2);

    return seed;
}

/**
 * KcpManager构造函数
 *
 * KcpManager负责管理多个KcpSession。
 *
 * 参数说明：
 * config：KCP管理器配置，包括UDP收发参数、session配置、空闲超时、fallback tick等。
 */
KcpManager::KcpManager(const KcpManagerConfig& config)
    : running_(false),

      // 初始化底层UDP分发器。
      // dispatcher_内部负责UDP socket、io_uring收发、timeout/post调度。
      dispatcher_(config.entries,
                  config.max_events,
                  config.recv_buffer_size),

      // KCP会话表，key为KcpSessionKey，value为SessionEntry。
      sessions_(),

      // 定时器堆，用于保存每个session下一次需要update的时间。
      timer_heap_(),

      // 上层消息回调，初始为空。
      message_callback_(),

      // 保存单个KcpSession的配置。
      session_config_(config.session_config),

      // 空闲超时时间。
      // 如果为0，表示不启用空闲session回收。
      idle_timeout_ms_(config.idle_timeout_ms),

      // 兜底tick间隔。
      // 如果用户配置为0，就默认使用10ms。
      fallback_tick_ms_(config.fallback_tick_ms == 0
                            ? 10
                            : config.fallback_tick_ms),

      // 下一次执行空闲GC的时间点，初始为0。
      next_idle_gc_mono_ms_(0),

      // 当前已经安排的tick到期时间，初始为0表示没有armed。
      armed_tick_due_mono_ms_(0),

      // tick调度序号，用于过滤旧timer。
      armed_tick_seq_(0) {
    // 预留一定容量，减少运行过程中频繁rehash和扩容。
    sessions_.reserve(1024);
    timer_heap_.reserve(1024);
}

/**
 * 析构函数
 *
 * 释放KcpManager资源。
 * stop中会停止dispatcher，并清理session和timer。
 */
KcpManager::~KcpManager() {
    stop();
}

/**
 * 绑定本地UDP地址
 *
 * 参数说明：
 * host：本地监听IP，例如"0.0.0.0"或"127.0.0.1"
 * port：本地UDP端口
 *
 * 实际绑定工作交给UdpDispatcher完成。
 */
bool KcpManager::bind(const std::string& host, std::uint16_t port) {
    return dispatcher_.bind(host, port);
}

/**
 * 设置KCP完整消息回调
 *
 * 当某个KcpSession重组出完整应用层消息时，
 * KcpManager最终会调用这个回调，把消息交给业务层。
 */
void KcpManager::set_message_callback(KcpManagerMessageCallback cb) {
    message_callback_ = std::move(cb);
}

/**
 * 启动KcpManager
 *
 * 启动流程：
 * 1.使用atomic保证只启动一次；
 * 2.给dispatcher_设置UDP收包回调；
 * 3.启动dispatcher_；
 * 4.投递一个post任务，在dispatcher事件循环线程里初始化定时tick。
 */
void KcpManager::start() {
    bool expected = false;

    // 如果running_原来是false，就改成true并继续启动。
    // 如果已经是true，说明已经启动过，直接返回。
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }

    // 设置UDP收包回调。
    // UdpDispatcher每收到一个UDP数据报，就会调用handle_udp_packet。
    dispatcher_.set_message_callback(
        [this](const sockaddr* peer,
               socklen_t peer_len,
               const char* data,
               std::size_t len) {
            handle_udp_packet(peer, peer_len, data, len);
        }
    );

    // 启动UDP/io_uring事件循环。
    dispatcher_.start();

    // 通过post把初始化tick的逻辑切到dispatcher_所在的事件循环线程执行。
    dispatcher_.post(
        [this](RequestContext*, int) {
            const std::uint64_t now = monotonic_ms();

            // 设置下一次空闲session回收时间。
            next_idle_gc_mono_ms_ = now + 1000;

            // 安排第一次KCP定时tick。
            // 因为此时可能还没有session，所以使用fallback_tick_ms_兜底。
            arm_tick_for_due_in_loop(now + fallback_tick_ms_);
        }
    );
}

/**
 * 停止KcpManager
 *
 * 停止流程：
 * 1.running_置为false；
 * 2.停止dispatcher_；
 * 3.清空所有KcpSession；
 * 4.清空定时堆；
 * 5.重置tick和GC状态。
 */
void KcpManager::stop() {
    const bool was_running =
        running_.exchange(false, std::memory_order_acq_rel);

    // 如果原来就没有运行，不需要重复停止。
    if (!was_running) {
        return;
    }

    dispatcher_.stop();

    // 清理所有session。
    // unique_ptr<KcpSession>会自动释放对应KCP控制块。
    sessions_.clear();

    // 清理timer堆。
    timer_heap_.clear();

    next_idle_gc_mono_ms_ = 0;
    armed_tick_due_mono_ms_ = 0;
    armed_tick_seq_ = 0;
}

/**
 * 根据KcpSessionKey发送应用层数据
 *
 * 这个函数可能从外部线程调用。
 * 为了保证sessions_、timer_heap_等结构只在dispatcher事件循环线程中访问，
 * 这里不会直接调用send_in_loop，而是通过dispatcher_.post投递到loop中执行。
 */
bool KcpManager::send(const KcpSessionKey& key,
                      const char* data,
                      std::size_t len) {
    // 基础参数检查。
    if (!key.valid() || data == nullptr || len == 0 ||
        !running_.load(std::memory_order_acquire)) {
        return false;
    }

    // 拷贝一份payload。
    // 原因：
    // send真正执行是在post之后，可能晚于当前函数返回。
    // 如果直接捕获外部data指针，外部buffer可能已经失效。
    std::string payload(data, len);

    // 投递到dispatcher事件循环线程执行。
    return dispatcher_.post(
        [this, key, payload = std::move(payload)](RequestContext*, int) {
            send_in_loop(key, payload.data(), payload.size());
        }
    );
}

/**
 * string版本发送接口
 *
 * 只是对char*版本的便利封装。
 */
bool KcpManager::send(const KcpSessionKey& key,
                      const std::string& data) {
    return send(key, data.data(), data.size());
}

bool KcpManager::send(const KcpSessionKey& key,
                      std::string&& data) {
    if (!key.valid() || data.empty() ||
        !running_.load(std::memory_order_acquire)) {
        return false;
    }

    return dispatcher_.post(
        [this, key, payload = std::move(data)](RequestContext*, int) {
            send_in_loop(key, payload.data(), payload.size());
        }
    );
}

/**
 * 根据peer地址和conv发送数据
 *
 * 内部先构造KcpSessionKey，再复用key版本send接口。
 */
bool KcpManager::send(const sockaddr* peer,
                      socklen_t peer_len,
                      const KcpConv& conv,
                      const char* data,
                      std::size_t len) {
    return send(KcpSessionKey(conv, peer, peer_len), data, len);
}

bool KcpManager::send(const sockaddr* peer,
                      socklen_t peer_len,
                      std::uint32_t conv,
                      const char* data,
                      std::size_t len) {
    return send(peer, peer_len, KcpConv::from_uint32(conv), data, len);
}

/**
 * 根据peer地址和conv发送string数据
 */
bool KcpManager::send(const sockaddr* peer,
                      socklen_t peer_len,
                      const KcpConv& conv,
                      const std::string& data) {
    return send(peer, peer_len, conv, data.data(), data.size());
}

bool KcpManager::send(const sockaddr* peer,
                      socklen_t peer_len,
                      std::uint32_t conv,
                      const std::string& data) {
    return send(peer, peer_len, KcpConv::from_uint32(conv), data);
}

bool KcpManager::send(const sockaddr* peer,
                      socklen_t peer_len,
                      const KcpConv& conv,
                      std::string&& data) {
    return send(KcpSessionKey(conv, peer, peer_len), std::move(data));
}

bool KcpManager::send(const sockaddr* peer,
                      socklen_t peer_len,
                      std::uint32_t conv,
                      std::string&& data) {
    return send(peer, peer_len, KcpConv::from_uint32(conv), std::move(data));
}

/**
 * 返回底层UDP socket fd
 *
 * 主要用于调试或与其他模块集成。
 */
int KcpManager::fd() const noexcept {
    return dispatcher_.fd();
}

/**
 * 处理底层UDP收到的KCP包
 *
 * 这个函数由UdpDispatcher的收包回调触发，通常运行在dispatcher事件循环线程中。
 *
 * 处理流程：
 * 1.从KCP包头提取conv；
 * 2.根据peer地址和conv构造KcpSessionKey；
 * 3.查找或创建对应KcpSession；
 * 4.把UDP数据喂给KcpSession::input_packet；
 * 5.KcpManager主动drain已经完成重组的应用层消息；
 * 6.更新该session的活跃时间；
 * 7.重新调度该session下一次update时间。
 */
void KcpManager::handle_udp_packet(const sockaddr* peer,
                                   socklen_t peer_len,
                                   const char* data,
                                   std::size_t len) {
    KcpConv conv;

    // KCP数据包前32字节是256-bit conv。
    // 如果连conv都解析不出来，就说明不是合法KCP包，直接丢弃。
    if (!extract_conv(data, len, &conv)) {
        return;
    }

    // 用对端地址+端口+conv构造session key。
    const KcpSessionKey key(conv, peer, peer_len);

    // 查找或创建对应KcpSession。
    SessionEntry* entry = find_or_create_session_entry_in_loop(key);
    if (entry == nullptr) {
        return;
    }

    const std::uint64_t mono_now = monotonic_ms();
    const std::uint32_t now = kcp_now_ms(mono_now);

    // 收到对端数据，更新活跃时间。
    entry->last_active_mono_ms = mono_now;

    // 把UDP负载喂给KCP。
    // KcpSession只负责协议处理，不再主动回调业务层。
    if (entry->session->input_packet(data, len, now) < 0) {
        return;
    }

    // 由KcpManager主动把已经重组好的应用层消息拉出来。
    drain_session_messages_in_loop(key, *entry);

    // input之后，KCP内部状态可能变化。
    // 因此需要重新计算下一次ikcp_update时间。
    schedule_session_in_loop(key, *entry, now, mono_now);
}

/**
 * 处理某个KcpSession输出的完整应用层消息
 *
 * KcpManager在drain某个KcpSession时，会调用这里，
 * 再把消息转交给业务层设置的message_callback_。
 */
void KcpManager::handle_session_message(const KcpSessionKey& key,
                                        const char* data,
                                        std::size_t len) {
    if (message_callback_) {
        message_callback_(key, data, len);
    }
}

/**
 * 在事件循环线程中主动提取某个session已经完成重组的应用层消息
 */
void KcpManager::drain_session_messages_in_loop(const KcpSessionKey& key,
                                                SessionEntry& entry) {
    if (!message_callback_) {
        return;
    }

    entry.session->drain_messages(
        [this, key](const char* data, std::size_t len) {
            handle_session_message(key, data, len);
        }
    );
}

/**
 * KCP定时tick处理函数
 *
 * 这是KcpManager调度KCP update的核心。
 *
 * timer_heap_中保存了所有session下一次应该update的时间。
 * 每次tick触发时：
 *
 * 1.取当前时间；
 * 2.检查堆顶session是否已经到期；
 * 3.到期则pop出来；
 * 4.过滤掉旧timer节点；
 * 5.调用session->check判断是否真的需要update；
 * 6.如果到期，调用session->update；
 * 7.update后重新schedule；
 * 8.最后安排下一次tick；
 * 9.顺便执行空闲session回收。
 */
void KcpManager::handle_tick() {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }

    const std::uint64_t mono_now = monotonic_ms();
    const std::uint32_t now = kcp_now_ms(mono_now);

    // 持续处理所有已经到期的timer节点。
    while (!timer_heap_.empty()) {
        const TimerNode& top = timer_heap_.front();

        // 如果堆顶还没到期，说明后面的节点也不会到期。
        // 直接为堆顶时间安排下一次tick。
        if (top.due_mono_ms > mono_now) {
            arm_tick_for_due_in_loop(top.due_mono_ms);
            break;
        }

        // 弹出堆顶节点。
        // std::pop_heap会把堆顶元素移动到vector末尾。
        std::pop_heap(
            timer_heap_.begin(),
            timer_heap_.end(),
            TimerNodeGreater()
        );
        TimerNode node = timer_heap_.back();
        timer_heap_.pop_back();

        // 根据timer中的key查找对应session。
        auto it = sessions_.find(node.key);
        if (it == sessions_.end()) {
            // session可能已经因为空闲超时被删除。
            continue;
        }

        SessionEntry& entry = it->second;

        // 过滤过期timer节点。
        //
        // 因为一个session可能被多次重新调度，
        // 旧的TimerNode不会从heap中删除，而是在触发时通过schedule_seq过滤。
        if (entry.schedule_seq != node.schedule_seq ||
            entry.next_due_mono_ms != node.due_mono_ms) {
            continue;
        }

        // 再问一次KCP：当前时间下，下一次应该什么时候update。
        const std::uint32_t next_due =
            entry.session->check(now);

        // 如果KCP认为已经到时间了，就真正调用update。
        if (kcp_time_reached(now, next_due)) {
            entry.session->update(now);
        }

        // update之后，KCP状态可能变化。
        // 重新计算该session下一次到期时间，并压回timer_heap_。
        schedule_session_in_loop(node.key, entry, now, mono_now);
    }

    // 如果堆为空，说明当前没有session，或者没有可调度节点。
    // 为了后续空闲GC或新session调度兜底，这里安排一个fallback tick。
    if (timer_heap_.empty() && running_.load(std::memory_order_acquire)) {
        arm_tick_for_due_in_loop(mono_now + fallback_tick_ms_);
    }

    // 周期性清理空闲session。
    maybe_collect_idle_sessions_in_loop(mono_now);
}

/**
 * 在事件循环线程中安排下一次tick
 *
 * 参数说明：
 * due_mono_ms：期望tick触发的绝对单调时间，单位毫秒。
 *
 * 逻辑：
 * 1.如果KcpManager已经停止，直接返回；
 * 2.如果当前已经armed了一个更早的tick，就不重复安排更晚的tick；
 * 3.根据due_mono_ms计算delay；
 * 4.通过dispatcher_.schedule_timeout安排定时任务；
 * 5.使用armed_tick_seq_过滤旧timer。
 */
void KcpManager::arm_tick_for_due_in_loop(std::uint64_t due_mono_ms) {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }

    // 如果已经安排了一个更早或相同时间的tick，就不需要再安排这个更晚的tick。
    if (armed_tick_due_mono_ms_ != 0 &&
        due_mono_ms >= armed_tick_due_mono_ms_) {
        return;
    }

    const std::uint64_t now = monotonic_ms();

    // 转成相对延迟。
    // 如果due已经过了，则用1ms，避免0ms导致立即反复触发。
    const std::uint64_t delay_ms =
        due_mono_ms > now ? (due_mono_ms - now) : 1;

    // 记录当前armed tick的到期时间。
    armed_tick_due_mono_ms_ = due_mono_ms;

    // 增加tick序号。
    // 后面如果又安排了更早的tick，旧timer触发时会因为seq不匹配被忽略。
    const std::uint64_t tick_seq = ++armed_tick_seq_;

    dispatcher_.schedule_timeout(
        delay_ms,
        [this, tick_seq](RequestContext*, int) {
            // 如果已经停止，或者这个timer不是当前最新timer，就忽略。
            if (!running_.load(std::memory_order_acquire) ||
                tick_seq != armed_tick_seq_) {
                return;
            }

            // 当前armed tick已经触发，清空armed时间。
            armed_tick_due_mono_ms_ = 0;

            // 真正处理KCP定时逻辑。
            handle_tick();
        }
    );
}

/**
 * 重新调度某个KcpSession的下一次update时间
 *
 * 参数说明：
 * key：session key
 * entry：session表中对应的entry
 * kcp_now：当前KCP时间，uint32_t毫秒
 * mono_now：当前单调时间，uint64_t毫秒
 *
 * 逻辑：
 * 1.调用session->check(kcp_now)，得到KCP下一次建议update时间；
 * 2.把KCP时间差转换成uint64_t delay；
 * 3.转换成单调时间体系下的due_mono_ms；
 * 4.更新entry.next_due_mono_ms；
 * 5.递增entry.schedule_seq；
 * 6.把新的TimerNode压入timer_heap_；
 * 7.尝试安排全局下一次tick。
 */
void KcpManager::schedule_session_in_loop(const KcpSessionKey& key,
                                          SessionEntry& entry,
                                          std::uint32_t kcp_now,
                                          std::uint64_t mono_now) {
    // 查询该session下一次最早需要调用ikcp_update的KCP时间点。
    const std::uint32_t due_kcp_ms = entry.session->check(kcp_now);

    // 计算距离due_kcp_ms还有多久。
    const std::uint64_t delay_ms = kcp_delay_ms(kcp_now, due_kcp_ms);

    // 转换到uint64_t单调时间体系。
    const std::uint64_t due_mono_ms = mono_now + delay_ms;

    // 更新entry当前有效的下一次到期时间。
    entry.next_due_mono_ms = due_mono_ms;

    // 每次重新调度都递增序号。
    // 旧的TimerNode不会立即从heap删除，后面靠这个序号过滤。
    ++entry.schedule_seq;

    // 压入新的timer节点。
    timer_heap_.push_back(TimerNode{due_mono_ms, entry.schedule_seq, key});

    // 维护小根堆。
    std::push_heap(
        timer_heap_.begin(),
        timer_heap_.end(),
        TimerNodeGreater()
    );

    // 如果这个session的due时间比当前armed tick更早，
    // arm_tick_for_due_in_loop会重新安排更早的tick。
    arm_tick_for_due_in_loop(due_mono_ms);
}

/**
 * 周期性回收空闲session
 *
 * 参数说明：
 * mono_now_ms：当前单调毫秒时间
 *
 * 如果idle_timeout_ms_为0，表示不启用空闲回收。
 *
 * 回收规则：
 * 当前时间-最后活跃时间>=idle_timeout_ms_，就删除该session。
 *
 * 这里每隔约1000ms执行一次GC，避免每次tick都遍历sessions_。
 */
void KcpManager::maybe_collect_idle_sessions_in_loop(
    std::uint64_t mono_now_ms) {
    if (idle_timeout_ms_ == 0) {
        return;
    }

    // 如果还没到下一次GC时间，直接返回。
    if (next_idle_gc_mono_ms_ != 0 && mono_now_ms < next_idle_gc_mono_ms_) {
        return;
    }

    // 下一次GC安排在1秒后。
    next_idle_gc_mono_ms_ = mono_now_ms + 1000;

    // 遍历session表，删除空闲超时的session。
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (mono_now_ms - it->second.last_active_mono_ms >= idle_timeout_ms_) {
            it = sessions_.erase(it);
            continue;
        }
        ++it;
    }
}

/**
 * 查找或创建SessionEntry
 *
 * 参数说明：
 * key：由peer地址、peer端口、conv组成的session key
 *
 * 返回值：
 * 找到或创建成功，返回SessionEntry指针；
 * 失败则返回nullptr。
 *
 * 该函数名字带_in_loop，表示它应该只在dispatcher事件循环线程中调用。
 * 这样sessions_就不需要额外加锁。
 */
KcpManager::SessionEntry* KcpManager::find_or_create_session_entry_in_loop(
    const KcpSessionKey& key) {
    // 先查找已有session。
    auto it = sessions_.find(key);
    if (it != sessions_.end()) {
        return &it->second;
    }

    // key非法则不能创建session。
    if (!key.valid()) {
        return nullptr;
    }

    // 根据key还原对端IPv4地址。
    const sockaddr_in peer = make_ipv4_addr(key);

    // 创建新的KcpSession。
    std::unique_ptr<KcpSession> session(new KcpSession(
        key.conv,
        reinterpret_cast<const sockaddr*>(&peer),
        sizeof(peer),

        // KCP底层输出回调。
        //
        // 当KcpSession内部ikcp_update触发output时，
        // 最终会调用这个lambda，把KCP底层报文通过UdpDispatcher发出去。
        [this](const sockaddr* out_peer,
               socklen_t out_peer_len,
               const char* data,
               std::size_t len) {
            return dispatcher_.send_to(out_peer, out_peer_len, data, len)
                ? 0
                : -1;
        },

        // 单个session的KCP配置。
        session_config_
    ));

    // 创建失败或session无效时返回nullptr。
    if (!session || !session->valid()) {
        return nullptr;
    }

    // 构造SessionEntry。
    SessionEntry entry;
    entry.session = std::move(session);

    // 创建时认为该session刚刚活跃。
    entry.last_active_mono_ms = monotonic_ms();

    // 初始还没有调度时间。
    entry.next_due_mono_ms = 0;

    // 初始调度序号为0。
    entry.schedule_seq = 0;

    // 插入session表。
    auto result = sessions_.emplace(key, std::move(entry));

    return &result.first->second;
}

/**
 * 在事件循环线程中发送数据
 *
 * 参数说明：
 * key：目标session key
 * data：要发送的应用层数据
 * len：数据长度
 *
 * 处理流程：
 * 1.检查参数；
 * 2.查找或创建session；
 * 3.更新活跃时间；
 * 4.调用KcpSession::send_app_data；
 * 5.重新调度该session下一次update时间。
 */
bool KcpManager::send_in_loop(const KcpSessionKey& key,
                              const char* data,
                              std::size_t len) {
    if (!key.valid() || data == nullptr || len == 0) {
        return false;
    }

    // 查找或创建对应session。
    SessionEntry* entry = find_or_create_session_entry_in_loop(key);
    if (entry == nullptr) {
        return false;
    }

    const std::uint64_t mono_now = monotonic_ms();
    const std::uint32_t now = kcp_now_ms(mono_now);

    // 主动发送数据，也算该session活跃。
    entry->last_active_mono_ms = mono_now;

    // 把应用层数据交给KCP。
    // KcpSession内部会调用ikcp_send，然后调用ikcp_update推动发送。
    if (entry->session->send_app_data(data, len, now) < 0) {
        return false;
    }

    // 发送后KCP状态会变化，例如发送队列、flush时间、重传时间都会变化。
    // 所以需要重新调度该session。
    schedule_session_in_loop(key, *entry, now, mono_now);

    return true;
}

}  // namespace kcp
}  // namespace myring
