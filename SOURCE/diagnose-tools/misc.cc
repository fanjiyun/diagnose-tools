:/*
 * Linux内核诊断工具--用户态杂项函数
 *
 * Copyright (C) 2020 Alibaba Ltd.
 *
 * 作者: Baoyou Xie <baoyou.xie@linux.alibaba.com>
 *
 * License terms: GNU General Public License (GPL) version 3
 *
 */

#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>

#include <sys/time.h>
#include <string.h>
#include <stdio.h>     /* for printf */
#include <stdlib.h>    /* for exit */

#include "internal.h"
#include "symbol.h"
#include "json/json.h"
#include <iostream>
#include <fstream>
#include <syslog.h>
#include <sys/utsname.h>

#include "unwind.h"

#include "curl/curl.h"

using namespace std;

class pid_cmdline pid_cmdline;
static string unknow_symbol("UNKNOWN");

void pid_cmdline::clear(void)
{
	cmdlines.clear();
}

std::string & pid_cmdline::get_pid_cmdline(int pid)
{
	if (cmdlines.count(pid) == 0) {
		int i;
		char buf[255];
		char file[255];
		std::fstream ifs;

		snprintf(file, sizeof(file), "/proc/%d/cmdline", pid);
		ifs.open(file, ios::binary | ios::in);
		ifs.getline(buf, 255);
		for (i = 0; i < ifs.gcount() && i < 255; i++) {
			if (buf[i] < ' ') {
				buf[i] = ' ';
			}
		}

		cmdlines[pid] = buf;
	}

	return cmdlines[pid];
}

void diag_printf_time(struct diag_timespec *tv)
{
	printf("    时间：[%lu:%lu].\n",
		tv->tv_sec, tv->tv_usec);
}

void diag_printf_inode(struct diag_inode_detail *inode)
{
	printf("    INODE信息：\n");
	printf("        编号：   %lu\n", inode->inode_number);
	printf("        MODE：   %lx\n", inode->inode_mode);
	printf("        LINK：   %lu\n", inode->inode_nlink);
	printf("        REF：    %lu\n", inode->inode_count);
	printf("        SIZE：   %lu\n", inode->inode_size);
	printf("        BLOCKS： %lu\n", inode->inode_blocks);
	printf("        BLK-BYTES： %lu\n", inode->inode_block_bytes);
}

void diag_printf_task(struct diag_task_detail *task)
{
	printf("    进程信息： [%s / %s]， PID： %d / %d\n",
		task->cgroup_buf, task->comm,
		task->tgid, task->pid);
}

void diag_printf_proc_chains(struct diag_proc_chains_detail *proc_chains,
	int reverse, int detail)
{
	int i;

	printf("    进程链信息：\n");
	if (reverse) {
		for (i = PROCESS_CHAINS_COUNT - 1; i >= 0; i--) {
			if (proc_chains->chains[i][0] == 0)
				continue;
			if (proc_chains->full_argv[i] == 0 && detail) {
				string cmdline = pid_cmdline.get_pid_cmdline(proc_chains->tgid[i]);
				printf("#^        0xffffffffffffff %s (UNKNOWN)\n", cmdline.c_str());
			} else {
				printf("#^        0xffffffffffffff %s (UNKNOWN)\n", proc_chains->chains[i]);
			}
		}
	} else {
		for (i = 0; i < PROCESS_CHAINS_COUNT; i++) {
			if (proc_chains->chains[i][0] == 0)
				break;
			if (proc_chains->full_argv[i] == 0 && detail) {
				string cmdline = pid_cmdline.get_pid_cmdline(proc_chains->tgid[i]);
				if (cmdline.length() > 0)
					printf("#^        0xffffffffffffff %s (UNKNOWN)\n", cmdline.c_str());
				else
					printf("#^        0xffffffffffffff %s (UNKNOWN)\n", proc_chains->chains[i]);
			} else {
				printf("#^        0xffffffffffffff %s (UNKNOWN)\n", proc_chains->chains[i]);
			}
		}
	}
}

void diag_printf_proc_chains(struct diag_proc_chains_detail *proc_chains, int reverse)
{
	diag_printf_proc_chains(proc_chains, reverse, 1);
}

void diag_printf_proc_chains(struct diag_proc_chains_detail *proc_chains)
{
	diag_printf_proc_chains(proc_chains, 0);
}

