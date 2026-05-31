#ifndef __KERNEL__
#define __KERNEL__
#endif /* __KERNEL__ */

#ifndef MODULE
#define MODULE
#endif /* MODULE */

#include "sipfw.h"
#include <linux/time.h>
#include <linux/time64.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/icmp.h>
#include <linux/igmp.h>
#include <linux/in.h>

/*
 * SIPFW_Localtime
 *
 * 功能：
 *   把Unix时间戳转换为本地时间结构struct tma。
 *
 * 参数：
 *   r    ：输出时间结构。
 *   time ：Unix时间戳，单位为秒，表示从1970-01-01 00:00:00 UTC开始经过的秒数。
 *
 * 说明：
 *   这里使用time64_to_tm()进行通用时间转换。
 *   第二个参数8 * 60 * 60表示东八区偏移量。
 */
void SIPFW_Localtime(struct tma *r, unsigned long time)
{
    struct tm tm;

    if (!r)
    {
        return;
    }

    /*
     * time64_to_tm的第二个参数是时区偏移，单位为秒。
     * 东八区就是8小时，即8 * 60 * 60秒。
     */
    time64_to_tm((time64_t)time, 8 * 60 * 60, &tm);

    /*
     * struct tm中的年份是从1900年开始计算的。
     * 例如2026年对应tm_year = 126。
     */
    r->year = tm.tm_year + 1900;

    /*
     * struct tm中的月份范围是0~11。
     * 如果你的struct tma希望月份是1~12，这里要加1。
     */
    r->mon = tm.tm_mon + 1;

    /*
     * 日期、时、分、秒可以直接使用。
     */
    r->mday = tm.tm_mday;
    r->hour = tm.tm_hour;
    r->min = tm.tm_min;
    r->sec = tm.tm_sec;
}

static void SIPFW_FreeRuleSnapshotRcu(struct rcu_head *rcu)
{
    struct sipfw_rule_snapshot *snapshot = NULL;

    snapshot = container_of(rcu, struct sipfw_rule_snapshot, rcu);
    kfree(snapshot);
}

struct sipfw_rule_snapshot *SIPFW_AllocRuleSnapshot(unsigned int count, gfp_t gfp)
{
    struct sipfw_rule_snapshot *snapshot = NULL;
    size_t size = 0;

    if (count == 0)
    {
        return NULL;
    }

    size = sizeof(*snapshot) + (sizeof(snapshot->rules[0]) * count);
    snapshot = kzalloc(size, gfp);
    if (!snapshot)
    {
        return NULL;
    }

    snapshot->count = count;
    return snapshot;
}

void SIPFW_PublishRuleSnapshotLocked(struct sipfw_list *l,
                                     struct sipfw_rule_snapshot *snapshot)
{
    struct sipfw_rule_snapshot *old = NULL;
    struct sipfw_rules *cur = NULL;
    unsigned int copied = 0;

    if (!l)
    {
        if (snapshot)
        {
            kfree(snapshot);
        }
        return;
    }

    if (snapshot)
    {
        snapshot->count = l->number;

        for (cur = l->rule; cur != NULL && copied < snapshot->count; cur = cur->next)
        {
            snapshot->rules[copied] = *cur;
            snapshot->rules[copied].next = NULL;
            copied++;
        }

        snapshot->count = copied;
    }

    old = rcu_dereference_protected(l->snapshot, 1);
    rcu_assign_pointer(l->snapshot, snapshot);

    if (old)
    {
        call_rcu(&old->rcu, SIPFW_FreeRuleSnapshotRcu);
    }
}

static int SIPFW_EnsurePacketData(struct sk_buff *skb,
                                  unsigned int ip_len,
                                  __u8 protocol)
{
    unsigned int need = 0;

    if (!skb)
    {
        return 0;
    }

    need = skb_network_offset(skb) + ip_len;

    switch (protocol)
    {
    case IPPROTO_TCP:
        need += sizeof(struct tcphdr);
        break;

    case IPPROTO_UDP:
        need += sizeof(struct udphdr);
        break;

    case IPPROTO_ICMP:
        need += sizeof(struct icmphdr);
        break;

    case IPPROTO_IGMP:
        need += sizeof(struct igmphdr);
        break;

    default:
        break;
    }

    return pskb_may_pull(skb, need);
}

