/*
 * Minimal curl implementation for busybox-ng
 *
 */

//config:config CURL
//config:	bool "curl (minimal curl implementation)"
//config:	default y
//config:	help
//config:	Minimal curl implementation.

//applet:IF_CURL(APPLET(curl, BB_DIR_USR_BIN, BB_SUID_DROP))

//kbuild:lib-$(CONFIG_CURL) += curl.o

//usage:#define curl_trivial_usage
//usage:       "[OPTIONS] URL"
//usage:#define curl_full_usage "\n\n"
//usage:       "Fetch content from URL\n"
//usage:     "\n	-X METH	HTTP method"
//usage:     "\n	-H HDR	HTTP header"
//usage:     "\n	-d DATA	HTTP data"
//usage:     "\n	-o FILE	Output file"
//usage:     "\n	-s	Silent"

#include "libbb.h"

int curl_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int curl_main(int argc UNUSED_PARAM, char **argv)
{
	char *method = (char *)"GET";
	char *data = NULL;
	char *outfile = (char *)"-";
	llist_t *headers = NULL;
	unsigned opt;
	char *url;
	int fd;
	FILE *fp;
	char *line;
	int is_https;
	len_and_sockaddr *lsa;
	char *host;
	int port;
	char *path;
	char *scheme;

	opt = getopt32(argv, "X:H:*d:o:s", &method, &headers, &data, &outfile);
	argv += optind;

	if (!argv[0])
		bb_show_usage();

	url = argv[0];

	/* Parse URL */
	scheme = xstrdup(url);
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

	char *colon = strrchr(host, ':');
	if (colon) {
		*colon = '\0';
		port = atoi(colon + 1);
	}

#if ENABLE_TLS
	if (is_https) {
		int sp[2];
		pid_t pid;
		char *host_port = xasprintf("%s:%u", host, port);
		if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0)
			bb_simple_perror_msg_and_die("socketpair");
		fflush_all();
		pid = xfork();
		if (pid == 0) {
			char *eargv[5];
			close(sp[0]);
			xmove_fd(sp[1], 0);
			xdup2(0, 1);
			eargv[0] = (char*)"ssl_client";
			eargv[1] = (char*)"-n";
			eargv[2] = host;
			eargv[3] = host_port;
			eargv[4] = NULL;
			execvp(bb_busybox_exec_path, eargv);
			bb_simple_perror_msg_and_die("can't execute busybox ssl_client");
		}
		close(sp[1]);
		fd = sp[0];
		free(host_port);
	} else
#endif
	{
		lsa = host2sockaddr(host, port);
		if (!lsa)
			bb_simple_error_msg_and_die("bad address");
		fd = xsocket(lsa->u.sa.sa_family, SOCK_STREAM, 0);
		xconnect(fd, &lsa->u.sa, lsa->len);
		free(lsa);
	}

	fp = fdopen(fd, "r+");
	if (!fp)
		bb_simple_perror_msg_and_die("fdopen");

	if (data && strcmp(method, "GET") == 0)
		method = (char *)"POST";

	fprintf(fp, "%s %s HTTP/1.1\r\n", method, path);
	fprintf(fp, "Host: %s\r\n", host);
	fprintf(fp, "User-Agent: curl/busybox-ng\r\n");
	fprintf(fp, "Accept: */*\r\n");
	
	if (data)
		fprintf(fp, "Content-Length: %u\r\n", (unsigned)strlen(data));

	while (headers) {
		fprintf(fp, "%s\r\n", (char *)headers->data);
		headers = headers->link;
	}
	fprintf(fp, "Connection: close\r\n\r\n");
	
	if (data)
		fprintf(fp, "%s", data);

	fflush(fp);

	/* Read status line */
	line = xmalloc_fgetline(fp);
	if (!line) {
		bb_simple_error_msg_and_die("no response from server");
	}
	free(line);
	
	/* Skip headers */
	while ((line = xmalloc_fgetline(fp)) != NULL) {
		if (line[0] == '\0' || (line[0] == '\r' && line[1] == '\0')) {
			free(line);
			break;
		}
		free(line);
	}

	int out_fd = STDOUT_FILENO;
	if (strcmp(outfile, "-") != 0) {
		out_fd = xopen(outfile, O_WRONLY | O_CREAT | O_TRUNC);
	}

	/* Read body */
	{
		char buf[4096];
		int n;
		while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
			if (full_write(out_fd, buf, n) != n) {
				bb_simple_perror_msg_and_die("write error");
			}
		}
	}

	fclose(fp);
	if (out_fd != STDOUT_FILENO)
		close(out_fd);

	return EXIT_SUCCESS;
}