void diag_printf_kern_stack(struct diag_kern_stack_detail *kern_stack, int reverse)
{
	int i;
    symbol sym;

	printf("    内核态堆栈：\n");
	if (reverse) {
		for (i = BACKTRACE_DEPTH - 1; i >= 0; i--) {
			if (kern_stack->stack[i] == (size_t)-1 || kern_stack->stack[i] == 0) {
				continue;
			}
			sym.reset(kern_stack->stack[i]);
			if (g_symbol_parser.find_kernel_symbol(sym)) {
				printf("#@        0x%lx %s ([kernel.kallsyms])\n",
					kern_stack->stack[i],
					sym.name.c_str());
			} else {
				printf("#@        0x%lx %s\n",
					kern_stack->stack[i],
					"UNKNOWN");
			}
		}
	} else {
		for (i = 0; i < BACKTRACE_DEPTH; i++) {
			if (kern_stack->stack[i] == (size_t)-1 || kern_stack->stack[i] == 0) {
				break;
			}
			sym.reset(kern_stack->stack[i]);
			if (g_symbol_parser.find_kernel_symbol(sym)) {
				printf("#@        0x%lx %s ([kernel.kallsyms])\n",
					kern_stack->stack[i],
					sym.name.c_str());
			} else {
				printf("#@        0x%lx %s\n",
					kern_stack->stack[i],
					"UNKNOWN");
			}
		}
	}
}

void diag_printf_kern_stack(struct diag_kern_stack_detail *kern_stack)
{
	diag_printf_kern_stack(kern_stack, 0);
}

void diag_printf_user_stack(int pid, int ns_pid, const char *comm,
	struct diag_user_stack_detail *user_stack, int attach, int reverse)
{
	int i;
	symbol sym;
	elf_file file;
	string symbol;
	printf("    用户态堆栈：\n");
	if (reverse) {
		for (i = BACKTRACE_DEPTH - 1; i >= 0; i--) {
			if (user_stack->stack[i] == (size_t)-1 || user_stack->stack[i] == 0) {
				continue;
			}

			if (g_symbol_parser.user_symbol == 0) {
				printf("#~        0x%lx 0x%lx ([symbol])\n",
							user_stack->stack[i],
							user_stack->stack[i]);
				continue;
			}

			sym.reset(user_stack->stack[i]);
			if (attach) {
				init_java_env("/tmp/libperfmap.so", pid, ns_pid, comm, g_symbol_parser.get_java_procs());
			}

			if (g_symbol_parser.find_symbol_in_cache(pid, user_stack->stack[i], symbol)) {
				printf("#~        0x%lx %s ([symbol])\n",
						user_stack->stack[i],
						symbol.c_str());
				continue;
			}

		    if (g_symbol_parser.get_symbol_info(pid, sym, file)) {
				if (g_symbol_parser.find_elf_symbol(sym, file, pid, ns_pid)) {
					printf("#~        0x%lx %s ([symbol])\n",
						user_stack->stack[i],
						sym.name.c_str());
					g_symbol_parser.putin_symbol_cache(pid, user_stack->stack[i], sym.name);
				} else {
					printf("#~        0x%lx %s ([symbol])\n",
						user_stack->stack[i],
						"UNKNOWN");
					g_symbol_parser.putin_symbol_cache(pid, user_stack->stack[i], unknow_symbol);
				}
			} else {
				printf("#~        0x%lx %s ([symbol])\n",
					user_stack->stack[i],
					"UNKNOWN");
				g_symbol_parser.putin_symbol_cache(pid, user_stack->stack[i], unknow_symbol);
			}
		}
	} else {
		for (i = 0; i < BACKTRACE_DEPTH; i++) {
			//diag_track_memory(1);
			if (user_stack->stack[i] == (size_t)-1 || user_stack->stack[i] == 0) {
				break;
			}

			if (g_symbol_parser.user_symbol == 0) {
				printf("#~        0x%lx 0x%lx ([symbol])\n",
							user_stack->stack[i],
							user_stack->stack[i]);
				continue;
			}

			sym.reset(user_stack->stack[i]);
			//diag_track_memory(2);
			if (attach) {
				init_java_env("/tmp/libperfmap.so", pid, ns_pid, comm, g_symbol_parser.get_java_procs());
			}
			//diag_track_memory(3);

			if (g_symbol_parser.find_symbol_in_cache(pid, user_stack->stack[i], symbol)) {
				printf("#~        0x%lx %s ([symbol])\n",
						user_stack->stack[i],
						symbol.c_str());
				continue;
			}

			if (g_symbol_parser.get_symbol_info(pid, sym, file)) {
				//diag_track_memory(4);
				if (g_symbol_parser.find_elf_symbol(sym, file, pid, ns_pid)) {
					printf("#~        0x%lx %s ([symbol])\n",
						user_stack->stack[i],
						sym.name.c_str());
					g_symbol_parser.putin_symbol_cache(pid, user_stack->stack[i], sym.name);
				} else {
					printf("#~        0x%lx %s ([symbol])\n",
						user_stack->stack[i],
						"UNKNOWN");
					g_symbol_parser.putin_symbol_cache(pid, user_stack->stack[i], unknow_symbol);
				}
				//diag_track_memory(5);
			} else {
				//diag_track_memory(4);
				printf("#~        0x%lx %s ([symbol])\n",
					user_stack->stack[i],
					"UNKNOWN");
				g_symbol_parser.putin_symbol_cache(pid, user_stack->stack[i], unknow_symbol);
			}
		}
	}
}

