#ifndef __SIPFW_H__
#define __SIPFW_H__

#ifdef __KERNEL__
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/netfilter_ipv4.h>
#include <linux/inet.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/netlink.h>
#include <linux/spinlock.h>
#include <linux/semaphore.h>
#include <net/sock.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/icmp.h>
#include <linux/igmp.h>
#include <linux/ctype.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <asm/uaccess.h>

#ifdef SIPFW_DEBUG
#define DBGPRINT printk
#else /*SIPFW_DEBUG*/
#define DBGPRINT(...) do { } while (0)
#endif /*SIPFW_DEBUG*/
struct sipfw_conf {

	/* server configuration */
	__u32	DefaultAction;
	__u8	RuleFilePath[256];
	__u8	LogFilePath[256];
	int		HitNumber;
	int		Invalid;
	int		LogPause;
};
struct tma 
{
	int sec;         /* seconds */
	int min;         /* minutes */
	int hour;        /* hours */
	int mday;        /* day of the month */
	int mon;         /* month */
	int year;        /* year */
};


#else /*__KERNEL__*/

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <linux/types.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <asm/types.h>
#include <linux/netlink.h>
#include <signal.h>
#include <getopt.h>
#ifdef SIPFW_DEBUG
#define DBGPRINT printf
#else /*SIPFW_DEBUG*/
#define DBGPRINT(...) do { } while (0)
#endif /*SIPFW_DEBUG*/

#endif /*__KERNEL__*/

#define SIPFW_MSG_PID		0
#define SIPFW_MSG_RULE		1
#define SIPFW_MSG_CLOSE	2
#define SIPFW_MSG_FAILURE	3
#define SIPFW_MSG_SUCCESS	4
#define NL_SIPFW      31
struct icgmp_flag		/*ICMP/IGMP*/
{
	__u8	valid;		//有效性
	__u8	type;		//类型
	__u8	code;		//代码
};

struct tcp_flag			//TCP选项
{
	__u16	res1:4,
			doff:4,
			fin:1,
			syn:1, //建立连接
			rst:1, //重制
			psh:1,
			ack:1, //响应
			urg:1,
			ece:1,
			cwr:1;
	__u8	valid;		//有效
};
//附加项，用来表示TCP、ICMP、IGMP中比较细节的规则
union addtion				
{
	__u32 			valid; //附加项是否有效
	struct icgmp_flag 	icgmp; //ICMP和IGMP的类型和代码
	struct tcp_flag	tcp; //TCP的状态
};
struct sipfw_rules{
	int		chain;				//链
	__be32	source;				//源ip地址
	__be32	dest;				//目的ip地址

	__be16	sport;				//源端口
	__be16	dport;				//目的端口
	__u8	protocol;			//协议类型
	int		action;				//规则的动作
	__u8	ifname[8];			//规则指定的网络接口
	union addtion addtion;
#ifdef __KERNEL__
	struct sipfw_rules* next;		//下一个规则
#endif
};
// 规则操作命令、规则动作类型、规则链类型、规则选项类型
enum {
    SIPFW_CMD_INSERT = 0,      /* 向规则链中插入新规则 */
    SIPFW_CMD_DELETE,          /* 从规则链中删除某条规则 */
    SIPFW_CMD_APPEND,          /* 将新规则追加到规则链末尾 */
    SIPFW_CMD_LIST,            /* 列出规则链中的所有规则 */
    SIPFW_CMD_FLUSH,           /* 清空规则链 */
    SIPFW_CMD_REPLACE,         /* 替换某条规则 */
    SIPFW_CMD_NUM = 6,         /* 命令的个数 */

    /*
     * 规则匹配后的动作类型。
     * 这些动作基本对应Netfilter hook函数的返回值。
     */
    SIPFW_ACTION_DROP = 0,     /* 丢弃数据包，对应NF_DROP */
    SIPFW_ACTION_ACCEPT = 1,   /* 放行数据包，对应NF_ACCEPT */
    SIPFW_ACTION_STOLEN = 2,   /* 数据包被当前模块接管，对应NF_STOLEN */
    SIPFW_ACTION_QUEUE = 3,    /* 将数据包送入用户态队列，对应NF_QUEUE */
    SIPFW_ACTION_REPEAT = 4,   /* 重新执行当前hook，对应NF_REPEAT */
    SIPFW_ACTION_STOP = 5,     /* 停止继续匹配后续规则，对应NF_STOP */

