/* vi: set sw=4 ts=4: */
/*
 * Mini lsof implementation for busybox
 *
 * Copyright (C) 2012 by Sven Oliver 'SvOlli' Moll <svolli@svolli.de>
 *
 * Licensed under GPLv2, see file LICENSE in this source tree.
 */
//config:config LSOF
//config:	bool "lsof (3.7 kb)"
//config:	default y
//config:	help
//config:	Show open files.

//applet:IF_LSOF(APPLET(lsof, BB_DIR_USR_BIN, BB_SUID_DROP))

//kbuild:lib-$(CONFIG_LSOF) += lsof.o

//usage:#define lsof_trivial_usage
//usage:       "[-t] [-p PID] [-u USER] [-c NAME] [-i [TCP|UDP][@host][:port]]"
//usage:#define lsof_full_usage "\n\n"
//usage:       "List open files\n"
//usage:     "\n	-p PID	List files for specified PID (comma-separated or multiple)"
//usage:     "\n	-u USER	List files for specified user (username or UID)"
//usage:     "\n	-c NAME	List files for commands matching NAME"
//usage:     "\n	-i [INET] List internet sockets matching protocol/port"
//usage:     "\n	-t	Terse output: print only process PIDs"

#include "libbb.h"

enum {
	OPT_p = (1 << 0),
	OPT_u = (1 << 1),
	OPT_c = (1 << 2),
	OPT_t = (1 << 3),
	OPT_i = (1 << 4),
};

enum {
	SOCK_PROTO_TCP = 1,
	SOCK_PROTO_UDP = 2,
	SOCK_PROTO_UNIX = 3,
};

struct sock_info {
	unsigned long inode;
	int proto;         /* SOCK_PROTO_* */
	int version;       /* 4 or 6, or 0 for unix */
	int port_local;
	int port_remote;
	char *ip_local;
	char *ip_remote;
	char *name;        /* formatted name string, e.g. "TCP *:80 (LISTEN)" */
	char *dev;         /* e.g. "0,9" or unix hex addr */
	struct sock_info *next;
};

#define SOCK_HASH_SIZE 1024

struct inet_filter {
	int active;        /* 1 if -i was specified */
	int proto;         /* 0: any, SOCK_PROTO_TCP, SOCK_PROTO_UDP */
	int version;       /* 0: any, 4, 6 */
	char *host;        /* host/IP string to match or NULL */
	int port;          /* port number to match or -1 */
};

struct globals {
	struct sock_info *sock_hash[SOCK_HASH_SIZE];
	struct inet_filter ifilter;
	llist_t *opt_p;
	llist_t *opt_u;
	llist_t *opt_c;
	char *opt_i;
	unsigned opts;
};
#define G (*ptr_to_globals)
#define INIT_G() do { \
	SET_PTR_TO_GLOBALS(xzalloc(sizeof(G))); \
	G.ifilter.port = -1; \
} while (0)

static void add_sock_entry(unsigned long inode, int proto, int version,
                          int port_local, int port_remote,
                          const char *ip_local, const char *ip_remote,
                          char *name, char *dev)
{
	unsigned h;
	struct sock_info *si;

	h = inode % SOCK_HASH_SIZE;
	si = xzalloc(sizeof(*si));
	si->inode = inode;
	si->proto = proto;
	si->version = version;
	si->port_local = port_local;
	si->port_remote = port_remote;
	si->ip_local = ip_local ? xstrdup(ip_local) : NULL;
	si->ip_remote = ip_remote ? xstrdup(ip_remote) : NULL;
	si->name = name;
	si->dev = dev;

	si->next = G.sock_hash[h];
	G.sock_hash[h] = si;
}

static struct sock_info *find_sock_entry(unsigned long inode)
{
	unsigned h;
	struct sock_info *si;

	h = inode % SOCK_HASH_SIZE;
	si = G.sock_hash[h];
	while (si) {
		if (si->inode == inode)
			return si;
		si = si->next;
	}
	return NULL;
}

