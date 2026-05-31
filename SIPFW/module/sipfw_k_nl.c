#ifndef __KERNEL__
#define __KERNEL__
#endif /* __KERNEL__ */

#ifndef MODULE
#define MODULE
#endif /* MODULE */

#include "sipfw.h"

/*
 * nlfd：内核侧Netlink套接字指针。
 *
 * 用户态程序通过Netlink向内核模块发送命令，内核模块也通过这个socket
 * 把处理结果或者规则列表返回给用户态。
 */
static struct sock *nlfd = NULL;

static void SIPFW_NLRecv(struct sk_buff *skb);


/*
 * SIPFW_NLSendToUser
 *
 * 功能：
 *   通过Netlink把内核中的数据发送给用户态程序。
 *
 * 参数：
 *   to   ：用户态发来的Netlink消息头，里面包含用户态进程的nlmsg_pid。
 *   data ：需要发送给用户态的数据缓冲区。
 *   len  ：data数据长度。
 *   type ：Netlink消息类型，例如SIPFW_MSG_RULE、SIPFW_MSG_SUCCESS等。
 *
 * 返回值：
 *   成功返回0。
 *
 * 注意：
 *   1. 这里使用GFP_ATOMIC申请skb，说明它可能被设计为在不能睡眠的上下文中使用。
 *   2. 当前代码没有检查alloc_skb()是否失败，实际工程中应该补充判断。
 *   3. 当前代码的alloc_skb(len, ...)空间偏小，后面又要放Netlink头部，
 *      更稳妥的写法应该申请NLMSG_SPACE(len)大小。
 */
static int SIPFW_NLSendToUser(struct nlmsghdr *to,
                              void *data,
                              int len,
                              int type)
{
    struct sk_buff *skb = NULL;
    struct nlmsghdr *nlmsgh = NULL;
    void *pos = NULL;
    int ret = 0;

    DBGPRINT("==>SIPFW_NLSendToUser\n");

    /*
     * 参数检查。
     */
    if (!to || !data || len <= 0)
    {
        return -EINVAL;
    }

    /*
     * 申请Netlink消息缓冲区。
     * NLMSG_SPACE(len)包含Netlink头部、payload和对齐空间。
     */
    skb = alloc_skb(NLMSG_SPACE(len), GFP_ATOMIC);
    if (!skb)
    {
        return -ENOMEM;
    }

    /*
     * 构造Netlink消息头。
     * 第五个参数传payload长度len即可，不要传NLMSG_SPACE(len)减头部。
     */
    nlmsgh = __nlmsg_put(skb, 0, 0, type, len, 0);
    if (!nlmsgh)
    {
        kfree_skb(skb);
        return -ENOMEM;
    }

    /*
     * 获取payload地址。
     */
    pos = NLMSG_DATA(nlmsgh);

    /*
     * 拷贝业务数据到Netlink payload中。
     */
    memcpy(pos, data, len);

    /*
     * 设置为单播发送。
     */
    NETLINK_CB(skb).dst_group = 0;

    /*
     * 发送给用户态。
     * netlink_unicast会接管skb，调用后不要再手动kfree_skb(skb)。
     */
    ret = netlink_unicast(nlfd, skb, to->nlmsg_pid, MSG_DONTWAIT);

    DBGPRINT("<==SIPFW_NLSendToUser\n");

    return ret < 0 ? ret : 0;
}


/*
 * SIPFW_NLAction_RuleList
 *
 * 功能：
 *   处理“查看规则列表”的Netlink命令。
 *   可以查看全部链，也可以只查看指定链。
 *
 * 参数：
 *   rule ：用户态传入的规则结构，主要使用其中的chain字段判断要查看哪条链。
 *   to   ：用户态发来的Netlink消息头，用于回包。
 *
 * 返回值：
 *   成功返回0。
 *
 * 发送流程：
 *   1. 先计算需要返回多少条规则。
 *   2. 先把规则数量count发给用户态。
 *   3. 再逐条遍历链表，把每条规则发给用户态。
 */
