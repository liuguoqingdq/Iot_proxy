#ifndef __KERNEL__
#define __KERNEL__
#endif /* __KERNEL__ */

#ifndef MODULE
#define MODULE
#endif /* MODULE */

#include "sipfw.h"

/*
 * 最大缓冲区长度。
 * PAGE_SIZE通常是4096字节，用来保存用户通过/proc写入的数据。
 */
#define MAX_COOKIE_LENGTH PAGE_SIZE


/*
 * /proc目录和文件项指针。
 *
 * sipfw_proc_dir            ：/proc/sipfw目录
 * sipfw_proc_info           ：/proc/sipfw/information，显示防火墙总体信息
 * sipfw_proc_defaultaction  ：/proc/sipfw/defaultaction，查看/修改默认动作
 * sipfw_proc_logpause       ：/proc/sipfw/logpause，查看/修改日志暂停状态
 * sipfw_proc_invalid        ：/proc/sipfw/invalid，查看/修改防火墙启停状态
 * proc_net                  ：父目录指针，这里原代码没有初始化，实际比较可疑
 */
static struct proc_dir_entry *sipfw_proc_dir;              /* PROC目录 */
static struct proc_dir_entry *sipfw_proc_info;             /* 防火墙信息文件 */
static struct proc_dir_entry *sipfw_proc_defaultaction;    /* 默认动作文件 */
static struct proc_dir_entry *sipfw_proc_logpause;         /* 日志暂停文件 */
static struct proc_dir_entry *sipfw_proc_invalid;          /* 防火墙启停文件 */


/*
 * 用于暂存用户写入/proc文件的数据。
 *
 * 用户态通过echo等方式写入/proc文件时，
 * 内核先通过copy_from_user()把数据复制到cookie_pot中，
 * 然后再用sscanf/strcmp等函数解析。
 */
static char *cookie_pot;

static ssize_t SIPFW_ProcReadInput(const char __user *ubuf, size_t count)
{
    size_t len = 0;

    if (!cookie_pot)
    {
        return -ENOMEM;
    }

    len = min_t(size_t, count, MAX_COOKIE_LENGTH - 1);
    if (copy_from_user(cookie_pot, ubuf, len))
    {
        return -EFAULT;
    }

    cookie_pot[len] = '\0';
    strim(cookie_pot);
    return count;
}


/*
 * 读取防火墙总体信息。
 *
 * 对应：
 *   cat /proc/sipfw/information
 *
 * 返回内容包括：
 *   默认动作
 *   规则文件路径
 *   日志文件路径
 *   当前规则总数
 *   规则命中次数
 *   防火墙是否有效
 */
static ssize_t SIPFW_ProcInfoRead(struct file *file,
	                              char __user *ubuf,
	                              size_t count,
	                              loff_t *ppos)
{
	char *kernel_buf = NULL;
	int len = 0;
	ssize_t ret = 0;

	if (*ppos > 0)
	{
		return 0;
	}

	kernel_buf = kzalloc(1024, GFP_KERNEL);
	if (!kernel_buf)
	{
		return -ENOMEM;
	}

    /*
     * 将防火墙状态格式化到内核缓冲区。
     *
     * sipfw_action_name[cf.DefaultAction].ptr：
     *   根据默认动作编号获取动作名称，比如ACCEPT或DROP。
     *
     * sipfw_tables[0/1/2].number：
     *   分别统计INPUT、OUTPUT、FORWARD三条链的规则数量。
     */
    len = sprintf(kernel_buf,
                  "DefaultAction:%s\n"
                  "RulesFile:%s\n"
                  "LogFile:%s\n"
                  "RulesNumber:%d\n"
                  "HitNumber:%d\n"
                  "FireWall:%s\n",
                  (char *)sipfw_action_name[cf.DefaultAction].ptr,
                  cf.RuleFilePath,
                  cf.LogFilePath,
                  sipfw_tables[0].number +
                  sipfw_tables[1].number +
                  sipfw_tables[2].number,
                  cf.HitNumber,
                  cf.Invalid ? "INVALID" : "VALID");

    /*
     * 将内核缓冲区内容复制到用户态缓冲区。
     * ubuf是用户态传进来的目标缓冲区，不能直接访问。
     */
    if (len > count)
    {
        len = count;
    }

	if (copy_to_user(ubuf, kernel_buf, len))
	{
		ret = -EFAULT;
		goto out;
	}

	*ppos += len;
	ret = len;

out:
	kfree(kernel_buf);
	return ret;
}


/*
 * 读取日志暂停状态。
 *
 * 对应：
 *   cat /proc/sipfw/logpause
 *
 * 返回：
 *   0：不暂停日志
 *   非0：暂停日志
 */
