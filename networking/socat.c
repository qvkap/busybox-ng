/* vi: set sw=4 ts=4: */
/*
 * Mini socat implementation for busybox-ng
 *
 * Licensed under GPLv2 or later, see the LICENSE file in this source tree.
 */
//config:config SOCAT
//config:	bool "socat (multipurpose relay)"
//config:	default y
//config:	help
//config:	socat is a relay for bidirectional data transfers between two
//config:	independent data channels.
//applet:IF_SOCAT(APPLET(socat, BB_DIR_USR_BIN, BB_SUID_DROP))
//kbuild:lib-$(CONFIG_SOCAT) += socat.o
//usage:#define socat_trivial_usage "[-v] [-d] [-t TIMEOUT] <address1> <address2>"
//usage:#define socat_full_usage "\n\n"
//usage:       "Bidirectional data relay between two channels\n"
//usage:     "\n	-v		Verbose (hex/text dump of transferred data)"
//usage:     "\n	-d		Increase verbosity / debug output"
//usage:     "\n	-t TIMEOUT	Timeout in seconds before closing other half on EOF"
//usage:     "\n"
//usage:     "\nSupported addresses:"
//usage:     "\n	- or STDIN / STDOUT"
//usage:     "\n	TCP:<host>:<port> or TCP4:<host>:<port>"
//usage:     "\n	TCP-LISTEN:<port> or TCP4-LISTEN:<port>"
//usage:     "\n	UDP:<host>:<port> or UDP4:<host>:<port>"
//usage:     "\n	UDP-LISTEN:<port>"
//usage:     "\n	UNIX-CONNECT:<path> or UNIX:<path>"
//usage:     "\n	UNIX-LISTEN:<path>"
//usage:     "\n	EXEC:\"<command>\""
//usage:     "\n	SYSTEM:\"<command>\""
//usage:     "\n	FILE:<path>"

#include "libbb.h"
#include <sys/un.h>

#define BUF_SIZE 8192

enum {
	OPT_v = 1 << 0,
	OPT_d = 1 << 1,
	OPT_t = 1 << 2,
};

struct endpoint {
	int in_fd;
	int out_fd;
	pid_t child_pid;
	int is_socket;
	int is_udp_listen;
};

static char *strip_quotes(char *str)
{
	size_t len = strlen(str);
	if (len >= 2 && ((str[0] == '"' && str[len - 1] == '"') || (str[0] == '\'' && str[len - 1] == '\''))) {
		str[len - 1] = '\0';
		return str + 1;
	}
	return str;
}

static void dump_hex(FILE *fp, const char *prefix, const void *buf, size_t len)
{
	const unsigned char *p = (const unsigned char *)buf;
	size_t i, j;
	char ascii[17];

	fprintf(fp, "%s %lu bytes\n", prefix, (unsigned long)len);
	for (i = 0; i < len; i += 16) {
		fprintf(fp, "%08lx  ", (unsigned long)i);
		for (j = 0; j < 16; j++) {
			if (i + j < len) {
				unsigned char c = p[i + j];
				fprintf(fp, "%02x ", c);
				ascii[j] = (c >= 32 && c < 127) ? c : '.';
			} else {
				fputs("   ", fp);
				ascii[j] = ' ';
			}
			if (j == 7)
				fputc(' ', fp);
		}
		ascii[16] = '\0';
		fprintf(fp, " |%s|\n", ascii);
	}
	fflush(fp);
}