static int SIPFW_NLAction_RuleList(struct sipfw_rules *rule, struct nlmsghdr *to)
{
    int start = 0;
    int end = 0;
    int i = 0;

    unsigned int count = 0;
    unsigned int copied = 0;

    struct sipfw_rules *snapshot = NULL;
    struct sipfw_rules *cur = NULL;

    DBGPRINT("==>SIPFW_NLAction_RuleList\n");

    if (!rule || !to)
    {
        return -EINVAL;
    }

    /*
     * 确定要遍历哪些链。
     */
    if (rule->chain == SIPFW_CHAIN_ALL)
    {
        start = 0;
        end = SIPFW_CHAIN_NUM;
    }
    else
    {
        /*
         * 防止rule->chain非法导致sipfw_tables数组越界。
         */
        if (rule->chain < 0 || rule->chain >= SIPFW_CHAIN_NUM)
        {
            kfree(rule);
            return -EINVAL;
        }

        start = rule->chain;
        end = rule->chain + 1;
    }

    /*
     * 第一次加读锁：统计规则数量。
     * 注意：读取l->number也应该加锁，因为写线程会修改它。
     */
    for (i = start; i < end; i++)
    {
        struct sipfw_list *l = &sipfw_tables[i];

        read_lock_bh(&l->lock);
        count += l->number;
        read_unlock_bh(&l->lock);
    }

    /*
     * 如果没有规则，直接返回数量0。
     */
    if (count == 0)
    {
        SIPFW_NLSendToUser(to, &count, sizeof(count), SIPFW_MSG_RULE);
        kfree(rule);

        DBGPRINT("<==SIPFW_NLAction_RuleList\n");
        return 0;
    }

    /*
     * 在锁外申请快照数组。
     * 不要在持有rwlock_t时做GFP_KERNEL内存申请。
     */
    snapshot = kcalloc(count, sizeof(*snapshot), GFP_KERNEL);
    if (!snapshot)
    {
        kfree(rule);
        return -ENOMEM;
    }

    /*
     * 第二次加读锁：复制规则快照。
     * 复制动作必须在读锁里面完成，防止cur被删除。
     */
    for (i = start; i < end; i++)
    {
        struct sipfw_list *l = &sipfw_tables[i];

        read_lock_bh(&l->lock);

        for (cur = l->rule; cur != NULL && copied < count; cur = cur->next)
        {
            snapshot[copied] = *cur;

            /*
             * next是内核链表指针，不应该暴露给用户态。
             * 发给用户态前把它置空更干净。
             */
            snapshot[copied].next = NULL;

            copied++;
        }

        read_unlock_bh(&l->lock);
    }

    /*
     * rule是SIPFW_NLDoAction中kmalloc出来的临时结构。
     */
    kfree(rule);

    /*
     * 注意：以实际复制出来的copied为准。
     * 因为统计count和复制snapshot之间，规则数量可能变化。
     */
    SIPFW_NLSendToUser(to, &copied, sizeof(copied), SIPFW_MSG_RULE);

    /*
     * 锁外发送规则快照。
     * 这里不再访问原链表节点，所以不会受删除操作影响。
     */
    for (i = 0; i < copied; i++)
    {
        SIPFW_NLSendToUser(to,
                           &snapshot[i],
                           sizeof(snapshot[i]),
                           SIPFW_MSG_RULE);
    }

    kfree(snapshot);

    DBGPRINT("<==SIPFW_NLAction_RuleList\n");
    return 0;
}

static int SIPFW_PrepareRuleSnapshotLocked(struct sipfw_list *l,
                                           unsigned int new_count,
                                           struct sipfw_rule_snapshot **snapshot)
{
    if (!l || !snapshot)
    {
        return -EINVAL;
    }

    *snapshot = NULL;
    if (new_count == 0)
    {
        return 0;
    }

    *snapshot = SIPFW_AllocRuleSnapshot(new_count, GFP_ATOMIC);
    return *snapshot ? 0 : -ENOMEM;
}

static void SIPFW_CommitRuleUpdateLocked(struct sipfw_list *l,
                                         unsigned int new_count,
                                         struct sipfw_rule_snapshot *snapshot)
{
    if (!l)
    {
        if (snapshot)
        {
            kfree(snapshot);
        }
        return;
    }

    l->number = new_count;
    SIPFW_PublishRuleSnapshotLocked(l, snapshot);
}


