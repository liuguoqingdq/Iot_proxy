#ifndef __KERNEL__
#define __KERNEL__
#endif /* __KERNEL__ */

#ifndef MODULE
#define MODULE
#endif /* MODULE */

#include "sipfw.h"
#include <linux/uaccess.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/fs.h>
#include <linux/timekeeping.h>

/*
 * 兼容旧代码中的f_dentry写法。
 *
 * 老版本内核中struct file可能直接有f_dentry字段；
 * 新版本内核一般通过f_path.dentry访问dentry。
 */
#define f_dentry f_path.dentry


/*
 * SIPFW_OpenFile
 *
 * 功能：
 *   在内核态打开一个文件，并返回struct file指针。
 *
 * 参数：
 *   filename ：要打开的文件路径。
 *   flags    ：打开标志，例如O_CREAT、O_RDWR、O_APPEND等。
 *   mode     ：文件权限模式。
 *
 * 返回值：
 *   成功：返回struct file指针。
 *   失败：返回NULL。
 *
 * 注意：
 *   1. 当前实现中调用filp_open(filename, flags, 0)，没有使用传入的mode参数。
 *      如果flags里带有O_CREAT，通常应该把mode传进去，例如0644。
 *   2. 内核态直接读写普通文件不是现代内核推荐方式，但老防火墙模块里常见这种写法。
 */
struct file *SIPFW_OpenFile(const char *filename, int flags, int mode)
{
    struct file *f = NULL;

    DBGPRINT("==>SIPFW_OpenFile\n");

    /*
     * filp_open用于在内核态打开文件。
     *
     * 参数含义：
     *   filename：文件路径。
     *   flags   ：打开方式。
     *   mode    ：创建文件时使用的权限。
     *
     * 注意：这里原代码把mode固定写成0，传入的mode参数没有被使用。
     */
    f = filp_open(filename, flags, mode);

    /*
     * filp_open失败时，可能返回ERR_PTR编码后的错误指针，
     * 所以需要用IS_ERR判断。
     */
    if (!f || IS_ERR(f))
    {
        f = NULL;
    }

    DBGPRINT("<==SIPFW_OpenFile\n");
    return f;
}



/*
 * SIPFW_KernelRead
 *
 * 功能：
 *   提供一个兼容不同内核版本的内核态文件读取接口。
 *
 * 说明：
 *   新一些的内核使用kernel_read()；
 *   老内核没有kernel_read()时，退回到vfs_read() + set_fs(KERNEL_DS)。
 */
static ssize_t SIPFW_KernelRead(struct file *file,
                                void *buf,
                                size_t len,
                                loff_t *pos)
{
    if (!file || IS_ERR(file) || !buf || len == 0 || !pos)
    {
        return -EINVAL;
    }

    if (!(file->f_mode & FMODE_READ))
    {
        return -EBADF;
    }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)

    /*
     * kernel_read接收内核缓冲区buf，不需要set_fs。
     */
    return kernel_read(file, buf, len, pos);

#else

    /*
     * 旧内核兼容路径。
     *
     * vfs_read的buf参数类型是char __user *，
     * 所以旧内核里通过set_fs(KERNEL_DS)让它接受内核缓冲区。
     */
    {
        mm_segment_t oldfs;
        ssize_t ret;

        oldfs = get_fs();
        set_fs(KERNEL_DS);

        ret = vfs_read(file,
                       (char __user *)buf,
                       len,
                       pos);

        set_fs(oldfs);

        return ret;
    }

#endif
}


/*
 * SIPFW_KernelWrite
 *
 * 功能：
 *   提供一个兼容不同内核版本的内核态文件写入接口。
 *
 * 说明：
 *   新一些的内核使用kernel_write()；
 *   老内核没有kernel_write()时，退回到vfs_write() + set_fs(KERNEL_DS)。
 */
