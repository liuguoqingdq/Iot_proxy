#ifndef __KERNEL__
#define __KERNEL__
#endif

#ifndef MODULE
#define MODULE
#endif

#include "sipfw.h"
#include "sipfw_para.h"

/*
 * 模块描述信息。
 * MODULE_DESCRIPTION：模块说明。
 * MODULE_AUTHOR：模块作者。
 */
MODULE_DESCRIPTION("Simple IP FireWall module ");
MODULE_AUTHOR("songjingbin<flyingfat@163.com>");

static unsigned int SIPFW_ApplyChain(struct sk_buff *skb,
                                     const struct nf_hook_state *state,
                                     int chain)
{
    struct sipfw_rules matched_rule;
    int found = 0;

    if (cf.Invalid)
    {
        return NF_ACCEPT;
    }

    found = SIPFW_IsMatch(skb, state, &sipfw_tables[chain], &matched_rule);
    if (found)
    {
        SIPFW_LogAppend(skb, &matched_rule);
        cf.HitNumber++;
        return matched_rule.action;
    }

    return cf.DefaultAction;
}


/*
 * 本机接收数据包处理函数。
 *
 * 该函数挂载在 NF_INET_LOCAL_IN hook点。
 * 当外部数据包经过路由判断后，发现目标地址是本机时，
 * 数据包会进入 LOCAL_IN 路径，然后触发该函数。
 *
 * 对应iptables里的INPUT链。
 */
static unsigned int
SIPFW_HookLocalIn(void *hook,
    struct sk_buff *pskb,
    const struct nf_hook_state *state)
{
    DBGPRINT("==>SIPFW_HookLocalIn\n");
    DBGPRINT("<==SIPFW_HookLocalIn\n");
    return SIPFW_ApplyChain(pskb, state, SIPFW_CHAIN_INPUT);
}


/*
 * 本机发送数据包处理函数。
 *
 * 该函数挂载在 NF_INET_LOCAL_OUT hook点。
 * 当本机进程主动发送数据包时，会触发该函数。
 *
 * 对应iptables里的OUTPUT链。
 */
static unsigned int
SIPFW_HookLocaOut(void *hook,
    struct sk_buff *pskb,
    const struct nf_hook_state *state)
{
    DBGPRINT("==>SIPFW_HookLocaOut\n");
    DBGPRINT("<==SIPFW_HookLocaOut\n");
    return SIPFW_ApplyChain(pskb, state, SIPFW_CHAIN_OUTPUT);
}


/*
 * 转发数据包处理函数。
 *
 * 该函数挂载在 NF_INET_FORWARD hook点。
 * 当Linux主机作为路由器/网关时，
 * 目标地址不是本机、需要转发的数据包会经过该hook。
 *
 * 对应iptables里的FORWARD链。
 */
static unsigned int
SIPFW_HookForward(void *hook,
    struct sk_buff *pskb,
    const struct nf_hook_state *state)
{
    DBGPRINT("==>SIPFW_HookForward\n");

    if (state && state->in && state->out)
    {
        DBGPRINT("in device:%s,out device:%s\n",
                 state->in->name,
                 state->out->name);
    }

    DBGPRINT("<==SIPFW_HookForward\n");
    return SIPFW_ApplyChain(pskb, state, SIPFW_CHAIN_FORWARD);
}


/*
 * PREROUTING数据包处理函数。
 *
 * 该函数挂载在 NF_INET_PRE_ROUTING hook点。
 * 外部数据包刚进入协议栈、还没有进行路由判断之前，会触发该函数。
 *
 * 常见用途：
 *   DNAT
 *   早期过滤
 *   连接跟踪
 *
 * 对应iptables里的PREROUTING链。
 */
static unsigned int
SIPFW_HookPreRouting(void *hook,
    struct sk_buff *pskb,
    const struct nf_hook_state *state)
{
    DBGPRINT("==>SIPFW_HookPreRouting\n");
    DBGPRINT("<==SIPFW_HookPreRouting\n");
    return SIPFW_ApplyChain(pskb, state, SIPFW_CHAIN_OUTPUT);
}


/*
 * POSTROUTING数据包处理函数。
 *
 * 该函数挂载在 NF_INET_POST_ROUTING hook点。
 * 数据包已经完成路由判断，即将从网卡发出之前，会触发该函数。
 *
 * 常见用途：
 *   SNAT
 *   MASQUERADE
 *   出口包修改
 *
 * 对应iptables里的POSTROUTING链。
 */
static unsigned int
SIPFW_HookPostRouting(void *hook,
    struct sk_buff *pskb,
    const struct nf_hook_state *state)
{
    DBGPRINT("==>SIPFW_HookPostRouting\n");
    DBGPRINT("<==SIPFW_HookPostRouting\n");
    return SIPFW_ApplyChain(pskb, state, SIPFW_CHAIN_OUTPUT);
}


/*
 * Netfilter hook挂载结构数组。
 *
 * 每一个nf_hook_ops对象表示向Netfilter注册一个hook函数。
 *
 * 字段说明：
 *   hook     ：回调函数
 *   pf       ：协议族，这里是PF_INET，即IPv4
 *   hooknum  ：挂载到哪个Netfilter hook点
 *   priority ：优先级，数值越小，执行越早
 */
