/* vi: set sw=4 ts=4: */
/*
 * ss implementation for busybox-ng
 *
 * Licensed under GPLv2 or later, see file LICENSE in this source tree.
 */
//config:config SS
//config:	bool "ss (12 kb)"
//config:	default y
//config:	help
//config:	Socket statistics. Similar to netstat but shows more information.

//applet:IF_SS(APPLET(ss, BB_DIR_USR_SBIN, BB_SUID_DROP))

//kbuild:lib-$(CONFIG_SS) += ss.o

//usage:#define ss_trivial_usage
//usage:       "[-nraeloepismbEZz460tuwxdS] [-N NSNAME] [-f FAMILY] [-A QUERY] [-D FILE] [-F FILE]"
//usage:#define ss_full_usage "\n\n"
//usage:       "Display socket statistics\n"
//usage:     "\n	-h, --help		Show this help"
//usage:     "\n	-V, --version		Show version"
//usage:     "\n	-n, --numeric		Don't resolve service names"
//usage:     "\n	-r, --resolve		Resolve host names"
//usage:     "\n	-a, --all		Display all sockets"
//usage:     "\n	-l, --listening		Display listening sockets"
//usage:     "\n	-o, --options		Show timer information"
//usage:     "\n	-e, --extended		Show detailed socket information"
//usage:     "\n	-m, --memory		Show socket memory usage"
//usage:     "\n	-p, --processes		Show process using socket"
//usage:     "\n	-i, --info		Show internal TCP information"
//usage:     "\n	-s, --summary		Show socket usage summary"
//usage:     "\n	-b, --bpf		Show BPF information"
//usage:     "\n	-E, --events		Display events"
//usage:     "\n	-Z, --context		Display security context"
//usage:     "\n	-z, --contexts		Display security contexts"
//usage:     "\n	-N, --net NSNAME	Switch network namespace"
//usage:     "\n	-4, --ipv4		Display only IP version 4 sockets"
//usage:     "\n	-6, --ipv6		Display only IP version 6 sockets"
//usage:     "\n	-0, --packet		Display PACKET sockets"
//usage:     "\n	-t, --tcp		Display only TCP sockets"
//usage:     "\n	-u, --udp		Display only UDP sockets"
//usage:     "\n	-d, --dccp		Display only DCCP sockets"
//usage:     "\n	-w, --raw		Display only RAW sockets"
//usage:     "\n	-x, --unix		Display only Unix domain sockets"
//usage:     "\n	-S, --sctp		Display only SCTP sockets"
//usage:     "\n	-f, --family=FAMILY	Display sockets of type FAMILY"
//usage:     "\n	-A, --query=QUERY	Display sockets of type QUERY"
//usage:     "\n	-D, --diag=FILE		Dump raw socket info to FILE"
//usage:     "\n	-F, --filter=FILE	Read filter from FILE"

#include "libbb.h"
#undef getservbyport
#include <net/if.h>

enum {
	OPT_h = (1 << 0),
	OPT_V = (1 << 1),
	OPT_n = (1 << 2),
	OPT_r = (1 << 3),
	OPT_a = (1 << 4),
	OPT_l = (1 << 5),
	OPT_o = (1 << 6),
	OPT_e = (1 << 7),
	OPT_m = (1 << 8),
	OPT_p = (1 << 9),
	OPT_i = (1 << 10),
	OPT_s = (1 << 11),
	OPT_b = (1 << 12),
	OPT_E = (1 << 13),
	OPT_Z = (1 << 14),
	OPT_z = (1 << 15),
	OPT_4 = (1 << 16),
	OPT_6 = (1 << 17),
	OPT_0 = (1 << 18),
	OPT_t = (1 << 19),
	OPT_u = (1 << 20),
	OPT_d = (1 << 21),
	OPT_w = (1 << 22),
	OPT_x = (1 << 23),
	OPT_S = (1 << 24),
	OPT_N = (1 << 25),
	OPT_f = (1 << 26),
	OPT_A = (1 << 27),
	OPT_D = (1 << 28),
	OPT_F = (1 << 29),
};