static const char *tcp_state_name(unsigned state)
{
	switch (state) {
	case 1:  return "ESTABLISHED";
	case 2:  return "SYN_SENT";
	case 3:  return "SYN_RECV";
	case 4:  return "FIN_WAIT1";
	case 5:  return "FIN_WAIT2";
	case 6:  return "TIME_WAIT";
	case 7:  return "CLOSE";
	case 8:  return "CLOSE_WAIT";
	case 9:  return "LAST_ACK";
	case 10: return "LISTEN";
	case 11: return "CLOSING";
	default: return "";
	}
}

static void decode_ip4(const char *hex_addr, unsigned port,
                       char *ip_out, size_t ip_out_sz, char *addr_port_out)
{
	uint32_t ip;
	struct in_addr in;

	ip = (uint32_t)strtoul(hex_addr, NULL, 16);
	if (ip == 0) {
		strcpy(ip_out, "*");
		sprintf(addr_port_out, "*:%u", port);
	} else {
		in.s_addr = ip;
		inet_ntop(AF_INET, &in, ip_out, ip_out_sz);
		sprintf(addr_port_out, "%s:%u", ip_out, port);
	}
}

static void decode_ip6(const char *hex_addr, unsigned port,
                       char *ip_out, size_t ip_out_sz, char *addr_port_out)
{
	struct in6_addr in6;
	uint32_t *w;
	int all_zero;
	int i;

	w = (uint32_t *)&in6;
	all_zero = 1;
	if (sscanf(hex_addr, "%08X%08X%08X%08X", &w[0], &w[1], &w[2], &w[3]) != 4) {
		strcpy(ip_out, "*");
		sprintf(addr_port_out, "*:%u", port);
		return;
	}

	for (i = 0; i < 4; i++) {
		if (w[i] != 0) {
			all_zero = 0;
			break;
		}
	}

	if (all_zero) {
		strcpy(ip_out, "*");
		sprintf(addr_port_out, "*:%u", port);
	} else {
		inet_ntop(AF_INET6, &in6, ip_out, ip_out_sz);
		sprintf(addr_port_out, "[%s]:%u", ip_out, port);
	}
}

static void parse_proc_net_ip(const char *path, int proto, int version)
{
	FILE *f;
	char buf[512];

	f = fopen_for_read(path);
	if (!f)
		return;

	/* Skip header line */
	if (!fgets(buf, sizeof(buf), f)) {
		fclose(f);
		return;
	}

	while (fgets(buf, sizeof(buf), f)) {
		char *tokens[11];
		char *p;
		char *colon_loc;
		char *colon_rem;
		char local_ip[INET6_ADDRSTRLEN];
		char rem_ip[INET6_ADDRSTRLEN];
		char local_addr_port[INET6_ADDRSTRLEN + 16];
		char rem_addr_port[INET6_ADDRSTRLEN + 16];
		char *name;
		unsigned state;
		unsigned port_loc;
		unsigned port_rem;
		unsigned long inode;
		int i;

		p = skip_whitespace(buf);
		i = 0;
		while (*p && i < 11) {
			tokens[i++] = p;
			while (*p && !isspace(*p))
				p++;
			if (*p)
				*p++ = '\0';
			p = skip_whitespace(p);
		}
		if (i < 10)
			continue;

		colon_loc = strchr(tokens[1], ':');
		colon_rem = strchr(tokens[2], ':');
		if (!colon_loc || !colon_rem)
			continue;

		*colon_loc = '\0';
		*colon_rem = '\0';
		port_loc = (unsigned)strtoul(colon_loc + 1, NULL, 16);
		port_rem = (unsigned)strtoul(colon_rem + 1, NULL, 16);
		state = (unsigned)strtoul(tokens[3], NULL, 16);
		inode = bb_strtoul(tokens[9], NULL, 10);

		if (version == 4) {
			decode_ip4(tokens[1], port_loc, local_ip, sizeof(local_ip), local_addr_port);
			decode_ip4(tokens[2], port_rem, rem_ip, sizeof(rem_ip), rem_addr_port);
		} else {
			decode_ip6(tokens[1], port_loc, local_ip, sizeof(local_ip), local_addr_port);
			decode_ip6(tokens[2], port_rem, rem_ip, sizeof(rem_ip), rem_addr_port);
		}

		if (proto == SOCK_PROTO_TCP) {
			const char *st_name = tcp_state_name(state);
			if (state == 10) { /* LISTEN */
				name = xasprintf("TCP %s (LISTEN)", local_addr_port);
			} else if (*st_name) {
				name = xasprintf("TCP %s->%s (%s)", local_addr_port, rem_addr_port, st_name);
			} else {
				name = xasprintf("TCP %s->%s", local_addr_port, rem_addr_port);
			}
		} else {
			if (port_rem == 0) {
				name = xasprintf("UDP %s", local_addr_port);
			} else {
				name = xasprintf("UDP %s->%s", local_addr_port, rem_addr_port);
			}
		}

		add_sock_entry(inode, proto, version, (int)port_loc, (int)port_rem,
		               local_ip, rem_ip, name, xstrdup("0,9"));
	}

	fclose(f);
}