static ssize_t SIPFW_KernelWrite(struct file *file,
                                 const void *buf,
                                 size_t len,
                                 loff_t *pos)
{
    if (!file || IS_ERR(file) || !buf || len == 0 || !pos)
    {
        return -EINVAL;
    }

    if (!(file->f_mode & FMODE_WRITE))
    {
        return -EBADF;
    }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)

    /*
     * kernel_write接收内核缓冲区buf，不需要set_fs。
     */
    return kernel_write(file, buf, len, pos);

#else

    /*
     * 旧内核兼容路径。
     *
     * vfs_write的buf参数类型是const char __user *，
     * 所以旧内核里通过set_fs(KERNEL_DS)让它接受内核缓冲区。
     */
    {
        mm_segment_t oldfs;
        ssize_t ret;

        oldfs = get_fs();
        set_fs(KERNEL_DS);

        ret = vfs_write(file,
                        (const char __user *)buf,
                        len,
                        pos);

        set_fs(oldfs);

        return ret;
    }

#endif
}


/*
 * SIPFW_ReadLine
 *
 * 功能：
 *   从文件中读取一行内容，最多读取len字节，并把结果放入buf。
 *
 * 注意：
 *   这里暂时保留原来的“逐字符读取”逻辑；
 *   只把底层读取方式从f->f_op->read()换成SIPFW_KernelRead()。
 */
ssize_t SIPFW_ReadLine(struct file *f, char *buf, size_t len)
{
#define EOF (-1)

    ssize_t count = -1;
    ssize_t ret = 0;
    struct inode *inode;
    char *pos = buf;

    DBGPRINT("==>SIPFW_ReadLine\n");

    /*
     * 参数合法性检查。
     * len至少要大于1，因为最后需要补'\0'。
     */
    if (!f || IS_ERR(f) || !buf || len <= 1)
    {
        goto out_error;
    }

    /*
     * 检查文件对象、dentry和inode是否有效。
     */
    if (!f->f_dentry || !f->f_dentry->d_inode)
    {
        goto out_error;
    }

    inode = f->f_dentry->d_inode;

    /*
     * 检查文件是否以可读方式打开。
     */
    if (!(f->f_mode & FMODE_READ))
    {
        goto out_error;
    }

    count = 0;

    /*
     * 先读取第一个字符。
     */
    ret = SIPFW_KernelRead(f, pos, 1, &f->f_pos);
    if (ret <= 0)
    {
        DBGPRINT("file read failure or EOF\n");
        goto out_error;
    }

    /*
     * 如果第一个字符就是EOF，则认为到达文件结尾。
     */
    if (*pos == EOF)
    {
        DBGPRINT("file EOF\n");
        goto out_error;
    }

    count = 1;

    /*
     * 逐字符读取，直到遇到EOF、字符串结束符、换行、回车、缓冲区快满或文件末尾。
     *
     * 这里使用count < len - 1，是为了给最后的'\0'预留一个位置。
     */
    while (*pos != EOF &&
           *pos != '\0' &&
           *pos != '\n' &&
           *pos != '\r' &&
           count < len - 1 &&
           f->f_pos <= inode->i_size)
    {
        pos += 1;

        ret = SIPFW_KernelRead(f, pos, 1, &f->f_pos);
        if (ret <= 0)
        {
            break;
        }

        count++;
    }

    /*
     * 如果最后读到的是换行、回车或EOF，就把它替换成字符串结束符。
     */
    if (*pos == '\r' || *pos == '\n' || *pos == EOF)
    {
        *pos = '\0';

        if (count > 0)
        {
            count -= 1;
        }
    }
    else
    {
        /*
         * 如果最后一个字符不是行结束符，则在后面补'\0'。
         */
        pos += 1;
        *pos = '\0';
    }

    DBGPRINT("<==SIPFW_ReadLine\n");
    return count;

out_error:
    DBGPRINT("<==SIPFW_ReadLine\n");
    return count;
}


