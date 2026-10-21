/* vi: set sw=4 ts=4: */
/*
 * Enhanced curl implementation for busybox-ng
 *
 * Licensed under GPLv2 or later.
 */

//config:config CURL
//config:	bool "curl (HTTP/HTTPS transfer tool)"
//config:	default y
//config:	help
//config:	cURL-compatible command-line tool for transferring data with URLs.
//config:	Supports HTTP/HTTPS, redirects (-L), custom headers (-H),
//config:	HEAD/include headers (-I/-i), basic auth (-u), remote name (-O),
//config:	verbose output (-v), and error handling (-f).

//applet:IF_CURL(APPLET(curl, BB_DIR_USR_BIN, BB_SUID_DROP))

//kbuild:lib-$(CONFIG_CURL) += curl.o

//usage:#define curl_trivial_usage
//usage:       "[-sSvILiOkf] [-X METHOD] [-H HDR] [-d DATA] [-u USER:PASS] [-o FILE] URL"
//usage:#define curl_full_usage "\n\n"
//usage:       "Transfer data from or to a server\n"
//usage:     "\n	-X METHOD	HTTP method (GET, POST, PUT, DELETE, HEAD, etc.)"
//usage:     "\n	-H HDR		Custom HTTP header (can be used multiple times)"
//usage:     "\n	-d DATA		HTTP POST data"
//usage:     "\n	-o FILE		Write output to FILE instead of stdout"
//usage:     "\n	-O		Write output to a local file named like remote file"
//usage:     "\n	-s		Silent mode (no progress or error messages)"
//usage:     "\n	-S		Show error message even with -s"
//usage:     "\n	-v		Verbose (show request/response headers)"
//usage:     "\n	-L		Follow HTTP redirects (301, 302, 303, 307, 308)"
//usage:     "\n	-i		Include HTTP response headers in output"
//usage:     "\n	-I		Show HTTP headers only (HEAD request)"
//usage:     "\n	-k		Allow insecure SSL/TLS connections"
//usage:     "\n	-u USER:PASS	Server user and password for authentication"
//usage:     "\n	-f		Fail silently on server errors (HTTP 4xx/5xx)"

#include "libbb.h"

enum {
	OPT_X = 1 << 0,
	OPT_H = 1 << 1,
	OPT_d = 1 << 2,
	OPT_o = 1 << 3,
	OPT_u = 1 << 4,
	OPT_s = 1 << 5,
	OPT_S = 1 << 6,
	OPT_v = 1 << 7,
	OPT_L = 1 << 8,
	OPT_i = 1 << 9,
	OPT_I = 1 << 10,
	OPT_O = 1 << 11,
	OPT_k = 1 << 12,
	OPT_f = 1 << 13,
};

static char *base64_encode(const char *src)
{
	size_t len = strlen(src);
	char *dst = xmalloc(len * 4 / 3 + 4);
	bb_uuencode(dst, src, len, bb_uuenc_tbl_base64);
	return dst;
}