static void parse_proc_net_unix(void)
{
	FILE *f;
	char buf[512];

	f = fopen_for_read("/proc/net/unix");
	if (!f)
		return;

	/* Skip header */
	if (!fgets(buf, sizeof(buf), f)) {
		fclose(f);
		return;
	}

	while (fgets(buf, sizeof(buf), f)) {
		char *p;
		char *tokens[8];
		char *dev_addr;
		char *path;
		char *name;
		unsigned long inode;
		int i;

		/* Format: Num RefCount Protocol Flags Type St Inode Path */
		p = skip_whitespace(buf);
		i = 0;
		while (*p && i < 7) {
			tokens[i++] = p;
			while (*p && !isspace(*p))
				p++;
			if (*p)
				*p++ = '\0';
			p = skip_whitespace(p);
		}
		if (i < 7)
			continue;

		inode = bb_strtoul(tokens[6], NULL, 10);
		dev_addr = tokens[0]; /* Hex pointer */
		p = strchr(dev_addr, ':');
		if (p)
			*p = '\0';

		path = skip_whitespace(p ? p + 1 : buf + strlen(buf));
		p = strchr(path, '\n');
		if (p)
			*p = '\0';

		if (*path)
			name = xstrdup(path);
		else
			name = xasprintf("socket:[%lu]", inode);

		add_sock_entry(inode, SOCK_PROTO_UNIX, 0, -1, -1, NULL, NULL,
		               name, xstrdup(dev_addr));
	}

	fclose(f);
}

static void parse_inet_filter(const char *arg)
{
	G.ifilter.active = 1;
	G.ifilter.proto = 0;
	G.ifilter.version = 0;
	G.ifilter.host = NULL;
	G.ifilter.port = -1;

	if (!arg || !*arg)
		return;

	if (strncasecmp(arg, "tcp", 3) == 0) {
		G.ifilter.proto = SOCK_PROTO_TCP;
		arg += 3;
		if (*arg == '4') {
			G.ifilter.version = 4;
			arg++;
		} else if (*arg == '6') {
			G.ifilter.version = 6;
			arg++;
		}
	} else if (strncasecmp(arg, "udp", 3) == 0) {
		G.ifilter.proto = SOCK_PROTO_UDP;
		arg += 3;
		if (*arg == '4') {
			G.ifilter.version = 4;
			arg++;
		} else if (*arg == '6') {
			G.ifilter.version = 6;
			arg++;
		}
	}

	if (*arg == '@') {
		const char *host_start;
		const char *colon;

		host_start = ++arg;
		colon = strrchr(arg, ':');
		if (colon) {
			G.ifilter.host = xstrndup(host_start, colon - host_start);
			arg = colon;
		} else {
			G.ifilter.host = xstrdup(host_start);
			arg += strlen(arg);
		}
	}

	if (*arg == ':') {
		arg++;
		if (isdigit(*arg)) {
			G.ifilter.port = atoi(arg);
		} else {
			char *etc_serv = NULL;
			const char *proto_str = NULL;
			if (G.ifilter.proto == SOCK_PROTO_TCP)
				proto_str = "tcp";
			else if (G.ifilter.proto == SOCK_PROTO_UDP)
				proto_str = "udp";
			G.ifilter.port = bb_get_servport_by_name(&etc_serv, arg, proto_str);
			free(etc_serv);
			if (G.ifilter.port < 0)
				G.ifilter.port = atoi(arg);
		}
	}
}