/*
 * SIPFW_NLAction_RuleAddpend
 *
 * 功能：
 *   把一条规则追加到指定链的尾部。
 *
 * 参数：
 *   rule：需要追加的新规则。
 *
 * 返回值：
 *   成功返回0。
 *
 * 注意：
 *   函数名Addpend看起来像是Append拼写错误，但不影响编译。
 */
static int SIPFW_NLAction_RuleAppend(struct sipfw_rules *rule)
{
    struct sipfw_list *l = NULL;
    struct sipfw_rules *cur = NULL;
    struct sipfw_rule_snapshot *snapshot = NULL;
    unsigned int new_count = 0;
    int ret = 0;

    DBGPRINT("==>SIPFW_NLAction_RuleAppend\n");

    if (!rule)
    {
        return -EINVAL;
    }

    if (rule->chain < 0 || rule->chain >= SIPFW_CHAIN_NUM)
    {
        kfree(rule);
        return -EINVAL;
    }

    l = &sipfw_tables[rule->chain];
    DBGPRINT("append to chain:%d==>%s,source:%x,dest:%x\n",
             rule->chain,
             (char *)sipfw_chain_name[rule->chain].ptr,
             rule->source,
             rule->dest);

    /*
     * 新规则作为链表节点加入时，最好明确把next置空。
     * 防止rule之前残留了旧next指针。
    */
    rule->next = NULL;
	write_lock_bh(&l->lock);
    new_count = l->number + 1;
    ret = SIPFW_PrepareRuleSnapshotLocked(l, new_count, &snapshot);
    if (ret)
    {
        write_unlock_bh(&l->lock);
        kfree(rule);
        return ret;
    }

    if (l->rule == NULL)
    {
        /*
         * 当前链为空，新规则直接作为头节点。
         */
        l->rule = rule;
    }
    else
    {
        /*
         * 当前链非空，找到最后一个节点。
         * 最后一个节点的特征是：cur->next == NULL。
         */
        cur = l->rule;
        while (cur->next != NULL)
        {
            cur = cur->next;
        }

        /*
         * 把新规则挂到尾节点后面。
         */
        cur->next = rule;
    }

    /*
     * 当前链规则数量加1。
     */
    SIPFW_CommitRuleUpdateLocked(l, new_count, snapshot);
	write_unlock_bh(&l->lock);
    DBGPRINT("<==SIPFW_NLAction_RuleAppend\n");
    return 0;
}


/*
 * SIPFW_NLAction_RuleDelete
 *
 * 功能：
 *   删除规则。
 *
 * 删除方式有两种：
 *   1. 如果number有效，则删除指定位置的规则。
 *   2. 如果number为-1，则按照rule中的字段匹配规则并删除。
 *
 * 参数：
 *   rule   ：用户态传入的规则描述。
 *   number ：要删除的规则位置。-1表示不按位置删除，而是按规则内容匹配删除。
 *
 * 返回值：
 *   成功返回0。
 */
