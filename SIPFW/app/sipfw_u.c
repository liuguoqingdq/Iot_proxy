#include "sipfw.h"
#include "sipfw_para.h"

/*
 * union response用于接收内核返回的数据。
 *
 * 因为内核返回的数据类型不固定：
 * 1. info_str：普通提示信息，例如添加成功、删除成功等。
 * 2. rule：返回单条防火墙规则。
 * 3. count：返回规则数量，例如list命令先返回规则总数。
 *
 * 使用union可以让同一块内存根据不同消息类型解释为不同数据。
 */
union response
{
	char info_str[128];          // 内核返回的字符串提示信息
	struct sipfw_rules rule;     // 内核返回的一条防火墙规则
	struct sipfw_cmd_opts cmd;   // 用户态发送给内核的命令
	unsigned int count;          // 内核返回的规则数量
};

/*
 * packet_u表示一次Netlink通信的数据包。
 *
 * nlmsgh：Netlink消息头，用来描述消息长度、类型、发送者PID等。
 * payload：真正携带的数据内容，也就是用户态和内核态之间传输的业务数据。
 */
struct packet_u
{
	struct nlmsghdr nlmsgh;      // Netlink消息头
	union response  payload;     // Netlink消息负载
};

/*
 * 全局Netlink通信相关变量。
 *
 * message：发送和接收时共用的消息缓冲区。
 * nlsource：用户态本地Netlink地址。
 * nldest：目标Netlink地址，通常是内核。
 * nls：Netlink套接字文件描述符。
 */
struct packet_u message;
struct sockaddr_nl nlsource, nldest;
int nls = -1;


/*
 * SIGINT信号处理函数。
 *
 * 当用户按Ctrl+C终止程序时，该函数会被调用。
 * 它的主要作用是：
 * 1. 构造一个SIPFW_MSG_CLOSE类型的Netlink消息。
 * 2. 通知内核用户态程序即将退出。
 * 3. 关闭Netlink套接字。
 * 4. 退出当前进程。
 */
static void sig_int(int signo)
{
	DBGPRINT("==>sig_int\n");

	/*
	 * 清空目标地址结构。
	 * 这里目标是内核，所以nl_pid设置为0。
	 */
	memset(&nldest, 0, sizeof(nldest));
	nldest.nl_family = AF_NETLINK;
	nldest.nl_pid    = 0;          // 发送给内核
	nldest.nl_groups = 0;          // 不使用组播

	/*
	 * 清空消息结构，并构造关闭消息。
	 */
	memset(&message, 0, sizeof(message));

	/*
	 * NLMSG_LENGTH(0)表示只有Netlink消息头，没有额外负载。
	 */
	message.nlmsgh.nlmsg_len 	= NLMSG_LENGTH(0);
	message.nlmsgh.nlmsg_flags 	= 0;

	/*
	 * SIPFW_MSG_CLOSE用于告诉内核：
	 * 当前用户态控制程序准备关闭Netlink连接。
	 */
	message.nlmsgh.nlmsg_type 	= SIPFW_MSG_CLOSE;

	/*
	 * 当前进程PID。
	 * 内核可以用这个PID识别是哪一个用户进程发来的消息。
	 */
	message.nlmsgh.nlmsg_pid 	= getpid();

	/*
	 * 通过Netlink套接字把关闭消息发送给内核。
	 */
	sendto(nls,
	       &message,
	       message.nlmsgh.nlmsg_len,
	       0,
	       (struct sockaddr*)&nldest,
	       sizeof(nldest));

	/*
	 * 关闭Netlink套接字。
	 */
	close(nls);

	DBGPRINT("<==sig_int\n");

	/*
	 * 直接退出进程。
	 */
	_exit(0);
}


/*
 * SIPFW_DisplayOpts用于显示解析后的命令行参数。
 *
 * 这个函数主要用于调试。
 * 用户输入的命令会先被解析到struct sipfw_cmd_opts结构中，
 * 然后通过该函数打印出来，方便确认解析结果是否正确。
 */
