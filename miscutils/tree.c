/* vi: set sw=4 ts=4: */
/*
 * Copyright (C) 2022 Roger Knecht <rknecht@pm.me>
 *
 * Licensed under GPLv2, see file LICENSE in this source tree.
 */
//config:config TREE
//config:	bool "tree (2.5 kb)"
//config:	default y
//config:	help
//config:	List files and directories in a tree structure.

//applet:IF_TREE(APPLET(tree, BB_DIR_USR_BIN, BB_SUID_DROP))

//kbuild:lib-$(CONFIG_TREE) += tree.o

//usage:#define tree_trivial_usage
//usage:       "[-adfhispugJ] [-L level] [DIR...]"
//usage:#define tree_full_usage "\n\n"
//usage:       "List contents of directories in a tree-like format\n"
//usage:     "\n	-a	All files (do not hide dotfiles)"
//usage:     "\n	-d	List directories only"
//usage:     "\n	-L level Max display depth of directory tree"
//usage:     "\n	-h	Print size in human readable format"
//usage:     "\n	-s	Print size in bytes"
//usage:     "\n	-p	Print file type and permissions"
//usage:     "\n	-u	Print file username or UID"
//usage:     "\n	-g	Print file groupname or GID"
//usage:     "\n	-f	Print full path prefix for each file"
//usage:     "\n	-i	Do not print indentation lines"
//usage:     "\n	-J	Output in JSON format"

#include "libbb.h"
#include "unicode.h"

enum {
	OPT_a = (1 << 0),
	OPT_d = (1 << 1),
	OPT_L = (1 << 2),
	OPT_h = (1 << 3),
	OPT_s = (1 << 4),
	OPT_p = (1 << 5),
	OPT_u = (1 << 6),
	OPT_g = (1 << 7),
	OPT_f = (1 << 8),
	OPT_i = (1 << 9),
	OPT_J = (1 << 10),
};

struct globals {
	unsigned count[2]; /* 0: dirs, 1: files */
	unsigned opts;
	int max_level;
	const char *bar;
	const char *mid;
	const char *end;
};
#define G (*ptr_to_globals)
#define INIT_G() do { \
	SET_PTR_TO_GLOBALS(xzalloc(sizeof(G))); \
	G.max_level = INT_MAX; \
	G.bar = "|   "; \
	G.mid = "|-- "; \
	G.end = "`-- "; \
} while (0)

static void json_print_str(const char *s)
{
	fputc('"', stdout);
	while (*s) {
		unsigned char c = (unsigned char)*s++;
		if (c == '"' || c == '\\') {
			fputc('\\', stdout);
			fputc(c, stdout);
		} else if (c == '\b') {
			fputs("\\b", stdout);
		} else if (c == '\f') {
			fputs("\\f", stdout);
		} else if (c == '\n') {
			fputs("\\n", stdout);
		} else if (c == '\r') {
			fputs("\\r", stdout);
		} else if (c == '\t') {
			fputs("\\t", stdout);
		} else if (c < 32) {
			printf("\\u%04x", c);
		} else {
			fputc(c, stdout);
		}
	}
	fputc('"', stdout);
}

static void json_print_meta(const struct stat *st)
{
	char mode_buf[12];

	if (G.opts & OPT_p) {
		bb_mode_string(mode_buf, st->st_mode);
		printf(",\"mode\":\"%04o\",\"prot\":", (unsigned)(st->st_mode & 07777));
		json_print_str(mode_buf);
	}
	if (G.opts & OPT_u) {
		printf(",\"user\":");
		json_print_str(get_cached_username(st->st_uid));
	}
	if (G.opts & OPT_g) {
		printf(",\"group\":");
		json_print_str(get_cached_groupname(st->st_gid));
	}
	if (G.opts & OPT_h) {
		printf(",\"size\":");
		json_print_str(make_human_readable_str(st->st_size, 1, 0));
	} else if (G.opts & OPT_s) {
		printf(",\"size\":%llu", (unsigned long long)st->st_size);
	}
}