static struct nf_hook_ops sipfw_hooks[] = {
    {
        .hook     = SIPFW_HookLocalIn,       /* 本机接收数据包处理函数 */
        .pf       = PF_INET,                 /* IPv4协议族 */
        .hooknum  = NF_INET_LOCAL_IN,        /* LOCAL_IN hook点，对应INPUT路径 */
        .priority = NF_IP_PRI_FILTER - 1,    /* 优先级，略早于FILTER */
    },

    {
        .hook     = SIPFW_HookLocaOut,       /* 本机发送数据包处理函数 */
        .pf       = PF_INET,                 /* IPv4协议族 */
        .hooknum  = NF_INET_LOCAL_OUT,       /* LOCAL_OUT hook点，对应OUTPUT路径 */
        .priority = NF_IP_PRI_FILTER - 1,    /* 优先级 */
    },

    {
        .hook     = SIPFW_HookForward,       /* 转发数据包处理函数 */
        .pf       = PF_INET,                 /* IPv4协议族 */
        .hooknum  = NF_INET_FORWARD,         /* FORWARD hook点 */
        .priority = NF_IP_PRI_FILTER,        /* FILTER优先级 */
    },

};


/*
 * 模块初始化函数。
 *
 * insmod加载模块时会调用该函数。
 *
 * 初始化流程：
 *   1.读取防火墙配置文件
 *   2.创建Netlink套接字，用于用户态和内核态通信
 *   3.创建/proc接口
 *   4.注册Netfilter hook函数
 */
static int __init SIPFW_Init(void)
{
    int ret = -1;
    int i = 0;

    DBGPRINT("==>SIPFW_Init\n");

    for (i = 0; i < SIPFW_CHAIN_NUM; i++)
    {
        sipfw_tables[i].rule = NULL;
        sipfw_tables[i].number = 0;
        RCU_INIT_POINTER(sipfw_tables[i].snapshot, NULL);
        rwlock_init(&sipfw_tables[i].lock);
    }

    /*
     * 读取防火墙配置文件。
     * 例如读取/etc/sipfw.rules等。
     *
     * 注意：
     * 原代码没有判断SIPFW_HandleConf返回值，
     * 如果读取配置失败，后面仍会继续执行。
     */
    ret = SIPFW_HandleConf();
    if (ret)
    {
        DBGPRINT("load /etc/sipfw.conf failed, keep built-in defaults\n");
        ret = 0;
    }

    ret = SIPFW_LoadRuleFile();
    if (ret)
    {
        DBGPRINT("load rules file failed, keep runtime tables empty\n");
        ret = 0;
    }

    /*
     * 创建Netlink socket。
     * 用于内核模块和用户态控制程序通信，
     * 比如用户态下发添加/删除/查看规则命令。
     */
    ret = SIPFW_NLCreate();
    if (ret)
    {
        goto error1;
    }

    /*
     * 初始化/proc文件接口。
     * 用户可以通过/proc查看或控制部分防火墙状态。
     */
    ret = SIPFW_Proc_Init();
    if (ret)
    {
        goto error2;
    }

    /*
     * 注册Netfilter hook数组。
     *
     * nf_register_net_hooks会把sipfw_hooks里的多个hook函数
     * 一次性注册到init_net网络命名空间。
     */
    ret = nf_register_net_hooks(&init_net,
                                sipfw_hooks,
                                ARRAY_SIZE(sipfw_hooks));
    if (ret)
    {
        goto error3;
    }

    /*
     * 注意：
     * 这里原代码写成 goto error1;
     * 这会导致即使初始化成功，也跳转到error1并返回ret。
     *
     * 如果ret此时为0，虽然表面上返回成功，
     * 但这个写法非常不清晰。
     *
     * 更规范的写法应该是：
     *
     * DBGPRINT("<==SIPFW_Init\n");
     * return 0;
     */
    DBGPRINT("<==SIPFW_Init\n");
    return 0;

error3:
    /*
     * 如果Netfilter hook注册失败，
     * 需要清理/proc接口。
     */
    SIPFW_Proc_CleanUp();

error2:
    /*
     * 如果/proc初始化失败，
     * 需要销毁Netlink socket。
     */
    SIPFW_NLDestory();

error1:
    DBGPRINT("<==SIPFW_Init\n");
    return ret;
}


/*
 * 模块退出函数。
 *
 * rmmod卸载模块时会调用该函数。
 *
 * 清理流程：
 *   1.销毁Netlink socket
 *   2.销毁规则链表
 *   3.清理/proc接口
 *   4.注销Netfilter hook
 */
static void __exit SIPFW_Exit(void)
{
    DBGPRINT("==>SIPFW_Exit\n");

    nf_unregister_net_hooks(&init_net,
                            sipfw_hooks,
                            ARRAY_SIZE(sipfw_hooks));

    SIPFW_NLDestory();
    SIPFW_Proc_CleanUp();
    SIPFW_ListDestroy();
    rcu_barrier();

    DBGPRINT("module sipfw exit\n");

    DBGPRINT("<==SIPFW_Exit\n");
}


/*
 * 指定模块加载和卸载入口。
 */
module_init(SIPFW_Init);
module_exit(SIPFW_Exit);

/*
 * 模块许可证。
 */
MODULE_LICENSE("Dual BSD/GPL");