static const char ss_longopts[] ALIGN1 =
	"help\0"       No_argument       "h"
	"version\0"    No_argument       "V"
	"numeric\0"    No_argument       "n"
	"resolve\0"    No_argument       "r"
	"all\0"        No_argument       "a"
	"listening\0"  No_argument       "l"
	"options\0"    No_argument       "o"
	"extended\0"   No_argument       "e"
	"memory\0"     No_argument       "m"
	"processes\0"  No_argument       "p"
	"info\0"       No_argument       "i"
	"summary\0"    No_argument       "s"
	"bpf\0"        No_argument       "b"
	"events\0"     No_argument       "E"
	"context\0"    No_argument       "Z"
	"contexts\0"   No_argument       "z"
	"ipv4\0"       No_argument       "4"
	"ipv6\0"       No_argument       "6"
	"packet\0"     No_argument       "0"
	"tcp\0"        No_argument       "t"
	"udp\0"        No_argument       "u"
	"dccp\0"       No_argument       "d"
	"raw\0"        No_argument       "w"
	"unix\0"       No_argument       "x"
	"sctp\0"       No_argument       "S"
	"net\0"        Required_argument "N"
	"family\0"     Required_argument "f"
	"query\0"      Required_argument "A"
	"socket\0"     Required_argument "A"
	"diag\0"       Required_argument "D"
	"filter\0"     Required_argument "F"
	;

static const char *const tcp_state_names[] ALIGN_PTR = {
	"",            /* 00 */
	"ESTABLISHED", /* 01 */
	"SYN_SENT",    /* 02 */
	"SYN_RECV",    /* 03 */
	"FIN_WAIT1",   /* 04 */
	"FIN_WAIT2",   /* 05 */
	"TIME_WAIT",   /* 06 */
	"CLOSE",       /* 07 */
	"CLOSE_WAIT",  /* 08 */
	"LAST_ACK",    /* 09 */
	"LISTEN",      /* 0A */
	"CLOSING",     /* 0B */
};

struct proc_user {
	struct proc_user *next;
	long inode;
	int pid;
	int fd;
	char comm[16];
};

#define PROC_HASH_SIZE 1024

struct globals {
	struct proc_user *user_hash[PROC_HASH_SIZE];
	int proc_cache_loaded;

	smallint filter_listen;   /* -l: 1, -a: -1 (all), 0: connected */
	smallint filter_numeric;  /* -n */
	smallint filter_resolve;  /* -r */
	smallint filter_options;  /* -o: timer */
	smallint filter_extended; /* -e: extended info */
	smallint filter_processes;/* -p: processes */

	smallint filter_ipv4;     /* -4 */
	smallint filter_ipv6;     /* -6 */

	smallint proto_tcp;
	smallint proto_udp;
	smallint proto_raw;
	smallint proto_unix;
	smallint proto_packet;
	smallint proto_dccp;
	smallint proto_sctp;

	smallint header_printed;
};
#define G (*(struct globals *)ptr_to_globals)
#define INIT_G() do { \
	SET_PTR_TO_GLOBALS(xzalloc(sizeof(G))); \
} while (0)

static void add_sock_user(long inode, int pid, int fd, const char *comm)
{
	unsigned hash;
	struct proc_user *u;

	hash = (unsigned long)inode % PROC_HASH_SIZE;
	for (u = G.user_hash[hash]; u; u = u->next) {
		if (u->inode == inode && u->pid == pid && u->fd == fd)
			return;
	}

	u = xzalloc(sizeof(*u));
	u->next = G.user_hash[hash];
	u->inode = inode;
	u->pid = pid;
	u->fd = fd;
	safe_strncpy(u->comm, comm, sizeof(u->comm));
	G.user_hash[hash] = u;
}