static int SIPFW_IsIfnameMatch(const struct sipfw_rules *r,
                               const struct nf_hook_state *state)
{
    size_t len = 0;

    if (!r)
    {
        return 0;
    }

    len = strnlen(r->ifname, sizeof(r->ifname));
    if (len == 0)
    {
        return 1;
    }

    if (!state)
    {
        return 0;
    }

    switch (r->chain)
    {
    case SIPFW_CHAIN_INPUT:
        return state->in &&
               !strncmp(r->ifname, state->in->name, sizeof(r->ifname));

    case SIPFW_CHAIN_OUTPUT:
        return state->out &&
               !strncmp(r->ifname, state->out->name, sizeof(r->ifname));

    case SIPFW_CHAIN_FORWARD:
        return (state->in &&
                !strncmp(r->ifname, state->in->name, sizeof(r->ifname))) ||
               (state->out &&
                !strncmp(r->ifname, state->out->name, sizeof(r->ifname)));

    default:
        return 0;
    }
}

/*
 * SIPFW_IsAddtionMatch
 *
 * 功能：
 *   判断数据包的传输层附加信息是否与规则匹配。
 *
 * 匹配内容包括：
 *   1. TCP源端口、目的端口、ACK/FIN/SYN标志位；
 *   2. UDP源端口、目的端口；
 *   3. ICMP/IGMP类型和代码。
 *
 * 参数：
 *   iph  ：IP头指针。
 *   data ：IP负载部分指针，也就是TCP/UDP/ICMP/IGMP头部开始的位置。
 *   r    ：待匹配的防火墙规则。
 *
 * 返回值：
 *   1：匹配成功。
 *   0：匹配失败。
 *
 * 注意：
 *   1. 这里没有检查skb长度是否足够，直接把data转换成TCP/UDP/ICMP头指针，存在越界风险。
 *   2. TCP/UDP端口在包里通常是网络字节序，规则里的端口字段也必须保持同样字节序，否则比较会失败。
 *   3. UDP分支用了struct tcphdr *udph，类型不准确，应该使用struct udphdr。
 */
