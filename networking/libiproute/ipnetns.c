/* vi: set sw=4 ts=4: */
/* ip netns - network namespace management */
#include "ip_common.h"
#include "utils.h"
#include <sys/mount.h>
#include <sched.h>

#ifndef MS_BIND
# define MS_BIND 4096
#endif
#ifndef MNT_DETACH
# define MNT_DETACH 2
#endif
#ifndef CLONE_NEWNET
# define CLONE_NEWNET 0x40000000
#endif

#define NETNS_RUN_DIR "/var/run/netns"

static int netns_list(void)
{
	DIR *dir;
	struct dirent *entry;

	dir = opendir(NETNS_RUN_DIR);
	if (!dir) {
		/* No namespaces dir = no namespaces */
		return 0;
	}
	while ((entry = readdir(dir)) != NULL) {
		if (entry->d_name[0] == '.')
			continue;
		printf("%s\n", entry->d_name);
	}
	closedir(dir);
	return 0;
}

static int netns_add(char *name)
{
	char netns_path[256];
	int fd;

	mkdir(NETNS_RUN_DIR, 0755);
	snprintf(netns_path, sizeof(netns_path), NETNS_RUN_DIR "/%s", name);

	fd = open(netns_path, O_RDONLY | O_CREAT | O_EXCL, 0);
	if (fd < 0)
		bb_perror_msg_and_die("Cannot create namespace file '%s'", netns_path);
	close(fd);

	if (unshare(CLONE_NEWNET) < 0) {
		unlink(netns_path);
		bb_perror_msg_and_die("unshare");
	}
	if (mount("/proc/self/ns/net", netns_path, "none", MS_BIND, NULL) < 0) {
		unlink(netns_path);
		bb_perror_msg_and_die("mount --bind");
	}
	return 0;
}

static int netns_del(char *name)
{
	char netns_path[256];

	snprintf(netns_path, sizeof(netns_path), NETNS_RUN_DIR "/%s", name);
	umount2(netns_path, MNT_DETACH);
	if (unlink(netns_path) < 0)
		bb_perror_msg_and_die("Cannot remove namespace file");
	return 0;
}

static int netns_exec(char *name, char **argv)
{
	char netns_path[256];
	int fd;

	snprintf(netns_path, sizeof(netns_path), NETNS_RUN_DIR "/%s", name);
	fd = open(netns_path, O_RDONLY);
	if (fd < 0)
		bb_perror_msg_and_die("Cannot open namespace '%s'", name);
	if (setns(fd, CLONE_NEWNET) < 0)
		bb_perror_msg_and_die("setns");
	close(fd);
	execvp(argv[0], argv);
	bb_perror_msg_and_die("exec '%s'", argv[0]);
	return 1;
}

static int netns_identify(char *pidstr)
{
	char proc_path[64];
	char ns_path[256];
	char target[256];
	struct stat ns_st;
	struct stat entry_st;
	DIR *dir;
	struct dirent *entry;
	pid_t pid;

	pid = pidstr ? atoi(pidstr) : getpid();
	snprintf(proc_path, sizeof(proc_path), "/proc/%d/ns/net", (int)pid);

	if (stat(proc_path, &ns_st) < 0)
		bb_perror_msg_and_die("stat %s", proc_path);

	dir = opendir(NETNS_RUN_DIR);
	if (!dir)
		return 0;

	while ((entry = readdir(dir)) != NULL) {
		if (entry->d_name[0] == '.')
			continue;
		snprintf(ns_path, sizeof(ns_path), NETNS_RUN_DIR "/%s", entry->d_name);
		if (stat(ns_path, &entry_st) == 0
		 && entry_st.st_dev == ns_st.st_dev
		 && entry_st.st_ino == ns_st.st_ino) {
			printf("%s\n", entry->d_name);
			closedir(dir);
			return 0;
		}
	}
	(void)target;
	closedir(dir);
	return 0;
}

int FAST_FUNC do_ipnetns(char **argv)
{
	static const char subcmds[] ALIGN1 =
		"list\0" "add\0" "del\0" "delete\0" "exec\0"
		"identify\0" "pids\0" "monitor\0" "show\0";
	char *name;
	int cmd;

	if (!*argv || (cmd = index_in_substrings(subcmds, *argv)) < 0)
		return netns_list();

	switch (cmd) {
	case 0: /* list */
	case 8: /* show */
		return netns_list();
	case 1: /* add */
		argv++;
		if (!*argv) bb_show_usage();
		return netns_add(*argv);
	case 2: /* del */
	case 3: /* delete */
		argv++;
		if (!*argv) bb_show_usage();
		return netns_del(*argv);
	case 4: /* exec */
		argv++;
		if (!*argv) bb_show_usage();
		name = *argv++;
		if (!*argv) bb_show_usage();
		return netns_exec(name, argv);
	case 5: /* identify */
		argv++;
		return netns_identify(*argv);
	case 6: /* pids */
	case 7: /* monitor */
	default:
		/* stub */
		break;
	}
	return 0;
}