static void parse_and_open_endpoint(const char *addr_str, struct endpoint *ep, int debug)
{
	const char *p;
	char *colon;
	char *host;
	char *cmd;
	char *cleaned_cmd;
	const char *path;
	int port;
	int sfd;
	int cfd;
	int fd;
	int sv[2];
	pid_t pid;
	len_and_sockaddr *lsa;
	struct sockaddr_un sun;

	memset(ep, 0, sizeof(*ep));
	ep->in_fd = -1;
	ep->out_fd = -1;

	if (strcmp(addr_str, "-") == 0 ||
	    strcasecmp(addr_str, "STDIN") == 0 ||
	    strcasecmp(addr_str, "STDOUT") == 0) {
		ep->in_fd = STDIN_FILENO;
		ep->out_fd = STDOUT_FILENO;
		ep->is_socket = 0;
		return;
	}

	if (strncasecmp(addr_str, "TCP-LISTEN:", 11) == 0 ||
	    strncasecmp(addr_str, "TCP4-LISTEN:", 12) == 0) {
		p = (addr_str[10] == ':') ? addr_str + 11 : addr_str + 12;
		port = bb_lookup_port(p, "tcp", 0);

		if (debug)
			bb_error_msg("listening on TCP port %d...", port);
		sfd = create_and_bind_stream_or_die(NULL, port);
		xlisten(sfd, 1);
		cfd = accept(sfd, NULL, NULL);
		if (cfd < 0)
			bb_simple_perror_msg_and_die("accept");
		if (debug)
			bb_error_msg("accepted TCP connection on port %d", port);
		close(sfd);
		ep->in_fd = ep->out_fd = cfd;
		ep->is_socket = 1;
		return;
	}

	if (strncasecmp(addr_str, "TCP:", 4) == 0 ||
	    strncasecmp(addr_str, "TCP4:", 5) == 0) {
		p = (addr_str[3] == ':') ? addr_str + 4 : addr_str + 5;
		colon = strrchr(p, ':');

		if (!colon)
			bb_error_msg_and_die("invalid TCP address: %s", addr_str);
		host = xstrndup(p, colon - p);
		port = bb_lookup_port(colon + 1, "tcp", 0);
		if (debug)
			bb_error_msg("connecting to TCP %s:%d...", host, port);
		fd = create_and_connect_stream_or_die(host, port);
		free(host);
		ep->in_fd = ep->out_fd = fd;
		ep->is_socket = 1;
		return;
	}

	if (strncasecmp(addr_str, "UDP-LISTEN:", 11) == 0) {
		p = addr_str + 11;
		port = bb_lookup_port(p, "udp", 0);

		if (debug)
			bb_error_msg("listening on UDP port %d...", port);
		fd = create_and_bind_dgram_or_die(NULL, port);
		ep->in_fd = ep->out_fd = fd;
		ep->is_socket = 1;
		ep->is_udp_listen = 1;
		return;
	}

	if (strncasecmp(addr_str, "UDP:", 4) == 0 ||
	    strncasecmp(addr_str, "UDP4:", 5) == 0) {
		p = (addr_str[3] == ':') ? addr_str + 4 : addr_str + 5;
		colon = strrchr(p, ':');

		if (!colon)
			bb_error_msg_and_die("invalid UDP address: %s", addr_str);
		host = xstrndup(p, colon - p);
		port = bb_lookup_port(colon + 1, "udp", 0);
		lsa = xhost2sockaddr(host, port);
		free(host);
		if (debug)
			bb_error_msg("connecting UDP to %s:%d...", p, port);
		fd = xsocket(lsa->u.sa.sa_family, SOCK_DGRAM, 0);
		xconnect(fd, &lsa->u.sa, lsa->len);
		free(lsa);
		ep->in_fd = ep->out_fd = fd;
		ep->is_socket = 1;
		return;
	}

	if (strncasecmp(addr_str, "UNIX-LISTEN:", 12) == 0) {
		path = addr_str + 12;

		unlink(path);
		sfd = xsocket(AF_UNIX, SOCK_STREAM, 0);
		memset(&sun, 0, sizeof(sun));
		sun.sun_family = AF_UNIX;
		safe_strncpy(sun.sun_path, path, sizeof(sun.sun_path));
		xbind(sfd, (struct sockaddr *)&sun, sizeof(sun));
		xlisten(sfd, 1);
		if (debug)
			bb_error_msg("listening on UNIX socket %s...", path);
		cfd = accept(sfd, NULL, NULL);
		if (cfd < 0)
			bb_simple_perror_msg_and_die("accept");
		if (debug)
			bb_error_msg("accepted UNIX connection on %s", path);
		close(sfd);
		ep->in_fd = ep->out_fd = cfd;
		ep->is_socket = 1;
		return;
	}

	if (strncasecmp(addr_str, "UNIX-CONNECT:", 13) == 0 ||
	    strncasecmp(addr_str, "UNIX:", 5) == 0) {
		path = (addr_str[4] == ':') ? addr_str + 5 : addr_str + 13;

		fd = xsocket(AF_UNIX, SOCK_STREAM, 0);
		memset(&sun, 0, sizeof(sun));
		sun.sun_family = AF_UNIX;
		safe_strncpy(sun.sun_path, path, sizeof(sun.sun_path));
		if (debug)
			bb_error_msg("connecting to UNIX socket %s...", path);
		xconnect(fd, (struct sockaddr *)&sun, sizeof(sun));
		ep->in_fd = ep->out_fd = fd;
		ep->is_socket = 1;
		return;
	}

	if (strncasecmp(addr_str, "EXEC:", 5) == 0 ||
	    strncasecmp(addr_str, "SYSTEM:", 7) == 0) {
		p = (addr_str[4] == ':') ? addr_str + 5 : addr_str + 7;
		cmd = xstrdup(p);
		cleaned_cmd = strip_quotes(cmd);

		if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
			bb_simple_perror_msg_and_die("socketpair");

		pid = fork();
		if (pid < 0)
			bb_simple_perror_msg_and_die("fork");

		if (pid == 0) {
			close(sv[0]);
			dup2(sv[1], STDIN_FILENO);
			dup2(sv[1], STDOUT_FILENO);
			close(sv[1]);
			execl("/bin/sh", "sh", "-c", cleaned_cmd, (char *)NULL);
			_exit(127);
		}

		close(sv[1]);
		ep->in_fd = ep->out_fd = sv[0];
		ep->child_pid = pid;
		ep->is_socket = 1;
		if (debug)
			bb_error_msg("started child process %d: %s", (int)pid, cleaned_cmd);
		free(cmd);
		return;
	}

	if (strncasecmp(addr_str, "FILE:", 5) == 0) {
		path = addr_str + 5;
		fd = open(path, O_RDWR | O_CREAT, 0666);
		if (fd < 0)
			fd = open(path, O_RDONLY);
		if (fd < 0)
			fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
		if (fd < 0)
			bb_perror_msg_and_die("can't open '%s'", path);
		if (debug)
			bb_error_msg("opened file %s", path);
		ep->in_fd = ep->out_fd = fd;
		ep->is_socket = 0;
		return;
	}

	bb_error_msg_and_die("unknown or unsupported address format '%s'", addr_str);
}