static int SIPFW_NLAction_RuleDelete(struct sipfw_rules *rule, int number)
{
    struct sipfw_list *l = NULL;
    struct sipfw_rules **link = NULL;
    struct sipfw_rules *cur = NULL;
    struct sipfw_rules *victim = NULL;
    struct sipfw_rule_snapshot *snapshot = NULL;
    int idx = 1;
    int ret = 0;
    unsigned int new_count = 0;

    DBGPRINT("==>SIPFW_NLAction_RuleDelete\n");

    if (!rule)
    {
        return -EINVAL;
    }

    /*
     * 建议加上chain合法性检查，防止数组越界。
     * 如果你有宏，比如SIPFW_CHAIN_MAX，就把3替换成对应宏。
     */
    if (rule->chain < 0 || rule->chain >= SIPFW_CHAIN_NUM)
    {
        kfree(rule);
        return -EINVAL;
    }

    /*
     * 找到目标链。
     */
    l = &sipfw_tables[rule->chain];

    /*
     * 删除链表节点属于写操作，必须拿写锁。
     */


    if (number != -1)
    {
		write_lock_bh(&l->lock);
        /*
         * number有效：按位置删除。
         * 规则位置从1开始计数。
         */
        if (number <= 0 || number > l->number)
        {
            ret = -ENOENT;
            goto out_unlock;
        }

        /*
         * link指向“当前节点指针所在的位置”。
         * 一开始指向头指针l->rule。
         */
        link = &l->rule;

        /*
         * 找到第number个节点。
         */
        while (*link != NULL && idx < number)
        {
            link = &(*link)->next;
            idx++;
        }

        /*
         * 如果找到了目标节点，就摘链。
         */
        if (*link != NULL)
        {
            new_count = l->number - 1;
            ret = SIPFW_PrepareRuleSnapshotLocked(l, new_count, &snapshot);
            if (ret)
            {
                goto out_unlock;
            }

            victim = *link;
            *link = victim->next;
            victim->next = NULL;
            SIPFW_CommitRuleUpdateLocked(l, new_count, snapshot);
        }
        else
        {
            ret = -ENOENT;
        }
    }
    else
    {
		write_lock_bh(&l->lock);
        /*
         * number为-1：不按位置删除，按规则字段匹配删除。
         */
        link = &l->rule;

        while (*link != NULL)
        {
            cur = *link;

            if (cur->action == rule->action &&
                cur->chain == rule->chain &&
                cur->source == rule->source &&
                cur->dest == rule->dest &&
                cur->sport == rule->sport &&
                cur->dport == rule->dport &&
                cur->protocol == rule->protocol &&
                !memcmp(cur->ifname, rule->ifname, sizeof(cur->ifname)) &&
                !memcmp(&cur->addtion, &rule->addtion, sizeof(cur->addtion)))
            {
                new_count = l->number - 1;
                ret = SIPFW_PrepareRuleSnapshotLocked(l, new_count, &snapshot);
                if (ret)
                {
                    break;
                }

                /*
                 * 找到匹配节点后摘链。
                 *
                 * 如果cur是头节点，相当于：
                 *     l->rule = cur->next;
                 *
                 * 如果cur是中间或尾节点，相当于：
                 *     prev->next = cur->next;
                 */
                victim = cur;
                *link = cur->next;
                victim->next = NULL;
                SIPFW_CommitRuleUpdateLocked(l, new_count, snapshot);
                break;
            }

            /*
             * 移动到下一个“节点指针位置”。
             */
            link = &cur->next;
        }

        if (!victim && !ret)
        {
            ret = -ENOENT;
        }
    }

out_unlock:
    write_unlock_bh(&l->lock);

    /*
     * victim已经从链表摘掉了，可以在锁外释放。
     * 这样可以减少写锁持有时间。
     */
    if (victim)
    {
        kfree(victim);
    }

    /*
     * rule是SIPFW_NLDoAction中临时kmalloc出来的参数结构。
     * 无论删除是否成功，都只在这里释放一次。
     */
    kfree(rule);

    DBGPRINT("<==SIPFW_NLAction_RuleDelete\n");
    return ret;
}


/*
 * SIPFW_NLAction_RuleReplace
 *
 * 功能：
 *   替换指定位置的规则。
 *
 * 参数：
 *   rule   ：新规则。
 *   number ：要替换的位置，通常从1开始计数。
 *
 * 返回值：
 *   成功返回0。
 *
 * 注意：
 *   当前代码中else if(number > l->number)放在number != -1之后，
 *   逻辑上可能不合理。因为number > l->number时也满足number != -1，
 *   会先进入第一个分支，导致这个else if基本走不到。
 */
