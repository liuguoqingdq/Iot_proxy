#ifndef __SIPFW_PARA_H__
#define __SIPFW_PARA_H__

/*
 * 防火墙规则链名称表。
 * 每一项使用 vec 结构保存：
 *   ptr   ：字符串指针
 *   len   ：字符串长度
 *   value ：附加整数值，这里暂时未使用，置为0
 *
 * 该数组用于把用户输入的链名字符串转换成内部链编号，
 * 例如 "INPUT" 对应 SIPFW_CHAIN_INPUT。
 */
const vec sipfw_chain_name[] = {
    {"INPUT",   5, 0},        /* INPUT链，处理发往本机的数据包 */
    {"OUTPUT",  6, 0},        /* OUTPUT链，处理本机发出的数据包 */
    {"FORWARD", 7, 0},        /* FORWARD链，处理需要转发的数据包 */
    {NULL,      0, 0}         /* 表结束标记 */
};


/*
 * 防火墙规则动作名称表。
 * 用于把用户输入的动作字符串转换成内部动作编号。
 *
 * 例如：
 *   "DROP"   -> SIPFW_ACTION_DROP
 *   "ACCEPT" -> SIPFW_ACTION_ACCEPT
 *
 * 注意：
 *   这里的数组下标需要和前面 enum 中 SIPFW_ACTION_* 的编号保持一致。
 */
const vec sipfw_action_name[] = {
    {"DROP",   4, 0},         /* DROP动作，丢弃数据包，对应NF_DROP */
    {"ACCEPT", 6, 0},         /* ACCEPT动作，放行数据包，对应NF_ACCEPT */
    {"STOLEN", 6, 0},         /* STOLEN动作，数据包由当前模块接管，对应NF_STOLEN */

    /*
     * 注意：QUEUE的字符串长度应为5，不是6。
     * 原代码中写成 {"QUEUE", 6, 0} 可能存在问题。
     */
    {"QUEUE",  5, 0},         /* QUEUE动作，将数据包送入用户态队列，对应NF_QUEUE */

    {"REPEAT", 6, 0},         /* REPEAT动作，重新执行当前hook，对应NF_REPEAT */
    {"STOP",   4, 0},         /* STOP动作，停止继续执行后续hook */

    {NULL,    0, 0}           /* 表结束标记 */
};


/*
 * 防火墙命令名称表。
 * 用于把用户态输入的命令字符串转换成内部命令编号。
 *
 * 例如：
 *   "INSERT" -> SIPFW_CMD_INSERT
 *   "DELETE" -> SIPFW_CMD_DELETE
 */
const vec sipfw_command_name[] = {
    {"INSERT", 6, 0},         /* 插入规则，在指定位置插入一条新规则 */
    {"DELETE", 6, 0},         /* 删除规则，从规则链中删除某条规则 */
    {"APPEND", 6, 0},         /* 追加规则，将新规则追加到规则链末尾 */
    {"LIST",   4, 0},         /* 列出规则，显示当前规则链中的规则 */
    {"FLUSH",  5, 0},         /* 清空规则，删除规则链中的所有规则 */
    {"REPLACE", 7, 0},        /* 替换指定位置的一条规则 */

    {NULL,     0, 0}          /* 表结束标记 */
};


/*
 * 协议名称表。
 * 用于把用户输入的协议字符串转换成内核中的协议号。
 *
 * 例如：
 *   "tcp"  -> IPPROTO_TCP
 *   "udp"  -> IPPROTO_UDP
 *   "icmp" -> IPPROTO_ICMP
 */
const vec sipfw_protocol_name[] = {
    {"tcp",  3, IPPROTO_TCP},     /* TCP协议 */
    {"udp",  3, IPPROTO_UDP},     /* UDP协议 */
    {"icmp", 4, IPPROTO_ICMP},    /* ICMP协议 */
    {"igmp", 4, IPPROTO_IGMP},    /* IGMP协议 */
    {NULL,   0, 0}                /* 表结束标记 */
};


#ifdef __KERNEL__

/*
 * SIPFW全局配置结构。
 *
 * 字段含义需要结合 struct sipfw_conf 的定义来看，
 * 从初始化值大致可以推测：
 *
 *   SIPFW_ACTION_ACCEPT ：默认动作，表示默认放行
 *   "/etc/sipfw.rules"  ：规则文件路径
 *   "/etc/sipfw.log"    ：日志文件路径
 *   后面的0             ：其他配置项默认初始化为0
 */
struct sipfw_conf cf = {
    SIPFW_ACTION_ACCEPT,
    "/etc/sipfw.rules",
    "/etc/sipfw.log",
    0,
    0,
    0
};


/*
 * 防火墙规则表。
 *
 * sipfw_tables 是一个规则链数组，
 * 数组大小为 SIPFW_CHAIN_NUM。
 *
 * 如果前面定义了：
 *   SIPFW_CHAIN_INPUT
 *   SIPFW_CHAIN_OUTPUT
 *   SIPFW_CHAIN_FORWARD
 *
 * 那么这里通常对应：
 *   sipfw_tables[0] -> INPUT链规则表
 *   sipfw_tables[1] -> OUTPUT链规则表
 *   sipfw_tables[2] -> FORWARD链规则表
 */
struct sipfw_list sipfw_tables[SIPFW_CHAIN_NUM];

#endif /* __KERNEL__ */

#endif /* __SIPFW_PARA_H__ */