static ssize_t SIPFW_ProcLogRead(struct file *file,
	                             char __user *ubuf,
	                             size_t count,
	                             loff_t *ppos)
{
	char *kernel_buf = NULL;
	int len = 0;
	ssize_t ret = 0;

	if (*ppos > 0)
	{
		return 0;
	}

	kernel_buf = kzalloc(64, GFP_KERNEL);
	if (!kernel_buf)
	{
		return -ENOMEM;
	}

	len = sprintf(kernel_buf, "%d\n", cf.LogPause);

    if (len > count)
    {
        len = count;
    }

	if (copy_to_user(ubuf, kernel_buf, len))
	{
		ret = -EFAULT;
		goto out;
	}

	*ppos += len;
	ret = len;

out:
	kfree(kernel_buf);
	return ret;
}


/*
 * 写日志暂停状态。
 *
 * 对应：
 *   echo 1 > /proc/sipfw/logpause
 *   echo 0 > /proc/sipfw/logpause
 *
 * 写入的数字会被解析到cf.LogPause中。
 */
static ssize_t SIPFW_ProcLogWrite(struct file *file,
                                  const char __user *ubuf,
                                  size_t count,
                                  loff_t *ppos)
{
    int value = 0;
    ssize_t ret = SIPFW_ProcReadInput(ubuf, count);

    if (ret < 0)
    {
        return ret;
    }

    if (kstrtoint(cookie_pot, 10, &value))
    {
        return -EINVAL;
    }

    cf.LogPause = value;

    return count;
}


/*
 * 读取默认动作。
 *
 * 对应：
 *   cat /proc/sipfw/defaultaction
 *
 * 返回当前默认动作名称：
 *   ACCEPT
 *   DROP
 */
static ssize_t SIPFW_ProcActionRead(struct file *file,
	                                char __user *ubuf,
	                                size_t count,
	                                loff_t *ppos)
{
	char *kernel_buf = NULL;
	int len = 0;
	ssize_t ret = 0;

	if (*ppos > 0)
	{
		return 0;
	}

	kernel_buf = kzalloc(64, GFP_KERNEL);
	if (!kernel_buf)
	{
		return -ENOMEM;
	}

	len = sprintf(kernel_buf,
	                  "%s\n",
                  (char *)sipfw_action_name[cf.DefaultAction].ptr);

    if (len > count)
    {
        len = count;
    }

	if (copy_to_user(ubuf, kernel_buf, len))
	{
		ret = -EFAULT;
		goto out;
	}

	*ppos += len;

	ret = len;

out:
	kfree(kernel_buf);
	return ret;
}


/*
 * 修改默认动作。
 *
 * 对应：
 *   echo ACCEPT > /proc/sipfw/defaultaction
 *   echo DROP   > /proc/sipfw/defaultaction
 *
 * 作用：
 *   当数据包没有匹配任何规则时，
 *   使用cf.DefaultAction作为默认处理动作。
 */
static ssize_t SIPFW_ProcActionWrite(struct file *file,
                                     const char __user *ubuf,
                                     size_t count,
                                     loff_t *ppos)
{
    int i = 0;
    ssize_t ret = SIPFW_ProcReadInput(ubuf, count);

    if (ret < 0)
    {
        return ret;
    }

    for (i = 0; i < SIPFW_ACTION_NUM; i++)
    {
        if (!strcmp(cookie_pot, sipfw_action_name[i].ptr))
        {
            cf.DefaultAction = i;
            return count;
        }
    }

    return -EINVAL;
}


/*
 * 注意：
 * 这个函数名字叫SIPFW_ProcInvalidRead，
 * 按理说它应该是读取cf.Invalid状态。
 *
 * 但是原代码里它使用了copy_from_user()，
 * 并且还在修改cf.DefaultAction。
 *
 * 这明显不对。
 *
 * read函数应该把内核数据复制给用户态，也就是copy_to_user()；
 * write函数才应该copy_from_user()。
 *
 * 所以这个函数大概率是写错了。
 */
static ssize_t SIPFW_ProcInvalidRead(struct file *file,
	                                 char __user *ubuf,
	                                 size_t count,
	                                 loff_t *ppos)
{
	char *kernel_buf = NULL;
	int len = 0;
	ssize_t ret = 0;

    /*
     * 如果已经读过一次，就返回0，表示文件结束。
     * 否则cat /proc/xxx时可能会反复读取。
     */
	if (*ppos > 0)
	{
		return 0;
	}

	kernel_buf = kzalloc(64, GFP_KERNEL);
	if (!kernel_buf)
	{
		return -ENOMEM;
	}

    len = scnprintf(kernel_buf,
                    sizeof(kernel_buf),
                    "%s\n",
                    cf.Invalid ? "INVALID" : "VALID");

    /*
     * 避免用户缓冲区比内核准备的数据还小。
     */
    if (len > count)
    {
        len = count;
    }

	if (copy_to_user(ubuf, kernel_buf, len))
	{
		ret = -EFAULT;
		goto out;
	}

	*ppos += len;

	ret = len;

out:
	kfree(kernel_buf);
	return ret;
}


/*
 * 修改防火墙启停状态。
 *
 * 对应：
 *   echo 1 > /proc/sipfw/invalid
 *   echo 0 > /proc/sipfw/invalid
 *
 * cf.Invalid含义：
 *   0：防火墙有效，正常匹配规则
 *   非0：防火墙无效，hook中直接NF_ACCEPT放行
 */