static int SIPFW_NLAction_RuleReplace(struct sipfw_rules *rule, int number)
{
    struct sipfw_list *l = NULL;
    struct sipfw_rules **link = NULL;
    struct sipfw_rules *old = NULL;
    struct sipfw_rule_snapshot *snapshot = NULL;
    int idx = 1;
    int ret = 0;
    unsigned int new_count = 0;

    DBGPRINT("==>SIPFW_NLAction_RuleReplace\n");

    if (!rule)
    {
        return -EINVAL;
    }

    /*
     * 检查chain是否合法，防止sipfw_tables数组越界。
     * 如果你有SIPFW_CHAIN_MAX宏，可以把3替换掉。
     */
    if (rule->chain < 0 || rule->chain >= SIPFW_CHAIN_NUM)
    {
        kfree(rule);
        return -EINVAL;
    }

    /*
     * replace必须指定有效位置。
     * number从1开始计数。
     */
    if (number <= 0)
    {
        kfree(rule);
        return -EINVAL;
    }

    l = &sipfw_tables[rule->chain];

    /*
     * 替换规则属于写操作，需要加写锁。
     */
    write_lock_bh(&l->lock);

    if (number > l->number)
    {
        ret = -ENOENT;
        goto out_unlock;
    }

    /*
     * link指向“当前节点指针所在的位置”。
     * 一开始指向链表头指针l->rule。
     */
    link = &l->rule;

    /*
     * 找到第number个节点。
     */
    while (*link != NULL && idx < number)
    {
        link = &(*link)->next;
        idx++;
    }

    /*
     * 没找到对应位置。
     */
    if (*link == NULL)
    {
        ret = -ENOENT;
        goto out_unlock;
    }

    new_count = l->number;
    ret = SIPFW_PrepareRuleSnapshotLocked(l, new_count, &snapshot);
    if (ret)
    {
        goto out_unlock;
    }

    /*
     * old是被替换的旧规则。
     */
    old = *link;

    /*
     * 新规则继承旧规则后面的链表。
     */
    rule->next = old->next;

    /*
     * 把当前位置替换成新规则。
     *
     * 如果替换头节点：
     *     *link等价于l->rule
     *
     * 如果替换中间或尾部节点：
     *     *link等价于prev->next
     */
    *link = rule;

    /*
     * 断开旧节点和链表的关系。
     */
    old->next = NULL;
    SIPFW_CommitRuleUpdateLocked(l, new_count, snapshot);

out_unlock:
    write_unlock_bh(&l->lock);

    /*
     * 替换成功：释放旧规则。
     * 替换失败：释放新规则。
     */
    if (old)
    {
        kfree(old);
    }
    else
    {
        kfree(rule);
    }

    DBGPRINT("<==SIPFW_NLAction_RuleReplace\n");
    return ret;
}

/*
 * SIPFW_NLAction_RuleInsert
 *
 * 功能：
 *   把新规则插入到指定链的某个位置。
 *
 * 参数：
 *   rule   ：新规则。
 *   number ：插入位置。number == 1表示插入头部。
 *
 * 返回值：
 *   成功返回0。
 *
 * 注意：
 *   1. 当前函数没有更新l->number，这会导致规则数量统计错误。
 *   2. number > l->number且链表为空时，prev仍然是NULL，prev->next会崩溃。
 *   3. 插入中间位置时，代码写成rule->next = cur->next，可能会跳过cur。
 *      如果语义是“插入到cur之前”，通常应该写rule->next = cur。
 */
static int SIPFW_NLAction_RuleInsert(struct sipfw_rules *rule, int number)
{
    struct sipfw_list *l = NULL;
    struct sipfw_rules **link = NULL;
    struct sipfw_rule_snapshot *snapshot = NULL;
    int idx = 1;
    int ret = 0;
    unsigned int new_count = 0;

    DBGPRINT("==>SIPFW_NLAction_RuleInsert\n");

    if (!rule)
    {
        return -EINVAL;
    }

    /*
     * 检查chain是否合法，防止sipfw_tables数组越界。
     * 如果你有SIPFW_CHAIN_MAX宏，可以把3替换掉。
     */
    if (rule->chain < 0 || rule->chain >= SIPFW_CHAIN_NUM)
    {
        kfree(rule);
        return -EINVAL;
    }

    /*
     * insert必须给出有效位置。
     * number从1开始计数。
     */
    if (number <= 0)
    {
        kfree(rule);
        return -EINVAL;
    }

    l = &sipfw_tables[rule->chain];

    /*
     * 插入规则属于写操作，需要加写锁。
     */
    write_lock_bh(&l->lock);
    new_count = l->number + 1;
    ret = SIPFW_PrepareRuleSnapshotLocked(l, new_count, &snapshot);
    if (ret)
    {
        goto out_unlock;
    }

    /*
     * link指向“当前节点指针所在的位置”。
     * 一开始指向链表头指针l->rule。
     */
    link = &l->rule;

    /*
     * 找到插入位置。
     *
     * 语义：
     *   number == 1：插入到头部
     *   number == 2：插入到原第2个节点前面
     *   number > l->number：插入到尾部
     */
    while (*link != NULL && idx < number)
    {
        link = &(*link)->next;
        idx++;
    }

    /*
     * 把新规则插入到link指向的位置。
     *
     * 如果插入头部：
     *     link等价于&l->rule
     *
     * 如果插入中间：
     *     link等价于&prev->next
     *
     * 如果插入尾部：
     *     *link此时是NULL
     */
    rule->next = *link;
    *link = rule;

    /*
     * 当前链规则数量加1。
     */
    SIPFW_CommitRuleUpdateLocked(l, new_count, snapshot);

out_unlock:
    write_unlock_bh(&l->lock);

    if (ret)
    {
        kfree(rule);
    }

    DBGPRINT("<==SIPFW_NLAction_RuleInsert\n");
    return ret;
}