void diag_printf_user_stack(int pid, int ns_pid, const char *comm,
	struct diag_user_stack_detail *user_stack, int attach)
{
	diag_printf_user_stack(pid, ns_pid, comm, user_stack, attach, 0);
}

void diag_printf_user_stack(int pid, int ns_pid, const char *comm,
	struct diag_user_stack_detail *user_stack)
{
	diag_printf_user_stack(pid, ns_pid, comm, user_stack, 1);
}

static int unwind_frame_callback(struct unwind_entry *entry, void *arg)
{
    symbol sym;
	string symbol;
    elf_file file;

    sym.reset(entry->ip);

	if (g_symbol_parser.find_symbol_in_cache(entry->pid, entry->ip, symbol)) {
		printf("#~        0x%lx %s ([symbol])\n",
				entry->ip,
				symbol.c_str());
		return 0;
	}

    if (g_symbol_parser.get_symbol_info(entry->pid, sym, file)) {
        if (g_symbol_parser.find_elf_symbol(sym, file, entry->pid, entry->pid_ns)) {
			printf("#~        0x%lx %s ([symbol])\n", entry->ip, sym.name.c_str());
			g_symbol_parser.putin_symbol_cache(entry->pid, entry->ip, sym.name);
        } else {
            printf("#~        0x%lx %s ([symbol])\n", entry->ip, "(unknown)[symbol]");
			g_symbol_parser.putin_symbol_cache(entry->pid, entry->ip, unknow_symbol);
        }
    } else {
        printf("#~        0x%lx %s ([symbol])\n", entry->ip, "(unknown)[vma,elf]");
		g_symbol_parser.putin_symbol_cache(entry->pid, entry->ip, unknow_symbol);
    }

    return 0;
}

void diag_printf_raw_stack(int pid, int ns_pid, const char *comm,
	struct diag_raw_stack_detail *raw_stack, int attach)
{
    struct perf_sample stack_sample;
    entry_cb_arg_t unwind_arg;
    static u64 regs_buf[3];

	printf("    用户态堆栈SP：%lx, BP:%lx, IP:%lx\n",
			raw_stack->sp, raw_stack->bp, raw_stack->ip);
	stack_sample.user_stack.offset = 0;
	stack_sample.user_stack.size = raw_stack->stack_size;
	stack_sample.user_stack.data = (char *)&raw_stack->stack[0];
	stack_sample.user_regs.regs = regs_buf;
	stack_sample.user_regs.regs[PERF_REG_IP] = raw_stack->ip;
	stack_sample.user_regs.regs[PERF_REG_SP] = raw_stack->sp;
	stack_sample.user_regs.regs[PERF_REG_BP] = raw_stack->bp;
	unwind__get_entries(unwind_frame_callback, &unwind_arg, &g_symbol_parser,
			pid, ns_pid,
			&stack_sample);
}

void diag_printf_raw_stack(int pid, int ns_pid, const char *comm,
	struct diag_raw_stack_detail *raw_stack)
{
	diag_printf_raw_stack(pid, ns_pid, comm, raw_stack, 1);
}

struct unwind_cb_arg {
	unsigned long *stack;
	int stack_deeps;
	int index;
};