int SIPFW_DisplayOpts(struct sipfw_cmd_opts *opts)
{
	DBGPRINT("==>SIPFW_DisplayOpts\n");

	if(opts)
	{
		struct in_addr source, dest;
		const char *command_name = "UNKNOWN";

		/*
		 * opts->source.v_uint和opts->dest.v_uint保存的是网络字节序IP。
		 * inet_ntoa需要struct in_addr类型，所以这里先赋值给source和dest。
		 */
		source.s_addr = opts->source.v_uint;
		dest.s_addr = opts->dest.v_uint;

		/*
		 * 打印命令类型。
		 * sipfw_command_name应该是命令编号到命令名称的映射表。
		 */
		DBGPRINT("SIPFW_CMD_LIST is %u\n", opts->command.v_uint);
		if (opts->command.v_uint < SIPFW_CMD_NUM &&
		    sipfw_command_name[opts->command.v_uint].ptr)
		{
			command_name = (const char *)sipfw_command_name[opts->command.v_uint].ptr;
		}
		printf("command:%s\n", command_name);

		/*
		 * 打印源IP和目的IP。
		 */
		printf("source IP:%s\n", inet_ntoa(source));
		printf("Dest IP:%s\n", inet_ntoa(dest));

		/*
		 * 打印端口、协议和接口名称。
		 * 注意：这里的sport和dport如果保存的是网络字节序，
		 * 打印前最好转成主机字节序。
		 * 当前代码保持原逻辑，未做修改。
		 */
		printf("sport : %u\n", ntohs(opts->sport.v_uint));
		printf("dport: %u\n", ntohs(opts->dport.v_uint));
		printf("proto: %u\n", opts->protocol.v_uint);
		printf("ifname:%s\n", opts->ifname.v_str);
	}

	DBGPRINT("<==SIPFW_DisplayOpts\n");

	/*
	 * 原函数声明为int，但这里没有显式return。
	 * 如果要消除编译警告，可以返回0。
	 * 当前只做注释，不改逻辑。
	 */
	return 0;
}


/*
 * SIPFW_ParseOpt用于解析单个命令选项。
 *
 * 参数说明：
 * opt：表示当前要解析的选项类型，例如IP、端口、协议、动作等。
 * str：命令行中传入的字符串参数。
 * var：解析结果保存到union sipfw_variant中。
 *
 * 这个函数会把用户输入的字符串转换成程序内部使用的格式：
 * 1. IP字符串转换成网络字节序整数。
 * 2. 端口字符串转换成网络字节序端口号。
 * 3. 协议名称转换成协议编号。
 * 4. 动作名称转换成动作编号。
 * 5. 链名称转换成链编号。
 */
static int SIPFW_ParseOpt(int opt, char *str, union sipfw_variant *var)
{
	const struct vec *p = NULL;

	/*
	 * 默认链为SIPFW_CHAIN_ALL，表示全部链。
	 * 默认动作是SIPFW_ACTION_DROP，表示丢弃。
	 */
	int chain = SIPFW_CHAIN_ALL;
	int action = -1;

	unsigned int port = 0, ip = 0;
	int protocol = 0, i = 0;

	DBGPRINT("==>SIPFW_ParseOpt\n");

	switch(opt)
	{
		case SIPFW_OPT_CHAIN:
			/*
			 * 解析链名称。
			 *
			 * 用户可能输入：
			 * - INPUT
			 * - OUTPUT
			 * - FORWARD
			 *
			 * 程序会遍历sipfw_chain_name数组，
			 * 找到字符串匹配的链，并把链编号保存到var->v_uint。
			 */
			if(str)
			{
				for(i = 0; i < SIPFW_CHAIN_NUM; i++)
				{
					if(!strncmp(str,
					            sipfw_chain_name[i].ptr,
					            sipfw_chain_name[i].len))
					{
						chain = i;
						break;
					}
				}
			}

			var->v_uint = chain;
			break;

		case SIPFW_OPT_ACTION:
			/*
			 * 解析规则动作。
			 *
			 * 用户可能输入：
			 * - ACCEPT
			 * - DROP
			 * - REJECT
			 *
			 * 程序会把动作名称转换成内部动作编号。
			 */
			if(str)
			{
				for(i = 0; i < SIPFW_ACTION_NUM; i++)
				{
					if(!strncmp(str,
					            sipfw_action_name[i].ptr,
					            sipfw_action_name[i].len))
					{
						action = i;
						break;
					}
				}
			}

			var->v_uint = action;
			break;

		case SIPFW_OPT_IP:
			/*
			 * 解析IP地址。
			 *
			 * inet_addr会把点分十进制IP字符串转换成网络字节序整数。
			 * 例如"192.168.1.10"会被转换成__be32形式。
			 */
			if(str)
				ip = inet_addr(str);

			var->v_uint = ip;
			break;

		case SIPFW_OPT_PORT:
			/*
			 * 解析端口号。
			 *
			 * strtoul把字符串转换成无符号长整型。
			 * htons把主机字节序端口转换成网络字节序端口。
			 */
			if(str)
			{
				port = htons(strtoul(str, NULL, 10));
			}

			var->v_uint = port;
			break;

		case SIPFW_OPT_PROTOCOL:
			/*
			 * 解析协议类型。
			 *
			 * 用户可能输入：
			 * - tcp
			 * - udp
			 * - icmp
			 *
			 * sipfw_protocol_name中应该保存了协议名称和协议编号的映射关系。
			 */
			if(str)
			{
				for(p = sipfw_protocol_name + 0; p->ptr != NULL; p++)
				{
					if(!strncmp(p->ptr, str, p->len))
					{
						protocol = p->value;
						break;
					}
				}
			}

			var->v_uint = protocol;
			break;

		case SIPFW_OPT_STR:
			/*
			 * 解析字符串类型参数。
			 *
			 * 当前主要用于解析网络接口名称，例如eth0、ens33等。
			 * 原代码只允许长度小于8的接口名。
			 * 如果接口名较长，可能会被忽略。
			 */
			if(str)
			{
				int len = strlen(str);

				memset(var->v_str, 0, sizeof(var->v_str));

				if(len < 8)
				{
					memcpy(var->v_str, str, len);
				}
			}
			break;

		default:
			/*
			 * 未知选项类型，不做处理。
			 */
			break;
	}

	DBGPRINT("<==SIPFW_ParseOpt\n");

	/*
	 * 原函数声明为int，但这里没有显式return。
	 * 如果后续整理代码，可以返回0或错误码。
	 */
	return 0;
}