static ssize_t SIPFW_ProcInvalidWrite(struct file *file,
                                      const char __user *ubuf,
                                      size_t count,
                                      loff_t *ppos)
{
    int value = 0;
    ssize_t ret = SIPFW_ProcReadInput(ubuf, count);

    if (ret < 0)
    {
        return ret;
    }

    if (kstrtoint(cookie_pot, 10, &value))
    {
        return -EINVAL;
    }

    cf.Invalid = value;

    return count;
}


/*
 * /proc/sipfw/information的操作函数。
 * 这里只提供读操作。
 */
static struct proc_ops myops1 =
{
    .proc_read = SIPFW_ProcInfoRead
};


/*
 * /proc/sipfw/defaultaction的操作函数。
 * 支持读和写。
 */
static struct proc_ops myops2 =
{
    .proc_read  = SIPFW_ProcActionRead,
    .proc_write = SIPFW_ProcActionWrite
};


/*
 * /proc/sipfw/logpause的操作函数。
 * 支持读和写。
 */
static struct proc_ops myops3 =
{
    .proc_read  = SIPFW_ProcLogRead,
    .proc_write = SIPFW_ProcLogWrite
};


/*
 * /proc/sipfw/invalid的操作函数。
 * 支持读和写。
 */
static struct proc_ops myops4 =
{
    .proc_read  = SIPFW_ProcInvalidRead,
    .proc_write = SIPFW_ProcInvalidWrite
};


/*
 * PROC虚拟文件初始化函数。
 *
 * 该函数在模块初始化时调用。
 *
 * 主要工作：
 *   1.申请cookie_pot缓冲区
 *   2.创建/proc/sipfw目录
 *   3.创建information/defaultaction/logpause/invalid文件
 *   4.绑定对应的读写操作函数
 */
int SIPFW_Proc_Init(void)
{
    int ret = 0;

    /*
     * 申请并清零cookie_pot。
     * 如果MAX_COOKIE_LENGTH比较大，用vzalloc比vmalloc + memset更简洁。
     */
    cookie_pot = vzalloc(MAX_COOKIE_LENGTH);
    if (!cookie_pot)
    {
        return -ENOMEM;
    }

    /*
     * 创建/proc/sipfw目录。
     *
     * 如果你想创建到/proc/sipfw，parent写NULL。
     * 如果你想创建到/proc/net/sipfw，需要确认proc_net来源是否正确。
     */
    sipfw_proc_dir = proc_mkdir("sipfw", NULL);
    if (!sipfw_proc_dir)
    {
        ret = -ENOMEM;
        goto err_cookie;
    }

    /*
     * 注意：权限要写0644，不要写0x0644。
     */
    sipfw_proc_info = proc_create("information",
                                  0644,
                                  sipfw_proc_dir,
                                  &myops1);
    if (!sipfw_proc_info)
    {
        ret = -ENOMEM;
        goto err_proc;
    }

    sipfw_proc_defaultaction = proc_create("defaultaction",
                                           0644,
                                           sipfw_proc_dir,
                                           &myops2);
    if (!sipfw_proc_defaultaction)
    {
        ret = -ENOMEM;
        goto err_proc;
    }

    sipfw_proc_logpause = proc_create("logpause",
                                      0644,
                                      sipfw_proc_dir,
                                      &myops3);
    if (!sipfw_proc_logpause)
    {
        ret = -ENOMEM;
        goto err_proc;
    }

    sipfw_proc_invalid = proc_create("invalid",
                                     0644,
                                     sipfw_proc_dir,
                                     &myops4);
    if (!sipfw_proc_invalid)
    {
        ret = -ENOMEM;
        goto err_proc;
    }

    return 0;

err_proc:
    /*
     * 删除整个/proc/sipfw目录及其下面已经创建成功的文件。
     */
    proc_remove(sipfw_proc_dir);

    sipfw_proc_dir = NULL;
    sipfw_proc_info = NULL;
    sipfw_proc_defaultaction = NULL;
    sipfw_proc_logpause = NULL;
    sipfw_proc_invalid = NULL;

err_cookie:
    vfree(cookie_pot);
    cookie_pot = NULL;

    return ret;
}


/*
 * PROC虚拟文件清理函数。
 *
 * 模块卸载时调用。
 *
 * 主要工作：
 *   1.删除/proc/sipfw下的各个文件
 *   2.删除/proc/sipfw目录
 *   3.释放cookie_pot缓冲区
 */
void SIPFW_Proc_CleanUp(void)
{
    if (sipfw_proc_dir)
    {
        proc_remove(sipfw_proc_dir);
        sipfw_proc_dir = NULL;
    }

    sipfw_proc_info = NULL;
    sipfw_proc_defaultaction = NULL;
    sipfw_proc_logpause = NULL;
    sipfw_proc_invalid = NULL;

    if (cookie_pot)
    {
        vfree(cookie_pot);
        cookie_pot = NULL;
    }
}