/*
 * SIPFW_WriteLine
 *
 * 功能：
 *   向文件写入一段数据。
 *
 * 注意：
 *   这里把原来的f->f_op->write()换成SIPFW_KernelWrite()。
 */
ssize_t SIPFW_WriteLine(struct file *f, char *buf, size_t len)
{
    ssize_t count = -1;

    DBGPRINT("==>SIPFW_WriteLine\n");

    /*
     * 参数合法性检查。
     */
    if (!f || IS_ERR(f) || !buf || len <= 0)
    {
        goto out_error;
    }

    /*
     * 检查文件对象、dentry和inode是否有效。
     */
    if (!f->f_dentry || !f->f_dentry->d_inode)
    {
        goto out_error;
    }

    /*
     * 写文件只需要检查FMODE_WRITE即可。
     * 原代码同时要求FMODE_READ和FMODE_WRITE，条件偏严格。
     */
    if (!(f->f_mode & FMODE_WRITE))
    {
        goto out_error;
    }

    /*
     * 使用兼容包装接口写入数据。
     */
    count = SIPFW_KernelWrite(f, buf, len, &f->f_pos);

    /*
     * 内核读写函数一般返回负错误码表示失败。
     */
    if (count < 0)
    {
        DBGPRINT("file write failure\n");
        goto out_error;
    }

out_error:
    DBGPRINT("<==SIPFW_WriteLine\n");
    return count;
}


/*
 * SIPFW_CloseFile
 *
 * 功能：
 *   关闭内核态打开的文件。
 *
 * 参数：
 *   f：需要关闭的struct file指针。
 */
void SIPFW_CloseFile(struct file *f)
{
    DBGPRINT("==>SIPFW_CloseFile\n");

    if (!f)
    {
        return;
    }

    /*
     * 关闭通过filp_open打开的文件。
     */
    filp_close(f, current->files);

    DBGPRINT("<==SIPFW_CloseFile\n");
}

struct sipfw_rule_record
{
    int chain;
    __be32 source;
    __be32 dest;
    __be16 sport;
    __be16 dport;
    __u8 protocol;
    int action;
    __u8 ifname[sizeof(((struct sipfw_rules *)0)->ifname)];
    union addtion addtion;
};

static void SIPFW_RecordFromRule(struct sipfw_rule_record *record,
                                 const struct sipfw_rules *rule)
{
    if (!record || !rule)
    {
        return;
    }

    memset(record, 0, sizeof(*record));
    record->chain = rule->chain;
    record->source = rule->source;
    record->dest = rule->dest;
    record->sport = rule->sport;
    record->dport = rule->dport;
    record->protocol = rule->protocol;
    record->action = rule->action;
    memcpy(record->ifname, rule->ifname, sizeof(record->ifname));
    memcpy(&record->addtion, &rule->addtion, sizeof(record->addtion));
}

static void SIPFW_RuleFromRecord(struct sipfw_rules *rule,
                                 const struct sipfw_rule_record *record)
{
    if (!rule || !record)
    {
        return;
    }

    memset(rule, 0, sizeof(*rule));
    rule->chain = record->chain;
    rule->source = record->source;
    rule->dest = record->dest;
    rule->sport = record->sport;
    rule->dport = record->dport;
    rule->protocol = record->protocol;
    rule->action = record->action;
    memcpy(rule->ifname, record->ifname, sizeof(rule->ifname));
    memcpy(&rule->addtion, &record->addtion, sizeof(rule->addtion));
    rule->next = NULL;
}