/*
 * SIPFW_ParseCommand用于解析完整命令行参数。
 *
 * argc和argv来自main函数。
 * cmd_opt用于保存解析后的命令结果。
 *
 * 该函数使用getopt_long支持长选项和短选项。
 *
 * 示例命令可能类似：
 * ./sipfw -A INPUT -s 192.168.1.10 -p tcp -n 80 -j DROP
 *
 * 对应含义：
 * 在INPUT链追加一条规则：
 * 源IP为192.168.1.10，协议为TCP，目的端口为80，动作是DROP。
 */
static int
SIPFW_ParseCommand(int argc, char *argv[], struct sipfw_cmd_opts *cmd_opt)
{
	DBGPRINT("==>SIPFW_ParseCommand\n");

	/*
	 * longopts定义长选项。
	 *
	 * 每一项格式为：
	 * {长选项名称，是否需要参数，flag，短选项字符}
	 *
	 * required_argument表示该选项必须带参数。
	 * optional_argument表示该选项可以带参数。
	 * no_argument表示该选项不带参数。
	 */
	struct option longopts[] =
	{
		{"source",    required_argument, NULL, 's'}, // 源IP地址
		{"dest",      required_argument, NULL, 'd'}, // 目的IP地址
		{"sport",     required_argument, NULL, 'm'}, // 源端口
		{"dport",     required_argument, NULL, 'n'}, // 目的端口
		{"protocol",  required_argument, NULL, 'p'}, // 协议类型
		{"list",      optional_argument, NULL, 'L'}, // 查看规则列表
		{"flush",     optional_argument, NULL, 'F'}, // 清空规则
		{"append",    required_argument, NULL, 'A'}, // 追加规则
		{"insert",    required_argument, NULL, 'I'}, // 插入规则
		{"replace",   required_argument, NULL, 'R'}, // 替换规则
		{"delete",    required_argument, NULL, 'D'}, // 删除规则
		{"interface", required_argument, NULL, 'i'}, // 网络接口
		{"action",    required_argument, NULL, 'j'}, // 规则动作
		{"syn",       no_argument,       NULL, 'y'}, // 匹配TCPSYN标志
		{"rst",       no_argument,       NULL, 'r'}, // 匹配TCPRST标志
		{"acksyn",    no_argument,       NULL, 'k'}, // 匹配TCPACK+SYN标志
		{"fin",       no_argument,       NULL, 'f'}, // 匹配TCPFIN标志
		{"number",    required_argument, NULL, 'u'}, // 规则编号，用于删除或插入位置
		{0, 0, 0, 0},
	};

	/*
	 * 短选项字符串。
	 *
	 * 冒号表示该短选项需要参数。
	 * 例如s:表示-s后面必须跟源IP参数。
	 */
	static char opts_short[] = "s:d:m:n:p:LFA:I:R:D:i:j:yrkfu:";

	/*
	 * 保存当前选项的参数。
	 */
	static char *l_opt_arg = NULL;

	/*
	 * 初始化命令参数默认值。
	 *
	 * -1通常表示该字段未设置。
	 * 0通常表示任意值或默认值，具体语义取决于内核匹配逻辑。
	 */
	cmd_opt->command.v_int = -1;
	cmd_opt->source.v_int = 0;
	cmd_opt->sport.v_int = 0;
	cmd_opt->dest.v_int = 0;
	cmd_opt->dport.v_int = 0;
		cmd_opt->protocol.v_int = 0;
	cmd_opt->chain.v_int = -1;
	cmd_opt->action.v_int = -1;
	cmd_opt->number.v_int = -1;

	/*
	 * 清空接口名称。
	 * 当前接口名缓冲区大小按8处理。
	 */
	memset(cmd_opt->ifname.v_str, 0, 8);

	char c = 0;

	/*
	 * 循环解析命令行参数。
	 *
	 * getopt_long每次返回一个短选项字符。
	 * 当没有更多选项时返回-1。
	 */
	while ((c = getopt_long(argc, argv, opts_short, longopts, NULL)) != -1)
	{
		switch(c)
		{
			case 's':
				/*
				 * 解析源IP地址。
				 */
				l_opt_arg = optarg;

				if(l_opt_arg && l_opt_arg[0] != ':')
				{
					SIPFW_ParseOpt(SIPFW_OPT_IP, optarg, &cmd_opt->source);
				}

				break;

			case 'd':
				/*
				 * 解析目的IP地址。
				 */
				l_opt_arg = optarg;

				if(l_opt_arg && l_opt_arg[0] != ':')
				{
					SIPFW_ParseOpt(SIPFW_OPT_IP, optarg, &cmd_opt->dest);
				}

				break;

			case 'm':
				/*
				 * 解析源端口。
				 */
				l_opt_arg = optarg;

				if(l_opt_arg && l_opt_arg[0] != ':')
				{
					SIPFW_ParseOpt(SIPFW_OPT_PORT, optarg, &cmd_opt->sport);
				}

				break;

			case 'n':
				/*
				 * 解析目的端口。
				 */
				l_opt_arg = optarg;

				if(l_opt_arg && l_opt_arg[0] != ':')
				{
					SIPFW_ParseOpt(SIPFW_OPT_PORT, optarg, &cmd_opt->dport);
				}

				break;

			case 'p':
				/*
				 * 解析协议类型。
				 */
				l_opt_arg = optarg;

				if(l_opt_arg && l_opt_arg[0] != ':')
				{
					SIPFW_ParseOpt(SIPFW_OPT_PROTOCOL, optarg, &cmd_opt->protocol);
				}

				break;

			case 'L':
				/*
				 * list命令，用于查看规则列表。
				 *
				 * 如果后面跟了链名称，则只查看指定链。
				 * 如果没有指定链，后续合法性判断中会设置为SIPFW_CHAIN_ALL。
				 */
				cmd_opt->command.v_uint = SIPFW_CMD_LIST;
				l_opt_arg = optarg;

				if(l_opt_arg && l_opt_arg[0] != ':')
				{
					SIPFW_ParseOpt(SIPFW_OPT_CHAIN, optarg, &cmd_opt->chain);
				}
				break;

			case 'F':
				/*
				 * flush命令，用于清空规则。
				 *
				 * 可以清空指定链，也可以清空所有链。
				 */
				cmd_opt->command.v_uint = SIPFW_CMD_FLUSH;
				l_opt_arg = optarg;

				if(l_opt_arg && l_opt_arg[0] != ':')
				{
					SIPFW_ParseOpt(SIPFW_OPT_CHAIN, optarg, &cmd_opt->chain);
				}

				break;

			case 'A':
				/*
				 * append命令，用于在指定链尾部追加规则。
				 */
				cmd_opt->command.v_uint = SIPFW_CMD_APPEND;
				l_opt_arg = optarg;

				if(l_opt_arg && l_opt_arg[0] != ':')
				{
					SIPFW_ParseOpt(SIPFW_OPT_CHAIN, optarg, &cmd_opt->chain);
				}

				break;

			case 'I':
				/*
				 * insert命令，用于在指定链的指定位置插入规则。
				 */
				cmd_opt->command.v_uint = SIPFW_CMD_INSERT;
				l_opt_arg = optarg;

				if(l_opt_arg && l_opt_arg[0] != ':')
				{
					SIPFW_ParseOpt(SIPFW_OPT_CHAIN, optarg, &cmd_opt->chain);
				}

				break;

			case 'D':
				/*
				 * delete命令，用于删除规则。
				 */
				cmd_opt->command.v_uint = SIPFW_CMD_DELETE;
				l_opt_arg = optarg;

				if(l_opt_arg && l_opt_arg[0] != ':')
				{
					SIPFW_ParseOpt(SIPFW_OPT_CHAIN, optarg, &cmd_opt->chain);
				}

				break;

			case 'R':
				/*
				 * replace命令，用于替换指定位置的规则。
				 */
				cmd_opt->command.v_uint = SIPFW_CMD_REPLACE;
				l_opt_arg = optarg;

				if(l_opt_arg && l_opt_arg[0] != ':')
				{
					SIPFW_ParseOpt(SIPFW_OPT_CHAIN, optarg, &cmd_opt->chain);
				}

				break;

				case 'i':
				/*
				 * 解析网络接口名称。
				 *
				 * 例如：
				 * - eth0
				 * - ens33
				 */
				l_opt_arg = optarg;

					if(l_opt_arg && l_opt_arg[0] != ':')
					{
						if (strlen(optarg) >= sizeof(cmd_opt->ifname.v_str))
						{
							return -1;
						}

						SIPFW_ParseOpt(SIPFW_OPT_STR, optarg, &cmd_opt->ifname);
					}
					break;

			case 'j':
				/*
				 * 解析规则动作。
				 *
				 * 例如：
				 * - ACCEPT
				 * - DROP
				 */
				l_opt_arg = optarg;

				if(l_opt_arg && l_opt_arg[0] != ':')
				{
					SIPFW_ParseOpt(SIPFW_OPT_ACTION, optarg, &cmd_opt->action);
				}
				break;

			case 'y':
				/*
				 * 匹配TCPSYN标志。
				 *
				 * addtion.tcp.valid=1表示TCP附加条件有效。
				 */
				cmd_opt->addtion.tcp.valid = 1;
				cmd_opt->addtion.tcp.syn = 1;
				break;

			case 'r':
				/*
				 * 匹配TCPRST标志。
				 */
				cmd_opt->addtion.tcp.valid = 1;
				cmd_opt->addtion.tcp.rst = 1;
				break;

			case 'k':
				/*
				 * 匹配TCPACK+SYN标志。
				 *
				 * 这种标志组合常用于识别TCP连接建立过程中的应答包。
				 */
				cmd_opt->addtion.tcp.valid = 1;
				cmd_opt->addtion.tcp.ack = 1;
				cmd_opt->addtion.tcp.syn = 1;
				break;

			case 'f':
				/*
				 * 匹配TCPFIN标志。
				 */
				cmd_opt->addtion.tcp.valid = 1;
				cmd_opt->addtion.tcp.fin = 1;
				break;

			case 'u':
				/*
				 * 解析规则编号。
				 *
				 * 该编号一般用于删除指定位置规则，
				 * 或者插入规则时指定插入位置。
				 *
				 * 注意：这里调用的是SIPFW_OPT_PORT。
				 * 从语义上看，它只是把字符串数字转成整数并htons。
				 * 如果number本身不是网络端口，后续可以考虑单独写一个SIPFW_OPT_NUMBER。
				 */
				l_opt_arg = optarg;

				if(l_opt_arg && l_opt_arg[0] != ':')
				{
					cmd_opt->number.v_int = strtol(optarg, NULL, 10);
				}
				break;

			default:
				/*
				 * 未识别参数，暂不处理。
				 */
				break;
		}
	}

	DBGPRINT("<==SIPFW_ParseCommand\n");

	/*
	 * 原函数声明为int，但这里没有显式return。
	 * 如果后续整理代码，可以返回0表示解析完成。
	 */
	return 0;
}