static char *format_meta(char *buf, const struct stat *st)
{
	int n;
	char mode_buf[12];

	n = 0;
	if (!(G.opts & (OPT_p | OPT_u | OPT_g | OPT_s | OPT_h))) {
		buf[0] = '\0';
		return buf;
	}

	buf[n++] = '[';
	if (G.opts & OPT_p) {
		bb_mode_string(mode_buf, st->st_mode);
		n += sprintf(buf + n, "%s", mode_buf);
	}
	if (G.opts & OPT_u) {
		const char *u = get_cached_username(st->st_uid);
		n += sprintf(buf + n, "%s%-8.32s", (n > 1 ? " " : ""), u);
	}
	if (G.opts & OPT_g) {
		const char *g = get_cached_groupname(st->st_gid);
		n += sprintf(buf + n, "%s%-8.32s", (n > 1 ? " " : ""), g);
	}
	if (G.opts & OPT_h) {
		const char *h = make_human_readable_str(st->st_size, 1, 0);
		n += sprintf(buf + n, "%s%4s", (n > 1 ? " " : ""), h);
	} else if (G.opts & OPT_s) {
		n += sprintf(buf + n, "%s%11llu", (n > 1 ? " " : ""), (unsigned long long)st->st_size);
	}
	strcpy(buf + n, "]  ");
	return buf;
}

static void tree_traverse(const char *dir_path, const char *display_name, const char *prefix, int level)
{
	struct dirent **entries;
	struct stat st;
	int stat_ok;
	int size;
	int num_visible;
	int i;

	stat_ok = (lstat(dir_path, &st) == 0);

	if (level == 0) {
		if (G.opts & OPT_J) {
			printf("{\"type\":\"directory\",\"name\":");
			json_print_str(display_name);
			if (stat_ok)
				json_print_meta(&st);
		} else {
			if (stat_ok && (G.opts & (OPT_p | OPT_u | OPT_g | OPT_s | OPT_h))) {
				char meta[256];
				fputs_stdout(format_meta(meta, &st));
			}
			puts(display_name);
		}
	}

	size = scandir(dir_path, &entries, NULL, alphasort);
	if (size < 0) {
		if (G.opts & OPT_J) {
			if (level > 0) {
				printf("{\"type\":\"directory\",\"name\":");
				json_print_str(display_name);
				if (stat_ok)
					json_print_meta(&st);
			}
			printf(",\"error\":\"error opening dir\"}");
		} else {
			if (level == 0) {
				/* For root dir error format matching testsuite: dir [error opening dir] */
				/* (puts(display_name) was already called, so output [error opening dir] without newline at front) */
				/* Wait, test expects: "dir [error opening dir]\n" */
			} else {
				puts(" [error opening dir]");
			}
		}
		return;
	}

	if (G.opts & OPT_J)
		printf(",\"contents\":[");

	num_visible = 0;
	for (i = 0; i < size; i++) {
		struct dirent *de = entries[i];
		if (de->d_name[0] == '.' && !(G.opts & OPT_a)) {
			free(de);
			continue;
		}
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
			free(de);
			continue;
		}
		if (G.opts & OPT_d) {
			char *cp = concat_path_file(dir_path, de->d_name);
			struct stat cst;
			if (lstat(cp, &cst) != 0 || !S_ISDIR(cst.st_mode)) {
				free(cp);
				free(de);
				continue;
			}
			free(cp);
		}
		entries[num_visible++] = de;
	}

	for (i = 0; i < num_visible; i++) {
		struct dirent *de = entries[i];
		int is_last = (i == num_visible - 1);
		char *child_path = concat_path_file(dir_path, de->d_name);
		char *print_name = (G.opts & OPT_f) ? concat_path_file(display_name, de->d_name) : de->d_name;
		struct stat cst;
		int cstat_ok = (lstat(child_path, &cst) == 0);

		if (G.opts & OPT_J) {
			if (i > 0)
				fputc(',', stdout);
			if (cstat_ok && S_ISDIR(cst.st_mode)) {
				G.count[0]++;
				if (level + 1 < G.max_level) {
					printf("{\"type\":\"directory\",\"name\":");
					json_print_str(print_name);
					json_print_meta(&cst);
					tree_traverse(child_path, print_name, NULL, level + 1);
				} else {
					printf("{\"type\":\"directory\",\"name\":");
					json_print_str(print_name);
					json_print_meta(&cst);
					printf("}");
				}
			} else if (cstat_ok && S_ISLNK(cst.st_mode)) {
				char *target = xmalloc_readlink(child_path);
				G.count[1]++;
				printf("{\"type\":\"link\",\"name\":");
				json_print_str(print_name);
				printf(",\"target\":");
				json_print_str(target ? target : "");
				json_print_meta(&cst);
				printf("}");
				free(target);
			} else {
				const char *ftype = "file";
				if (cstat_ok) {
					if (S_ISCHR(cst.st_mode))
						ftype = "char";
					else if (S_ISBLK(cst.st_mode))
						ftype = "block";
					else if (S_ISFIFO(cst.st_mode))
						ftype = "fifo";
					else if (S_ISSOCK(cst.st_mode))
						ftype = "socket";
				}
				G.count[1]++;
				printf("{\"type\":\"%s\",\"name\":", ftype);
				json_print_str(print_name);
				if (cstat_ok)
					json_print_meta(&cst);
				printf("}");
			}
		} else {
			char meta[256];
			meta[0] = '\0';
			if (cstat_ok)
				format_meta(meta, &cst);
			if (!(G.opts & OPT_i))
				printf("%s%s", prefix, is_last ? G.end : G.mid);
			fputs_stdout(meta);

			if (cstat_ok && S_ISLNK(cst.st_mode)) {
				char *target = xmalloc_readlink(child_path);
				printf("%s -> %s\n", print_name, target ? target : "");
				free(target);
				G.count[1]++;
			} else if (cstat_ok && S_ISDIR(cst.st_mode)) {
				puts(print_name);
				G.count[0]++;
				if (level + 1 < G.max_level) {
					char *new_prefix = xasprintf("%s%s", prefix, is_last ? "    " : G.bar);
					tree_traverse(child_path, print_name, new_prefix, level + 1);
					free(new_prefix);
				}
			} else {
				puts(print_name);
				G.count[1]++;
			}
		}

		if (G.opts & OPT_f)
			free(print_name);
		free(child_path);
		free(de);
	}

	if (G.opts & OPT_J)
		printf("]}");

	free(entries);
}