/*
 * SIPFW_LogAppend
 *
 * 功能：
 *   把一次被规则处理的数据包信息追加写入日志文件。
 *
 * 日志格式大致为：
 *   Time: yyyy-mm-dd hh:mm:ss From 源IP To 目的IP 协议 was 动作ed
 *
 * 参数：
 *   skb ：当前处理的数据包。
 *   r   ：匹配到的防火墙规则。
 *
 * 返回值：
 *   成功：返回0。
 *   失败或日志暂停：返回-1。
 *
 * 注意：
 *   1. 当前函数在包处理路径中打开、写入、关闭文件，性能开销非常大。
 *      真正工程里一般不建议在每个包路径中直接写普通文件。
 *   2. 更常见做法是使用printk、trace、ring buffer、relayfs，或者把日志事件交给用户态处理。
 */
int SIPFW_LogAppend(struct sk_buff *skb, struct sipfw_rules *r)
{
	/*
	 * 保存格式化后的日志字符串。
	 */
	char *buff = NULL;

    /*
     * 日志文件指针。
     */
    struct file *f;

    /*
     * 函数返回值。
     */
    int retval = 0;

    /*
     * 当前本地时间结构。
     */
    struct tma cur;

    /*
     * 当前时间戳。
     *
     * 注意：原代码声明time后直接time = cur.sec，此时cur未初始化，逻辑可疑。
     * 应该先获取当前时间，再转换成本地时间。
     */
    unsigned long time;

    /*
     * 协议名称表项。
     */
    const struct vec *proto;
    const char *proto_name = "unknown";
    const char *action_name = "UNKNOWN";

    /*
	 * 从skb中取IP头。
	 */
	struct iphdr *iph = NULL;

	if (!skb || !r)
	{
		retval = -1;
		goto EXITSIPFW_LogAppend;
	}

	iph = ip_hdr(skb);
	if (!iph)
	{
		retval = -1;
		goto EXITSIPFW_LogAppend;
	}

    /*
     * 如果日志暂停标志打开，则不写日志。
     */
	if (cf.LogPause)
	{
		retval = -1;
		goto EXITSIPFW_LogAppend;
	}

	buff = kmalloc(2048, GFP_ATOMIC);
	if (!buff)
	{
		retval = -ENOMEM;
		goto EXITSIPFW_LogAppend;
	}

    /*
     * 打开日志文件。
     * O_APPEND表示追加写入。
     *
     * 注意：这里mode传0，如果文件不存在且O_CREAT生效，权限可能不符合预期。
     */
    f = SIPFW_OpenFile(cf.LogFilePath, O_CREAT | O_RDWR | O_APPEND, 0644);
    if (f == NULL)
    {
        retval = -1;
        goto EXITSIPFW_LogAppend;
    }

    /*
     * 原代码这里用cur.sec给time赋值，但cur还没有初始化。
     * 这行很可能存在bug。
     */
    time = ktime_get_real_seconds();

    /*
     * 将时间戳转换为本地时间结构。
     */
    SIPFW_Localtime(&cur, time);

    /*
     * 根据IP头中的协议号查找协议名称。
     */
    for (proto = &sipfw_protocol_name[0];
         proto->ptr != NULL && proto->value != iph->protocol;
         proto++)
        ;

    if (proto->ptr)
    {
        proto_name = proto->ptr;
    }

    if (r->action >= 0 &&
        r->action < SIPFW_ACTION_NUM &&
        sipfw_action_name[r->action].ptr)
    {
        action_name = sipfw_action_name[r->action].ptr;
    }

    /*
     * 格式化日志内容。
     *
     * 注意：iph->saddr和iph->daddr是网络字节序，这里按字节位移打印。
     * 在小端机器上通常能得到点分十进制效果，但更规范可使用%pI4。
     */
    snprintf(buff,
             2048,
             "Time: %04d-%02d-%02d "
             "%02d:%02d:%02d  "
             "From %pI4 "
             "To %pI4 "
             "%s PROTOCOL "
             "was %s\n",
             cur.year, cur.mon, cur.mday,
             cur.hour, cur.min, cur.sec,
             &iph->saddr,
             &iph->daddr,
             proto_name,
             action_name);

    /*
     * 把日志字符串写入文件。
     */
    SIPFW_WriteLine(f, buff, strlen(buff));

    /*
     * 关闭日志文件。
     */
	SIPFW_CloseFile(f);

EXITSIPFW_LogAppend:
	kfree(buff);
	return retval;
}