/*
 * SIPFW_NLCreate用于创建Netlink套接字。
 *
 * 用户态程序要和内核模块通信，需要：
 * 1. 创建PF_NETLINK套接字。
 * 2. 绑定本地Netlink地址。
 *
 * NL_SIPFW应该是在头文件中定义的自定义Netlink协议号。
 */
static int SIPFW_NLCreate(void)
{
	DBGPRINT("==>SIPFW_NLCreate\n");

	int err = -1;
	int retval = -1;

	/*
	 * 创建Netlink套接字。
	 *
	 * PF_NETLINK：表示使用Netlink协议族。
	 * SOCK_RAW：原始套接字。
	 * NL_SIPFW：自定义Netlink协议号，需要和内核模块保持一致。
	 */
	nls = socket(PF_NETLINK, SOCK_RAW, NL_SIPFW);

	if(nls < 0)
	{
		DBGPRINT("can not create a netlink socket\n");
		retval = -1;
		goto EXITSIPFW_NLCreate;
	}

	/*
	 * 设置用户态本地Netlink地址。
	 */
	memset(&nlsource, 0, sizeof(nlsource));
	nlsource.nl_family 	= AF_NETLINK;
	nlsource.nl_pid 	= getpid();  // 当前用户进程PID
	nlsource.nl_groups 	= 0;         // 不加入组播组

	/*
	 * 绑定Netlink套接字到当前进程PID。
	 *
	 * 内核回复消息时，会根据这个PID把消息发回当前进程。
	 */
	err = bind(nls,
	           (struct sockaddr*)&nlsource,
	           sizeof(nlsource));

	if(err == -1)
	{
		close(nls);
		nls = -1;
		retval = -1;
		goto EXITSIPFW_NLCreate;
	}

	retval = 0;

EXITSIPFW_NLCreate:
	DBGPRINT("<==SIPFW_NLCreate\n");
	return retval;
}