/*
 * SIPFW_NLAction_RuleFlush
 *
 * 功能：
 *   清空规则链。
 *   可以清空全部链，也可以只清空指定链。
 *
 * 参数：
 *   rule：用户态传入的规则结构，主要使用其中的chain字段。
 *
 * 返回值：
 *   成功返回0。
 */
static int SIPFW_NLAction_RuleFlush(struct sipfw_rules *rule)
{
    struct sipfw_list *l = NULL;
    struct sipfw_rules *head = NULL;
    struct sipfw_rules *cur = NULL;
    struct sipfw_rules *next = NULL;
    int start = 0;
    int end = 0;
    int i = 0;

    DBGPRINT("==>SIPFW_NLAction_RuleFlush\n");

    if (!rule)
    {
        return -EINVAL;
    }

    /*
     * 确定要清空的链范围。
     */
    if (rule->chain == SIPFW_CHAIN_ALL)
    {
        /*
         * 清空全部三条链。
         */
        start = 0;
        end = SIPFW_CHAIN_NUM;
    }
    else
    {
        /*
         * 检查chain是否合法，防止数组越界。
         * 如果你有SIPFW_CHAIN_MAX宏，可以把3替换成对应宏。
         */
        if (rule->chain < 0 || rule->chain >= SIPFW_CHAIN_NUM)
        {
            kfree(rule);
            return -EINVAL;
        }

        /*
         * 只清空指定链。
         */
        start = rule->chain;
        end = rule->chain + 1;
    }

    for (i = start; i < end; i++)
    {
        l = &sipfw_tables[i];

        /*
         * 清空链表属于写操作，需要加写锁。
         *
         * 锁内只做三件事：
         *   1. 保存旧链表头；
         *   2. 把链表头置空；
         *   3. 把规则数量置0。
         */
        write_lock_bh(&l->lock);

        head = l->rule;
        l->rule = NULL;
        SIPFW_CommitRuleUpdateLocked(l, 0, NULL);

        write_unlock_bh(&l->lock);

        /*
         * 旧链表已经从sipfw_tables中摘下来了。
         * 后续新读者已经看不到这些节点，所以可以在锁外释放。
         */
        cur = head;
        while (cur != NULL)
        {
            next = cur->next;
            cur->next = NULL;
            kfree(cur);
            cur = next;
        }
    }

    /*
     * rule是SIPFW_NLDoAction中临时kmalloc出来的参数结构。
     */
    kfree(rule);

    DBGPRINT("<==SIPFW_NLAction_RuleFlush\n");
    return 0;
}


/*
 * SIPFW_NLDoAction
 *
 * 功能：
 *   解析用户态通过Netlink发来的命令，并调用对应的规则处理函数。
 *
 * 参数：
 *   payload ：Netlink消息的数据区，实际类型是struct sipfw_cmd_opts。
 *   nlmsgh  ：Netlink消息头，用于获取用户态pid并回包。
 *
 * 返回值：
 *   成功返回0。
 *
 * 工作流程：
 *   1. 把payload转换为sipfw_cmd_opts。
 *   2. 从cmd_opt里取出命令类型、链、地址、端口、协议、动作等字段。
 *   3. kmalloc一个sipfw_rules结构。
 *   4. 根据命令类型调用插入、删除、替换、追加、查看、清空等函数。
 *   5. 根据执行结果向用户态发送SUCCESS或FAILURE。
 */
