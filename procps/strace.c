/* vi: set sw=4 ts=4: */
/*
 * Mini strace implementation for busybox-ng
 *
 * Licensed under GPLv2 or later, see the LICENSE file in this source tree.
 */
//config:config STRACE
//config:	bool "strace (trace system calls)"
//config:	default y
//config:	help
//config:	strace is a diagnostic, debugging and instructional userspace
//config:	tracer for Linux.
//applet:IF_STRACE(APPLET(strace, BB_DIR_USR_BIN, BB_SUID_DROP))
//kbuild:lib-$(CONFIG_STRACE) += strace.o
//usage:#define strace_trivial_usage "[-c] [-f] [-s STRSIZE] [-o FILE] [-p PID | PROG [ARGS...]]"
//usage:#define strace_full_usage "\n\n"
//usage:       "Trace system calls and signals\n"
//usage:     "\n	-p PID		Attach to process with PID"
//usage:     "\n	-c		Count time, calls, and errors for each syscall and report summary"
//usage:     "\n	-f		Follow forks"
//usage:     "\n	-o FILE		Write trace output to FILE"
//usage:     "\n	-s STRSIZE	Maximum string length to print (default 32)"
//usage:     "\n	-q		Suppress attach/detach messages"

#include "libbb.h"
#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <asm/unistd.h>
#include <sys/syscall.h>

#ifndef NT_PRSTATUS
# define NT_PRSTATUS 1
#endif

#ifndef __WALL
# define __WALL 0x40000000
#endif

#ifndef PTRACE_O_TRACESYSGOOD
# define PTRACE_O_TRACESYSGOOD 0x00000001
#endif
#ifndef PTRACE_O_TRACEFORK
# define PTRACE_O_TRACEFORK    0x00000002
#endif
#ifndef PTRACE_O_TRACEVFORK
# define PTRACE_O_TRACEVFORK   0x00000004
#endif
#ifndef PTRACE_O_TRACECLONE
# define PTRACE_O_TRACECLONE   0x00000008
#endif

#ifndef PTRACE_EVENT_FORK
# define PTRACE_EVENT_FORK     1
#endif
#ifndef PTRACE_EVENT_VFORK
# define PTRACE_EVENT_VFORK    2
#endif
#ifndef PTRACE_EVENT_CLONE
# define PTRACE_EVENT_CLONE    3
#endif

enum {
	OPT_c = 1 << 0,
	OPT_f = 1 << 1,
	OPT_s = 1 << 2,
	OPT_o = 1 << 3,
	OPT_p = 1 << 4,
	OPT_q = 1 << 5,
};

struct sys_regs {
	long sc_nr;
	long ret;
	unsigned long args[6];
};

struct tracee {
	struct tracee *next;
	pid_t pid;
	int initialized;
	int in_syscall;
	long sc_nr;
	unsigned long sc_args[6];
	char *saved_str[6];
	int str_trunc[6];
	struct timespec start_ts;
};

struct syscall_entry {
	int nr;
	const char *name;
	int nargs;
	unsigned str_mask;
};