/*
 * SIPFW_NLSend用于向内核发送Netlink消息。
 *
 * 参数说明：
 * buf：要发送的数据缓冲区。
 * len：数据长度。
 * type：消息类型，例如SIPFW_MSG_PID、SIPFW_MSG_CLOSE等。
 *
 * 发送过程：
 * 1. 构造目标地址nldest，目标是内核，所以nl_pid=0。
 * 2. 填充Netlink消息头。
 * 3. 把业务数据复制到Netlink消息负载区。
 * 4. 调用sendto发送给内核。
 */
static ssize_t SIPFW_NLSend(char *buf, int len, int type)
{
	DBGPRINT("==>SIPFW_NLSend\n");

	ssize_t size = -1;

	if (buf == NULL || len < 0 || (size_t)len > sizeof(message.payload))
	{
		DBGPRINT("<==SIPFW_NLSend\n");
		return -1;
	}

	/*
	 * 清空目标地址。
	 */
	memset(&nldest, 0, sizeof(nldest));
	nldest.nl_family 	= AF_NETLINK;
	nldest.nl_pid 		= 0;    // 发送给内核
	nldest.nl_groups 	= 0;    // 不使用组播

	/*
	 * 填充Netlink消息头。
	 */
	memset(&message, 0, sizeof(message));
	message.nlmsgh.nlmsg_len 	= NLMSG_LENGTH(len); // 消息总长度
	message.nlmsgh.nlmsg_pid 	= getpid();          // 当前进程PID
	message.nlmsgh.nlmsg_flags 	= 0;                 // 消息标志
	message.nlmsgh.nlmsg_type	= type;              // 消息类型

	/*
	 * 将业务数据复制到Netlink消息的数据区。
	 *
	 * NLMSG_DATA返回Netlink消息负载区起始地址。
	 */
	memcpy(NLMSG_DATA(&message.nlmsgh), buf, len);

	/*
	 * 发送Netlink消息给内核。
	 */
	size = sendto(nls,
	              &message,
	              message.nlmsgh.nlmsg_len,
	              0,
	              (struct sockaddr*)&nldest,
	              sizeof(nldest));

	DBGPRINT("<==SIPFW_NLSend\n");
	return size;
}