static int unwind_frame_callback_2(struct unwind_entry *entry, void *arg)
{
	struct unwind_cb_arg *cb_arg = (struct unwind_cb_arg *)arg;

	if (cb_arg->index >= cb_arg->stack_deeps)
		return 0;

	cb_arg->stack[cb_arg->index] = entry->ip;
	cb_arg->index++;

	return 0;
}

void diag_unwind_raw_stack(int pid, int ns_pid,
	struct diag_raw_stack_detail *raw_stack, unsigned long stack[BACKTRACE_DEPTH])
{
    struct perf_sample stack_sample;
    static u64 regs_buf[3];
	struct unwind_cb_arg cb_arg;

	memset(&cb_arg, 0, sizeof(cb_arg));
	cb_arg.stack_deeps = BACKTRACE_DEPTH;
	cb_arg.stack = stack;

	stack_sample.user_stack.offset = 0;
	stack_sample.user_stack.size = raw_stack->stack_size;
	stack_sample.user_stack.data = (char *)&raw_stack->stack[0];
	stack_sample.user_regs.regs = regs_buf;
	stack_sample.user_regs.regs[PERF_REG_IP] = raw_stack->ip;
	stack_sample.user_regs.regs[PERF_REG_SP] = raw_stack->sp;
	stack_sample.user_regs.regs[PERF_REG_BP] = raw_stack->bp;
	unwind__get_entries(unwind_frame_callback_2, (void *)&cb_arg, &g_symbol_parser,
			pid, ns_pid,
			&stack_sample);
}

void diag_sls_time(struct diag_timespec *tv, Json::Value &owner)
{
	owner["tv_sec"] = Json::Value(tv->tv_sec);
	owner["tv_usec"] = Json::Value(tv->tv_usec);
}

void diag_sls_task(struct diag_task_detail *tsk_info, Json::Value &task)
{
	task["cgroup"] = Json::Value(tsk_info->cgroup_buf);
	task["pid"] = Json::Value(tsk_info->pid);
	task["tgid"] = Json::Value(tsk_info->tgid);
	task["container_pid"] = Json::Value(tsk_info->container_pid);
	task["container_tgid"] = Json::Value(tsk_info->container_tgid);
	task["task_type"] = Json::Value(tsk_info->task_type);
	task["user_mode"] = Json::Value(tsk_info->user_mode);
	if (0 == tsk_info->state) {
		task["state"] = Json::Value("R");
	} else if (tsk_info->state & 2) {
		task["state"] = Json::Value("D");
	} else {
		task["state"] = Json::Value("S");
	}
	task["syscallno"] = Json::Value(tsk_info->syscallno);
	task["sys_task"] = Json::Value(tsk_info->sys_task);
	task["comm"] = Json::Value(tsk_info->comm);
}

int log_config(char *arg, char *sls_file, int *p_syslog_enabled)
{
	char log_option[2][256];
	int ret_split1;
	int ret_split2;
	int ret;
	int i;

	ret = sscanf(arg, "%[^','],%[^',']", log_option[0], log_option[1]);
	if ((0 > ret) || (2 < ret))
		return 0;

	for (i = 0; i < ret; i++)
	{
		ret_split1=sscanf(log_option[i], "sls=%s", sls_file);
		ret_split2=sscanf(log_option[i], "syslog=%d", p_syslog_enabled);
		if ((1 != ret_split1) && (1 != ret_split2))
		{
			continue;
		}
	}

	//if not set sls-file or syslog, return;
	if ((*sls_file == '\0') && (0 == *p_syslog_enabled))
	{
		return 0;
	}

	return 1;
}

void write_syslog(int enabled, const char mod[], struct diag_timespec *tv, unsigned long id, int seq, Json::Value &root)
{
	std::string str_log;
	stringstream ss;

	if (1 != enabled)
	{
		return;
	}

	ss.str("");
	ss << "diagnose-tools | " << mod << " | ";
	ss << tv->tv_sec << "." << tv->tv_usec << " | ";
	ss << id << " | ";
	ss << seq << " | ";

	str_log.append(ss.str());
	str_log.append(root.toStyledString());
	syslog(LOG_DEBUG, "%s", str_log.c_str());

	return;
}

