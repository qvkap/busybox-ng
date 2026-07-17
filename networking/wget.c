/* vi: set sw=4 ts=4: */
/*
 * wget - minimal HTTP/HTTPS downloader with wget-compatible options
 *
 * Copyright (C) 2024 busybox-ng contributors
 *
 * Licensed under GPLv2 or later, see file LICENSE in this source tree.
 *
 * Implements a trimmed-down subset of wget features:
 *   - HTTP/1.1 GET with chunked transfer encoding
 *   - HTTPS via internal TLS (same as wget)
 *   - Parallel downloads (-j N) — the key wget feature
 *   - -O FILE, -q, -P DIR, --no-check-certificate
 *   - HTTP redirects (301/302/303/307/308)
 *   - Resume with -c (Content-Range)
 *   - User-Agent: Wget/busybox
 */
//config:config WGET
//config:	bool "wget (minimal built-in, ~12 kb)"
//config:	default y
//config:	help
//config:	Minimal wget-compatible downloader built into busybox.
//config:	Supports HTTP/1.1, HTTPS, redirects, chunked encoding,
//config:	and parallel downloads (-j N).
//config:
//config:config WGET_NO_CHECK_CERT
//config:	bool "Support --no-check-certificate option"
//config:	default y
//config:	depends on WGET && TLS

//applet:IF_WGET(APPLET(wget, BB_DIR_USR_BIN, BB_SUID_DROP))

//kbuild:lib-$(CONFIG_WGET) += wget.o

//usage:#define wget_trivial_usage
//usage:       "[-qc] [-O FILE] [-P DIR] [-j JOBS] [-U AGENT] [--no-check-certificate] URL..."
//usage:#define wget_full_usage "\n\n"
//usage:       "Minimal wget-compatible downloader\n"
//usage:     "\n	-q		Quiet mode"
//usage:     "\n	-c		Continue (resume) download"
//usage:     "\n	-O FILE		Save to FILE ('-' for stdout)"
//usage:     "\n	-P DIR		Save files to DIR"
//usage:     "\n	-j N		Download N URLs in parallel (default: 1)"
//usage:     "\n	-U STR		Set User-Agent"
//usage:     "\n	--no-check-certificate  Don't verify TLS certificate"

#include "libbb.h"
#include <sys/wait.h>
#define W2_USER_AGENT  "Wget/busybox"
#define W2_BUF_SIZE    (32 * 1024)
#define W2_MAX_REDIR   10

enum {
	OPT_QUIET      = (1 << 0),  /* q */
	OPT_CONTINUE   = (1 << 1),  /* c */
	OPT_OUTFILE    = (1 << 2),  /* O: */
	OPT_PREFIX     = (1 << 3),  /* P: */
	OPT_JOBS       = (1 << 4),  /* j: */
	OPT_AGENT      = (1 << 5),  /* U: */
#if ENABLE_TLS
	OPT_NO_CHECK_CERT = (1 << 6), /* --no-check-certificate */
#endif
};

struct w2_url {
	char *scheme;    /* "http" or "https" */
	char *host;
	int   port;
	char *path;
	char *alloc;     /* original string to free */
};

/* Parse URL into parts. Returns 0 on error. */
static int w2_parse_url(const char *url, struct w2_url *u)
{
	char *s, *p;

	u->alloc = xstrdup(url);
	s = u->alloc;

	/* scheme */
	p = strstr(s, "://");
	if (!p) {
		bb_simple_error_msg("bad URL (no scheme)");
		goto err;
	}
	*p = '\0';
	u->scheme = s;
	s = p + 3;

	/* host[:port] */
	u->port = 80;
	if (strcasecmp(u->scheme, "https") == 0)
		u->port = 443;
	else if (strcasecmp(u->scheme, "http") != 0) {
		bb_error_msg("unsupported scheme '%s'", u->scheme);
		goto err;
	}

	u->host = s;
	/* find path */
	p = strchr(s, '/');
	u->path = p ? p : (char*)"/";
	if (p) *p = '\0';

	/* port in host? */
	p = strchr(u->host, ':');
	if (p) {
		*p++ = '\0';
		u->port = xatoi_positive(p);
	}

	return 1;
 err:
	free(u->alloc);
	u->alloc = NULL;
	return 0;
}

/* Open TCP connection, optionally wrapped in TLS.
 * Returns a FILE* open for reading (the fd is also writable).
 */
static FILE *w2_connect(struct w2_url *u, int no_check_cert UNUSED_PARAM)
{
	len_and_sockaddr *lsa;
	int fd;
	int is_https = (strcasecmp(u->scheme, "https") == 0);
#if ENABLE_TLS
	if (is_https) {
		/* Let ssl_client connect and wrap in TLS */
		int sp[2];
		pid_t pid;
		char *servername = u->host;
		char *host_port = xasprintf("%s:%u", u->host, u->port);
		if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0)
			bb_simple_perror_msg_and_die("socketpair");
		fflush_all();
		pid = xfork();
		if (pid == 0) {
			/* child: runs TLS, connects to host_port, proxies to sp[1] */
			char *argv[5];
			close(sp[0]);
			xmove_fd(sp[1], 0);
			xdup2(0, 1);
			argv[0] = (char*)"ssl_client";
			argv[1] = (char*)"-n";
			argv[2] = servername;
			argv[3] = host_port;
			argv[4] = NULL;
			BB_EXECVP_or_die(argv);
		}
		close(sp[1]);
		fd = sp[0];
		free(host_port);
	} else