int curl_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int curl_main(int argc UNUSED_PARAM, char **argv)
{
	char *method_opt = NULL;
	char *data = NULL;
	char *outfile_opt = NULL;
	char *user_auth = NULL;
	llist_t *headers = NULL;
	llist_t *h;
	unsigned opts;
	char *cur_url;
	int redirect_count = 0;
	int out_fd = -1;
	int status_code = 0;

	opts = getopt32(argv, "^" "X:H:*d:o:u:sSvLiIOkf" "\0" "-1",
		&method_opt, &headers, &data, &outfile_opt, &user_auth);
	argv += optind;

	cur_url = xstrdup(argv[0]);

	while (1) {
		char *scheme;
		char *host;
		char *path;
		char *colon;
		int port;
		int is_https;
		int fd;
		FILE *fp;
		char *method;
		char *status_line;
		char *hdr_line;
		char *location = NULL;
		int chunked = 0;
		long content_len = -1;

		/* Parse URL */
		scheme = xstrdup(cur_url);
		host = strstr(scheme, "://");
		if (host) {
			*host = '\0';
			host += 3;
		} else {
			host = scheme;
			scheme = (char *)"http";
		}

		path = strchr(host, '/');
		if (path) {
			*path++ = '\0';
			path = xasprintf("/%s", path);
		} else {
			path = xstrdup("/");
		}

		is_https = (strcasecmp(scheme, "https") == 0);
		port = is_https ? 443 : 80;

		colon = strrchr(host, ':');
		if (colon && !strchr(colon, ']')) {
			*colon = '\0';
			port = atoi(colon + 1);
		}

		/* Determine HTTP method */
		if (opts & OPT_I)
			method = (char *)"HEAD";
		else if (method_opt)
			method = method_opt;
		else if (data)
			method = (char *)"POST";
		else
			method = (char *)"GET";

		/* Connect */
#if ENABLE_TLS
		if (is_https) {
			int sp[2];
			pid_t pid;
			char *host_port = xasprintf("%s:%u", host, port);
			if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0)
				bb_simple_perror_msg_and_die("socketpair");
			fflush_all();
			pid = xvfork();
			if (pid == 0) {
				char *eargv[6];
				int eargc = 0;
				close(sp[0]);
				xmove_fd(sp[1], 0);
				xdup2(0, 1);
				eargv[eargc++] = (char *)"ssl_client";
				eargv[eargc++] = (char *)"-n";
				eargv[eargc++] = host;
				eargv[eargc++] = host_port;
				eargv[eargc] = NULL;
				execvp(bb_busybox_exec_path, eargv);
				bb_simple_perror_msg_and_die("can't execute ssl_client");
			}
			close(sp[1]);
			fd = sp[0];
			free(host_port);
		} else
#endif
		{
			len_and_sockaddr *lsa = host2sockaddr(host, port);
			if (!lsa)
				bb_error_msg_and_die("can't resolve host '%s'", host);
			fd = xsocket(lsa->u.sa.sa_family, SOCK_STREAM, 0);
			xconnect(fd, &lsa->u.sa, lsa->len);
			free(lsa);
		}

		fp = fdopen(fd, "r+");
		if (!fp)
			bb_simple_perror_msg_and_die("fdopen");

		/* Send Request */
		if (opts & OPT_v)
			fprintf(stderr, "> %s %s HTTP/1.1\r\n> Host: %s\r\n", method, path, host);
		fprintf(fp, "%s %s HTTP/1.1\r\n", method, path);
		fprintf(fp, "Host: %s\r\n", host);
		fprintf(fp, "User-Agent: curl/busybox-ng\r\n");
		fprintf(fp, "Accept: */*\r\n");

		if (user_auth) {
			char *b64 = base64_encode(user_auth);
			if (opts & OPT_v)
				fprintf(stderr, "> Authorization: Basic %s\r\n", b64);
			fprintf(fp, "Authorization: Basic %s\r\n", b64);
			free(b64);
		}

		if (data) {
			if (opts & OPT_v) {
				fprintf(stderr, "> Content-Length: %u\r\n", (unsigned)strlen(data));
				fprintf(stderr, "> Content-Type: application/x-www-form-urlencoded\r\n");
			}
			fprintf(fp, "Content-Length: %u\r\n", (unsigned)strlen(data));
			fprintf(fp, "Content-Type: application/x-www-form-urlencoded\r\n");
		}

		for (h = headers; h; h = h->link) {
			if (opts & OPT_v)
				fprintf(stderr, "> %s\r\n", (char *)h->data);
			fprintf(fp, "%s\r\n", (char *)h->data);
		}

		if (opts & OPT_v)
			fprintf(stderr, "> Connection: close\r\n>\r\n");
		fprintf(fp, "Connection: close\r\n\r\n");

		if (data) {
			if (opts & OPT_v)
				fprintf(stderr, "%s", data);
			fputs(data, fp);
		}
		fflush(fp);

		/* Read Status line */
		status_line = xmalloc_fgetline(fp);
		if (!status_line) {
			if (!(opts & OPT_s) || (opts & OPT_S))
				bb_simple_error_msg("no response from server");
			exit(1);
		}

		if (opts & OPT_v)
			fprintf(stderr, "< %s\n", status_line);

		/* Parse status code (e.g. "HTTP/1.1 200 OK") */
		{
			char *sp = strchr(status_line, ' ');
			if (sp)
				status_code = atoi(sp + 1);
		}

		/* Read Response Headers */
		while ((hdr_line = xmalloc_fgetline(fp)) != NULL) {
			if (hdr_line[0] == '\0' || (hdr_line[0] == '\r' && hdr_line[1] == '\0')) {
				free(hdr_line);
				break;
			}
			if (opts & OPT_v)
				fprintf(stderr, "< %s\n", hdr_line);

			if (strncasecmp(hdr_line, "Location:", 9) == 0) {
				char *loc = hdr_line + 9;
				while (*loc == ' ' || *loc == '\t') loc++;
				location = xstrdup(loc);
				location[strcspn(location, "\r\n")] = '\0';
			} else if (strncasecmp(hdr_line, "Transfer-Encoding:", 18) == 0 &&
			           strstr(hdr_line + 18, "chunked")) {
				chunked = 1;
			} else if (strncasecmp(hdr_line, "Content-Length:", 15) == 0) {
				content_len = atol(hdr_line + 15);
			}

			if (opts & (OPT_i | OPT_I)) {
				/* print headers to output */
				puts(hdr_line);
			}
			free(hdr_line);
		}
		if (opts & (OPT_i | OPT_I))
			putchar('\n');

		/* Handle Redirection */
		if ((opts & OPT_L) && location &&
		    (status_code == 301 || status_code == 302 || status_code == 303 ||
		     status_code == 307 || status_code == 308)) {
			if (++redirect_count > 10) {
				bb_simple_error_msg_and_die("maximum redirects exceeded");
			}
			if (opts & OPT_v)
				fprintf(stderr, "* Follow redirect to: %s\n", location);

			/* Handle relative vs absolute Location */
			if (strstr(location, "://")) {
				free(cur_url);
				cur_url = location;
			} else if (location[0] == '/') {
				char *new_url = xasprintf("%s://%s:%d%s", scheme, host, port, location);
				free(location);
				free(cur_url);
				cur_url = new_url;
			} else {
				char *new_url = xasprintf("%s://%s:%d/%s", scheme, host, port, location);
				free(location);
				free(cur_url);
				cur_url = new_url;
			}
			fclose(fp);
			free(scheme);
			free(path);
			free(status_line);
			continue;
		}

		free(location);

		/* Check error flag -f */
		if ((opts & OPT_f) && status_code >= 400) {
			fclose(fp);
			free(scheme);
			free(path);
			free(status_line);
			return 22; /* curl error for HTTP fail */
		}

		/* If HEAD request (-I), we are done */
		if (opts & OPT_I) {
			fclose(fp);
			free(scheme);
			free(path);
			free(status_line);
			break;
		}

		/* Open output file if not already open */
		if (out_fd < 0) {
			if (opts & OPT_O) {
				char *fname = bb_basename(path);
				if (!fname || !*fname || strcmp(fname, "/") == 0)
					fname = (char *)"index.html";
				out_fd = xopen(fname, O_WRONLY | O_CREAT | O_TRUNC);
			} else if (outfile_opt && strcmp(outfile_opt, "-") != 0) {
				out_fd = xopen(outfile_opt, O_WRONLY | O_CREAT | O_TRUNC);
			} else {
				out_fd = STDOUT_FILENO;
			}
		}

		/* Read body */
		{
			char buf[8192];
			if (chunked) {
				while (1) {
					char *sz_line;
					size_t chunk_sz;
					char *crlf;

					sz_line = xmalloc_fgetline(fp);
					if (!sz_line) break;
					chunk_sz = strtoul(sz_line, NULL, 16);
					free(sz_line);
					if (chunk_sz == 0) break;
					while (chunk_sz > 0) {
						size_t to_read = chunk_sz < sizeof(buf) ? chunk_sz : sizeof(buf);
						size_t got = fread(buf, 1, to_read, fp);
						if (got == 0) goto body_done;
						if (full_write(out_fd, buf, got) != got)
							bb_simple_perror_msg_and_die("write error");
						chunk_sz -= got;
					}
					crlf = xmalloc_fgetline(fp);
					free(crlf);
				}
			} else {
				int n;
				long total_read = 0;
				while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
					if (full_write(out_fd, buf, n) != n) {
						bb_simple_perror_msg_and_die("write error");
					}
					total_read += n;
					if (content_len >= 0 && total_read >= content_len)
						break;
				}
			}
body_done:
			;
		}

		fclose(fp);
		free(scheme);
		free(path);
		free(status_line);
		break;
	}

	if (out_fd >= 0 && out_fd != STDOUT_FILENO)
		close(out_fd);

	free(cur_url);
	return EXIT_SUCCESS;
}