#define S_ENTRY(name, nargs, mask) \
	{ __NR_##name, #name, nargs, mask }

static const struct syscall_entry syscall_table[] = {
#ifdef __NR_read
	S_ENTRY(read, 3, (1 << 1)),
#endif
#ifdef __NR_write
	S_ENTRY(write, 3, (1 << 1)),
#endif
#ifdef __NR_open
	S_ENTRY(open, 3, 1),
#endif
#ifdef __NR_close
	S_ENTRY(close, 1, 0),
#endif
#ifdef __NR_stat
	S_ENTRY(stat, 2, 1),
#endif
#ifdef __NR_fstat
	S_ENTRY(fstat, 2, 0),
#endif
#ifdef __NR_lstat
	S_ENTRY(lstat, 2, 1),
#endif
#ifdef __NR_poll
	S_ENTRY(poll, 3, 0),
#endif
#ifdef __NR_lseek
	S_ENTRY(lseek, 3, 0),
#endif
#ifdef __NR_mmap
	S_ENTRY(mmap, 6, 0),
#endif
#ifdef __NR_mprotect
	S_ENTRY(mprotect, 3, 0),
#endif
#ifdef __NR_munmap
	S_ENTRY(munmap, 2, 0),
#endif
#ifdef __NR_brk
	S_ENTRY(brk, 1, 0),
#endif
#ifdef __NR_rt_sigaction
	S_ENTRY(rt_sigaction, 4, 0),
#endif
#ifdef __NR_rt_sigprocmask
	S_ENTRY(rt_sigprocmask, 4, 0),
#endif
#ifdef __NR_rt_sigreturn
	S_ENTRY(rt_sigreturn, 0, 0),
#endif
#ifdef __NR_ioctl
	S_ENTRY(ioctl, 3, 0),
#endif
#ifdef __NR_pread64
	S_ENTRY(pread64, 4, (1 << 1)),
#endif
#ifdef __NR_pwrite64
	S_ENTRY(pwrite64, 4, (1 << 1)),
#endif
#ifdef __NR_readv
	S_ENTRY(readv, 3, 0),
#endif
#ifdef __NR_writev
	S_ENTRY(writev, 3, 0),
#endif
#ifdef __NR_access
	S_ENTRY(access, 2, 1),
#endif
#ifdef __NR_pipe
	S_ENTRY(pipe, 1, 0),
#endif
#ifdef __NR_select
	S_ENTRY(select, 5, 0),
#endif
#ifdef __NR_sched_yield
	S_ENTRY(sched_yield, 0, 0),
#endif
#ifdef __NR_mremap
	S_ENTRY(mremap, 5, 0),
#endif
#ifdef __NR_msync
	S_ENTRY(msync, 3, 0),
#endif
#ifdef __NR_mincore
	S_ENTRY(mincore, 3, 0),
#endif
#ifdef __NR_madvise
	S_ENTRY(madvise, 3, 0),
#endif
#ifdef __NR_dup
	S_ENTRY(dup, 1, 0),
#endif
#ifdef __NR_dup2
	S_ENTRY(dup2, 2, 0),
#endif
#ifdef __NR_pause
	S_ENTRY(pause, 0, 0),
#endif
#ifdef __NR_nanosleep
	S_ENTRY(nanosleep, 2, 0),
#endif
#ifdef __NR_getitimer
	S_ENTRY(getitimer, 2, 0),
#endif
#ifdef __NR_alarm
	S_ENTRY(alarm, 1, 0),
#endif
#ifdef __NR_setitimer
	S_ENTRY(setitimer, 3, 0),
#endif
#ifdef __NR_getpid
	S_ENTRY(getpid, 0, 0),
#endif
#ifdef __NR_sendfile
	S_ENTRY(sendfile, 4, 0),
#endif
#ifdef __NR_socket
	S_ENTRY(socket, 3, 0),
#endif
#ifdef __NR_connect
	S_ENTRY(connect, 3, 0),
#endif
#ifdef __NR_accept
	S_ENTRY(accept, 3, 0),
#endif
#ifdef __NR_sendto
	S_ENTRY(sendto, 6, (1 << 1)),
#endif
#ifdef __NR_recvfrom
	S_ENTRY(recvfrom, 6, (1 << 1)),
#endif
#ifdef __NR_sendmsg
	S_ENTRY(sendmsg, 3, 0),
#endif
#ifdef __NR_recvmsg
	S_ENTRY(recvmsg, 3, 0),
#endif
#ifdef __NR_shutdown
	S_ENTRY(shutdown, 2, 0),
#endif
#ifdef __NR_bind
	S_ENTRY(bind, 3, 0),
#endif
#ifdef __NR_listen
	S_ENTRY(listen, 2, 0),
#endif
#ifdef __NR_getsockname
	S_ENTRY(getsockname, 3, 0),
#endif
#ifdef __NR_getpeername
	S_ENTRY(getpeername, 3, 0),
#endif
#ifdef __NR_socketpair
	S_ENTRY(socketpair, 4, 0),
#endif
#ifdef __NR_setsockopt
	S_ENTRY(setsockopt, 5, 0),
#endif
#ifdef __NR_getsockopt
	S_ENTRY(getsockopt, 5, 0),
#endif
#ifdef __NR_clone
	S_ENTRY(clone, 5, 0),
#endif
#ifdef __NR_fork
	S_ENTRY(fork, 0, 0),
#endif
#ifdef __NR_vfork
	S_ENTRY(vfork, 0, 0),
#endif
#ifdef __NR_execve
	S_ENTRY(execve, 3, 1),
#endif
#ifdef __NR_exit
	S_ENTRY(exit, 1, 0),
#endif
#ifdef __NR_wait4
	S_ENTRY(wait4, 4, 0),
#endif
#ifdef __NR_kill
	S_ENTRY(kill, 2, 0),
#endif
#ifdef __NR_uname
	S_ENTRY(uname, 1, 0),
#endif
#ifdef __NR_fcntl
	S_ENTRY(fcntl, 3, 0),
#endif
#ifdef __NR_flock
	S_ENTRY(flock, 2, 0),
#endif
#ifdef __NR_fsync
	S_ENTRY(fsync, 1, 0),
#endif
#ifdef __NR_fdatasync
	S_ENTRY(fdatasync, 1, 0),
#endif
#ifdef __NR_truncate
	S_ENTRY(truncate, 2, 1),
#endif
#ifdef __NR_ftruncate
	S_ENTRY(ftruncate, 2, 0),
#endif
#ifdef __NR_getdents
	S_ENTRY(getdents, 3, 0),
#endif
#ifdef __NR_getcwd
	S_ENTRY(getcwd, 2, 0),
#endif
#ifdef __NR_chdir
	S_ENTRY(chdir, 1, 1),
#endif
#ifdef __NR_fchdir
	S_ENTRY(fchdir, 1, 0),
#endif
#ifdef __NR_rename
	S_ENTRY(rename, 2, 3),
#endif
#ifdef __NR_mkdir
	S_ENTRY(mkdir, 2, 1),
#endif
#ifdef __NR_rmdir
	S_ENTRY(rmdir, 1, 1),
#endif
#ifdef __NR_creat
	S_ENTRY(creat, 2, 1),
#endif
#ifdef __NR_link
	S_ENTRY(link, 2, 3),
#endif
#ifdef __NR_unlink
	S_ENTRY(unlink, 1, 1),
#endif
#ifdef __NR_symlink
	S_ENTRY(symlink, 2, 3),
#endif
#ifdef __NR_readlink
	S_ENTRY(readlink, 3, 1),
#endif
#ifdef __NR_chmod
	S_ENTRY(chmod, 2, 1),
#endif
#ifdef __NR_fchmod
	S_ENTRY(fchmod, 2, 0),
#endif
#ifdef __NR_chown
	S_ENTRY(chown, 3, 1),
#endif
#ifdef __NR_fchown
	S_ENTRY(fchown, 3, 0),
#endif
#ifdef __NR_lchown
	S_ENTRY(lchown, 3, 1),
#endif
#ifdef __NR_umask
	S_ENTRY(umask, 1, 0),
#endif
#ifdef __NR_gettimeofday
	S_ENTRY(gettimeofday, 2, 0),
#endif
#ifdef __NR_getrlimit
	S_ENTRY(getrlimit, 2, 0),
#endif
#ifdef __NR_getrusage
	S_ENTRY(getrusage, 2, 0),
#endif
#ifdef __NR_sysinfo
	S_ENTRY(sysinfo, 1, 0),
#endif
#ifdef __NR_times
	S_ENTRY(times, 1, 0),
#endif
#ifdef __NR_ptrace
	S_ENTRY(ptrace, 4, 0),
#endif
#ifdef __NR_getuid
	S_ENTRY(getuid, 0, 0),
#endif
#ifdef __NR_syslog
	S_ENTRY(syslog, 3, 0),
#endif
#ifdef __NR_getgid
	S_ENTRY(getgid, 0, 0),
#endif
#ifdef __NR_setuid
	S_ENTRY(setuid, 1, 0),
#endif
#ifdef __NR_setgid
	S_ENTRY(setgid, 1, 0),
#endif
#ifdef __NR_geteuid
	S_ENTRY(geteuid, 0, 0),
#endif
#ifdef __NR_getegid
	S_ENTRY(getegid, 0, 0),
#endif
#ifdef __NR_setpgid
	S_ENTRY(setpgid, 2, 0),
#endif
#ifdef __NR_getppid
	S_ENTRY(getppid, 0, 0),
#endif
#ifdef __NR_getpgrp
	S_ENTRY(getpgrp, 0, 0),
#endif
#ifdef __NR_setsid
	S_ENTRY(setsid, 0, 0),
#endif
#ifdef __NR_setreuid
	S_ENTRY(setreuid, 2, 0),
#endif
#ifdef __NR_setregid
	S_ENTRY(setregid, 2, 0),
#endif
#ifdef __NR_getgroups
	S_ENTRY(getgroups, 2, 0),
#endif
#ifdef __NR_setgroups
	S_ENTRY(setgroups, 2, 0),
#endif
#ifdef __NR_setresuid
	S_ENTRY(setresuid, 3, 0),
#endif
#ifdef __NR_getresuid
	S_ENTRY(getresuid, 3, 0),
#endif
#ifdef __NR_setresgid
	S_ENTRY(setresgid, 3, 0),
#endif
#ifdef __NR_getresgid
	S_ENTRY(getresgid, 3, 0),
#endif
#ifdef __NR_getpgid
	S_ENTRY(getpgid, 1, 0),
#endif
#ifdef __NR_setfsuid
	S_ENTRY(setfsuid, 1, 0),
#endif
#ifdef __NR_setfsgid
	S_ENTRY(setfsgid, 1, 0),
#endif
#ifdef __NR_getsid
	S_ENTRY(getsid, 1, 0),
#endif
#ifdef __NR_capget
	S_ENTRY(capget, 2, 0),
#endif
#ifdef __NR_capset
	S_ENTRY(capset, 2, 0),
#endif
#ifdef __NR_sigaltstack
	S_ENTRY(sigaltstack, 2, 0),
#endif
#ifdef __NR_statfs
	S_ENTRY(statfs, 2, 1),
#endif
#ifdef __NR_fstatfs
	S_ENTRY(fstatfs, 2, 0),
#endif
#ifdef __NR_prctl
	S_ENTRY(prctl, 5, 0),
#endif
#ifdef __NR_arch_prctl
	S_ENTRY(arch_prctl, 2, 0),
#endif
#ifdef __NR_adjtimex
	S_ENTRY(adjtimex, 1, 0),
#endif
#ifdef __NR_setrlimit
	S_ENTRY(setrlimit, 2, 0),
#endif
#ifdef __NR_chroot
	S_ENTRY(chroot, 1, 1),
#endif
#ifdef __NR_sync
	S_ENTRY(sync, 0, 0),
#endif
#ifdef __NR_mount
	S_ENTRY(mount, 5, 7),
#endif
#ifdef __NR_umount2
	S_ENTRY(umount2, 2, 1),
#endif
#ifdef __NR_swapon
	S_ENTRY(swapon, 2, 1),
#endif
#ifdef __NR_swapoff
	S_ENTRY(swapoff, 1, 1),
#endif
#ifdef __NR_reboot
	S_ENTRY(reboot, 4, 0),
#endif
#ifdef __NR_sethostname
	S_ENTRY(sethostname, 2, 1),
#endif
#ifdef __NR_setdomainname
	S_ENTRY(setdomainname, 2, 1),
#endif
#ifdef __NR_gettid
	S_ENTRY(gettid, 0, 0),
#endif
#ifdef __NR_readahead
	S_ENTRY(readahead, 3, 0),
#endif
#ifdef __NR_setxattr
	S_ENTRY(setxattr, 5, 3),
#endif
#ifdef __NR_lsetxattr
	S_ENTRY(lsetxattr, 5, 3),
#endif
#ifdef __NR_fsetxattr
	S_ENTRY(fsetxattr, 5, 2),
#endif
#ifdef __NR_getxattr
	S_ENTRY(getxattr, 4, 3),
#endif
#ifdef __NR_lgetxattr
	S_ENTRY(lgetxattr, 4, 3),
#endif
#ifdef __NR_fgetxattr
	S_ENTRY(fgetxattr, 4, 2),
#endif
#ifdef __NR_time
	S_ENTRY(time, 1, 0),
#endif
#ifdef __NR_futex
	S_ENTRY(futex, 6, 0),
#endif
#ifdef __NR_sched_setaffinity
	S_ENTRY(sched_setaffinity, 3, 0),
#endif
#ifdef __NR_sched_getaffinity
	S_ENTRY(sched_getaffinity, 3, 0),
#endif
#ifdef __NR_epoll_create
	S_ENTRY(epoll_create, 1, 0),
#endif
#ifdef __NR_getdents64
	S_ENTRY(getdents64, 3, 0),
#endif
#ifdef __NR_set_tid_address
	S_ENTRY(set_tid_address, 1, 0),
#endif
#ifdef __NR_restart_syscall
	S_ENTRY(restart_syscall, 0, 0),
#endif
#ifdef __NR_fadvise64
	S_ENTRY(fadvise64, 4, 0),
#endif
#ifdef __NR_clock_settime
	S_ENTRY(clock_settime, 2, 0),
#endif
#ifdef __NR_clock_gettime
	S_ENTRY(clock_gettime, 2, 0),
#endif
#ifdef __NR_clock_getres
	S_ENTRY(clock_getres, 2, 0),
#endif
#ifdef __NR_clock_nanosleep
	S_ENTRY(clock_nanosleep, 4, 0),
#endif
#ifdef __NR_exit_group
	S_ENTRY(exit_group, 1, 0),
#endif
#ifdef __NR_epoll_wait
	S_ENTRY(epoll_wait, 4, 0),
#endif
#ifdef __NR_epoll_ctl
	S_ENTRY(epoll_ctl, 4, 0),
#endif
#ifdef __NR_tgkill
	S_ENTRY(tgkill, 3, 0),
#endif
#ifdef __NR_utimes
	S_ENTRY(utimes, 2, 1),
#endif
#ifdef __NR_waitid
	S_ENTRY(waitid, 5, 0),
#endif
#ifdef __NR_inotify_init
	S_ENTRY(inotify_init, 0, 0),
#endif
#ifdef __NR_inotify_add_watch
	S_ENTRY(inotify_add_watch, 3, 2),
#endif
#ifdef __NR_inotify_rm_watch
	S_ENTRY(inotify_rm_watch, 2, 0),
#endif
#ifdef __NR_openat
	S_ENTRY(openat, 4, 2),
#endif
#ifdef __NR_mkdirat
	S_ENTRY(mkdirat, 3, 2),
#endif
#ifdef __NR_mknodat
	S_ENTRY(mknodat, 4, 2),
#endif
#ifdef __NR_fchownat
	S_ENTRY(fchownat, 5, 2),
#endif
#ifdef __NR_futimesat
	S_ENTRY(futimesat, 3, 2),
#endif
#ifdef __NR_newfstatat
	S_ENTRY(newfstatat, 4, 2),
#endif
#ifdef __NR_unlinkat
	S_ENTRY(unlinkat, 3, 2),
#endif
#ifdef __NR_renameat
	S_ENTRY(renameat, 4, 10),
#endif
#ifdef __NR_linkat
	S_ENTRY(linkat, 5, 10),
#endif
#ifdef __NR_symlinkat
	S_ENTRY(symlinkat, 3, 5),
#endif
#ifdef __NR_readlinkat
	S_ENTRY(readlinkat, 4, 2),
#endif
#ifdef __NR_fchmodat
	S_ENTRY(fchmodat, 3, 2),
#endif
#ifdef __NR_faccessat
	S_ENTRY(faccessat, 3, 2),
#endif
#ifdef __NR_pselect6
	S_ENTRY(pselect6, 6, 0),
#endif
#ifdef __NR_ppoll
	S_ENTRY(ppoll, 5, 0),
#endif
#ifdef __NR_unshare
	S_ENTRY(unshare, 1, 0),
#endif
#ifdef __NR_set_robust_list
	S_ENTRY(set_robust_list, 2, 0),
#endif
#ifdef __NR_get_robust_list
	S_ENTRY(get_robust_list, 3, 0),
#endif
#ifdef __NR_splice
	S_ENTRY(splice, 6, 0),
#endif
#ifdef __NR_tee
	S_ENTRY(tee, 4, 0),
#endif
#ifdef __NR_sync_file_range
	S_ENTRY(sync_file_range, 4, 0),
#endif
#ifdef __NR_vmsplice
	S_ENTRY(vmsplice, 4, 0),
#endif
#ifdef __NR_utimensat
	S_ENTRY(utimensat, 4, 2),
#endif
#ifdef __NR_epoll_pwait
	S_ENTRY(epoll_pwait, 5, 0),
#endif
#ifdef __NR_signalfd
	S_ENTRY(signalfd, 3, 0),
#endif
#ifdef __NR_timerfd_create
	S_ENTRY(timerfd_create, 2, 0),
#endif
#ifdef __NR_eventfd
	S_ENTRY(eventfd, 1, 0),
#endif
#ifdef __NR_fallocate
	S_ENTRY(fallocate, 4, 0),
#endif
#ifdef __NR_timerfd_settime
	S_ENTRY(timerfd_settime, 4, 0),
#endif
#ifdef __NR_timerfd_gettime
	S_ENTRY(timerfd_gettime, 2, 0),
#endif
#ifdef __NR_accept4
	S_ENTRY(accept4, 4, 0),
#endif
#ifdef __NR_signalfd4
	S_ENTRY(signalfd4, 4, 0),
#endif
#ifdef __NR_eventfd2
	S_ENTRY(eventfd2, 2, 0),
#endif
#ifdef __NR_epoll_create1
	S_ENTRY(epoll_create1, 1, 0),
#endif
#ifdef __NR_dup3
	S_ENTRY(dup3, 3, 0),
#endif
#ifdef __NR_pipe2
	S_ENTRY(pipe2, 2, 0),
#endif
#ifdef __NR_inotify_init1
	S_ENTRY(inotify_init1, 1, 0),
#endif
#ifdef __NR_preadv
	S_ENTRY(preadv, 4, 0),
#endif
#ifdef __NR_pwritev
	S_ENTRY(pwritev, 4, 0),
#endif
#ifdef __NR_prlimit64
	S_ENTRY(prlimit64, 4, 0),
#endif
#ifdef __NR_name_to_handle_at
	S_ENTRY(name_to_handle_at, 5, 2),
#endif
#ifdef __NR_open_by_handle_at
	S_ENTRY(open_by_handle_at, 3, 0),
#endif
#ifdef __NR_syncfs
	S_ENTRY(syncfs, 1, 0),
#endif
#ifdef __NR_sendmmsg
	S_ENTRY(sendmmsg, 4, 0),
#endif
#ifdef __NR_setns
	S_ENTRY(setns, 2, 0),
#endif
#ifdef __NR_getcpu
	S_ENTRY(getcpu, 3, 0),
#endif
#ifdef __NR_process_vm_readv
	S_ENTRY(process_vm_readv, 6, 0),
#endif
#ifdef __NR_process_vm_writev
	S_ENTRY(process_vm_writev, 6, 0),
#endif
#ifdef __NR_sched_setattr
	S_ENTRY(sched_setattr, 3, 0),
#endif
#ifdef __NR_sched_getattr
	S_ENTRY(sched_getattr, 4, 0),
#endif
#ifdef __NR_renameat2
	S_ENTRY(renameat2, 5, 10),
#endif
#ifdef __NR_seccomp
	S_ENTRY(seccomp, 3, 0),
#endif
#ifdef __NR_getrandom
	S_ENTRY(getrandom, 3, 0),
#endif
#ifdef __NR_memfd_create
	S_ENTRY(memfd_create, 2, 1),
#endif
#ifdef __NR_bpf
	S_ENTRY(bpf, 3, 0),
#endif
#ifdef __NR_execveat
	S_ENTRY(execveat, 5, 2),
#endif
#ifdef __NR_copy_file_range
	S_ENTRY(copy_file_range, 6, 0),
#endif
#ifdef __NR_statx
	S_ENTRY(statx, 5, 2),
#endif
#ifdef __NR_pidfd_send_signal
	S_ENTRY(pidfd_send_signal, 4, 0),
#endif
#ifdef __NR_io_uring_setup
	S_ENTRY(io_uring_setup, 2, 0),
#endif
#ifdef __NR_io_uring_enter
	S_ENTRY(io_uring_enter, 6, 0),
#endif
#ifdef __NR_io_uring_register
	S_ENTRY(io_uring_register, 4, 0),
#endif
#ifdef __NR_open_tree
	S_ENTRY(open_tree, 3, 2),
#endif
#ifdef __NR_pidfd_open
	S_ENTRY(pidfd_open, 2, 0),
#endif
#ifdef __NR_clone3
	S_ENTRY(clone3, 2, 0),
#endif
#ifdef __NR_close_range
	S_ENTRY(close_range, 3, 0),
#endif
#ifdef __NR_openat2
	S_ENTRY(openat2, 4, 2),
#endif
#ifdef __NR_pidfd_getfd
	S_ENTRY(pidfd_getfd, 3, 0),
#endif
#ifdef __NR_faccessat2
	S_ENTRY(faccessat2, 4, 2),
#endif
#ifdef __NR_process_madvise
	S_ENTRY(process_madvise, 5, 0),
#endif
};

static const struct syscall_entry *lookup_syscall(int nr)
{
	size_t i;
	for (i = 0; i < ARRAY_SIZE(syscall_table); i++) {
		if (syscall_table[i].nr == nr)
			return &syscall_table[i];
	}
	return NULL;
}

static const char *errno_name(int err)
{
	switch (err) {
	case EPERM: return "EPERM";
	case ENOENT: return "ENOENT";
	case ESRCH: return "ESRCH";
	case EINTR: return "EINTR";
	case EIO: return "EIO";
	case ENXIO: return "ENXIO";
	case E2BIG: return "E2BIG";
	case ENOEXEC: return "ENOEXEC";
	case EBADF: return "EBADF";
	case ECHILD: return "ECHILD";
	case EAGAIN: return "EAGAIN";
	case ENOMEM: return "ENOMEM";
	case EACCES: return "EACCES";
	case EFAULT: return "EFAULT";
	case ENOTBLK: return "ENOTBLK";
	case EBUSY: return "EBUSY";
	case EEXIST: return "EEXIST";
	case EXDEV: return "EXDEV";
	case ENODEV: return "ENODEV";
	case ENOTDIR: return "ENOTDIR";
	case EISDIR: return "EISDIR";
	case EINVAL: return "EINVAL";
	case ENFILE: return "ENFILE";
	case EMFILE: return "EMFILE";
	case ENOTTY: return "ENOTTY";
	case ETXTBSY: return "ETXTBSY";
	case EFBIG: return "EFBIG";
	case ENOSPC: return "ENOSPC";
	case ESPIPE: return "ESPIPE";
	case EROFS: return "EROFS";
	case EMLINK: return "EMLINK";
	case EPIPE: return "EPIPE";
	case EDOM: return "EDOM";
	case ERANGE: return "ERANGE";
	case EDEADLK: return "EDEADLK";
	case ENAMETOOLONG: return "ENAMETOOLONG";
	case ENOLCK: return "ENOLCK";
	case ENOSYS: return "ENOSYS";
	case ENOTEMPTY: return "ENOTEMPTY";
	case ELOOP: return "ELOOP";
	case ENOMSG: return "ENOMSG";
	case EIDRM: return "EIDRM";
	case EILSEQ: return "EILSEQ";
	case ENODATA: return "ENODATA";
	case ETIME: return "ETIME";
	case ENOSR: return "ENOSR";
	case ENOSTR: return "ENOSTR";
	case ENOPROTOOPT: return "ENOPROTOOPT";
	case EPROTONOSUPPORT: return "EPROTONOSUPPORT";
	case ESOCKTNOSUPPORT: return "ESOCKTNOSUPPORT";
	case EOPNOTSUPP: return "EOPNOTSUPP";
	case EPFNOSUPPORT: return "EPFNOSUPPORT";
	case EAFNOSUPPORT: return "EAFNOSUPPORT";
	case EADDRINUSE: return "EADDRINUSE";
	case EADDRNOTAVAIL: return "EADDRNOTAVAIL";
	case ENETDOWN: return "ENETDOWN";
	case ENETUNREACH: return "ENETUNREACH";
	case ENETRESET: return "ENETRESET";
	case ECONNABORTED: return "ECONNABORTED";
	case ECONNRESET: return "ECONNRESET";
	case ENOBUFS: return "ENOBUFS";
	case EISCONN: return "EISCONN";
	case ENOTCONN: return "ENOTCONN";
	case ESHUTDOWN: return "ESHUTDOWN";
	case ETOOMANYREFS: return "ETOOMANYREFS";
	case ETIMEDOUT: return "ETIMEDOUT";
	case ECONNREFUSED: return "ECONNREFUSED";
	case EHOSTDOWN: return "EHOSTDOWN";
	case EHOSTUNREACH: return "EHOSTUNREACH";
	case EALREADY: return "EALREADY";
	case EINPROGRESS: return "EINPROGRESS";
	default: return "EUNKNOWN";
	}
}

static int get_syscall_regs(pid_t pid, struct sys_regs *sr)
{
#if defined(__x86_64__)
	struct user_regs_struct regs;
	if (ptrace(PTRACE_GETREGS, pid, NULL, &regs) < 0)
		return -1;
	sr->sc_nr = (long)regs.orig_rax;
	sr->ret = (long)regs.rax;
	sr->args[0] = (unsigned long)regs.rdi;
	sr->args[1] = (unsigned long)regs.rsi;
	sr->args[2] = (unsigned long)regs.rdx;
	sr->args[3] = (unsigned long)regs.r10;
	sr->args[4] = (unsigned long)regs.r8;
	sr->args[5] = (unsigned long)regs.r9;
	return 0;
#elif defined(__i386__)
	struct user_regs_struct regs;
	if (ptrace(PTRACE_GETREGS, pid, NULL, &regs) < 0)
		return -1;
	sr->sc_nr = (long)regs.orig_eax;
	sr->ret = (long)regs.eax;
	sr->args[0] = (unsigned long)regs.ebx;
	sr->args[1] = (unsigned long)regs.ecx;
	sr->args[2] = (unsigned long)regs.edx;
	sr->args[3] = (unsigned long)regs.esi;
	sr->args[4] = (unsigned long)regs.edi;
	sr->args[5] = (unsigned long)regs.ebp;
	return 0;
#elif defined(__aarch64__)
	struct user_pt_regs regs;
	struct iovec iov;
	iov.iov_base = &regs;
	iov.iov_len = sizeof(regs);
	if (ptrace(PTRACE_GETREGSET, pid, (void *)(uintptr_t)NT_PRSTATUS, &iov) < 0)
		return -1;
	sr->sc_nr = (long)regs.regs[8];
	sr->ret = (long)regs.regs[0];
	sr->args[0] = (unsigned long)regs.regs[0];
	sr->args[1] = (unsigned long)regs.regs[1];
	sr->args[2] = (unsigned long)regs.regs[2];
	sr->args[3] = (unsigned long)regs.regs[3];
	sr->args[4] = (unsigned long)regs.regs[4];
	sr->args[5] = (unsigned long)regs.regs[5];
	return 0;
#elif defined(__arm__)
	struct user_regs regs;
	if (ptrace(PTRACE_GETREGS, pid, NULL, &regs) < 0)
		return -1;
	sr->sc_nr = (long)regs.uregs[7];
	sr->ret = (long)regs.uregs[0];
	sr->args[0] = (unsigned long)regs.uregs[0];
	sr->args[1] = (unsigned long)regs.uregs[1];
	sr->args[2] = (unsigned long)regs.uregs[2];
	sr->args[3] = (unsigned long)regs.uregs[3];
	sr->args[4] = (unsigned long)regs.uregs[4];
	sr->args[5] = (unsigned long)regs.uregs[5];
	return 0;
#elif defined(__riscv)
	struct user_regs_struct regs;
	struct iovec iov;
	iov.iov_base = &regs;
	iov.iov_len = sizeof(regs);
	if (ptrace(PTRACE_GETREGSET, pid, (void *)(uintptr_t)NT_PRSTATUS, &iov) < 0)
		return -1;
	sr->sc_nr = (long)regs.a7;
	sr->ret = (long)regs.a0;
	sr->args[0] = (unsigned long)regs.a0;
	sr->args[1] = (unsigned long)regs.a1;
	sr->args[2] = (unsigned long)regs.a2;
	sr->args[3] = (unsigned long)regs.a3;
	sr->args[4] = (unsigned long)regs.a4;
	sr->args[5] = (unsigned long)regs.a5;
	return 0;
#elif defined(__mips__)
	struct pt_regs regs;
	if (ptrace(PTRACE_GETREGS, pid, NULL, &regs) < 0)
		return -1;
	sr->sc_nr = (long)regs.regs[2];
	sr->ret = (long)regs.regs[2];
	sr->args[0] = (unsigned long)regs.regs[4];
	sr->args[1] = (unsigned long)regs.regs[5];
	sr->args[2] = (unsigned long)regs.regs[6];
	sr->args[3] = (unsigned long)regs.regs[7];
	sr->args[4] = (unsigned long)regs.regs[8];
	sr->args[5] = (unsigned long)regs.regs[9];
	return 0;
#else
# error "strace: unsupported architecture"
#endif
}

static int read_tracee_string(pid_t pid, unsigned long addr, char *buf, size_t maxlen)
{
	struct iovec local, remote;
	ssize_t nread;
	size_t i;

	if (addr == 0 || maxlen == 0)
		return -1;

	local.iov_base = buf;
	local.iov_len = maxlen;
	remote.iov_base = (void *)addr;
	remote.iov_len = maxlen;

	nread = process_vm_readv(pid, &local, 1, &remote, 1, 0);
	if (nread > 0) {
		for (i = 0; i < (size_t)nread; i++) {
			if (buf[i] == '\0')
				return 0;
		}
		if ((size_t)nread < maxlen)
			buf[nread] = '\0';
		else
			buf[maxlen - 1] = '\0';
		return 1;
	}

	for (i = 0; i < maxlen; ) {
		long word;
		size_t j;
		char *wptr;

		errno = 0;
		word = ptrace(PTRACE_PEEKDATA, pid, (void *)(addr + i), NULL);
		if (errno != 0) {
			if (i > 0) {
				buf[i] = '\0';
				return 0;
			}
			return -1;
		}

		wptr = (char *)&word;
		for (j = 0; j < sizeof(long) && i < maxlen; j++, i++) {
			buf[i] = wptr[j];
			if (wptr[j] == '\0')
				return 0;
		}
	}
	buf[maxlen - 1] = '\0';
	return 1;
}

static void print_escaped_string(FILE *fp, const char *buf, size_t maxlen, int truncated)
{
	size_t i;
	fputc('"', fp);
	for (i = 0; i < maxlen && buf[i] != '\0'; i++) {
		unsigned char c = (unsigned char)buf[i];
		switch (c) {
		case '\\': fputs("\\\\", fp); break;
		case '\"': fputs("\\\"", fp); break;
		case '\n': fputs("\\n", fp); break;
		case '\r': fputs("\\r", fp); break;
		case '\t': fputs("\\t", fp); break;
		case '\v': fputs("\\v", fp); break;
		case '\f': fputs("\\f", fp); break;
		default:
			if (c >= 32 && c < 127) {
				fputc(c, fp);
			} else {
				fprintf(fp, "\\%03o", c);
			}
			break;
		}
	}
	fputc('"', fp);
	if (truncated)
		fputs("...", fp);
}

struct syscall_stat {
	int nr;
	const char *name;
	unsigned long calls;
	unsigned long errors;
	double total_time;
};

#define MAX_TRACKED_SYSCALLS 1024
static struct syscall_stat stats_table[MAX_TRACKED_SYSCALLS];
static int num_stats = 0;

static void record_stat(int nr, const char *name, double elapsed, int is_error)
{
	int i;
	for (i = 0; i < num_stats; i++) {
		if (stats_table[i].nr == nr) {
			stats_table[i].calls++;
			if (is_error)
				stats_table[i].errors++;
			stats_table[i].total_time += elapsed;
			return;
		}
	}
	if (num_stats < MAX_TRACKED_SYSCALLS) {
		stats_table[num_stats].nr = nr;
		stats_table[num_stats].name = name;
		stats_table[num_stats].calls = 1;
		stats_table[num_stats].errors = is_error ? 1 : 0;
		stats_table[num_stats].total_time = elapsed;
		num_stats++;
	}
}

static int stat_cmp(const void *p1, const void *p2)
{
	const struct syscall_stat *s1 = (const struct syscall_stat *)p1;
	const struct syscall_stat *s2 = (const struct syscall_stat *)p2;
	if (s1->total_time > s2->total_time) return -1;
	if (s1->total_time < s2->total_time) return 1;
	if (s1->calls > s2->calls) return -1;
	if (s1->calls < s2->calls) return 1;
	return 0;
}

static void print_summary(FILE *fp)
{
	double grand_time = 0.0;
	unsigned long grand_calls = 0;
	unsigned long grand_errors = 0;
	int i;

	for (i = 0; i < num_stats; i++) {
		grand_time += stats_table[i].total_time;
		grand_calls += stats_table[i].calls;
		grand_errors += stats_table[i].errors;
	}

	qsort(stats_table, num_stats, sizeof(struct syscall_stat), stat_cmp);

	fprintf(fp, "%% time     seconds  usecs/call     calls    errors syscall\n");
	fprintf(fp, "------ ----------- ----------- --------- --------- ----------------\n");
	for (i = 0; i < num_stats; i++) {
		double pct = grand_time > 0.0 ? (stats_table[i].total_time / grand_time * 100.0) : 0.0;
		unsigned long usecs = stats_table[i].calls ? (unsigned long)(stats_table[i].total_time * 1e6 / stats_table[i].calls) : 0;
		if (stats_table[i].errors > 0) {
			fprintf(fp, "%6.2f %11.6f %11lu %9lu %9lu %s\n",
				pct, stats_table[i].total_time, usecs, stats_table[i].calls, stats_table[i].errors, stats_table[i].name);
		} else {
			fprintf(fp, "%6.2f %11.6f %11lu %9lu           %s\n",
				pct, stats_table[i].total_time, usecs, stats_table[i].calls, stats_table[i].name);
		}
	}
	fprintf(fp, "------ ----------- ----------- --------- --------- ----------------\n");
	if (grand_errors > 0) {
		fprintf(fp, "100.00 %11.6f             %9lu %9lu total\n",
			grand_time, grand_calls, grand_errors);
	} else {
		fprintf(fp, "100.00 %11.6f             %9lu           total\n",
			grand_time, grand_calls);
	}
	fflush(fp);
}

static void print_syscall(FILE *fp, pid_t pid, int nr, const unsigned long *args,
                          char **saved_str, const int *str_trunc, long ret,
                          int strsize, int multi)
{
	const struct syscall_entry *se = lookup_syscall(nr);
	char unknown_name[32];
	const char *name;
	int nargs;
	int i;

	if (multi)
		fprintf(fp, "[pid %u] ", (unsigned)pid);

	if (se) {
		name = se->name;
		nargs = se->nargs;
	} else {
		sprintf(unknown_name, "syscall_%d", nr);
		name = unknown_name;
		nargs = 6;
	}

	fprintf(fp, "%s(", name);
	for (i = 0; i < nargs; i++) {
		if (i > 0)
			fputs(", ", fp);
		if (saved_str && saved_str[i]) {
			print_escaped_string(fp, saved_str[i], strsize, str_trunc[i]);
		} else if (args[i] == 0 && se && (se->str_mask & (1 << i))) {
			fputs("NULL", fp);
		} else if ((long)args[i] == -100 || (int)args[i] == -100) {
			fputs("AT_FDCWD", fp);
		} else if ((long)args[i] >= -100 && (long)args[i] <= 100000) {
			fprintf(fp, "%ld", (long)args[i]);
		} else {
			fprintf(fp, "%#lx", args[i]);
		}
	}
	fputs(") = ", fp);

	if (ret < 0 && ret >= -4095) {
		int err = (int)-ret;
		fprintf(fp, "-1 %s (%s)\n", errno_name(err), strerror(err));
	} else if (ret > 0 && (strcmp(name, "mmap") == 0 || strcmp(name, "brk") == 0)) {
		fprintf(fp, "%#lx\n", (unsigned long)ret);
	} else {
		fprintf(fp, "%ld\n", ret);
	}
	fflush(fp);
}

static struct tracee *g_tracees = NULL;
static volatile sig_atomic_t g_interrupted = 0;

static void sig_handler(int sig UNUSED_PARAM)
{
	g_interrupted = 1;
}

static struct tracee *find_tracee(pid_t pid)
{
	struct tracee *t = g_tracees;
	while (t) {
		if (t->pid == pid)
			return t;
		t = t->next;
	}
	return NULL;
}

static struct tracee *add_tracee(pid_t pid)
{
	struct tracee *t = xzalloc(sizeof(*t));
	t->pid = pid;
	t->sc_nr = -1;
	t->next = g_tracees;
	g_tracees = t;
	return t;
}

static void remove_tracee(pid_t pid)
{
	struct tracee **curr = &g_tracees;
	struct tracee *to_free;
	int i;

	while (*curr) {
		if ((*curr)->pid == pid) {
			to_free = *curr;
			for (i = 0; i < 6; i++) {
				free(to_free->saved_str[i]);
			}
			*curr = to_free->next;
			free(to_free);
			return;
		}
		curr = &(*curr)->next;
	}
}

int strace_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int strace_main(int argc UNUSED_PARAM, char **argv)
{
	unsigned opts;
	char *opt_s = NULL;
	char *opt_o = NULL;
	char *opt_p = NULL;
	int strsize = 32;
	FILE *out_fp = stderr;
	pid_t target_pid = 0;
	pid_t child;
	pid_t pid;
	long ptrace_options;
	int status;
	struct tracee *t;
	struct tracee *nt;
	int exit_status;
	int sig;
	unsigned int event;
	unsigned long new_pid;
	struct sys_regs sr;
	const struct syscall_entry *se;
	const char *name;
	int nargs;
	unsigned mask;
	int i;
	int r;
	struct timespec end_ts;
	double elapsed;
	int is_error;

	opts = getopt32(argv, "+cfs:o:p:q", &opt_s, &opt_o, &opt_p);
	argv += optind;

	if (!(opts & OPT_p) && !argv[0])
		bb_show_usage();

	if (opts & OPT_s) {
		strsize = xatoi_positive(opt_s);
		if (strsize <= 0)
			strsize = 32;
	}

	if (opts & OPT_o) {
		out_fp = xfopen_for_write(opt_o);
	}

	ptrace_options = PTRACE_O_TRACESYSGOOD;
	if (opts & OPT_f) {
		ptrace_options |= PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK | PTRACE_O_TRACECLONE;
	}

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);
	signal(SIGQUIT, sig_handler);

	if (opts & OPT_p) {
		target_pid = xatoi_positive(opt_p);
		if (ptrace(PTRACE_ATTACH, target_pid, NULL, NULL) < 0)
			bb_perror_msg_and_die("attach: ptrace(%d)", target_pid);
		add_tracee(target_pid);
		if (!(opts & OPT_q) && !(opts & OPT_c))
			fprintf(out_fp, "Process %d attached\n", target_pid);
	} else {
		if (!argv[0])
			bb_show_usage();
		child = fork();
		if (child < 0)
			bb_simple_perror_msg_and_die("fork");
		if (child == 0) {
			if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) < 0)
				bb_simple_perror_msg_and_die("ptrace(TRACEME)");
			raise(SIGSTOP);
			BB_EXECVP(argv[0], argv);
			bb_perror_msg_and_die("can't execute '%s'", argv[0]);
		}
		target_pid = child;
		add_tracee(child);
	}

	while (g_tracees != NULL && !g_interrupted) {
		pid = waitpid(-1, &status, __WALL);

		if (pid < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		t = find_tracee(pid);
		if (!t)
			t = add_tracee(pid);

		if (WIFEXITED(status)) {
			exit_status = WEXITSTATUS(status);
			if (t->in_syscall) {
				se = lookup_syscall((int)t->sc_nr);
				name = se ? se->name : "exit";
				if (opts & OPT_c) {
					record_stat((int)t->sc_nr, name, 0.0, 0);
				} else {
					if (opts & OPT_f)
						fprintf(out_fp, "[pid %u] ", (unsigned)pid);
					fprintf(out_fp, "%s(%ld) = ?\n", name, (long)t->sc_args[0]);
				}
			}
			if (!(opts & OPT_q) && !(opts & OPT_c)) {
				if (opts & OPT_f)
					fprintf(out_fp, "[pid %u] ", (unsigned)pid);
				fprintf(out_fp, "+++ exited with %d +++\n", exit_status);
				fflush(out_fp);
			}
			remove_tracee(pid);
			continue;
		}

		if (WIFSIGNALED(status)) {
			sig = WTERMSIG(status);
			if (!(opts & OPT_q) && !(opts & OPT_c)) {
				if (opts & OPT_f)
					fprintf(out_fp, "[pid %u] ", (unsigned)pid);
				fprintf(out_fp, "+++ killed by %s +++\n", strsignal(sig));
				fflush(out_fp);
			}
			remove_tracee(pid);
			continue;
		}

		if (WIFSTOPPED(status)) {
			sig = WSTOPSIG(status);
			event = (unsigned int)((status >> 16) & 0xffff);

			if (!t->initialized) {
				t->initialized = 1;
				ptrace(PTRACE_SETOPTIONS, pid, NULL, (void *)ptrace_options);
				ptrace(PTRACE_SYSCALL, pid, NULL, 0);
				continue;
			}

			if (event == PTRACE_EVENT_FORK || event == PTRACE_EVENT_VFORK || event == PTRACE_EVENT_CLONE) {
				new_pid = 0;
				if (ptrace(PTRACE_GETEVENTMSG, pid, NULL, &new_pid) == 0 && new_pid > 0) {
					nt = add_tracee((pid_t)new_pid);
					nt->initialized = 0;
				}
				ptrace(PTRACE_SYSCALL, pid, NULL, 0);
				continue;
			}

			if (sig == (SIGTRAP | 0x80)) {
				if (get_syscall_regs(pid, &sr) < 0) {
					ptrace(PTRACE_SYSCALL, pid, NULL, 0);
					continue;
				}

				if (!t->in_syscall) {
					/* Syscall entry */
					t->in_syscall = 1;
					t->sc_nr = sr.sc_nr;
					memcpy(t->sc_args, sr.args, sizeof(sr.args));
					clock_gettime(CLOCK_MONOTONIC, &t->start_ts);

					se = lookup_syscall((int)sr.sc_nr);
					nargs = se ? se->nargs : 6;
					mask = se ? se->str_mask : 0;

					for (i = 0; i < nargs && i < 6; i++) {
						t->saved_str[i] = NULL;
						t->str_trunc[i] = 0;
						if ((mask & (1 << i)) && sr.args[i] != 0) {
							t->saved_str[i] = xmalloc(strsize + 1);
							r = read_tracee_string(pid, sr.args[i], t->saved_str[i], strsize + 1);
							if (r < 0) {
								free(t->saved_str[i]);
								t->saved_str[i] = NULL;
							} else {
								t->str_trunc[i] = (r == 1);
							}
						}
					}
					ptrace(PTRACE_SYSCALL, pid, NULL, 0);
				} else {
					/* Syscall exit */
					t->in_syscall = 0;
					clock_gettime(CLOCK_MONOTONIC, &end_ts);
					elapsed = (end_ts.tv_sec - t->start_ts.tv_sec) +
					          (end_ts.tv_nsec - t->start_ts.tv_nsec) / 1e9;

					se = lookup_syscall((int)t->sc_nr);
					name = se ? se->name : "unknown";

					if (opts & OPT_c) {
						is_error = (sr.ret < 0 && sr.ret >= -4095);
						record_stat((int)t->sc_nr, name, elapsed, is_error);
					} else {
						print_syscall(out_fp, pid, (int)t->sc_nr, t->sc_args,
						              t->saved_str, t->str_trunc, sr.ret,
						              strsize, (opts & OPT_f));
					}

					for (i = 0; i < 6; i++) {
						free(t->saved_str[i]);
						t->saved_str[i] = NULL;
					}
					ptrace(PTRACE_SYSCALL, pid, NULL, 0);
				}
				continue;
			}

			/* Signal delivered to child */
			if (!(opts & OPT_c)) {
				if (sig != SIGTRAP && sig != SIGSTOP) {
					if (opts & OPT_f)
						fprintf(out_fp, "[pid %u] ", (unsigned)pid);
					fprintf(out_fp, "--- %s {si_signo=%d} ---\n", strsignal(sig), sig);
					fflush(out_fp);
				}
			}
			ptrace(PTRACE_SYSCALL, pid, NULL, (sig == SIGTRAP || sig == SIGSTOP) ? 0 : sig);
		}
	}

	if (g_interrupted) {
		t = g_tracees;
		while (t) {
			if (opts & OPT_p) {
				ptrace(PTRACE_DETACH, t->pid, NULL, NULL);
				if (!(opts & OPT_q) && !(opts & OPT_c))
					fprintf(out_fp, "Process %d detached\n", t->pid);
			} else {
				kill(t->pid, SIGTERM);
			}
			t = t->next;
		}
	}

	if (opts & OPT_c) {
		print_summary(out_fp);
	}

	if (out_fp != stderr && out_fp != stdout)
		fclose(out_fp);

	return 0;
}