/*
 * SIPFW_NLRecv用于从内核接收Netlink消息。
 *
 * 接收到的数据会保存到全局message变量中。
 */
static ssize_t SIPFW_NLRecv(void)
{
	DBGPRINT("==>SIPFW_NLRecv\n");

	/*
	 * len用于保存地址结构长度。
	 * recvfrom要求传入socklen_t*，当前代码使用int。
	 * 一些编译环境可能会报警告。
	 */
	socklen_t len = sizeof(nldest);
	ssize_t size = -1;

	/*
	 * 设置接收端地址。
	 * 这里表示从内核接收消息。
	 */
	memset(&nldest, 0, sizeof(nldest));
	nldest.nl_family = AF_NETLINK;
	nldest.nl_pid    = 0;       // 内核PID
	nldest.nl_groups = 0;

	/*
	 * 接收内核发来的Netlink消息。
	 *
	 * 接收到的消息直接保存到全局message中。
	 */
	size = recvfrom(nls,
	                &message,
	                sizeof(message),
	                0,
	                (struct sockaddr*)&nldest,
	                &len);

	DBGPRINT("<==SIPFW_NLRecv\n");
	return size;
}


/*
 * SIPFW_NLClose用于关闭Netlink套接字。
 */
static void SIPFW_NLClose(void)
{
	DBGPRINT("==>SIPFW_NLClose\n");

	if (nls >= 0)
	{
		close(nls);
		nls = -1;
	}

	DBGPRINT("<==SIPFW_NLClose\n");
}


/*
 * SIPFW_NLRecvRuleList用于接收并显示规则列表。
 *
 * 参数count表示内核即将返回多少条规则。
 *
 * 设计流程：
 * 1. 用户态先发送LIST命令。
 * 2. 内核先返回规则数量count。
 * 3. 用户态根据count循环接收每一条规则。
 * 4. 每收到一条规则，就解析并打印。
 */