void write_file(char *sls_file, const char mod[], struct diag_timespec *tv, unsigned long id, int seq, Json::Value &root)
{
	ofstream os;
	Json::StreamWriterBuilder builder;
	builder.settings_["indentation"] = " ";
	std::unique_ptr<Json::StreamWriter> writer(builder.newStreamWriter());
	writer->newline = false;

	if (*sls_file == '\0')
	{
		return;
	}

	os.open(sls_file, std::ios::out | std::ios::app);
	if (1 != os.is_open())
	{
		return;
	}

	os << "diagnose-tools | "<< mod << " | ";
	os << tv->tv_sec << "." << tv->tv_usec << " | ";
	os << id << " | ";
	os << seq << " | ";
	writer->write(root, &os);
	os << endl;

	return;
}

void diag_ip_addr_to_str(unsigned char *ip_addr,const char type[], Json::Value &root)
{
	stringstream ss;

	ss.str("");
	ss << (unsigned int)(ip_addr[0]) << ".";
	ss << (unsigned int)(ip_addr[1]) << ".";
	ss << (unsigned int)(ip_addr[2]) << ".";
	ss << (unsigned int)(ip_addr[3]);
	root[type] = Json::Value(ss.str());

	return;
}

static string& replace_all(string& str, const string& old_value, const string& new_value)
{
	while(true) {
		string::size_type pos(0);
		if((pos=str.find(old_value)) != string::npos)
		{
			str.replace(pos,old_value.length(),new_value);
		} else {
			break;
		}
	}

	return str;
}

void diag_sls_proc_chains(struct diag_proc_chains_detail *proc_chains, Json::Value &task)
{
	int i;
	string cmd;

	for (i = 0; i < PROCESS_CHAINS_COUNT; i++) {
		if (proc_chains->chains[i][0] == 0)
			break;
		if (proc_chains->full_argv[i] == 0) {
			cmd = pid_cmdline.get_pid_cmdline(proc_chains->tgid[i]);
		} else {
			cmd = proc_chains->chains[i];
		}
		replace_all(cmd, "|", "!");
		task["proc_chains"].append(cmd);
	};

	if (i == 0)
		task["proc_chains"] = "";
}

void diag_sls_kern_stack(struct diag_kern_stack_detail *kern_stack, Json::Value &task)
{
	int i;
    symbol sym;
    elf_file file;
	char buf[255];

	for (i = 0; i < BACKTRACE_DEPTH; i++) {
		if (kern_stack->stack[i] == (size_t)-1 || kern_stack->stack[i] == 0) {
			continue;
		}
		sym.reset(kern_stack->stack[i]);
		if (g_symbol_parser.find_kernel_symbol(sym)) {
			snprintf(buf, 255, "%s", sym.name.c_str());
		} else {
			snprintf(buf, 255, "%s", "(unknown)");
		}
		task["kern_stack"].append(buf);
	}
}

void diag_sls_user_stack(pid_t pid, pid_t ns_pid, const char *comm,
	struct diag_user_stack_detail *user_stack, Json::Value &task, int attach)
{
	int i;
	symbol sym;
	string symbol;
	elf_file file;
	char buf[255];

	for (i = 0; i < BACKTRACE_DEPTH; i++) {
		if (user_stack->stack[i] == (size_t)-1 || user_stack->stack[i] == 0) {
			continue;
		}

		if (g_symbol_parser.user_symbol == 0) {
			snprintf(buf, 255, "%lx", user_stack->stack[i]);
			task["user_stack"].append(buf);
			continue;
		}

		sym.reset(user_stack->stack[i]);
		if (attach) {
			init_java_env("/tmp/libperfmap.so", pid, ns_pid, comm, g_symbol_parser.get_java_procs());
		}

		if (g_symbol_parser.find_symbol_in_cache(pid, user_stack->stack[i], symbol)) {
			snprintf(buf, 255, "%s", symbol.c_str());
			task["user_stack"].append(buf);
			continue;
		}

		if (g_symbol_parser.get_symbol_info(pid, sym, file)) {
			if (g_symbol_parser.find_elf_symbol(sym, file, pid, ns_pid)) {
				snprintf(buf, 255, "%s", sym.name.c_str());
				g_symbol_parser.putin_symbol_cache(pid, user_stack->stack[i], sym.name);
			} else {
				snprintf(buf, 255, "%s", "UNKNOWN");
				g_symbol_parser.putin_symbol_cache(pid, user_stack->stack[i], unknow_symbol);
			}
		} else {
			snprintf(buf, 255, "%s", "UNKNOWN");
			g_symbol_parser.putin_symbol_cache(pid, user_stack->stack[i], unknow_symbol);
		}
		task["user_stack"].append(buf);
	}
}