static int SIPFW_IsAddtionMatch(struct iphdr *iph, void *data, const struct sipfw_rules *r)
{
    int found = 0;

    DBGPRINT("==>SIPFW_IsAddtionMatch\n");

    /*
     * 基础参数检查。
     */
    if (!iph || !data || !r)
    {
        goto out;
    }

    /*
     * IP头长度至少应为5，即20字节。
     * 这里只能检查iph本身字段，无法检查skb真实长度。
     */
    if (iph->ihl < 5)
    {
        goto out;
    }

    switch (iph->protocol)
    {
    case IPPROTO_TCP:
    {
        struct tcphdr *tcph = (struct tcphdr *)data;

        /*
         * TCP端口匹配。
         *
         * r->sport == 0 表示不限制源端口。
         * r->dport == 0 表示不限制目的端口。
         *
         * 注意：
         * tcph->source和tcph->dest是网络字节序。
         * 如果规则里的sport/dport也是网络字节序，可以直接比较。
         * 如果规则里保存的是主机字节序，则需要在规则入表时统一转换，或者这里用htons()比较。
         */
        if ((tcph->source != r->sport && r->sport != 0) ||
            (tcph->dest != r->dport && r->dport != 0))
        {
            break;
        }

        /*
         * 如果规则没有启用TCP标志位匹配，则端口匹配即可。
         */
        if (!r->addtion.valid)
        {
            found = 1;
            break;
        }

        /*
         * 如果启用了附加匹配，则继续比较ACK/FIN/SYN标志位。
         */
        {
            const struct tcp_flag *tcpf = &r->addtion.tcp;

            if (tcpf->ack == tcph->ack &&
                tcpf->fin == tcph->fin &&
                tcpf->syn == tcph->syn &&
                tcpf->rst == tcph->rst)
            {
                found = 1;
            }
        }
    }
    break;

    case IPPROTO_UDP:
    {
        /*
         * UDP应该使用struct udphdr，不能用struct tcphdr。
         */
        struct udphdr *udph = (struct udphdr *)data;

        if ((udph->source == r->sport || r->sport == 0) &&
            (udph->dest == r->dport || r->dport == 0))
        {
            found = 1;
        }
    }
    break;

    case IPPROTO_ICMP:
    {
        /*
         * ICMP使用struct icmphdr。
         */
        struct icmphdr *icmph = (struct icmphdr *)data;

        /*
         * 没有启用附加匹配时，只要IP层协议匹配即可。
         */
        if (!r->addtion.valid)
        {
            found = 1;
            break;
        }

        /*
         * 启用附加匹配时，比较ICMP type/code。
         */
        if (r->addtion.icgmp.type == icmph->type &&
            r->addtion.icgmp.code == icmph->code)
        {
            found = 1;
        }
    }
    break;

    case IPPROTO_IGMP:
    {
        /*
         * IGMP使用struct igmphdr。
         */
        struct igmphdr *igmph = (struct igmphdr *)data;

        /*
         * 没有启用附加匹配时，只要IP层协议匹配即可。
         */
        if (!r->addtion.valid)
        {
            found = 1;
            break;
        }

        /*
         * 启用附加匹配时，比较IGMP type/code。
         */
        if (r->addtion.icgmp.type == igmph->type &&
            r->addtion.icgmp.code == igmph->code)
        {
            found = 1;
        }
    }
    break;

    default:
        /*
         * 其他协议。
         *
         * 原代码这里直接found = 0。
         * 如果你的系统只支持TCP/UDP/ICMP/IGMP规则，保持0是对的。
         */
        found = 0;
        break;
    }

out:
    DBGPRINT("<==SIPFW_IsAddtionMatch\n");
    return found;
}


/*
 * SIPFW_IsIPMatch
 *
 * 功能：
 *   判断数据包IP层字段是否与规则匹配。
 *
 * 匹配内容包括：
 *   1. 目的IP地址；
 *   2. 源IP地址；
 *   3. IP协议号。
 *
 * 参数：
 *   iph ：IP头指针。
 *   r   ：待匹配的防火墙规则。
 *
 * 返回值：
 *   1：匹配成功。
 *   0：匹配失败。
 *
 * 规则约定：
 *   r->dest == 0表示不限制目的地址。
 *   r->source == 0表示不限制源地址。
 *   r->protocol == 0表示不限制协议。
 */
static int SIPFW_IsIPMatch(struct iphdr *iph, const struct sipfw_rules *r)
{
    int found = 0;

    DBGPRINT("==>SIPFW_IsIPMatch\n");

    if ((iph->daddr == r->dest || r->dest == 0) &&
        (iph->saddr == r->source || r->source == 0) &&
        (iph->protocol == r->protocol || r->protocol == 0))
    {
        found = 1;
    }

    DBGPRINT("<==SIPFW_IsIPMatch\n");
    return found;
}


/*
 * SIPFW_IsMatch
 *
 * 功能：
 *   判断一个数据包是否匹配某条规则链中的任意规则。
 *
 * 参数：
 *   skb     ：当前网络数据包。
 *   l       ：规则表，内部持有当前RCU快照。
 *   matched ：命中规则的输出副本。
 *
 * 返回值：
 *   1：匹配成功，matched中带回命中规则。
 *   0：没有匹配到规则。
 *
 * 匹配流程：
 *   1. 从skb中取出IP头；
 *   2. 计算IP负载部分位置；
 *   3. 在RCU读侧临界区内取得当前规则快照；
 *   4. 先匹配IP层字段；
 *   5. 再匹配传输层或ICMP/IGMP附加字段；
 *   6. 找到第一条匹配规则后立即返回。
 *
 * 注意：
 *   1. 这里没有检查skb是否为NULL，也没有检查IP头是否合法。
 *   2. p = skb->data + iph->ihl * 4依赖skb->data当前指向IP头开始处，
 *      在不同Netfilter hook位置要确认这一点是否成立。
 *   3. 如果规则链在其他线程中可能被修改或释放，这里遍历时应该加读锁，
 *      或者使用RCU保护。
 */