static int sock_matches_inet_filter(struct sock_info *si)
{
	if (!G.ifilter.active)
		return 1;

	/* Must be an internet socket (TCP or UDP) */
	if (!si || (si->proto != SOCK_PROTO_TCP && si->proto != SOCK_PROTO_UDP))
		return 0;

	if (G.ifilter.proto && si->proto != G.ifilter.proto)
		return 0;

	if (G.ifilter.version && si->version != G.ifilter.version)
		return 0;

	if (G.ifilter.port != -1) {
		if (si->port_local != G.ifilter.port && si->port_remote != G.ifilter.port)
			return 0;
	}

	if (G.ifilter.host) {
		int match = 0;
		if (si->ip_local && strcmp(si->ip_local, G.ifilter.host) == 0)
			match = 1;
		if (si->ip_remote && strcmp(si->ip_remote, G.ifilter.host) == 0)
			match = 1;
		if (!match)
			return 0;
	}

	return 1;
}

static int pid_in_list(pid_t pid, llist_t *list)
{
	while (list) {
		char *s = list->data;
		char *p;
		while (*s) {
			p = strchr(s, ',');
			if (p)
				*p = '\0';
			if ((pid_t)bb_strtou(s, NULL, 10) == pid) {
				if (p)
					*p = ',';
				return 1;
			}
			if (!p)
				break;
			*p = ',';
			s = p + 1;
		}
		list = list->link;
	}
	return 0;
}

static int user_in_list(uid_t uid, llist_t *list)
{
	const char *uname = get_cached_username(uid);

	while (list) {
		char *s = list->data;
		char *p;
		while (*s) {
			p = strchr(s, ',');
			if (p)
				*p = '\0';
			if (isdigit(*s)) {
				if ((uid_t)bb_strtou(s, NULL, 10) == uid) {
					if (p)
						*p = ',';
					return 1;
				}
			} else {
				if (uname && strcmp(uname, s) == 0) {
					if (p)
						*p = ',';
					return 1;
				}
			}
			if (!p)
				break;
			*p = ',';
			s = p + 1;
		}
		list = list->link;
	}
	return 0;
}

static int comm_in_list(const char *comm, llist_t *list)
{
	while (list) {
		if (strncmp(comm, list->data, strlen(list->data)) == 0)
			return 1;
		list = list->link;
	}
	return 0;
}

static int process_matches(pid_t pid, uid_t uid, const char *comm)
{
	if (!G.opt_p && !G.opt_u && !G.opt_c)
		return 1;

	if (G.opt_p && pid_in_list(pid, G.opt_p))
		return 1;
	if (G.opt_u && user_in_list(uid, G.opt_u))
		return 1;
	if (G.opt_c && comm_in_list(comm, G.opt_c))
		return 1;

	return 0;
}

static char get_fd_mode(unsigned pid, const char *fd_name, unsigned long long *pos_out)
{
	char path[sizeof("/proc/%u/fdinfo/") + sizeof(int)*3 + 32];
	FILE *f;
	char buf[128];
	char mode;
	unsigned long flags;

	mode = 'u';
	if (pos_out)
		*pos_out = 0;

	sprintf(path, "/proc/%u/fdinfo/%s", pid, fd_name);
	f = fopen_for_read(path);
	if (!f)
		return mode;

	while (fgets(buf, sizeof(buf), f)) {
		if (is_prefixed_with(buf, "pos:\t")) {
			if (pos_out)
				*pos_out = strtoull(buf + 5, NULL, 10);
		} else if (is_prefixed_with(buf, "flags:\t")) {
			flags = strtoul(buf + 7, NULL, 8);
			if ((flags & O_ACCMODE) == O_RDONLY)
				mode = 'r';
			else if ((flags & O_ACCMODE) == O_WRONLY)
				mode = 'w';
			else
				mode = 'u';
		}
	}

	fclose(f);
	return mode;
}