static int SIPFW_NLDoAction(void *payload, struct nlmsghdr *nlmsgh)
{
    struct sipfw_cmd_opts *cmd_opt = NULL;
    struct sipfw_rules *rule = NULL;
    int cmd = -1;
    int number = -1;
    int err = 0;

    /*
     * 给用户态返回的成功和失败字符串。
     * 这里长度8包含字符串结尾的'\0'。
     */
    vec NLSUCCESS = {"SUCCESS", 8};
    vec NLFAILRE  = {"FAILURE", 8};

    DBGPRINT("==>SIPFW_NLDoAction\n");

    /*
     * 参数检查。
     */
    if (!payload || !nlmsgh)
    {
        return -EINVAL;
    }

    /*
     * 把Netlink payload解释为命令参数结构。
     */
    cmd_opt = (struct sipfw_cmd_opts *)payload;

    /*
     * 取出用户态请求的命令类型和规则序号。
     */
    cmd = cmd_opt->command.v_uint;
    number = cmd_opt->number.v_int;

    if ((cmd == SIPFW_CMD_APPEND ||
         cmd == SIPFW_CMD_INSERT ||
         cmd == SIPFW_CMD_REPLACE) &&
        (cmd_opt->action.v_int < 0 ||
         cmd_opt->action.v_int >= SIPFW_ACTION_NUM))
    {
        return -EINVAL;
    }

    /*
     * 为规则申请内存。
     * 使用kzalloc可以把结构体字段清零，避免未初始化字段带来问题。
     */
    rule = kzalloc(sizeof(*rule), GFP_KERNEL);
    if (!rule)
    {
        DBGPRINT("Malloc rule struct failure\n");
        return -ENOMEM;
    }

    /*
     * 根据用户态传入的命令参数填充规则字段。
     */
    rule->next = NULL;
    rule->chain = cmd_opt->chain.v_int;

    rule->source = cmd_opt->source.v_uint;

    /*
     * 原代码这里重复写了source。
     * 如果cmd_opt中存在dest字段，这里应该赋值给rule->dest。
     */
    rule->dest = cmd_opt->dest.v_uint;

    rule->sport = cmd_opt->sport.v_uint;
    rule->dport = cmd_opt->dport.v_uint;
    rule->protocol = cmd_opt->protocol.v_int > 0 ? cmd_opt->protocol.v_uint : 0;
    rule->action = cmd_opt->action.v_uint;
    memcpy(rule->ifname, cmd_opt->ifname.v_str, sizeof(rule->ifname));
    memcpy(&rule->addtion, &cmd_opt->addtion, sizeof(rule->addtion));

    /*
     * 根据用户态命令执行不同动作。
     *
     * 注意：
     * 下面这些SIPFW_NLAction_xxx函数内部会接管rule：
     *   INSERT成功后，rule进入链表；
     *   DELETE/LIST/FLUSH内部会释放临时rule；
     *   REPLACE成功后，rule进入链表；
     *   APPEND成功后，rule进入链表。
     *
     * 所以这里调用之后不要再统一kfree(rule)。
     */
    switch (cmd)
    {
    case SIPFW_CMD_INSERT:
        /*
         * 插入规则。
         */
        err = SIPFW_NLAction_RuleInsert(rule, number);
        break;

    case SIPFW_CMD_DELETE:
        /*
         * 删除规则。
         */
        err = SIPFW_NLAction_RuleDelete(rule, number);
        break;

    case SIPFW_CMD_REPLACE:
        /*
         * 替换规则。
         */
        err = SIPFW_NLAction_RuleReplace(rule, number);
        break;

    case SIPFW_CMD_APPEND:
        /*
         * 追加规则到链表尾部。
         *
         * 注意：
         * 你前面已经把Addpend改成Append的话，这里也要同步改函数名。
         */
        err = SIPFW_NLAction_RuleAppend(rule);
        break;

    case SIPFW_CMD_LIST:
        /*
         * 查看规则列表。
         * RuleList内部负责释放rule，并且会自己向用户态发送规则数量和规则内容。
         */
        err = SIPFW_NLAction_RuleList(rule, nlmsgh);
        if (!err)
        {
            DBGPRINT("<==SIPFW_NLDoAction\n");
            return 0;
        }
        break;

    case SIPFW_CMD_FLUSH:
        /*
         * 清空规则。
         */
        err = SIPFW_NLAction_RuleFlush(rule);
        break;

    default:
        /*
         * 未知命令。
         * rule没有被任何处理函数接管，所以这里必须释放。
         */
        kfree(rule);
        err = -EINVAL;
        break;
    }

    if (!err &&
        (cmd == SIPFW_CMD_INSERT ||
         cmd == SIPFW_CMD_DELETE ||
         cmd == SIPFW_CMD_REPLACE ||
         cmd == SIPFW_CMD_APPEND ||
         cmd == SIPFW_CMD_FLUSH))
    {
        err = SIPFW_SaveRuleFile();
    }

    /*
     * LIST命令已经在上面单独返回。
     * 其他命令统一返回SUCCESS或FAILURE。
     */
    if (!err)
    {
        SIPFW_NLSendToUser(nlmsgh,
                           NLSUCCESS.ptr,
                           NLSUCCESS.len,
                           SIPFW_MSG_SUCCESS);
    }
    else
    {
        SIPFW_NLSendToUser(nlmsgh,
                           NLFAILRE.ptr,
                           NLFAILRE.len,
                           SIPFW_MSG_FAILURE);
    }

    DBGPRINT("<==SIPFW_NLDoAction\n");
    return err;
}