void diag_sls_user_stack(pid_t pid, pid_t ns_pid, const char *comm,
	struct diag_user_stack_detail *user_stack, Json::Value &task)
{
	diag_sls_user_stack(pid, ns_pid, comm, user_stack, task, 1);
}

void diag_sls_inode(struct diag_inode_detail *inode, Json::Value &root)
{
	stringstream ss;

	root["inode_number"] = Json::Value(inode->inode_number);

	ss << std::hex << inode->inode_mode;
	root["mode"] = Json::Value(ss.str());

	root["link"] = Json::Value(inode->inode_nlink);
	root["ref"] = Json::Value(inode->inode_count);
	root["size"] = Json::Value(inode->inode_size);
	root["blocks"] = Json::Value(inode->inode_blocks);
	root["block_bytes"] = Json::Value(inode->inode_block_bytes);
}

int diag_syscall(const char func[])
{
	ofstream os;

	os.open("/proc/ali-linux/diagnose/controller", std::ios::out);
	if (os.is_open()) {
		os << "syscall " << func << endl;
		os.close();
	} else {
		return 0;
	}

	return 1;
}

int diag_activate(const char func[])
{
	ofstream os;
	if ( !strcmp(func, "reboot"))
		diag_syscall("on");

	os.open("/proc/ali-linux/diagnose/controller", std::ios::out);
	if (os.is_open()) {
		os << "activate " << func << endl;
		os.close();
	} else {
		return 0;
	}

	return 1;
}

int diag_deactivate(const char func[])
{
	ofstream os;

	os.open("/proc/ali-linux/diagnose/controller", std::ios::out);
	if (os.is_open()) {
		os << "deactivate " << func << endl;
		os.close();
	} else {
		return -EINVAL;
	}

	return 0;
}

static int big_little_endian(void)
{
	int data = 0x1;

	if (*((char *)&data) == 0x1)
		return LITTLE_ENDIAN;

	return BIG_ENDIAN;
}

unsigned int ipstr2int(const char *ipstr)
{
	unsigned int a, b, c, d;
	unsigned int ip = 0;
	int count;

	count = sscanf(ipstr, "%u.%u.%u.%u", &a, &b, &c, &d);
	if (count == 4) {
		a = (a << 24);
		b = (b << 16);
		c = (c << 8);
		d = (d << 0);
		ip = a | b | c | d;

		return ip;
	} else {
		return 0;
	}
}

char *int2ipstr(const unsigned int ip, char *ipstr, const unsigned int ip_str_len)
{
	unsigned int len;

	if (big_little_endian() == LITTLE_ENDIAN)
		len = snprintf(ipstr, ip_str_len, "%u.%u.%u.%u",
				(unsigned char) * ((char *)(&ip) + 3),
				(unsigned char) * ((char *)(&ip) + 2),
				(unsigned char) * ((char *)(&ip) + 1),
				(unsigned char) * ((char *)(&ip) + 0));
	else
		len = snprintf(ipstr, ip_str_len, "%u.%u.%u.%u",
				(unsigned char) * ((char *)(&ip) + 0),
				(unsigned char) * ((char *)(&ip) + 1),
				(unsigned char) * ((char *)(&ip) + 2),
				(unsigned char) * ((char *)(&ip) + 3));

	if (len < ip_str_len)
		return ipstr;
	else
		return NULL;
}

int linux_2_6_x = 0;

int is_linux_2_6_x(void)
{
   struct utsname utsname;

    uname(&utsname);

	if (strncmp(utsname.release, "2.6.", 4) == 0) {
		return 1;
    }

    return 0;
}

#include <sys/time.h>
extern "C" {
void diag_gettimeofday(struct diag_timespec *tv, struct timezone *tz)
{
	struct timeval ts;
	
	gettimeofday(&ts, tz);
	tv->tv_sec = ts.tv_sec;
	tv->tv_usec = ts.tv_usec;
}
}

void load_str_from_proc(string &file, string &res)
{
    FILE *fp;
    char buf[256];

    res = "";
    fp = fopen(file.c_str(), "r");
    if (!fp) {
        perror("打开文件失败");
        return;
    }

    while (fgets(buf, sizeof(buf), fp)) {
        res += buf;
    }

    fclose(fp);
}