int SIPFW_SaveRuleFile(void)
{
    struct file *f = NULL;
    const struct sipfw_rule_snapshot *snapshot = NULL;
    struct sipfw_rule_record record;
    struct sipfw_rules *rules_copy = NULL;
    ssize_t written = 0;
    int i = 0;
    int j = 0;
    int ret = 0;
    unsigned int rule_count = 0;

    if (cf.RuleFilePath[0] == '\0')
    {
        return 0;
    }

    f = SIPFW_OpenFile(cf.RuleFilePath, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (!f)
    {
        return -ENOENT;
    }

    for (i = 0; i < SIPFW_CHAIN_NUM && !ret; i++)
    {
        rcu_read_lock();
        snapshot = rcu_dereference(sipfw_tables[i].snapshot);
        rule_count = snapshot ? snapshot->count : 0;

        if (rule_count > 0)
        {
            rules_copy = kmemdup(snapshot->rules,
                                 sizeof(*rules_copy) * rule_count,
                                 GFP_ATOMIC);
        }

        rcu_read_unlock();

        if (rule_count == 0)
        {
            continue;
        }

        if (!rules_copy)
        {
            ret = -ENOMEM;
            break;
        }

        for (j = 0; j < rule_count; j++)
        {
            SIPFW_RecordFromRule(&record, &rules_copy[j]);
            written = SIPFW_KernelWrite(f, &record, sizeof(record), &f->f_pos);
            if (written != sizeof(record))
            {
                ret = written < 0 ? (int)written : -EIO;
                break;
            }
        }

        kfree(rules_copy);
        rules_copy = NULL;
    }

    if (rules_copy)
    {
        kfree(rules_copy);
    }

    SIPFW_CloseFile(f);
    return ret;
}

int SIPFW_LoadRuleFile(void)
{
    struct file *f = NULL;
    struct sipfw_rule_record record;
    struct sipfw_rules *rule = NULL;
    struct sipfw_rules **tail = NULL;
    struct sipfw_rule_snapshot *snapshot = NULL;
    struct sipfw_list *l = NULL;
    ssize_t count = 0;
    int i = 0;
    int ret = 0;

    if (cf.RuleFilePath[0] == '\0')
    {
        return 0;
    }

    f = SIPFW_OpenFile(cf.RuleFilePath, O_RDONLY, 0);
    if (!f)
    {
        return 0;
    }

    while ((count = SIPFW_KernelRead(f, &record, sizeof(record), &f->f_pos)) > 0)
    {
        if (count != sizeof(record) ||
            record.chain < 0 ||
            record.chain >= SIPFW_CHAIN_NUM ||
            record.action < 0 ||
            record.action >= SIPFW_ACTION_NUM)
        {
            ret = -EINVAL;
            goto out_error;
        }

        rule = kzalloc(sizeof(*rule), GFP_KERNEL);
        if (!rule)
        {
            ret = -ENOMEM;
            goto out_error;
        }

        SIPFW_RuleFromRecord(rule, &record);

        l = &sipfw_tables[rule->chain];
        tail = &l->rule;
        while (*tail != NULL)
        {
            tail = &(*tail)->next;
        }

        *tail = rule;
        l->number++;
        rule = NULL;
    }

    if (count < 0)
    {
        ret = (int)count;
        goto out_error;
    }

    for (i = 0; i < SIPFW_CHAIN_NUM; i++)
    {
        l = &sipfw_tables[i];
        if (l->number == 0)
        {
            continue;
        }

        snapshot = SIPFW_AllocRuleSnapshot(l->number, GFP_KERNEL);
        if (!snapshot)
        {
            ret = -ENOMEM;
            goto out_error;
        }

        write_lock_bh(&l->lock);
        SIPFW_PublishRuleSnapshotLocked(l, snapshot);
        write_unlock_bh(&l->lock);
    }

    SIPFW_CloseFile(f);
    return 0;

out_error:
    if (rule)
    {
        kfree(rule);
    }

    SIPFW_CloseFile(f);
    SIPFW_ListDestroy();
    return ret;
}


/*
 * SIPFW_HandleConf
 *
 * 功能：
 *   从/etc/sipfw.conf配置文件中读取防火墙配置。
 *
 * 当前支持的配置项：
 *   DefaultAction ：默认动作，例如ACCEPT或DROP。
 *   RulesFile     ：规则文件路径。
 *   LogFile       ：日志文件路径。
 *
 * 返回值：
 *   成功：返回0。
 *   失败：返回-1。
 *
 * 配置文件示例：
 *   DefaultAction ACCEPT
 *   RulesFile /etc/sipfw.rules
 *   LogFile /var/log/sipfw.log
 */
int SIPFW_HandleConf(void)
{
    int retval = 0, count;
    char *pos = NULL;
    struct file *f = NULL;
    char line[256];

    DBGPRINT("==>SIPFW_HandleConf\n");

    /*
     * 打开配置文件。
     *
     * 注意：读取配置通常不应该使用O_APPEND，O_RDONLY或O_RDWR更合适。
     * 这里带O_CREAT会在配置不存在时创建空文件，是否符合预期要看项目设计。
     */
    f = SIPFW_OpenFile("/etc/sipfw.conf", O_RDONLY, 0);
    if (f == NULL)
    {
        retval = 0;
        goto EXITSIPFW_HandleConf;
    }

    /*
     * 逐行读取配置文件。
     */
    while ((count = SIPFW_ReadLine(f, line, 256)) >= 0)
    {
        strim(line);

        /*
         * pos指向当前行的开头。
         */
        pos = line;

        if (*pos == '\0' || *pos == '#')
        {
            continue;
        }

        while (isspace(*pos))
        {
            pos++;
        }

        /*
         * 解析DefaultAction配置项。
         */
        if (!strncmp(pos, "DefaultAction", 13))
        {
            /*
             * 跳过"DefaultAction"和后面的一个分隔符。
             */
            pos += 13;
            while (*pos == '=' || isspace(*pos))
            {
                pos++;
            }

            if (!strncmp(pos, "ACCEPT", 6))
            {
                cf.DefaultAction = SIPFW_ACTION_ACCEPT;
            }
            else if (!strncmp(pos, "DROP", 4))
            {
                cf.DefaultAction = SIPFW_ACTION_DROP;
            }
        }
        /*
         * 解析RulesFile配置项。
         */
        else if (!strncmp(pos, "RulesFile", 9))
        {
            pos += 9;
            while (*pos == '=' || isspace(*pos))
            {
                pos++;
            }

            /*
             * 保存规则文件路径。
             *
             * 注意：strcpy不检查目标缓冲区大小，存在溢出风险。
             * 更稳妥应使用strscpy或strncpy，并确保cf.RuleFilePath容量足够。
             */
            strscpy(cf.RuleFilePath, pos, sizeof(cf.RuleFilePath));
        }
        /*
         * 解析LogFile配置项。
         */
        else if (!strncmp(pos, "LogFile", 7))
        {
            pos += 7;
            while (*pos == '=' || isspace(*pos))
            {
                pos++;
            }

            /*
             * 保存日志文件路径。
             *
             * 注意：这里同样存在strcpy溢出风险。
             */
            strscpy(cf.LogFilePath, pos, sizeof(cf.LogFilePath));
        }
    }

    /*
     * 关闭配置文件。
     */
    SIPFW_CloseFile(f);

EXITSIPFW_HandleConf:
    DBGPRINT("<==SIPFW_HandleConf\n");
    return retval;
}