int tree_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int tree_main(int argc UNUSED_PARAM, char **argv)
{
	char *opt_L = NULL;
	char **dirs;
	int dir_idx;
	const char *default_dirs[2];

	INIT_G();
	init_unicode();

#if ENABLE_UNICODE_SUPPORT
	if (unicode_status == UNICODE_ON) {
		G.bar = "│   ";
		G.mid = "├── ";
		G.end = "└── ";
	}
#endif

	G.opts = getopt32(argv, "^" "adL:hspugfiJ" "\0", &opt_L);
	if (G.opts & OPT_L)
		G.max_level = xatou_range(opt_L, 1, INT_MAX);

	argv += optind;

	default_dirs[0] = ".";
	default_dirs[1] = NULL;
	dirs = *argv ? argv : (char **)default_dirs;

	if (G.opts & OPT_J)
		fputc('[', stdout);

	for (dir_idx = 0; dirs[dir_idx]; dir_idx++) {
		if (G.opts & OPT_J && dir_idx > 0)
			fputc(',', stdout);
		/* Check if root dir is accessible first for proper error handling */
		{
			DIR *d = opendir(dirs[dir_idx]);
			if (!d) {
				if (G.opts & OPT_J) {
					printf("{\"type\":\"directory\",\"name\":");
					json_print_str(dirs[dir_idx]);
					printf(",\"error\":\"error opening dir\"}");
				} else {
					fputs_stdout(dirs[dir_idx]);
					puts(" [error opening dir]");
				}
				continue;
			}
			closedir(d);
		}
		tree_traverse(dirs[dir_idx], dirs[dir_idx], "", 0);
	}

	if (G.opts & OPT_J) {
		printf(",{\"type\":\"report\",\"directories\":%u,\"files\":%u}]\n",
		       G.count[0], G.count[1]);
	} else {
		printf("\n%u directories, %u files\n", G.count[0], G.count[1]);
	}

	return EXIT_SUCCESS;
}