static void print_file_row(const char *comm, unsigned pid, const char *user,
                           const char *fd, const char *type, const char *device,
                           const char *size_off, const char *node, const char *name)
{
	printf("%-10.10s %5u %-6.6s %-4.4s   %-13.13s %10.10s %12.12s %10.10s %s\n",
	       comm, pid, user, fd, type, device, size_off, node, name);
}

static void read_proc_comm(unsigned pid, char *buf, size_t bufsz)
{
	char path[sizeof("/proc/%u/comm") + sizeof(int)*3];
	int fd;
	ssize_t r;

	buf[0] = '\0';
	sprintf(path, "/proc/%u/comm", pid);
	fd = open(path, O_RDONLY);
	if (fd >= 0) {
		r = read(fd, buf, bufsz - 1);
		close(fd);
		if (r > 0) {
			while (r > 0 && (buf[r-1] == '\n' || buf[r-1] == '\r'))
				r--;
			buf[r] = '\0';
		}
	}
}

static void scan_proc_mem(unsigned pid, const char *comm, const char *user)
{
	char path[sizeof("/proc/%u/maps") + sizeof(int)*3];
	FILE *f;
	char buf[512];
	unsigned long seen_inodes[128];
	int num_seen;

	num_seen = 0;
	sprintf(path, "/proc/%u/maps", pid);
	f = fopen_for_read(path);
	if (!f)
		return;

	while (fgets(buf, sizeof(buf), f)) {
		char *pathname;
		char dev[32];
		char size[32];
		char node[32];
		struct stat st;
		unsigned long inode;
		int i;

		pathname = strchr(buf, '/');
		if (!pathname)
			continue;

		pathname[strcspn(pathname, "\r\n")] = '\0';

		if (stat(pathname, &st) != 0)
			continue;

		inode = (unsigned long)st.st_ino;
		for (i = 0; i < num_seen; i++) {
			if (seen_inodes[i] == inode)
				break;
		}
		if (i < num_seen)
			continue;

		if (num_seen < 128)
			seen_inodes[num_seen++] = inode;

		sprintf(dev, "%u,%u", major(st.st_dev), minor(st.st_dev));
		sprintf(size, "%llu", (unsigned long long)st.st_size);
		sprintf(node, "%lu", inode);
		print_file_row(comm, pid, user, "mem", "REG", dev, size, node, pathname);
	}

	fclose(f);
}