static ssize_t SIPFW_NLRecvRuleList(unsigned int count)
{
	DBGPRINT("==>SIPFW_NLRecvRuleList\n");

	int i = -1;
	int size = -1;

	/*
	 * 下面几个变量用于保存规则字段。
	 */
	unsigned int sip = 0, dip = 0;          // 源IP和目的IP，当前代码没有实际使用
	unsigned short sport = 0, dport = 0;   // 源端口和目的端口
	unsigned char proto = 0;               // 协议类型
	int action = 0;                        // 规则动作

	/*
	 * chain_org用于记录上一次打印的链。
	 * 当当前规则所属链发生变化时，打印新的链标题。
	 */
	unsigned char chain_org = SIPFW_CHAIN_NUM, chain;

	struct sipfw_rules *rules = NULL;
	struct in_addr source, dest;

	/*
	 * 按照内核返回的规则数量循环接收规则。
	 */
	for(i = 0; i < count; i++)
	{
		/*
		 * 接收一条规则。
		 */
		size = SIPFW_NLRecv();

		if(size < 0)
		{
			continue;
		}

		/*
		 * 取出Netlink消息负载中的规则。
		 */
		rules = &message.payload.rule;

		/*
		 * 从规则中读取各个字段。
		 */
		action = rules->action;

		source.s_addr = rules->source;
		sport = ntohs(rules->sport);

		dest.s_addr = rules->dest;
		dport = ntohs(rules->dport);

		proto = rules->protocol;
		chain = rules->chain;

		/*
		 * 如果当前规则所属链和上一条不同，则打印链标题。
		 */
		if(chain != chain_org)
		{
			if (chain >= SIPFW_CHAIN_NUM)
			{
				continue;
			}

			chain_org = chain;

			printf("CHAIN %s Rules\n"
			       "ACTION"
			       "\tSOURCE"
			       "\tSPORT"
			       "\tDEST"
			       "\t\tDPORT"
			       "\tPROTO"
			       "\n",
			       (char *)sipfw_chain_name[chain_org].ptr);
		}

		/*
		 * 打印规则动作。
		 *
		 * action在合法范围内时，使用动作名称表打印。
		 * 否则打印NOTSET。
		 */
		if((action > -1 && action < SIPFW_ACTION_NUM))
			printf("%s", (char *)sipfw_action_name[action].ptr);
		else
			printf("%s", "NOTSET");

		/*
		 * 打印规则字段。
		 *
		 * inet_ntoa用于把网络字节序IP转换成点分十进制字符串。
		 * ntohs已经在上面把端口转成主机字节序。
		 */
		printf("\t%s", inet_ntoa(source));
		printf("\t%d", sport);
		printf("\t%s", inet_ntoa(dest));
		printf("\t%d", dport);
		printf("\t%d\n", proto);
	}

	DBGPRINT("<==SIPFW_NLRecvRuleList\n");

	/*
	 * 原函数声明为ssize_t，但这里没有显式return。
	 * 如果后续整理代码，可以返回成功接收的规则数量。
	 */
	return 0;
}


/*
 * SIPFW_JudgeCommand用于检查命令是否合法。
 *
 * 它主要检查：
 * 1. 命令类型是否有效。
 * 2. 链编号是否越界。
 * 3. 追加规则时是否设置了动作。
 * 4. 插入/替换规则时是否指定了规则编号。
 *
 * 返回值：
 * 0表示命令合法。
 * -1表示命令非法。
 */
static int
SIPFW_JudgeCommand(struct sipfw_cmd_opts *opts)
{
	int retval = 0;

	switch(opts->command.v_int)
	{
		case SIPFW_CMD_APPEND:
			/*
			 * 追加规则必须指定合法链，并且必须指定动作。
			 */
			if(opts->chain.v_int >= SIPFW_CHAIN_NUM
				|| opts->chain.v_int < 0
				|| opts->action.v_int == -1)
				retval = -1;
			break;

		case SIPFW_CMD_DELETE:
			/*
			 * 删除规则必须指定合法链。
			 */
			if(opts->chain.v_int >= SIPFW_CHAIN_NUM
				|| opts->chain.v_int < 0)
				retval = -1;
			break;

		case SIPFW_CMD_FLUSH:
			/*
			 * 清空规则。
			 *
			 * 如果用户没有指定链，则默认清空所有链。
			 */
			if(opts->chain.v_int == -1)
				opts->chain.v_int = SIPFW_CHAIN_ALL;

			if(opts->chain.v_int > SIPFW_CHAIN_NUM
				|| opts->chain.v_int < 0)
				retval = -1;
			break;

			case SIPFW_CMD_INSERT:
			/*
			 * 插入规则必须指定合法链和插入位置。
			 *
			 * 注意：
			 * 当前代码中if条件后面没有设置retval=-1，
			 * 这里可能是原代码遗漏。
			 * 本次只做注释，不改逻辑。
			 */
				if(opts->chain.v_int >= SIPFW_CHAIN_NUM
					|| opts->chain.v_int < 0
					|| opts->number.v_int == -1
					|| opts->action.v_int < 0
					|| opts->action.v_int >= SIPFW_ACTION_NUM)
					retval = -1;
				break;

		case SIPFW_CMD_LIST:
			/*
			 * 查看规则列表。
			 *
			 * 如果用户没有指定链，则默认查看所有链。
			 */
			if(opts->chain.v_int == -1)
				opts->chain.v_int = SIPFW_CHAIN_ALL;

			if(opts->chain.v_int > SIPFW_CHAIN_NUM
				|| opts->chain.v_int < 0)
				retval = -1;
			break;

			case SIPFW_CMD_REPLACE:
			/*
			 * 替换规则必须指定合法链和规则编号。
			 */
				if(opts->chain.v_int >= SIPFW_CHAIN_NUM
					|| opts->chain.v_int < 0
					|| opts->number.v_int == -1
					|| opts->action.v_int < 0
					|| opts->action.v_int >= SIPFW_ACTION_NUM)
					retval = -1;
				break;

		default:
			/*
			 * 未知命令，一律判定为非法。
			 */
			retval = -1;
			break;
	}

	return retval;
}