int socat_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int socat_main(int argc UNUSED_PARAM, char **argv)
{
	unsigned opts;
	char *timeout_str = NULL;
	int timeout_sec = -1;
	struct endpoint ep1;
	struct endpoint ep2;
	int rfd1_open = 1;
	int rfd2_open = 1;
	int wfd1_open = 1;
	int wfd2_open = 1;
	int timeout_started = 0;
	struct timespec timeout_end;
	char *buf;
	struct pollfd pfds[2];
	int timeout_ms;
	int n;
	ssize_t nread;

	opts = getopt32(argv, "^" "vdt:" "\0" "=2", &timeout_str);
	argv += optind;

	if (opts & OPT_t)
		timeout_sec = xatoi_positive(timeout_str);

	signal(SIGPIPE, SIG_IGN);

	parse_and_open_endpoint(argv[0], &ep1, (opts & OPT_d));
	parse_and_open_endpoint(argv[1], &ep2, (opts & OPT_d));

	buf = xmalloc(BUF_SIZE);

	for (;;) {
		timeout_ms = -1;

		if (timeout_started) {
			struct timespec now;
			long diff_ms;
			clock_gettime(CLOCK_MONOTONIC, &now);
			diff_ms = (timeout_end.tv_sec - now.tv_sec) * 1000 +
			          (timeout_end.tv_nsec - now.tv_nsec) / 1000000;
			if (diff_ms <= 0)
				break;
			timeout_ms = (int)diff_ms;
		}

		pfds[0].fd = (rfd1_open && wfd2_open) ? ep1.in_fd : -1;
		pfds[0].events = POLLIN;
		pfds[1].fd = (rfd2_open && wfd1_open) ? ep2.in_fd : -1;
		pfds[1].events = POLLIN;

		if (pfds[0].fd < 0 && pfds[1].fd < 0)
			break;

		n = safe_poll(pfds, 2, timeout_ms);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (n == 0) {
			/* Timeout expired */
			break;
		}

		/* Check ep1 -> ep2 */
		if (pfds[0].revents & (POLLIN | POLLERR | POLLHUP)) {
			if (ep1.is_udp_listen) {
				struct sockaddr_storage peer;
				socklen_t plen = sizeof(peer);
				recvfrom(ep1.in_fd, buf, BUF_SIZE, MSG_PEEK, (struct sockaddr *)&peer, &plen);
				connect(ep1.in_fd, (struct sockaddr *)&peer, plen);
				ep1.is_udp_listen = 0;
			}
			nread = safe_read(ep1.in_fd, buf, BUF_SIZE);
			if (nread > 0) {
				if (opts & OPT_v)
					dump_hex(stderr, ">", buf, nread);
				if (full_write(ep2.out_fd, buf, nread) != nread) {
					wfd2_open = 0;
				}
			} else {
				/* EOF on ep1 */
				rfd1_open = 0;
				if (ep2.is_socket)
					shutdown(ep2.out_fd, SHUT_WR);
				if (ep1.child_pid > 0)
					wfd1_open = 0;
				if (!timeout_started && timeout_sec >= 0) {
					timeout_started = 1;
					clock_gettime(CLOCK_MONOTONIC, &timeout_end);
					timeout_end.tv_sec += timeout_sec;
				}
			}
		}

		/* Check ep2 -> ep1 */
		if (pfds[1].revents & (POLLIN | POLLERR | POLLHUP)) {
			if (ep2.is_udp_listen) {
				struct sockaddr_storage peer;
				socklen_t plen = sizeof(peer);
				recvfrom(ep2.in_fd, buf, BUF_SIZE, MSG_PEEK, (struct sockaddr *)&peer, &plen);
				connect(ep2.in_fd, (struct sockaddr *)&peer, plen);
				ep2.is_udp_listen = 0;
			}
			nread = safe_read(ep2.in_fd, buf, BUF_SIZE);
			if (nread > 0) {
				if (opts & OPT_v)
					dump_hex(stderr, "<", buf, nread);
				if (full_write(ep1.out_fd, buf, nread) != nread) {
					wfd1_open = 0;
				}
			} else {
				/* EOF on ep2 */
				rfd2_open = 0;
				if (ep1.is_socket)
					shutdown(ep1.out_fd, SHUT_WR);
				if (ep2.child_pid > 0)
					wfd2_open = 0;
				if (!timeout_started && timeout_sec >= 0) {
					timeout_started = 1;
					clock_gettime(CLOCK_MONOTONIC, &timeout_end);
					timeout_end.tv_sec += timeout_sec;
				}
			}
		}
	}

	free(buf);

	/* Clean up endpoints */
	if (ep1.child_pid > 0) {
		kill(ep1.child_pid, SIGTERM);
		waitpid(ep1.child_pid, NULL, WNOHANG);
	}
	if (ep2.child_pid > 0) {
		kill(ep2.child_pid, SIGTERM);
		waitpid(ep2.child_pid, NULL, WNOHANG);
	}

	if (ep1.in_fd >= 0 && ep1.in_fd != STDIN_FILENO)
		close(ep1.in_fd);
	if (ep1.out_fd >= 0 && ep1.out_fd != STDOUT_FILENO && ep1.out_fd != ep1.in_fd)
		close(ep1.out_fd);

	if (ep2.in_fd >= 0 && ep2.in_fd != STDIN_FILENO)
		close(ep2.in_fd);
	if (ep2.out_fd >= 0 && ep2.out_fd != STDOUT_FILENO && ep2.out_fd != ep2.in_fd)
		close(ep2.out_fd);

	return 0;
}