#endif
	{
		/* Connect */
		lsa = host2sockaddr(u->host, u->port);
		if (!lsa)
			bb_simple_error_msg_and_die("bad address");
		fd = xsocket(lsa->u.sa.sa_family, SOCK_STREAM, 0);
		xconnect(fd, &lsa->u.sa, lsa->len);
		free(lsa);
	}

	return fdopen(fd, "r+");
}

/* Read a line from socket, strip \r\n */
static char *w2_read_line(FILE *fp)
{
	char *line = xmalloc_fgetline(fp);
	if (line) {
		int len = strlen(line);
		while (len > 0 && (line[len-1] == '\r' || line[len-1] == '\n'))
			line[--len] = '\0';
	}
	return line;
}

/*
 * Download a single URL.
 * Returns exit code: 0 = OK, 1 = error.
 */
static int w2_download(const char *url, const char *outfile,
                       const char *prefix, int opts, const char *agent)
{
	struct w2_url u;
	FILE *fp = NULL;
	char *line;
	int out_fd = -1;
	int redir_count = 0;
	int status;
	int chunked = 0;
	int ret = 1;
	off_t content_len = -1;
	off_t resume_from = 0;
	char *current_url = xstrdup(url);
	char *allocated_outfile = NULL;
#if ENABLE_TLS
	int no_check = !!(opts & OPT_NO_CHECK_CERT);
#else
	int no_check = 0;
#endif
	int quiet = !!(opts & OPT_QUIET);
	int do_continue = !!(opts & OPT_CONTINUE);

 redirect:
	if (redir_count > W2_MAX_REDIR) {
		bb_simple_error_msg("too many redirects");
		goto out;
	}
	if (!w2_parse_url(current_url, &u))
		goto out;

	fp = w2_connect(&u, no_check);
	if (!fp)
		goto out_free;

	/* Determine output filename */
	if (!outfile && !allocated_outfile) {
		const char *fname = bb_basename(u.path);
		if (!fname || !*fname || strcmp(fname, "/") == 0)
			fname = "index.html";
		if (prefix)
			allocated_outfile = xasprintf("%s/%s", prefix, fname);
		else
			allocated_outfile = xstrdup(fname);
	}
	const char *actual_outfile = allocated_outfile ? allocated_outfile : outfile;

	/* Resume? */
	resume_from = 0;
	if (do_continue && strcmp(actual_outfile, "-") != 0) {
		struct stat st;
		if (stat(actual_outfile, &st) == 0)
			resume_from = st.st_size;
	}

	/* Send request */
	fprintf(fp, "GET %s HTTP/1.1\r\n"
	            "Host: %s\r\n"
	            "User-Agent: %s\r\n"
	            "Connection: close\r\n"
	            "Accept: */*\r\n",
	        u.path, u.host,
	        agent ? agent : W2_USER_AGENT);
	if (resume_from > 0)
		fprintf(fp, "Range: bytes=%"OFF_FMT"d-\r\n", resume_from);
	fprintf(fp, "\r\n");
	fflush(fp);

	/* Read status line */
	line = w2_read_line(fp);
	if (!line) {
		bb_simple_error_msg("no response from server");
		goto out_close;
	}

	/* "HTTP/1.x NNN ..." */
	{
		char *p = strchr(line, ' ');
		if (!p) { free(line); goto out_close; }
		status = atoi(p + 1);
		if (!quiet)
			fprintf(stderr, "HTTP %d %s\n", status, p + 5);
		free(line);
	}

	/* Read headers */
	content_len = -1;
	chunked = 0;
	while ((line = w2_read_line(fp)) != NULL) {
		if (!*line) { free(line); break; }

		if (strncasecmp(line, "Content-Length:", 15) == 0)
			content_len = strtoll(line + 15, NULL, 10);
		else if (strncasecmp(line, "Transfer-Encoding:", 18) == 0 &&
		         strstr(line + 18, "chunked"))
			chunked = 1;
		else if ((status == 301 || status == 302 ||
		          status == 303 || status == 307 ||
		          status == 308) &&
		         strncasecmp(line, "Location:", 9) == 0) {
			char *loc = line + 9;
			while (*loc == ' ') loc++;
			free(current_url);
			current_url = xstrdup(loc);
		}
		free(line);
	}

	/* Handle redirects */
	if (status == 301 || status == 302 || status == 303 ||
	    status == 307 || status == 308) {
		fclose(fp); fp = NULL;
		free(u.alloc); u.alloc = NULL;
		free(allocated_outfile); allocated_outfile = NULL;
		redir_count++;
		goto redirect;
	}

	if (status != 200 && status != 206) {
		bb_error_msg("server returned HTTP %d", status);
		goto out_close;
	}

	/* Open output */
	if (strcmp(actual_outfile, "-") == 0) {
		out_fd = STDOUT_FILENO;
	} else {
		int flags = O_WRONLY | O_CREAT;
		flags |= (resume_from > 0) ? O_APPEND : O_TRUNC;
		out_fd = xopen(actual_outfile, flags);
		if (!quiet)
			fprintf(stderr, "Saving to: '%s'\n", actual_outfile);
	}

	/* Read body */
	{
		char *buf = xmalloc(W2_BUF_SIZE);
		off_t total = 0;

		if (chunked) {
			/* Chunked transfer encoding */
			while (1) {
				char *sz_line = w2_read_line(fp);
				size_t chunk_sz;
				if (!sz_line) break;
				chunk_sz = strtoul(sz_line, NULL, 16);
				free(sz_line);
				if (chunk_sz == 0) break;
				while (chunk_sz > 0) {
					size_t to_read = chunk_sz < (size_t)W2_BUF_SIZE
					                 ? chunk_sz : (size_t)W2_BUF_SIZE;
					size_t got = fread(buf, 1, to_read, fp);
					if (got == 0) goto body_done;
					xwrite(out_fd, buf, got);
					total += got;
					chunk_sz -= got;
				}
				/* trailing CRLF after chunk */
				w2_read_line(fp);
			}
		} else {
			/* Plain read */
			size_t got;
			while ((got = fread(buf, 1, W2_BUF_SIZE, fp)) > 0) {
				xwrite(out_fd, buf, got);
				total += got;
			}
		}
 body_done:
		if (!quiet && out_fd != STDOUT_FILENO) {
			if (content_len > 0)
				fprintf(stderr, "Downloaded %"OFF_FMT"d/%"OFF_FMT"d bytes\n",
				        resume_from + total, resume_from + content_len);
			else
				fprintf(stderr, "Downloaded %"OFF_FMT"d bytes\n",
				        resume_from + total);
		}
		free(buf);
	}

	ret = 0;

 out_close:
	if (fp) fclose(fp);
	if (out_fd >= 0 && out_fd != STDOUT_FILENO) close(out_fd);
 out_free:
	free(u.alloc);
 out:
	free(current_url);
	free(allocated_outfile);
	return ret;
}