    /*
     * 注意：这里写成2可能有问题。
     * 如果动作包含DROP和ACCEPT两个动作，那ACTION_NUM=2可以理解；
     * 但上面实际定义了DROP/ACCEPT/STOLEN/QUEUE/REPEAT/STOP共6种动作，
     * 所以更合理的值应该是6。
     */
    SIPFW_ACTION_NUM = 6,

    /*
     * 规则所属的链。
     * SIPFW只定义了INPUT、OUTPUT、FORWARD三条链。
     */
    SIPFW_CHAIN_INPUT = 0,     /* INPUT链，处理发往本机的数据包 */
    SIPFW_CHAIN_OUTPUT,        /* OUTPUT链，处理本机发出的数据包 */
    SIPFW_CHAIN_FORWARD,       /* FORWARD链，处理需要转发的数据包 */
    SIPFW_CHAIN_NUM,           /* 链的个数 */
    SIPFW_CHAIN_ALL = 3,       /* 所有链 */

    /*
     * 规则选项类型。
     * 用于表示一条规则中具体设置了哪些匹配条件或动作。
     */
    SIPFW_OPT_CHAIN,           /* 链选项 */
    SIPFW_OPT_IP,              /* IP选项 */
    SIPFW_OPT_PORT,            /* 端口选项 */
    SIPFW_OPT_PROTOCOL,        /* 协议选项 */
    SIPFW_OPT_STR,             /* 字符串选项 */
    SIPFW_OPT_ACTION           /* 动作选项 */
};

typedef struct vec {
    void *ptr;                 /* 指向数据的指针，常用于字符串或其他参数 */
    unsigned long len;         /* 数据长度 */
    int value;                 /* 整型数值参数 */
} vec;

//通过联合体保存所有类型值
union sipfw_variant {				
	char			v_str[8];			//字符串变量类型
	int			v_int;				//整数类型
	unsigned int	v_uint;				//无符号整数类型
	struct vec	v_vec;				//
};
struct sipfw_cmd_opts {
	union sipfw_variant	command;	//命令
	union sipfw_variant	source;		//源地址
	union sipfw_variant	dest;		//目的地址
	union sipfw_variant	sport;		//源端口
	union sipfw_variant	dport;		//目的端口
	union sipfw_variant	protocol;	//协议
	union sipfw_variant	chain;		//链
	union sipfw_variant	ifname;		//网络接口
	union sipfw_variant	action;		//动作
	union addtion 		addtion;		//附加项
	union sipfw_variant	number;		//增加或者删除的序号
};

	
#ifdef __KERNEL__
struct sipfw_rule_snapshot
{
	struct rcu_head			rcu;
	unsigned int			count;
	struct sipfw_rules		rules[];
};

//链中的规则
struct sipfw_list
{
	struct sipfw_rules		*rule;//链中规则的头指针
	struct sipfw_rule_snapshot __rcu *snapshot;//热路径使用的RCU快照
	rwlock_t 				lock;//读写锁
	int 					number;//链中规则的个数
};
extern struct sipfw_conf cf;
extern struct sipfw_list sipfw_tables[];
extern const vec sipfw_protocol_name[];
extern const vec sipfw_command_name[] ;
extern const vec sipfw_chain_name[] ;
extern const vec sipfw_action_name[] ;
extern struct file *SIPFW_OpenFile(const char *filename, int flags, int mode);
extern ssize_t SIPFW_ReadLine(struct file *f, char *buf, size_t len);
extern ssize_t SIPFW_WriteLine(struct file *f, char *buf, size_t len);
extern void SIPFW_CloseFile(struct file *f);
extern int SIPFW_LogAppend(struct sk_buff *skb, struct sipfw_rules *r);
int SIPFW_HandleConf(void);


extern int SIPFW_Proc_Init( void );
extern void SIPFW_Proc_CleanUp( void );

extern int SIPFW_IsMatch(struct sk_buff *skb,
				 const struct nf_hook_state *state,
				 const struct sipfw_list *l,
				 struct sipfw_rules *matched);
extern void SIPFW_Localtime(struct tma *r, unsigned long time);
extern struct sipfw_rule_snapshot *SIPFW_AllocRuleSnapshot(unsigned int count,
						      gfp_t gfp);
extern void SIPFW_PublishRuleSnapshotLocked(struct sipfw_list *l,
					      struct sipfw_rule_snapshot *snapshot);
extern int SIPFW_LoadRuleFile(void);
extern int SIPFW_SaveRuleFile(void);

extern int SIPFW_ListDestroy(void);
extern int SIPFW_NLCreate(void);
extern int SIPFW_NLDestory(void);




#else /*__KERNEL__*/

#endif /*__KERNEL__*/


#endif