static void load_process_cache(void)
{
	DIR *proc_dir;
	struct dirent *de;

	if (G.proc_cache_loaded)
		return;
	G.proc_cache_loaded = 1;

	proc_dir = opendir("/proc");
	if (!proc_dir)
		return;

	while ((de = readdir(proc_dir)) != NULL) {
		char *endptr;
		int pid;
		char comm[16];
		char path[256];
		DIR *fd_dir;
		struct dirent *fde;
		int comm_fd;
		ssize_t n;
		char *nl;

		if (!isdigit(de->d_name[0]))
			continue;

		pid = bb_strtou(de->d_name, &endptr, 10);
		if (*endptr != '\0')
			continue;

		/* Read /proc/PID/comm */
		comm[0] = '\0';
		snprintf(path, sizeof(path), "/proc/%u/comm", pid);
		comm_fd = open(path, O_RDONLY);
		if (comm_fd >= 0) {
			n = read(comm_fd, comm, sizeof(comm) - 1);
			if (n > 0) {
				comm[n] = '\0';
				nl = strpbrk(comm, "\r\n");
				if (nl)
					*nl = '\0';
			}
			close(comm_fd);
		}
		if (comm[0] == '\0')
			continue;

		/* Scan /proc/PID/fd */
		snprintf(path, sizeof(path), "/proc/%u/fd", pid);
		fd_dir = opendir(path);
		if (!fd_dir)
			continue;

		while ((fde = readdir(fd_dir)) != NULL) {
			char fd_path[280];
			char target[256];
			ssize_t len;
			long inode;
			int sfd;

			if (!isdigit(fde->d_name[0]))
				continue;

			sfd = bb_strtou(fde->d_name, &endptr, 10);
			if (*endptr != '\0')
				continue;

			snprintf(fd_path, sizeof(fd_path), "/proc/%u/fd/%s", pid, fde->d_name);
			len = readlink(fd_path, target, sizeof(target) - 1);
			if (len <= 0)
				continue;
			target[len] = '\0';

			if (strncmp(target, "socket:[", 8) == 0) {
				inode = bb_strtoul(target + 8, &endptr, 10);
				if (*endptr == ']')
					add_sock_user(inode, pid, sfd, comm);
			} else if (strncmp(target, "[0000]:", 7) == 0) {
				inode = bb_strtoul(target + 7, NULL, 10);
				if (inode > 0)
					add_sock_user(inode, pid, sfd, comm);
			}
		}
		closedir(fd_dir);
	}
	closedir(proc_dir);
}

static char *get_process_info(long inode)
{
	unsigned hash;
	struct proc_user *u;
	char *res = NULL;

	hash = (unsigned long)inode % PROC_HASH_SIZE;
	for (u = G.user_hash[hash]; u; u = u->next) {
		if (u->inode == inode) {
			if (!res) {
				res = xasprintf("users:((\"%s\",pid=%d,fd=%d)", u->comm, u->pid, u->fd);
			} else {
				char *old = res;
				res = xasprintf("%s,(\"%s\",pid=%d,fd=%d)", old, u->comm, u->pid, u->fd);
				free(old);
			}
		}
	}
	if (res) {
		char *old = res;
		res = xasprintf("%s)", old);
		free(old);
	}
	return res;
}

static void print_header(void)
{
	if (G.header_printed)
		return;
	G.header_printed = 1;

	printf("%-6s %-11s %-7s %-7s %-25s %-25s",
	       "Netid", "State", "Recv-Q", "Send-Q",
	       "Local Address:Port", "Peer Address:Port");
	if (G.filter_processes)
		printf(" Process");
	bb_putchar('\n');
}

static void print_line(const char *netid, const char *state,
                       unsigned long rxq, unsigned long txq,
                       const char *local_addr, const char *peer_addr,
                       long inode, const char *timer_info, const char *ext_info)
{
	printf("%-6s %-11s %-7lu %-7lu %-25s %-25s",
	       netid, state, rxq, txq, local_addr, peer_addr);

	if (G.filter_processes) {
		char *pinfo = (inode > 0) ? get_process_info(inode) : NULL;
		if (pinfo) {
			printf(" %s", pinfo);
			free(pinfo);
		}
	}

	if (G.filter_options && timer_info && timer_info[0]) {
		printf(" %s", timer_info);
	}

	if (G.filter_extended && ext_info && ext_info[0]) {
		printf(" %s", ext_info);
	}

	bb_putchar('\n');
}