int wget_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int wget_main(int argc UNUSED_PARAM, char **argv)
{
	char *outfile = NULL;
	char *prefix = NULL;
	char *agent = NULL;
	char *jobs_str = NULL;
	unsigned opts;
	int jobs = 1;
	int ret = 0;

#if ENABLE_TLS
# define OPTSTR_NOCHK "no-check-certificate"
# define LONGOPTS     , OPTSTR_NOCHK
#else
# define LONGOPTS
#endif

	opts = getopt32long(argv,
	        "qcO:P:j:U:",
	        "quiet\0"          No_argument       "q"
	        "continue\0"       No_argument       "c"
	        "output-file\0"    Required_argument "O"
	        "directory-prefix\0" Required_argument "P"
	        "user-agent\0"     Required_argument "U"
	        "no-check-certificate\0" No_argument "\xff"
	        ,
	        &outfile, &prefix, &jobs_str, &agent);
	argv += optind;

	if (!*argv)
		bb_show_usage();

	if (jobs_str)
		jobs = xatoi_positive(jobs_str);
	if (jobs < 1) jobs = 1;
#if ENABLE_TLS
	if (opts & OPT_NO_CHECK_CERT)
		; /* stored in opts bitmask */
#endif

	/*
	 * Parallel downloads: fork one child per URL (up to `jobs` at a time).
	 * This is the flagship feature that distinguishes wget from wget.
	 */
	if (jobs == 1 || !argv[1]) {
		/* Sequential */
		while (*argv) {
			ret |= w2_download(*argv, outfile, prefix, opts, agent);
			argv++;
			outfile = NULL; /* -O only applies to first URL */
		}
	} else {
		/* Parallel: up to `jobs` children at once */
		int running = 0;
		while (*argv || running > 0) {
			/* Spawn up to `jobs` children */
			while (*argv && running < jobs) {
				pid_t pid = vfork();
				if (pid == 0) {
					/* child */
					_exit(w2_download(*argv, outfile,
					                   prefix, opts, agent));
				}
				/* parent */
				if (pid < 0)
					bb_simple_perror_msg_and_die("vfork");
				argv++;
				outfile = NULL;
				running++;
			}
			/* Wait for one child to finish */
			if (running > 0) {
				int wstatus;
				if (safe_waitpid(-1, &wstatus, 0) > 0) {
					running--;
					if (!WIFEXITED(wstatus) ||
					    WEXITSTATUS(wstatus) != 0)
						ret = 1;
				}
			}
		}
	}

	return ret;
}