static int scan_proc_files(procps_status_t *proc)
{
	char path[sizeof("/proc/%u/fd/0123456789") + sizeof(int)*3 + 32];
	char comm_buf[32];
	const char *comm;
	const char *user;
	DIR *d_fd;
	struct dirent *entry;
	int baseofs;
	int matched_any;

	matched_any = 0;
	read_proc_comm(proc->pid, comm_buf, sizeof(comm_buf));
	comm = comm_buf[0] ? comm_buf : (proc->comm[0] ? proc->comm : "?");
	user = get_cached_username(proc->uid);

	if (!process_matches(proc->pid, proc->uid, comm))
		return 0;

	if ((G.opts & OPT_t) && !G.ifilter.active) {
		printf("%u\n", proc->pid);
		return 1;
	}

	if (!G.ifilter.active) {
		/* cwd */
		sprintf(path, "/proc/%u/cwd", proc->pid);
		{
			char *target = xmalloc_readlink(path);
			if (target) {
				struct stat st;
				if (stat(path, &st) == 0) {
					char dev[32], size[32], node[32];
					sprintf(dev, "%u,%u", major(st.st_dev), minor(st.st_dev));
					sprintf(size, "%llu", (unsigned long long)st.st_size);
					sprintf(node, "%lu", (unsigned long)st.st_ino);
					print_file_row(comm, proc->pid, user, "cwd", "DIR", dev, size, node, target);
					matched_any = 1;
				}
				free(target);
			}
		}

		/* rtd */
		sprintf(path, "/proc/%u/root", proc->pid);
		{
			char *target = xmalloc_readlink(path);
			if (target) {
				struct stat st;
				if (stat(path, &st) == 0) {
					char dev[32], size[32], node[32];
					sprintf(dev, "%u,%u", major(st.st_dev), minor(st.st_dev));
					sprintf(size, "%llu", (unsigned long long)st.st_size);
					sprintf(node, "%lu", (unsigned long)st.st_ino);
					print_file_row(comm, proc->pid, user, "rtd", "DIR", dev, size, node, target);
					matched_any = 1;
				}
				free(target);
			}
		}

		/* txt */
		sprintf(path, "/proc/%u/exe", proc->pid);
		{
			char *target = xmalloc_readlink(path);
			if (target) {
				struct stat st;
				if (stat(path, &st) == 0) {
					char dev[32], size[32], node[32];
					sprintf(dev, "%u,%u", major(st.st_dev), minor(st.st_dev));
					sprintf(size, "%llu", (unsigned long long)st.st_size);
					sprintf(node, "%lu", (unsigned long)st.st_ino);
					print_file_row(comm, proc->pid, user, "txt", "REG", dev, size, node, target);
					matched_any = 1;
				}
				free(target);
			}
		}

		/* mem */
		scan_proc_mem(proc->pid, comm, user);
	}

	/* Now scan /proc/PID/fd */
	baseofs = sprintf(path, "/proc/%u/fd/", proc->pid);
	d_fd = opendir(path);
	if (d_fd) {
		while ((entry = readdir(d_fd)) != NULL) {
			char *fdlink;
			unsigned long long pos;
			char mode;

			if (entry->d_name[0] == '.')
				continue;

			safe_strncpy(path + baseofs, entry->d_name, sizeof(path) - baseofs);
			fdlink = xmalloc_readlink(path);
			if (!fdlink)
				continue;

			pos = 0;
			mode = get_fd_mode(proc->pid, entry->d_name, &pos);

			if (is_prefixed_with(fdlink, "socket:[")) {
				unsigned long sock_ino;
				struct sock_info *si;

				sock_ino = 0;
				sscanf(fdlink, "socket:[%lu]", &sock_ino);
				si = find_sock_entry(sock_ino);

				if (G.ifilter.active) {
					if (!si || !sock_matches_inet_filter(si)) {
						free(fdlink);
						continue;
					}
				}

				if (G.opts & OPT_t) {
					printf("%u\n", proc->pid);
					free(fdlink);
					closedir(d_fd);
					return 1;
				}

				{
					char *fd_str = xasprintf("%s%c", entry->d_name, mode);
					char node[32];
					const char *type_str;
					const char *dev_str;
					const char *name_str;

					sprintf(node, "%lu", sock_ino);
					if (si) {
						if (si->proto == SOCK_PROTO_UNIX)
							type_str = "unix";
						else
							type_str = (si->version == 6) ? "IPv6" : "IPv4";
						dev_str = si->dev ? si->dev : "0,9";
						name_str = si->name ? si->name : fdlink;
					} else {
						type_str = "sock";
						dev_str = "0,9";
						name_str = fdlink;
					}
					print_file_row(comm, proc->pid, user, fd_str, type_str, dev_str, "", node, name_str);
					free(fd_str);
					matched_any = 1;
				}
			} else if (is_prefixed_with(fdlink, "pipe:[")) {
				if (G.ifilter.active) {
					free(fdlink);
					continue;
				}
				if (G.opts & OPT_t) {
					printf("%u\n", proc->pid);
					free(fdlink);
					closedir(d_fd);
					return 1;
				}
				{
					unsigned long pipe_ino;
					char *fd_str;
					char node[32];

					pipe_ino = 0;
					sscanf(fdlink, "pipe:[%lu]", &pipe_ino);
					fd_str = xasprintf("%s%c", entry->d_name, mode);
					sprintf(node, "%lu", pipe_ino);
					print_file_row(comm, proc->pid, user, fd_str, "FIFO", "0,13", "", node, fdlink);
					free(fd_str);
					matched_any = 1;
				}
			} else if (is_prefixed_with(fdlink, "anon_inode:")) {
				if (G.ifilter.active) {
					free(fdlink);
					continue;
				}
				if (G.opts & OPT_t) {
					printf("%u\n", proc->pid);
					free(fdlink);
					closedir(d_fd);
					return 1;
				}
				{
					char *fd_str = xasprintf("%s%c", entry->d_name, mode);
					print_file_row(comm, proc->pid, user, fd_str, "a_inode", "0,14", "", "", fdlink);
					free(fd_str);
					matched_any = 1;
				}
			} else {
				if (G.ifilter.active) {
					free(fdlink);
					continue;
				}
				if (G.opts & OPT_t) {
					printf("%u\n", proc->pid);
					free(fdlink);
					closedir(d_fd);
					return 1;
				}
				{
					struct stat st;
					char dev[32];
					char size[32];
					char node[32];
					const char *ftype;
					char *fd_str;

					dev[0] = size[0] = node[0] = '\0';
					ftype = "REG";
					if (stat(path, &st) == 0) {
						sprintf(dev, "%u,%u", major(st.st_dev), minor(st.st_dev));
						sprintf(node, "%lu", (unsigned long)st.st_ino);
						if (S_ISDIR(st.st_mode))
							ftype = "DIR";
						else if (S_ISCHR(st.st_mode))
							ftype = "CHR";
						else if (S_ISBLK(st.st_mode))
							ftype = "BLK";
						else if (S_ISFIFO(st.st_mode))
							ftype = "FIFO";
						else if (S_ISSOCK(st.st_mode))
							ftype = "unix";
						else
							ftype = "REG";

						if (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode))
							sprintf(size, "%llu", (unsigned long long)st.st_size);
						else if (pos > 0)
							sprintf(size, "0t%llu", (unsigned long long)pos);
					}
					fd_str = xasprintf("%s%c", entry->d_name, mode);
					print_file_row(comm, proc->pid, user, fd_str, ftype, dev, size, node, fdlink);
					free(fd_str);
					matched_any = 1;
				}
			}

			free(fdlink);
		}
		closedir(d_fd);
	}

	return matched_any;
}