static void format_ipv4(char *buf, size_t buflen, const char *hex_addr, unsigned port, const char *proto)
{
	uint32_t ip;
	char host[256];
	char port_str[32];
	struct servent *se;
	struct hostent *he;

	ip = bb_strtou(hex_addr, NULL, 16);

	if (ip == 0) {
		strcpy(host, "0.0.0.0");
	} else if (G.filter_resolve) {
		he = gethostbyaddr((const void *)&ip, sizeof(ip), AF_INET);
		if (he && he->h_name) {
			safe_strncpy(host, he->h_name, sizeof(host));
		} else {
			snprintf(host, sizeof(host), "%u.%u.%u.%u",
			         ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
		}
	} else {
		snprintf(host, sizeof(host), "%u.%u.%u.%u",
		         ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
	}

	if (port == 0) {
		strcpy(port_str, "*");
	} else if (G.filter_numeric) {
		snprintf(port_str, sizeof(port_str), "%u", port);
	} else if (strcmp(proto, "tcp") == 0 || strcmp(proto, "udp") == 0) {
		se = getservbyport(htons(port), proto);
		if (se && se->s_name)
			safe_strncpy(port_str, se->s_name, sizeof(port_str));
		else
			snprintf(port_str, sizeof(port_str), "%u", port);
	} else {
		snprintf(port_str, sizeof(port_str), "%u", port);
	}

	snprintf(buf, buflen, "%s:%s", host, port_str);
}

static void format_ipv6(char *buf, size_t buflen, const char *hex_addr, unsigned port, const char *proto)
{
	struct in6_addr in6;
	char ip6_str[INET6_ADDRSTRLEN];
	char port_str[32];
	struct servent *se;
	struct hostent *he;
	int i;
	int is_unspec;

	memset(&in6, 0, sizeof(in6));
	for (i = 0; i < 4; i++) {
		char hex8[9];
		uint32_t val;
		safe_strncpy(hex8, hex_addr + i * 8, 9);
		val = bb_strtou(hex8, NULL, 16);
		in6.s6_addr[i * 4 + 0] = val & 0xFF;
		in6.s6_addr[i * 4 + 1] = (val >> 8) & 0xFF;
		in6.s6_addr[i * 4 + 2] = (val >> 16) & 0xFF;
		in6.s6_addr[i * 4 + 3] = (val >> 24) & 0xFF;
	}

	is_unspec = IN6_IS_ADDR_UNSPECIFIED(&in6);

	if (port == 0) {
		strcpy(port_str, "*");
	} else if (G.filter_numeric) {
		snprintf(port_str, sizeof(port_str), "%u", port);
	} else if (strcmp(proto, "tcp") == 0 || strcmp(proto, "udp") == 0) {
		se = getservbyport(htons(port), proto);
		if (se && se->s_name)
			safe_strncpy(port_str, se->s_name, sizeof(port_str));
		else
			snprintf(port_str, sizeof(port_str), "%u", port);
	} else {
		snprintf(port_str, sizeof(port_str), "%u", port);
	}

	if (is_unspec) {
		if (port == 0)
			safe_strncpy(buf, "[::]:*", buflen);
		else
			snprintf(buf, buflen, "*:%s", port_str);
		return;
	}

	inet_ntop(AF_INET6, &in6, ip6_str, sizeof(ip6_str));
	if (G.filter_resolve) {
		he = gethostbyaddr((const void *)&in6, sizeof(in6), AF_INET6);
		if (he && he->h_name) {
			snprintf(buf, buflen, "[%s]:%s", he->h_name, port_str);
			return;
		}
	}

	snprintf(buf, buflen, "[%s]:%s", ip6_str, port_str);
}

static void read_proc_inet(const char *filename, const char *netid, int is_ipv6, int is_tcp, int is_udp)
{
	FILE *fp;
	char line[1024];

	fp = fopen_for_read(filename);
	if (!fp)
		return;

	/* Skip header */
	if (!fgets(line, sizeof(line), fp)) {
		fclose(fp);
		return;
	}

	while (fgets(line, sizeof(line), fp)) {
		char loc_hex[65], rem_hex[65];
		unsigned loc_port, rem_port, state;
		unsigned long txq, rxq;
		unsigned tr;
		unsigned long tm_when, retrnsmt;
		unsigned uid;
		unsigned long inode;
		unsigned long sk;
		int n;
		const char *state_str;
		char local_str[256];
		char peer_str[256];
		char timer_buf[64];
		char ext_buf[64];
		int is_listen;

		loc_hex[0] = '\0';
		rem_hex[0] = '\0';
		loc_port = rem_port = state = 0;
		txq = rxq = 0;
		tr = 0;
		tm_when = retrnsmt = 0;
		uid = 0;
		inode = 0;
		sk = 0;

		n = sscanf(line,
		           "%*d: %64[0-9A-Fa-f]:%X %64[0-9A-Fa-f]:%X %X %lX:%lX %X:%lX %lX %u %*d %lu %*d %lx",
		           loc_hex, &loc_port,
		           rem_hex, &rem_port,
		           &state,
		           &txq, &rxq,
		           &tr, &tm_when, &retrnsmt,
		           &uid, &inode, &sk);

		if (n < 7)
			continue;

		if (is_tcp) {
			is_listen = (state == 0x0A);
		} else {
			is_listen = (rem_port == 0);
		}

		if (G.filter_listen == 1 && !is_listen)
			continue;
		if (G.filter_listen == 0 && is_listen)
			continue;

		if (is_tcp) {
			if (state < ARRAY_SIZE(tcp_state_names) && tcp_state_names[state][0])
				state_str = tcp_state_names[state];
			else
				state_str = "UNKNOWN";
		} else if (is_udp) {
			state_str = is_listen ? "UNCONN" : "ESTAB";
		} else {
			state_str = "UNCONN";
		}

		if (is_ipv6) {
			format_ipv6(local_str, sizeof(local_str), loc_hex, loc_port, netid);
			format_ipv6(peer_str, sizeof(peer_str), rem_hex, rem_port, netid);
		} else {
			format_ipv4(local_str, sizeof(local_str), loc_hex, loc_port, netid);
			format_ipv4(peer_str, sizeof(peer_str), rem_hex, rem_port, netid);
		}

		timer_buf[0] = '\0';
		if (G.filter_options && tr != 0) {
			const char *tname;
			switch (tr) {
			case 1: tname = "on"; break;
			case 2: tname = "keepalive"; break;
			case 3: tname = "timewait"; break;
			case 4: tname = "probe"; break;
			default: tname = "unknown"; break;
			}
			snprintf(timer_buf, sizeof(timer_buf), "timer:(%s,%lu.%03lusec,%lu)",
			         tname, tm_when / 100, (tm_when % 100) * 10, retrnsmt);
		}

		ext_buf[0] = '\0';
		if (G.filter_extended && inode > 0) {
			snprintf(ext_buf, sizeof(ext_buf), "uid:%u ino:%lu sk:%lx", uid, inode, sk);
		}

		print_line(netid, state_str, rxq, txq, local_str, peer_str, inode, timer_buf, ext_buf);
	}

	fclose(fp);
}

static void read_proc_unix(void)
{
	FILE *fp;
	char line[1024];

	fp = fopen_for_read("/proc/net/unix");
	if (!fp)
		return;

	/* Skip header */
	if (!fgets(line, sizeof(line), fp)) {
		fclose(fp);
		return;
	}

	while (fgets(line, sizeof(line), fp)) {
		unsigned long flags;
		unsigned type, st;
		long inode;
		char path[512];
		const char *netid;
		const char *state;
		int is_listen;
		char local_addr[550];
		char peer_addr[32];
		int n;

		flags = 0;
		type = st = 0;
		inode = 0;
		path[0] = '\0';

		n = sscanf(line, "%*p: %*x %*x %lx %x %x %ld %511s",
		           &flags, &type, &st, &inode, path);
		if (n < 4)
			continue;

		switch (type) {
		case 1: netid = "u_str"; break;
		case 2: netid = "u_dgr"; break;
		case 5: netid = "u_seq"; break;
		default: netid = "u_raw"; break;
		}

		is_listen = ((flags & 0x00010000) != 0);

		if (G.filter_listen == 1 && !is_listen)
			continue;
		if (G.filter_listen == 0 && is_listen)
			continue;

		if (is_listen) {
			state = "LISTEN";
		} else {
			switch (st) {
			case 1: state = "UNCONN"; break;
			case 2: state = "SYN_SENT"; break;
			case 3: state = "ESTABLISHED"; break;
			case 4: state = "CLOSING"; break;
			default: state = "UNKNOWN"; break;
			}
		}

		if (path[0] != '\0')
			snprintf(local_addr, sizeof(local_addr), "%s %ld", path, inode);
		else
			snprintf(local_addr, sizeof(local_addr), "* %ld", inode);

		strcpy(peer_addr, "* 0");

		print_line(netid, state, 0, 0, local_addr, peer_addr, inode, NULL, NULL);
	}

	fclose(fp);
}

static void read_proc_packet(void)
{
	FILE *fp;
	char line[1024];

	if (G.filter_listen == 1)
		return;

	fp = fopen_for_read("/proc/net/packet");
	if (!fp)
		return;

	/* Skip header */
	if (!fgets(line, sizeof(line), fp)) {
		fclose(fp);
		return;
	}

	while (fgets(line, sizeof(line), fp)) {
		unsigned type, proto, iface;
		long inode;
		char proto_str[16];
		char ifname[IFNAMSIZ];
		char local_addr[64];
		const char *netid;
		int n;

		type = proto = iface = 0;
		inode = 0;

		n = sscanf(line, "%*s %*d %u %x %u %*d %*d %*d %ld",
		           &type, &proto, &iface, &inode);
		if (n < 4)
			continue;

		netid = (type == 2) ? "p_dgr" : "p_raw";
		if (proto == 0x0800)
			strcpy(proto_str, "ip");
		else if (proto == 0x86dd)
			strcpy(proto_str, "ipv6");
		else if (proto == 0x0806)
			strcpy(proto_str, "arp");
		else if (proto == 0x0003)
			strcpy(proto_str, "*");
		else
			snprintf(proto_str, sizeof(proto_str), "%04x", proto);

		if (if_indextoname(iface, ifname) == NULL)
			snprintf(ifname, sizeof(ifname), "%u", iface);

		snprintf(local_addr, sizeof(local_addr), "%s:%s", proto_str, ifname);

		print_line(netid, "UNCONN", 0, 0, local_addr, "*", inode, NULL, NULL);
	}

	fclose(fp);
}

static void print_summary(void)
{
	FILE *fp;
	char line[256];
	unsigned sockstat_total = 0;
	unsigned tcp_inuse4 = 0, tcp_orphan = 0, tcp_tw = 0, tcp_alloc = 0;
	unsigned udp_inuse4 = 0, raw_inuse4 = 0, frag_inuse4 = 0;
	unsigned tcp_inuse6 = 0, udp_inuse6 = 0, raw_inuse6 = 0, frag_inuse6 = 0;
	unsigned tcp_estab = 0, tcp_closed = 0;
	unsigned total_raw, total_udp, total_tcp, total_inet, total_frag;
	unsigned inet4, inet6;

	fp = fopen_for_read("/proc/net/sockstat");
	if (fp) {
		while (fgets(line, sizeof(line), fp)) {
			if (sscanf(line, "sockets: used %u", &sockstat_total) == 1)
				continue;
			if (sscanf(line, "TCP: inuse %u orphan %u tw %u alloc %u",
			           &tcp_inuse4, &tcp_orphan, &tcp_tw, &tcp_alloc) >= 1)
				continue;
			if (sscanf(line, "UDP: inuse %u", &udp_inuse4) == 1)
				continue;
			if (sscanf(line, "RAW: inuse %u", &raw_inuse4) == 1)
				continue;
			if (sscanf(line, "FRAG: inuse %u", &frag_inuse4) == 1)
				continue;
		}
		fclose(fp);
	}

	fp = fopen_for_read("/proc/net/sockstat6");
	if (fp) {
		while (fgets(line, sizeof(line), fp)) {
			if (sscanf(line, "TCP6: inuse %u", &tcp_inuse6) == 1)
				continue;
			if (sscanf(line, "UDP6: inuse %u", &udp_inuse6) == 1)
				continue;
			if (sscanf(line, "RAW6: inuse %u", &raw_inuse6) == 1)
				continue;
			if (sscanf(line, "FRAG6: inuse %u", &frag_inuse6) == 1)
				continue;
		}
		fclose(fp);
	}

	fp = fopen_for_read("/proc/net/tcp");
	if (fp) {
		while (fgets(line, sizeof(line), fp)) {
			unsigned st;
			if (sscanf(line, "%*d: %*s %*s %X", &st) == 1) {
				if (st == 0x01)
					tcp_estab++;
				else if (st == 0x07 || st == 0x06)
					tcp_closed++;
			}
		}
		fclose(fp);
	}

	fp = fopen_for_read("/proc/net/tcp6");
	if (fp) {
		while (fgets(line, sizeof(line), fp)) {
			unsigned st;
			if (sscanf(line, "%*d: %*s %*s %X", &st) == 1) {
				if (st == 0x01)
					tcp_estab++;
				else if (st == 0x07 || st == 0x06)
					tcp_closed++;
			}
		}
		fclose(fp);
	}

	total_raw = raw_inuse4 + raw_inuse6;
	total_udp = udp_inuse4 + udp_inuse6;
	total_tcp = tcp_inuse4 + tcp_inuse6;
	inet4 = raw_inuse4 + udp_inuse4 + tcp_inuse4;
	inet6 = raw_inuse6 + udp_inuse6 + tcp_inuse6;
	total_inet = inet4 + inet6;
	total_frag = frag_inuse4 + frag_inuse6;

	printf("Total: %u\n", sockstat_total);
	printf("TCP:   %u (estab %u, closed %u, orphaned %u, timewait %u)\n\n",
	       tcp_alloc, tcp_estab, tcp_closed, tcp_orphan, tcp_tw);
	printf("Transport Total  IPv4  IPv6\n");
	printf("%-9s %-6u %-5u %-5u\n", "*", sockstat_total, inet4, inet6);
	printf("%-9s %-6u %-5u %-5u\n", "RAW", total_raw, raw_inuse4, raw_inuse6);
	printf("%-9s %-6u %-5u %-5u\n", "UDP", total_udp, udp_inuse4, udp_inuse6);
	printf("%-9s %-6u %-5u %-5u\n", "TCP", total_tcp, tcp_inuse4, tcp_inuse6);
	printf("%-9s %-6u %-5u %-5u\n", "INET", total_inet, inet4, inet6);
	printf("%-9s %-6u %-5u %-5u\n", "FRAG", total_frag, frag_inuse4, frag_inuse6);
}

int ss_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int ss_main(int argc UNUSED_PARAM, char **argv)
{
	char *opt_N = NULL;
	char *opt_f = NULL;
	char *opt_A = NULL;
	char *opt_D = NULL;
	char *opt_F = NULL;
	uint32_t opts;

	INIT_G();

	G.filter_ipv4 = 1;
	G.filter_ipv6 = 1;

	opts = getopt32long(argv, "^hVnraloempisbEZz460tudwxSN:f:A:D:F:", ss_longopts,
	                    &opt_N, &opt_f, &opt_A, &opt_D, &opt_F);

	if (opts & OPT_h)
		bb_show_usage();

	if (opts & OPT_V) {
		printf("ss utility, iproute2-7.2.0 (busybox-ng %s)\n", BB_VER);
		return EXIT_SUCCESS;
	}

	if (opts & OPT_s) {
		print_summary();
		return EXIT_SUCCESS;
	}

	/* Family -f */
	if (opt_f) {
		if (strcmp(opt_f, "inet") == 0) {
			G.filter_ipv4 = 1;
			G.filter_ipv6 = 0;
		} else if (strcmp(opt_f, "inet6") == 0) {
			G.filter_ipv6 = 1;
			G.filter_ipv4 = 0;
		} else if (strcmp(opt_f, "unix") == 0) {
			G.proto_unix = 1;
			G.filter_ipv4 = 0;
			G.filter_ipv6 = 0;
		} else if (strcmp(opt_f, "link") == 0 || strcmp(opt_f, "packet") == 0) {
			G.proto_packet = 1;
			G.filter_ipv4 = 0;
			G.filter_ipv6 = 0;
		} else if (strcmp(opt_f, "all") == 0) {
			G.filter_ipv4 = 1;
			G.filter_ipv6 = 1;
			G.proto_unix = 1;
			G.proto_packet = 1;
		}
	}

	/* Query -A */
	if (opt_A) {
		char *str = xstrdup(opt_A);
		char *token;
		char *saveptr;

		for (token = strtok_r(str, ",", &saveptr); token; token = strtok_r(NULL, ",", &saveptr)) {
			if (strcmp(token, "all") == 0) {
				G.proto_tcp = 1;
				G.proto_udp = 1;
				G.proto_raw = 1;
				G.proto_unix = 1;
				G.proto_packet = 1;
			} else if (strcmp(token, "inet") == 0) {
				G.proto_tcp = 1;
				G.proto_udp = 1;
				G.proto_raw = 1;
			} else if (strcmp(token, "tcp") == 0) {
				G.proto_tcp = 1;
			} else if (strcmp(token, "udp") == 0) {
				G.proto_udp = 1;
			} else if (strcmp(token, "raw") == 0) {
				G.proto_raw = 1;
			} else if (strcmp(token, "unix") == 0 || is_prefixed_with(token, "unix_")) {
				G.proto_unix = 1;
			} else if (strcmp(token, "packet") == 0 || is_prefixed_with(token, "packet_")) {
				G.proto_packet = 1;
			} else if (strcmp(token, "dccp") == 0) {
				G.proto_dccp = 1;
			} else if (strcmp(token, "sctp") == 0) {
				G.proto_sctp = 1;
			}
		}
		free(str);
	}

	if (opts & OPT_4) {
		G.filter_ipv4 = 1;
		G.filter_ipv6 = 0;
	}
	if (opts & OPT_6) {
		G.filter_ipv6 = 1;
		if (!(opts & OPT_4))
			G.filter_ipv4 = 0;
	}

	if (opts & OPT_t) G.proto_tcp = 1;
	if (opts & OPT_u) G.proto_udp = 1;
	if (opts & OPT_w) G.proto_raw = 1;
	if (opts & OPT_x) G.proto_unix = 1;
	if (opts & OPT_0) G.proto_packet = 1;
	if (opts & OPT_d) G.proto_dccp = 1;
	if (opts & OPT_S) G.proto_sctp = 1;

	if (opts & OPT_a)
		G.filter_listen = -1; /* all */
	else if (opts & OPT_l)
		G.filter_listen = 1;  /* listening only */
	else
		G.filter_listen = 0;  /* connected only */

	G.filter_numeric = (opts & OPT_n) ? 1 : 0;
	G.filter_resolve = (opts & OPT_r) ? 1 : 0;
	G.filter_options = (opts & OPT_o) ? 1 : 0;
	G.filter_extended = (opts & OPT_e) ? 1 : 0;
	G.filter_processes = (opts & OPT_p) ? 1 : 0;

	/* If no protocols were selected at all */
	if (!G.proto_tcp && !G.proto_udp && !G.proto_raw && !G.proto_unix && !G.proto_packet && !G.proto_dccp && !G.proto_sctp) {
		G.proto_tcp = 1;
		G.proto_udp = 1;
		G.proto_raw = 1;
		if (G.filter_ipv4 && G.filter_ipv6 && !(opts & (OPT_4 | OPT_6))) {
			G.proto_unix = 1;
			G.proto_packet = 1;
		}
	}

	if (G.filter_processes)
		load_process_cache();

	print_header();

	if (G.proto_tcp) {
		if (G.filter_ipv4)
			read_proc_inet("/proc/net/tcp", "tcp", 0, 1, 0);
		if (G.filter_ipv6)
			read_proc_inet("/proc/net/tcp6", "tcp", 1, 1, 0);
	}

	if (G.proto_dccp) {
		if (G.filter_ipv4)
			read_proc_inet("/proc/net/dccp", "dccp", 0, 1, 0);
		if (G.filter_ipv6)
			read_proc_inet("/proc/net/dccp6", "dccp", 1, 1, 0);
	}

	if (G.proto_udp) {
		if (G.filter_ipv4)
			read_proc_inet("/proc/net/udp", "udp", 0, 0, 1);
		if (G.filter_ipv6)
			read_proc_inet("/proc/net/udp6", "udp", 1, 0, 1);
	}

	if (G.proto_raw) {
		if (G.filter_ipv4)
			read_proc_inet("/proc/net/raw", "raw", 0, 0, 0);
		if (G.filter_ipv6)
			read_proc_inet("/proc/net/raw6", "raw", 1, 0, 0);
	}

	if (G.proto_unix) {
		read_proc_unix();
	}

	if (G.proto_packet) {
		read_proc_packet();
	}

	return EXIT_SUCCESS;
}