/*
 * SIPFW_NLCreate
 *
 * 功能：
 *   创建内核侧Netlink socket。
 *
 * 返回值：
 *   成功返回0。
 *   失败返回-1。
 *
 * 注意：
 *   1. 这里使用的是旧接口：netlink_kernel_create(&init_net, 1, NULL)。
 *   2. 第三个参数传NULL意味着没有设置接收回调，这样用户态发来的消息
 *      可能无法被正常处理。
 *   3. 新内核中一般使用struct netlink_kernel_cfg配置.input回调。
 */
static void SIPFW_NLRecv(struct sk_buff *skb)
{
    struct nlmsghdr *nlmsgh = NULL;
    int remaining = 0;

    if (!skb)
    {
        return;
    }

    nlmsgh = nlmsg_hdr(skb);
    remaining = skb->len;

    while (NLMSG_OK(nlmsgh, remaining))
    {
        switch (nlmsgh->nlmsg_type)
        {
        case SIPFW_MSG_CLOSE:
            break;

        case SIPFW_MSG_PID:
            if (nlmsg_len(nlmsgh) < sizeof(struct sipfw_cmd_opts))
            {
                static char failure[] = "FAILURE";

                SIPFW_NLSendToUser(nlmsgh,
                                   failure,
                                   sizeof(failure),
                                   SIPFW_MSG_FAILURE);
                break;
            }

            SIPFW_NLDoAction(NLMSG_DATA(nlmsgh), nlmsgh);
            break;

        default:
            break;
        }

        nlmsgh = NLMSG_NEXT(nlmsgh, remaining);
    }
}

int SIPFW_NLCreate(void)
{
    struct netlink_kernel_cfg cfg = {
        .input = SIPFW_NLRecv,
    };

    nlfd = netlink_kernel_create(&init_net, NL_SIPFW, &cfg);
    if (!nlfd)
    {
        return -1;
    }

    return 0;
}


/*
 * SIPFW_NLDestory
 *
 * 功能：
 *   销毁Netlink socket。
 *
 * 返回值：
 *   返回0。
 *
 * 注意：
 *   1. 函数名Destory疑似拼写错误，通常应为Destroy。
 *   2. 当前代码调用sock_release(nlfd->sk_socket)，旧内核可能可用。
 *   3. 新内核中更推荐使用netlink_kernel_release(nlfd)。
 *   4. 释放后最好把nlfd置为NULL，避免悬空指针。
 */
int SIPFW_NLDestory(void)
{
    if (nlfd)
    {
        netlink_kernel_release(nlfd);
        nlfd = NULL;
    }

    return 0;
}