int lsof_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int lsof_main(int argc UNUSED_PARAM, char **argv)
{
	procps_status_t *proc;

	INIT_G();

	G.opts = getopt32(argv, "p:*u:*c:*ti::", &G.opt_p, &G.opt_u, &G.opt_c, &G.opt_i);

	argv += optind;

	if ((G.opts & OPT_i) && !G.opt_i && *argv) {
		char *arg = *argv;
		if (arg[0] == ':' || arg[0] == '@' ||
		    strncasecmp(arg, "tcp", 3) == 0 ||
		    strncasecmp(arg, "udp", 3) == 0) {
			G.opt_i = *argv++;
		}
	}

	if (G.opts & OPT_i)
		parse_inet_filter(G.opt_i);

	parse_proc_net_ip("/proc/net/tcp", SOCK_PROTO_TCP, 4);
	parse_proc_net_ip("/proc/net/tcp6", SOCK_PROTO_TCP, 6);
	parse_proc_net_ip("/proc/net/udp", SOCK_PROTO_UDP, 4);
	parse_proc_net_ip("/proc/net/udp6", SOCK_PROTO_UDP, 6);
	parse_proc_net_unix();

	if (!(G.opts & OPT_t)) {
		puts("COMMAND    PID   USER   FD      TYPE             DEVICE     SIZE/OFF       NODE NAME");
	}

	proc = NULL;
	while ((proc = procps_scan(proc, PSSCAN_PID | PSSCAN_COMM | PSSCAN_UIDGID)) != NULL) {
		scan_proc_files(proc);
	}

	return EXIT_SUCCESS;
}