int SIPFW_IsMatch(struct sk_buff *skb,
                  const struct nf_hook_state *state,
                  const struct sipfw_list *l,
                  struct sipfw_rules *matched)
{
    const struct sipfw_rule_snapshot *snapshot = NULL;
    const struct sipfw_rules *r = NULL;
    struct iphdr *iph = NULL;
    void *p = NULL;
    int found = 0;
    unsigned int i = 0;
    unsigned int ip_len = 0;

    if (!skb || !l || !matched)
    {
        goto EXITSIPFW_IsMatch;
    }

    memset(matched, 0, sizeof(*matched));

    /*
     * 获取IP头指针。
     */
    if (!pskb_may_pull(skb, skb_network_offset(skb) + sizeof(struct iphdr)))
    {
        goto EXITSIPFW_IsMatch;
    }

    iph = ip_hdr(skb);
    if (!iph || iph->ihl < 5)
    {
        goto EXITSIPFW_IsMatch;
    }

    /*
     * 计算IP负载部分起始地址。
     * iph->ihl单位是4字节，所以要乘以4。
     */
    ip_len = iph->ihl * 4;
    if (!SIPFW_EnsurePacketData(skb, ip_len, iph->protocol))
    {
        goto EXITSIPFW_IsMatch;
    }

    iph = ip_hdr(skb);
    if (!iph)
    {
        goto EXITSIPFW_IsMatch;
    }

    p = (unsigned char *)iph + ip_len;

    DBGPRINT("source IP:%x,dest:%x\n", iph->saddr, iph->daddr);

    rcu_read_lock();
    snapshot = rcu_dereference(l->snapshot);
    if (!snapshot)
    {
        goto out_rcu;
    }

    for (i = 0; i < snapshot->count; i++)
    {
        r = &snapshot->rules[i];

        if (SIPFW_IsIfnameMatch(r, state) &&
            SIPFW_IsIPMatch(iph, r) &&
            SIPFW_IsAddtionMatch(iph, p, r))
        {
            *matched = *r;
            matched->next = NULL;
            found = 1;
            break;
        }
    }

out_rcu:
    rcu_read_unlock();

EXITSIPFW_IsMatch:
    return found;
}


/*
 * SIPFW_ListDestroy
 *
 * 功能：
 *   销毁全部规则链，并释放规则节点占用的内存。
 *
 * 返回值：
 *   成功返回0。
 *
 * 注意：
 *   1. 原释放逻辑会漏掉每条链中的最后一个节点。
 *   2. prev在每条链开始前没有重新置NULL，清理多条链时容易出问题。
 *   3. 如果规则链可能被并发访问，这里需要加写锁或在模块退出前确保没有并发读者。
 *   4. 更稳妥的写法是使用next临时变量逐个释放节点。
 */
int SIPFW_ListDestroy(void)
{
    struct sipfw_list *l = NULL;
    struct sipfw_rules *cur = NULL;
    struct sipfw_rules *next = NULL;
    int i;

    DBGPRINT("==>SIPFW_ListDestroy\n");

    for (i = 0; i < SIPFW_CHAIN_NUM; i++)
    {
        l = &sipfw_tables[i];
        write_lock_bh(&l->lock);
        cur = l->rule;
        l->rule = NULL;
        l->number = 0;
        SIPFW_PublishRuleSnapshotLocked(l, NULL);
        write_unlock_bh(&l->lock);

        while (cur != NULL)
        {
            next = cur->next;
            cur->next = NULL;
            kfree(cur);
            cur = next;
        }
    }

    DBGPRINT("<==SIPFW_ListDestroy\n");
    return 0;
}