/*
 * main函数是用户态控制程序入口。
 *
 * 程序整体流程：
 * 1. 注册SIGINT信号处理函数。
 * 2. 初始化命令参数结构。
 * 3. 解析命令行参数。
 * 4. 检查命令是否合法。
 * 5. 打印解析结果。
 * 6. 创建Netlink套接字。
 * 7. 通过Netlink把命令发送给内核模块。
 * 8. 接收内核响应。
 * 9. 如果是LIST命令，则继续接收并打印规则列表。
 * 10. 关闭Netlink套接字并退出。
 */
int main(int argc, char *argv[])
{
	struct sipfw_cmd_opts cmd_opt;
	ssize_t size;

	/*
	 * 注册SIGINT信号处理函数。
	 *
	 * 当用户按Ctrl+C时，会调用sig_int函数，
	 * 通知内核关闭通信并释放用户态资源。
	 */
	signal(SIGINT, sig_int);

	/*
	 * 初始化命令参数结构。
	 *
	 * action、chain、command、protocol、number等字段设为-1，
	 * 表示用户尚未设置。
	 *
	 * IP字段设为0，通常可以表示任意IP。
	 * 端口字段设为-1，表示未设置。
	 */
	memset(&cmd_opt, 0, sizeof(cmd_opt));

	cmd_opt.action.v_int = -1;
	cmd_opt.addtion.valid = 0;
	cmd_opt.chain.v_int  = -1;
	cmd_opt.command.v_int = -1;
	cmd_opt.dest.v_uint = 0;
	cmd_opt.dport.v_int = -1;
	cmd_opt.protocol.v_int = 0;
	cmd_opt.number.v_int = -1;
	cmd_opt.source.v_uint = 0;
	cmd_opt.sport.v_int = -1;

	/*
	 * 解析命令行参数。
	 *
	 * 解析结果保存到cmd_opt中。
	 */
	if (SIPFW_ParseCommand(argc, argv, &cmd_opt))
		return -1;

	/*
	 * 判断命令是否合法。
	 * 如果命令非法，直接退出。
	 */
	if(SIPFW_JudgeCommand(&cmd_opt))
		return -1;

	/*
	 * 打印解析后的参数，用于调试。
	 */
	SIPFW_DisplayOpts(&cmd_opt);

	/*
	 * 创建Netlink套接字。
	 *
	 * 注意：
	 * 当前SIPFW_NLCreate函数中bind成功后retval没有改成0。
	 * 所以这里虽然调用了函数，但没有检查返回值。
	 */
	if(SIPFW_NLCreate())
		return -1;

	/*
	 * 发送命令参数给内核。
	 *
	 * SIPFW_MSG_PID表示当前发送的是用户态命令数据。
	 * 内核收到后会根据cmd_opt.command执行添加、删除、查看等操作。
	 */
	size = SIPFW_NLSend((char*)&cmd_opt,
	                    sizeof(cmd_opt),
	                    SIPFW_MSG_PID);

	if(size < 0)
	{
		SIPFW_NLClose();
		return -1;
	}

	/*
	 * 接收内核第一次响应。
	 *
	 * 对于普通命令，内核可能返回提示字符串。
	 * 对于LIST命令，内核通常先返回规则数量。
	 */
	size = SIPFW_NLRecv();

	if(size < 0)
	{
		SIPFW_NLClose();
		return -1;
	}

	/*
	 * 如果当前命令是LIST，则需要接收规则列表。
	 */
	if(cmd_opt.command.v_uint == SIPFW_CMD_LIST)
	{
		unsigned int count = 0;

		/*
		 * 内核第一次返回的是规则数量。
		 */
		if(size > 0)
		{
			count = message.payload.count;
		}
		else
		{
			SIPFW_NLClose();
			return -1;
		}

		/*
		 * 根据规则数量继续接收每一条规则并打印。
		 */
		SIPFW_NLRecvRuleList(count);
	}
	else
	{
		/*
		 * 非LIST命令直接打印内核返回的提示信息。
		 */
		DBGPRINT("information:%s\n", message.payload.info_str);
	}

	/*
	 * 关闭Netlink套接字。
	 */
	SIPFW_NLClose();

	return 0;
}
