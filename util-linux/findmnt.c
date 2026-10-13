/* vi: set sw=4 ts=4: */
/*
 * findmnt - find a (mounted) filesystem
 *
 * Licensed under GPLv2 or later, see file LICENSE in this source tree.
 */
//config:config FINDMNT
//config:	bool "findmnt (8 kb)"
//config:	default y
//config:	help
//config:	Findmnt will list all mounted filesystems or search for
//config:	a filesystem.

//applet:IF_FINDMNT(APPLET(findmnt, BB_DIR_USR_BIN, BB_SUID_DROP))

//kbuild:lib-$(CONFIG_FINDMNT) += findmnt.o

//usage:#define findmnt_trivial_usage
//usage:       "[OPTIONS] [DEVICE|MOUNTPOINT]"
//usage:#define findmnt_full_usage "\n\n"
//usage:       "Find a (mounted) filesystem\n"
//usage:     "\n	-s, --fstab          Search in /etc/fstab"
//usage:     "\n	-m, --mtab           Search in /proc/mounts"
//usage:     "\n	-k, --kernel         Search in /proc/self/mountinfo (default)"
//usage:     "\n	-c, --canonicalize   Canonicalize printed paths"
//usage:     "\n	-C, --nocanonicalize Don't canonicalize when comparing paths"
//usage:     "\n	-D, --df             Imitate df(1) output"
//usage:     "\n	-d, --direction DIR  Direction of search: forward/backward"
//usage:     "\n	-e, --evaluate       Convert tags (LABEL/UUID) to device names"
//usage:     "\n	-F, --tab-file PATH  Alternative file for -s, -m or -k"
//usage:     "\n	-f, --first-only     Print first matching filesystem only"
//usage:     "\n	-i, --invert         Invert sense of matching"
//usage:     "\n	-J, --json           Use JSON output format"
//usage:     "\n	-l, --list           Use list output format"
//usage:     "\n	-n, --noheadings     Don't print column headings"
//usage:     "\n	-u, --notruncate     Don't truncate text in columns"
//usage:     "\n	-O, --options LIST   Limit filesystems by mount options"
//usage:     "\n	-o, --output LIST    Output columns"
//usage:     "\n	-P, --pairs          Use key=\"value\" output format"
//usage:     "\n	-p, --poll[=LIST]    Monitor filesystem events"
//usage:     "\n	-r, --raw            Use raw output format"
//usage:     "\n	-a, --ascii          Use ASCII characters for tree formatting"
//usage:     "\n	-t, --types LIST     Limit filesystems by FS type"
//usage:     "\n	-R, --submounts      Print submounts for selected filesystems"
//usage:     "\n	-S, --source SRC     Explicitly define mount source"
//usage:     "\n	-T, --target PATH    Explicitly define target path"
//usage:     "\n	-M, --mountpoint DIR Explicitly define mountpoint directory"
//usage:     "\n	-w, --timeout MSEC   Upper limit on time for --poll"
//usage:     "\n	-x, --verify         Verify mount table content"
//usage:     "\n	-b, --bytes          Print sizes in bytes"
//usage:     "\n	-U, --uniq           Ignore duplicate targets"
//usage:     "\n	-N, --task PID       Use /proc/PID/mountinfo"
//usage:     "\n	-v, --nofsroot       Don't print [/dir] for bind mounts"
//usage:     "\n	-A, --all            Disable built-in filters"
//usage:     "\n	-V, --version        Display version"
//usage:     "\n	-h, --help           Display help"
//usage:     "\n	--verbose            Print more details"
//usage:     "\n	--shadowed           Print filesystems over-shadowed by another"

#include "libbb.h"
#include <sys/statvfs.h>
#include <sys/poll.h>
#include <getopt.h>

enum {
	COL_SOURCE = 0,
	COL_TARGET,
	COL_FSTYPE,
	COL_OPTIONS,
	COL_VFS_OPTIONS,
	COL_FS_OPTIONS,
	COL_FREQ,
	COL_PASSNO,
	COL_LABEL,
	COL_UUID,
	COL_PARTLABEL,
	COL_PARTUUID,
	COL_ID,
	COL_PARENT,
	COL_MAJOR,
	COL_MINOR,
	COL_ROOT,
	COL_SIZE,
	COL_AVAIL,
	COL_USED,
	COL_USE_PCT,
	COL_PROPAGATION,
	COL_MAJ_MIN,
	COL_OLD_TARGET,
	COL_OLD_OPTIONS,
	NUM_COLS
};

static const char *const col_names[NUM_COLS] = {
	[COL_SOURCE]      = "SOURCE",
	[COL_TARGET]      = "TARGET",
	[COL_FSTYPE]      = "FSTYPE",
	[COL_OPTIONS]     = "OPTIONS",
	[COL_VFS_OPTIONS] = "VFS-OPTIONS",
	[COL_FS_OPTIONS]  = "FS-OPTIONS",
	[COL_FREQ]        = "FREQ",
	[COL_PASSNO]      = "PASSNO",
	[COL_LABEL]       = "LABEL",
	[COL_UUID]        = "UUID",
	[COL_PARTLABEL]   = "PARTLABEL",
	[COL_PARTUUID]    = "PARTUUID",
	[COL_ID]          = "ID",
	[COL_PARENT]      = "PARENT",
	[COL_MAJOR]       = "MAJOR",
	[COL_MINOR]       = "MINOR",
	[COL_ROOT]        = "ROOT",
	[COL_SIZE]        = "SIZE",
	[COL_AVAIL]       = "AVAIL",
	[COL_USED]        = "USED",
	[COL_USE_PCT]     = "USE%",
	[COL_PROPAGATION] = "PROPAGATION",
	[COL_MAJ_MIN]     = "MAJ:MIN",
	[COL_OLD_TARGET]  = "OLD-TARGET",
	[COL_OLD_OPTIONS] = "OLD-OPTIONS",
};

static const char *const col_json_names[NUM_COLS] = {
	[COL_SOURCE]      = "source",
	[COL_TARGET]      = "target",
	[COL_FSTYPE]      = "fstype",
	[COL_OPTIONS]     = "options",
	[COL_VFS_OPTIONS] = "vfs-options",
	[COL_FS_OPTIONS]  = "fs-options",
	[COL_FREQ]        = "freq",
	[COL_PASSNO]      = "passno",
	[COL_LABEL]       = "label",
	[COL_UUID]        = "uuid",
	[COL_PARTLABEL]   = "partlabel",
	[COL_PARTUUID]    = "partuuid",
	[COL_ID]          = "id",
	[COL_PARENT]      = "parent",
	[COL_MAJOR]       = "major",
	[COL_MINOR]       = "minor",
	[COL_ROOT]        = "root",
	[COL_SIZE]        = "size",
	[COL_AVAIL]       = "avail",
	[COL_USED]        = "used",
	[COL_USE_PCT]     = "use%",
	[COL_PROPAGATION] = "propagation",
	[COL_MAJ_MIN]     = "maj:min",
	[COL_OLD_TARGET]  = "old-target",
	[COL_OLD_OPTIONS] = "old-options",
};

enum {
	OPT_all             = (1 << 0),
	OPT_ascii           = (1 << 1),
	OPT_bytes           = (1 << 2),
	OPT_nocanonicalize  = (1 << 3),
	OPT_canonicalize    = (1 << 4),
	OPT_df              = (1 << 5),
	OPT_evaluate        = (1 << 6),
	OPT_first_only      = (1 << 7),
	OPT_invert          = (1 << 8),
	OPT_json            = (1 << 9),
	OPT_kernel          = (1 << 10),
	OPT_list            = (1 << 11),
	OPT_mtab            = (1 << 12),
	OPT_noheadings      = (1 << 13),
	OPT_pairs           = (1 << 14),
	OPT_submounts       = (1 << 15),
	OPT_raw             = (1 << 16),
	OPT_fstab           = (1 << 17),
	OPT_uniq            = (1 << 18),
	OPT_notruncate      = (1 << 19),
	OPT_nofsroot        = (1 << 20),
	OPT_verify          = (1 << 21),
	OPT_poll            = (1 << 22),
	OPT_verbose         = (1 << 23),
	OPT_shadowed        = (1 << 24),
};

static const struct option findmnt_longopts[] = {
	{ "all",             no_argument,       NULL, 'A' },
	{ "ascii",           no_argument,       NULL, 'a' },
	{ "bytes",           no_argument,       NULL, 'b' },
	{ "nocanonicalize",  no_argument,       NULL, 'C' },
	{ "canonicalize",    no_argument,       NULL, 'c' },
	{ "df",              no_argument,       NULL, 'D' },
	{ "direction",       required_argument, NULL, 'd' },
	{ "evaluate",        no_argument,       NULL, 'e' },
	{ "tab-file",        required_argument, NULL, 'F' },
	{ "first-only",      no_argument,       NULL, 'f' },
	{ "help",            no_argument,       NULL, 'h' },
	{ "invert",          no_argument,       NULL, 'i' },
	{ "json",            no_argument,       NULL, 'J' },
	{ "kernel",          no_argument,       NULL, 'k' },
	{ "list",            no_argument,       NULL, 'l' },
	{ "mountpoint",      required_argument, NULL, 'M' },
	{ "mtab",            no_argument,       NULL, 'm' },
	{ "task",            required_argument, NULL, 'N' },
	{ "noheadings",      no_argument,       NULL, 'n' },
	{ "options",         required_argument, NULL, 'O' },
	{ "output",          required_argument, NULL, 'o' },
	{ "pairs",           no_argument,       NULL, 'P' },
	{ "poll",            optional_argument, NULL, 'p' },
	{ "submounts",       no_argument,       NULL, 'R' },
	{ "raw",             no_argument,       NULL, 'r' },
	{ "source",          required_argument, NULL, 'S' },
	{ "fstab",           no_argument,       NULL, 's' },
	{ "target",          required_argument, NULL, 'T' },
	{ "types",           required_argument, NULL, 't' },
	{ "uniq",            no_argument,       NULL, 'U' },
	{ "notruncate",      no_argument,       NULL, 'u' },
	{ "version",         no_argument,       NULL, 'V' },
	{ "nofsroot",        no_argument,       NULL, 'v' },
	{ "timeout",         required_argument, NULL, 'w' },
	{ "verify",          no_argument,       NULL, 'x' },
	{ "verbose",         no_argument,       NULL, 256 },
	{ "shadowed",        no_argument,       NULL, 257 },
	{ NULL, 0, NULL, 0 }
};

struct mount_entry {
	int id;
	int parent_id;
	unsigned maj;
	unsigned min;
	char *root;
	char *target;
	char *vfs_opts;
	char *fstype;
	char *source;
	char *fs_opts;
	char *propagation;
	int freq;
	int passno;

	char *combined_options;

	int stat_done;
	unsigned long long size;
	unsigned long long avail;
	unsigned long long used;
	int use_pct;

	struct mount_entry *parent;
	struct mount_entry *first_child;
	struct mount_entry *next_sibling;

	struct mount_entry *disp_parent;
	struct mount_entry *disp_first_child;
	struct mount_entry *disp_last_child;
	struct mount_entry *disp_next_sibling;

	int depth;
	int match;
	int visible;
	int shadowed;
	int duplicate;

	char *old_target;
	char *old_options;
};

struct tag_entry {
	struct tag_entry *next;
	char *dev;
	char *val;
	int type; /* 0=uuid, 1=label, 2=partuuid, 3=partlabel */
};

struct globals {
	struct mount_entry **entries;
	int count;

	struct mount_entry *disp_roots;
	struct mount_entry *disp_roots_last;

	struct tag_entry *tags;
	int tags_scanned;

	unsigned opts;
	char *opt_direction;
	char *opt_tab_file;
	char *opt_mountpoint;
	char *opt_task;
	char *opt_options;
	char *opt_output;
	char *opt_poll_arg;
	char *opt_source;
	char *opt_target;
	char *opt_types;
	char *opt_timeout;

	char *arg_device_or_mountpoint;

	int out_cols[NUM_COLS];
	int num_out_cols;
	int col_width[NUM_COLS];
};
#define G (*ptr_to_globals)
#define INIT_G() do { \
	SET_PTR_TO_GLOBALS(xzalloc(sizeof(G))); \
} while (0)

static void unescape_octal(char *s)
{
	char *d;

	d = s;
	while (*s) {
		if (s[0] == '\\' && s[1] >= '0' && s[1] <= '7'
		 && s[2] >= '0' && s[2] <= '7'
		 && s[3] >= '0' && s[3] <= '7') {
			*d++ = (char)(((s[1] - '0') << 6) | ((s[2] - '0') << 3) | (s[3] - '0'));
			s += 4;
		} else {
			*d++ = *s++;
		}
	}
	*d = '\0';
}

static char *format_size(unsigned long long bytes, int bytes_mode)
{
	static const char units[] = "\0KMGTPE";
	unsigned long long v;
	unsigned long long frac;
	int u;

	if (bytes_mode)
		return xasprintf("%llu", bytes);
	if (bytes == 0)
		return xstrdup("0");

	v = bytes;
	frac = 0;
	u = 0;
	while (v >= 1024 && u < 6) {
		frac = (((v % 1024) * 10) + 512) / 1024;
		v /= 1024;
		u++;
		if (frac >= 10) {
			v++;
			frac = 0;
		}
	}
	if (u == 0)
		return xasprintf("%lluB", bytes);
	if (v >= 10 || frac == 0)
		return xasprintf("%llu%c", v, units[u]);
	return xasprintf("%llu.%llu%c", v, frac, units[u]);
}

static char *combine_options(const char *vfs, const char *fs)
{
	char *res;
	char *fs_copy, *token, *next;
	char *p;
	int found;
	size_t tlen;

	if (!vfs || !*vfs)
		return xstrdup(fs ? fs : "");
	if (!fs || !*fs)
		return xstrdup(vfs);

	res = xstrdup(vfs);
	fs_copy = xstrdup(fs);
	token = fs_copy;

	while (token && *token) {
		next = strchr(token, ',');
		if (next)
			*next++ = '\0';

		p = res;
		found = 0;
		tlen = strlen(token);

		while (p) {
			if (strncmp(p, token, tlen) == 0 && (p[tlen] == '\0' || p[tlen] == ',')) {
				found = 1;
				break;
			}
			p = strchr(p, ',');
			if (p)
				p++;
		}

		if (!found) {
			xasprintf_inplace(res, "%s,%s", res, token);
		}

		token = next;
	}

	free(fs_copy);
	return res;
}

static void cache_dir(const char *dir, int type)
{
	DIR *d;
	struct dirent *de;
	char *full, *target;
	struct tag_entry *te;

	d = opendir(dir);
	if (!d)
		return;
	while ((de = readdir(d)) != NULL) {
		if (DOT_OR_DOTDOT(de->d_name))
			continue;
		full = concat_path_file(dir, de->d_name);
		target = realpath(full, NULL);
		free(full);
		if (target) {
			te = xzalloc(sizeof(*te));
			te->dev = target;
			te->val = xstrdup(de->d_name);
			te->type = type;
			te->next = G.tags;
			G.tags = te;
		}
	}
	closedir(d);
}

static void init_tags(void)
{
	if (G.tags_scanned)
		return;
	G.tags_scanned = 1;
	cache_dir("/dev/disk/by-uuid", 0);
	cache_dir("/dev/disk/by-label", 1);
	cache_dir("/dev/disk/by-partuuid", 2);
	cache_dir("/dev/disk/by-partlabel", 3);
}

static const char *get_tag_value(const char *devpath, int type)
{
	char *real;
	struct tag_entry *te;

	if (!devpath || !*devpath)
		return "";
	init_tags();
	real = realpath(devpath, NULL);
	for (te = G.tags; te; te = te->next) {
		if (te->type == type) {
			if ((real && strcmp(real, te->dev) == 0)
			 || strcmp(devpath, te->dev) == 0) {
				free(real);
				return te->val;
			}
		}
	}
	free(real);
	return "";
}

static char *resolve_tag_string(const char *spec)
{
	int type;
	const char *val;
	struct tag_entry *te;

	type = -1;
	val = NULL;
	if (strncmp(spec, "UUID=", 5) == 0) {
		type = 0;
		val = spec + 5;
	} else if (strncmp(spec, "LABEL=", 6) == 0) {
		type = 1;
		val = spec + 6;
	} else if (strncmp(spec, "PARTUUID=", 9) == 0) {
		type = 2;
		val = spec + 9;
	} else if (strncmp(spec, "PARTLABEL=", 10) == 0) {
		type = 3;
		val = spec + 10;
	}
	if (type < 0)
		return NULL;

	init_tags();
	for (te = G.tags; te; te = te->next) {
		if (te->type == type && strcmp(te->val, val) == 0)
			return te->dev;
	}
	return NULL;
}

static int match_types(const char *fstype, const char *types)
{
	char *t_copy, *cur, *next;
	int has_positive;
	int pos_match;

	if (!types || !*types)
		return 1;
	if (!fstype)
		return 0;

	has_positive = 0;
	pos_match = 0;
	t_copy = xstrdup(types);
	cur = t_copy;

	while (cur && *cur) {
		next = strchr(cur, ',');
		if (next)
			*next++ = '\0';
		if (cur[0] == 'n' && cur[1] == 'o') {
			if (strcmp(fstype, cur + 2) == 0) {
				free(t_copy);
				return 0;
			}
		} else {
			has_positive = 1;
			if (strcmp(fstype, cur) == 0)
				pos_match = 1;
		}
		cur = next;
	}
	free(t_copy);
	if (has_positive)
		return pos_match;
	return 1;
}

static int match_options(const char *opts_str, const char *filter)
{
	char *f_copy, *cur, *next;
	const char *p;
	size_t len;
	int is_no, found;

	if (!filter || !*filter)
		return 1;
	if (!opts_str)
		return 0;

	f_copy = xstrdup(filter);
	cur = f_copy;

	while (cur && *cur) {
		next = strchr(cur, ',');
		if (next)
			*next++ = '\0';

		is_no = (cur[0] == 'n' && cur[1] == 'o');
		if (is_no)
			cur += 2;

		len = strlen(cur);
		found = 0;
		p = opts_str;

		while (p) {
			if (strncmp(p, cur, len) == 0 && (p[len] == '\0' || p[len] == ',' || p[len] == '=')) {
				found = 1;
				break;
			}
			p = strchr(p, ',');
			if (p)
				p++;
		}

		if (is_no && found) {
			free(f_copy);
			return 0;
		}
		if (!is_no && !found) {
			free(f_copy);
			return 0;
		}

		cur = next;
	}
	free(f_copy);
	return 1;
}

static int match_target_or_source(struct mount_entry *me, const char *arg, int nocanon)
{
	struct stat st;
	char *real_arg, *real_target, *real_source;
	int match;

	if (!arg || !*arg)
		return 1;

	if (strcmp(me->target, arg) == 0)
		return 1;
	if (me->source && strcmp(me->source, arg) == 0)
		return 1;

	if (!nocanon) {
		real_arg = realpath(arg, NULL);
		if (real_arg) {
			real_target = realpath(me->target, NULL);
			if (real_target) {
				match = (strcmp(real_arg, real_target) == 0);
				free(real_target);
				if (match) {
					free(real_arg);
					return 1;
				}
			}
			if (me->source && me->source[0] == '/') {
				real_source = realpath(me->source, NULL);
				if (real_source) {
					match = (strcmp(real_arg, real_source) == 0);
					free(real_source);
					if (match) {
						free(real_arg);
						return 1;
					}
				}
			}
			free(real_arg);
		}
	}

	if (stat(arg, &st) == 0) {
		if (S_ISBLK(st.st_mode)) {
			if (st.st_rdev == makedev(me->maj, me->min))
				return 1;
		} else {
			if (st.st_dev == makedev(me->maj, me->min)) {
				if (strcmp(me->target, arg) == 0)
					return 1;
			}
		}
	}

	return 0;
}

static int match_target_path(struct mount_entry *me, const char *path)
{
	struct stat st;

	if (stat(path, &st) == 0) {
		if (makedev(me->maj, me->min) == st.st_dev)
			return 1;
	}
	if (strcmp(me->target, path) == 0)
		return 1;
	if (is_prefixed_with(path, me->target))
		return 1;
	return 0;
}

static char *get_column_value(struct mount_entry *me, int col_idx,
		int opt_bytes, int opt_canonicalize, int opt_nofsroot, int opt_evaluate)
{
	char *r, *eval;
	const char *src;

	switch (col_idx) {
	case COL_TARGET:
		if (opt_canonicalize) {
			r = realpath(me->target, NULL);
			return r ? r : xstrdup(me->target);
		}
		return xstrdup(me->target);

	case COL_SOURCE:
		src = me->source;
		if (opt_evaluate && src) {
			eval = resolve_tag_string(src);
			if (eval)
				src = eval;
		}
		if (!opt_nofsroot && me->root && strcmp(me->root, "/") != 0) {
			return xasprintf("%s[%s]", src ? src : "", me->root);
		}
		if (opt_canonicalize && src && src[0] == '/') {
			r = realpath(src, NULL);
			return r ? r : xstrdup(src);
		}
		return xstrdup(src ? src : "");

	case COL_FSTYPE:
		return xstrdup(me->fstype ? me->fstype : "");

	case COL_OPTIONS:
		return xstrdup(me->combined_options ? me->combined_options : "");

	case COL_VFS_OPTIONS:
		return xstrdup(me->vfs_opts ? me->vfs_opts : "");

	case COL_FS_OPTIONS:
		return xstrdup(me->fs_opts ? me->fs_opts : "");

	case COL_FREQ:
		return (me->freq >= 0) ? xasprintf("%d", me->freq) : xstrdup("");

	case COL_PASSNO:
		return (me->passno >= 0) ? xasprintf("%d", me->passno) : xstrdup("");

	case COL_LABEL:
		if (me->source && strncmp(me->source, "LABEL=", 6) == 0)
			return xstrdup(me->source + 6);
		return xstrdup(get_tag_value(me->source, 1));

	case COL_UUID:
		if (me->source && strncmp(me->source, "UUID=", 5) == 0)
			return xstrdup(me->source + 5);
		return xstrdup(get_tag_value(me->source, 0));

	case COL_PARTLABEL:
		if (me->source && strncmp(me->source, "PARTLABEL=", 10) == 0)
			return xstrdup(me->source + 10);
		return xstrdup(get_tag_value(me->source, 3));

	case COL_PARTUUID:
		if (me->source && strncmp(me->source, "PARTUUID=", 9) == 0)
			return xstrdup(me->source + 9);
		return xstrdup(get_tag_value(me->source, 2));

	case COL_ID:
		return (me->id >= 0) ? xasprintf("%d", me->id) : xstrdup("");

	case COL_PARENT:
		return (me->parent_id >= 0) ? xasprintf("%d", me->parent_id) : xstrdup("");

	case COL_MAJOR:
		return xasprintf("%u", me->maj);

	case COL_MINOR:
		return xasprintf("%u", me->min);

	case COL_ROOT:
		return xstrdup(me->root ? me->root : "/");

	case COL_SIZE:
		return format_size(me->size, opt_bytes);

	case COL_AVAIL:
		return format_size(me->avail, opt_bytes);

	case COL_USED:
		return format_size(me->used, opt_bytes);

	case COL_USE_PCT:
		return xasprintf("%d%%", me->use_pct);

	case COL_PROPAGATION:
		return xstrdup(me->propagation ? me->propagation : "private");

	case COL_MAJ_MIN:
		return xasprintf("%u:%u", me->maj, me->min);

	case COL_OLD_TARGET:
		return xstrdup(me->old_target ? me->old_target : "");

	case COL_OLD_OPTIONS:
		return xstrdup(me->old_options ? me->old_options : "");

	default:
		return xstrdup("");
	}
}

static void json_print_escaped(const char *s)
{
	unsigned char c;

	putchar('"');
	while (s && *s) {
		c = (unsigned char)*s++;
		switch (c) {
		case '"':  fputs("\\\"", stdout); break;
		case '\\': fputs("\\\\", stdout); break;
		case '\b': fputs("\\b", stdout); break;
		case '\f': fputs("\\f", stdout); break;
		case '\n': fputs("\\n", stdout); break;
		case '\r': fputs("\\r", stdout); break;
		case '\t': fputs("\\t", stdout); break;
		default:
			if (c < 0x20)
				printf("\\u%04x", c);
			else
				putchar(c);
			break;
		}
	}
	putchar('"');
}

static void pairs_print_escaped(const char *s)
{
	unsigned char c;

	putchar('"');
	while (s && *s) {
		c = (unsigned char)*s++;
		if (c == '"' || c == '\\')
			putchar('\\');
		putchar(c);
	}
	putchar('"');
}

static void print_tree_prefix(int depth, const unsigned char *is_last, int opt_ascii)
{
	int i;

	if (depth == 0)
		return;
	for (i = 1; i < depth; i++) {
		if (is_last[i]) {
			fputs("  ", stdout);
		} else {
			if (opt_ascii)
				fputs("| ", stdout);
			else
				fputs("│ ", stdout);
		}
	}
	if (is_last[depth]) {
		if (opt_ascii)
			fputs("`-", stdout);
		else
			fputs("└─", stdout);
	} else {
		if (opt_ascii)
			fputs("|-", stdout);
		else
			fputs("├─", stdout);
	}
}

static void render_tree_node(struct mount_entry *node, int depth, unsigned char *is_last_flags, int tree_col_k)
{
	int k, pad;
	char *val;
	struct mount_entry *child;

	for (k = 0; k < G.num_out_cols; k++) {
		val = get_column_value(node, G.out_cols[k],
				G.opts & OPT_bytes,
				G.opts & OPT_canonicalize,
				G.opts & OPT_nofsroot,
				G.opts & OPT_evaluate);
		if (k == tree_col_k) {
			print_tree_prefix(depth, is_last_flags, G.opts & OPT_ascii);
			fputs(val, stdout);
			pad = G.col_width[k] - (depth * 2 + strlen(val));
		} else {
			fputs(val, stdout);
			pad = G.col_width[k] - strlen(val);
		}
		if (k < G.num_out_cols - 1) {
			while (pad-- >= 0)
				putchar(' ');
		}
		free(val);
	}
	putchar('\n');

	if (G.opts & OPT_first_only)
		return;

	for (child = node->disp_first_child; child; child = child->disp_next_sibling) {
		is_last_flags[depth + 1] = (child->disp_next_sibling == NULL);
		render_tree_node(child, depth + 1, is_last_flags, tree_col_k);
	}
}

static void print_json_tree(struct mount_entry *node, int indent, int is_first)
{
	int k;
	char *val;
	struct mount_entry *child;
	int child_first;

	if (!is_first)
		fputs(",\n", stdout);

	printf("%*s{\n", indent, "");
	for (k = 0; k < G.num_out_cols; k++) {
		val = get_column_value(node, G.out_cols[k],
				G.opts & OPT_bytes,
				G.opts & OPT_canonicalize,
				G.opts & OPT_nofsroot,
				G.opts & OPT_evaluate);
		printf("%*s\"%s\": ", indent + 3, "", col_json_names[G.out_cols[k]]);
		json_print_escaped(val);
		free(val);
		if (k < G.num_out_cols - 1 || node->disp_first_child)
			putchar(',');
		putchar('\n');
	}

	if (node->disp_first_child) {
		printf("%*s\"children\": [\n", indent + 3, "");
		child_first = 1;
		for (child = node->disp_first_child; child; child = child->disp_next_sibling) {
			print_json_tree(child, indent + 6, child_first);
			child_first = 0;
		}
		printf("\n%*s]\n", indent + 3, "");
	}

	printf("%*s}", indent, "");
}

static void mark_descendants_match(struct mount_entry *me)
{
	struct mount_entry *child;

	for (child = me->first_child; child; child = child->next_sibling) {
		child->match = 1;
		mark_descendants_match(child);
	}
}

static void calc_tree_depths(struct mount_entry *node, int depth)
{
	struct mount_entry *child;

	node->depth = depth;
	for (child = node->disp_first_child; child; child = child->disp_next_sibling) {
		calc_tree_depths(child, depth + 1);
	}
}

static struct mount_entry *parse_mountinfo_line(char *line)
{
	struct mount_entry *me;
	char *p, *tok;
	char *prop_buf;

	p = skip_whitespace(line);
	if (!*p || *p == '#')
		return NULL;

	me = xzalloc(sizeof(*me));
	me->freq = -1;
	me->passno = -1;

	me->id = strtol(p, &p, 10);
	me->parent_id = strtol(p, &p, 10);
	me->maj = strtoul(p, &p, 10);
	if (*p == ':') p++;
	me->min = strtoul(p, &p, 10);

	p = skip_whitespace(p);
	tok = p;
	p = skip_non_whitespace(p);
	if (*p) *p++ = '\0';
	unescape_octal(tok);
	me->root = xstrdup(tok);

	p = skip_whitespace(p);
	tok = p;
	p = skip_non_whitespace(p);
	if (*p) *p++ = '\0';
	unescape_octal(tok);
	me->target = xstrdup(tok);

	p = skip_whitespace(p);
	tok = p;
	p = skip_non_whitespace(p);
	if (*p) *p++ = '\0';
	me->vfs_opts = xstrdup(tok);

	prop_buf = NULL;
	while (*p) {
		p = skip_whitespace(p);
		if (*p == '-' && (p[1] == ' ' || p[1] == '\t' || p[1] == '\0')) {
			p++;
			break;
		}
		tok = p;
		p = skip_non_whitespace(p);
		if (*p) *p++ = '\0';
		if (prop_buf)
			xasprintf_inplace(prop_buf, "%s,%s", prop_buf, tok);
		else
			prop_buf = xstrdup(tok);
	}
	me->propagation = prop_buf ? prop_buf : xstrdup("private");

	p = skip_whitespace(p);
	tok = p;
	p = skip_non_whitespace(p);
	if (*p) *p++ = '\0';
	unescape_octal(tok);
	me->fstype = xstrdup(tok);

	p = skip_whitespace(p);
	tok = p;
	p = skip_non_whitespace(p);
	if (*p) *p++ = '\0';
	unescape_octal(tok);
	me->source = xstrdup(tok);

	p = skip_whitespace(p);
	tok = p;
	p = skip_non_whitespace(p);
	if (*p) *p = '\0';
	me->fs_opts = xstrdup(tok);

	me->combined_options = combine_options(me->vfs_opts, me->fs_opts);
	return me;
}

static struct mount_entry *parse_mounts_or_fstab_line(char *line, int is_fstab, int idx)
{
	struct mount_entry *me;
	char *p, *tok;
	struct stat st;

	p = skip_whitespace(line);
	if (!*p || *p == '#')
		return NULL;

	me = xzalloc(sizeof(*me));
	me->id = idx + 1;
	me->parent_id = 0;
	me->root = xstrdup("/");
	me->propagation = xstrdup("private");
	me->freq = -1;
	me->passno = -1;

	tok = p;
	p = skip_non_whitespace(p);
	if (*p) *p++ = '\0';
	unescape_octal(tok);
	me->source = xstrdup(tok);

	p = skip_whitespace(p);
	tok = p;
	p = skip_non_whitespace(p);
	if (*p) *p++ = '\0';
	unescape_octal(tok);
	me->target = xstrdup(tok);

	p = skip_whitespace(p);
	tok = p;
	p = skip_non_whitespace(p);
	if (*p) *p++ = '\0';
	unescape_octal(tok);
	me->fstype = xstrdup(tok);

	p = skip_whitespace(p);
	tok = p;
	p = skip_non_whitespace(p);
	if (*p) *p++ = '\0';
	me->vfs_opts = xstrdup(tok);
	me->fs_opts = xstrdup(tok);
	me->combined_options = xstrdup(tok);

	p = skip_whitespace(p);
	if (*p) {
		tok = p;
		p = skip_non_whitespace(p);
		if (*p) *p++ = '\0';
		me->freq = atoi(tok);
	}

	p = skip_whitespace(p);
	if (*p) {
		tok = p;
		p = skip_non_whitespace(p);
		if (*p) *p = '\0';
		me->passno = atoi(tok);
	}

	if (!is_fstab && stat(me->target, &st) == 0) {
		me->maj = major(st.st_dev);
		me->min = minor(st.st_dev);
	}

	return me;
}

static void read_mount_table(const char *file, int format)
{
	FILE *fp;
	char *line;
	struct mount_entry *me;

	fp = fopen_for_read(file);
	if (!fp)
		bb_perror_msg_and_die("cannot open %s", file);

	while ((line = xmalloc_fgetline(fp)) != NULL) {
		if (format == 0)
			me = parse_mountinfo_line(line);
		else
			me = parse_mounts_or_fstab_line(line, (format == 2), G.count);
		free(line);
		if (!me)
			continue;
		G.entries = xrealloc_vector(G.entries, 4, G.count);
		G.entries[G.count++] = me;
	}
	fclose(fp);
}

static void parse_output_cols(const char *opt_o)
{
	char *copy, *tok, *next;
	int k, found;

	if (!opt_o || !*opt_o)
		return;

	if (*opt_o == '+') {
		opt_o++;
	} else {
		G.num_out_cols = 0;
	}

	copy = xstrdup(opt_o);
	tok = copy;

	while (tok && *tok) {
		next = strchr(tok, ',');
		if (next)
			*next++ = '\0';

		found = -1;
		for (k = 0; k < NUM_COLS; k++) {
			if (strcasecmp(tok, col_names[k]) == 0) {
				found = k;
				break;
			}
		}
		if (found < 0) {
			if (strcasecmp(tok, "FSROOT") == 0 || strcasecmp(tok, "FS-ROOT") == 0)
				found = COL_ROOT;
			else if (strcasecmp(tok, "MAJMIN") == 0)
				found = COL_MAJ_MIN;
			else if (strcasecmp(tok, "USE-PCT") == 0)
				found = COL_USE_PCT;
		}
		if (found < 0) {
			bb_error_msg_and_die("unknown column: %s", tok);
		}
		if (G.num_out_cols < NUM_COLS) {
			G.out_cols[G.num_out_cols++] = found;
		}
		tok = next;
	}
	free(copy);
}

static int needs_statvfs(void)
{
	int k;

	if (G.opts & OPT_df)
		return 1;
	for (k = 0; k < G.num_out_cols; k++) {
		int c = G.out_cols[k];
		if (c == COL_SIZE || c == COL_AVAIL || c == COL_USED || c == COL_USE_PCT)
			return 1;
	}
	return 0;
}

static void do_statvfs_entries(void)
{
	int i;
	struct statvfs sfs;
	struct mount_entry *me;

	for (i = 0; i < G.count; i++) {
		me = G.entries[i];
		if (statvfs(me->target, &sfs) == 0) {
			me->stat_done = 1;
			me->size = (unsigned long long)sfs.f_blocks * sfs.f_frsize;
			me->avail = (unsigned long long)sfs.f_bavail * sfs.f_frsize;
			me->used = me->size - ((unsigned long long)sfs.f_bfree * sfs.f_frsize);
			if (me->used + me->avail > 0)
				me->use_pct = (int)((me->used * 100 + (me->used + me->avail - 1)) / (me->used + me->avail));
			else if (me->size > 0)
				me->use_pct = (int)((me->used * 100 + me->size / 2) / me->size);
			else
				me->use_pct = 0;
		}
	}
}

static int do_verify(const char *fstab_file)
{
	FILE *fp;
	char *line, *p, *resolved;
	int lineno;
	unsigned parse_errors, errors, warnings;
	char *tokens[6];
	int ntokens;
	struct stat st;

	lineno = 0;
	parse_errors = 0;
	errors = 0;
	warnings = 0;

	fp = fopen_for_read(fstab_file ? fstab_file : "/etc/fstab");
	if (!fp)
		bb_perror_msg_and_die("cannot open %s", fstab_file ? fstab_file : "/etc/fstab");

	while ((line = xmalloc_fgetline(fp)) != NULL) {
		lineno++;
		p = skip_whitespace(line);
		if (!*p || *p == '#') {
			free(line);
			continue;
		}

		ntokens = 0;
		while (*p && ntokens < 6) {
			tokens[ntokens++] = p;
			p = skip_non_whitespace(p);
			if (*p) *p++ = '\0';
			p = skip_whitespace(p);
		}

		if (ntokens < 4) {
			printf("line %d: parse error\n", lineno);
			parse_errors++;
			free(line);
			continue;
		}

		unescape_octal(tokens[1]);
		unescape_octal(tokens[0]);

		if (stat(tokens[1], &st) != 0) {
			printf("%s\n   [W] target does not exist (%s)\n", tokens[1], strerror(errno));
			warnings++;
		} else if (!S_ISDIR(st.st_mode)) {
			printf("%s\n   [W] target is not a directory\n", tokens[1]);
			warnings++;
		}

		if (tokens[0][0] == '/') {
			if (stat(tokens[0], &st) != 0) {
				printf("%s\n   [W] source does not exist (%s)\n", tokens[1], strerror(errno));
				warnings++;
			}
		} else if (strncmp(tokens[0], "UUID=", 5) == 0
		        || strncmp(tokens[0], "LABEL=", 6) == 0
		        || strncmp(tokens[0], "PARTUUID=", 9) == 0
		        || strncmp(tokens[0], "PARTLABEL=", 10) == 0) {
			resolved = resolve_tag_string(tokens[0]);
			if (!resolved) {
				printf("%s\n   [W] cannot resolve %s\n", tokens[1], tokens[0]);
				warnings++;
			}
		}

		if (G.opts & OPT_verbose) {
			printf("%s: OK\n", tokens[1]);
		}

		free(line);
	}
	fclose(fp);

	printf("%u parse errors, %u errors, %u warnings\n", parse_errors, errors, warnings);
	return (errors || parse_errors) ? EXIT_FAILURE : EXIT_SUCCESS;
}

static void do_poll(const char *tab_file, int timeout_ms)
{
	int fd;
	struct pollfd pfd;
	int ret;
	const char *path;

	path = tab_file ? tab_file : "/proc/self/mountinfo";
	fd = xopen(path, O_RDONLY);
	pfd.fd = fd;
	pfd.events = POLLPRI | POLLERR;

	while (1) {
		ret = poll(&pfd, 1, timeout_ms);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			bb_perror_msg_and_die("poll");
		}
		if (ret == 0) {
			break;
		}
		lseek(fd, 0, SEEK_SET);
		if (timeout_ms >= 0)
			break;
	}
	close(fd);
}

int findmnt_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int findmnt_main(int argc, char **argv)
{
	int opt;
	int i, j, k;
	int format_type;
	char *filename;
	int tree_mode;
	int tree_col_k;
	struct mount_entry *e, *p, *anc, *node;
	unsigned char is_last_flags[256];
	int vis_width, pad;
	char *val;
	int is_first;
	int has_matching_ancestor;
	int visible_count;

	INIT_G();

	while ((opt = getopt_long(argc, argv, "AaBbCcDd:eF:fhiJklM:mN:nO:o:Pp::RrS:sT:t:UuVvw:x", findmnt_longopts, NULL)) != -1) {
		switch (opt) {
		case 'A': G.opts |= OPT_all; break;
		case 'a': G.opts |= OPT_ascii; break;
		case 'b': G.opts |= OPT_bytes; break;
		case 'C': G.opts |= OPT_nocanonicalize; break;
		case 'c': G.opts |= OPT_canonicalize; break;
		case 'D': G.opts |= OPT_df; break;
		case 'd': G.opt_direction = optarg; break;
		case 'e': G.opts |= OPT_evaluate; break;
		case 'F': G.opt_tab_file = optarg; break;
		case 'f': G.opts |= OPT_first_only; break;
		case 'h': bb_show_usage(); break;
		case 'i': G.opts |= OPT_invert; break;
		case 'J': G.opts |= OPT_json; break;
		case 'k': G.opts |= OPT_kernel; break;
		case 'l': G.opts |= OPT_list; break;
		case 'M': G.opt_mountpoint = optarg; break;
		case 'm': G.opts |= OPT_mtab; break;
		case 'N': G.opt_task = optarg; break;
		case 'n': G.opts |= OPT_noheadings; break;
		case 'O': G.opt_options = optarg; break;
		case 'o': G.opt_output = optarg; break;
		case 'P': G.opts |= OPT_pairs; break;
		case 'p': G.opts |= OPT_poll; G.opt_poll_arg = optarg; break;
		case 'R': G.opts |= OPT_submounts; break;
		case 'r': G.opts |= OPT_raw; break;
		case 'S': G.opt_source = optarg; break;
		case 's': G.opts |= OPT_fstab; break;
		case 'T': G.opt_target = optarg; break;
		case 't': G.opt_types = optarg; break;
		case 'U': G.opts |= OPT_uniq; break;
		case 'u': G.opts |= OPT_notruncate; break;
		case 'V': printf("findmnt from %s\n", BB_VER); return EXIT_SUCCESS;
		case 'v': G.opts |= OPT_nofsroot; break;
		case 'w': G.opt_timeout = optarg; break;
		case 'x': G.opts |= OPT_verify; break;
		case 256: G.opts |= OPT_verbose; break;
		case 257: G.opts |= OPT_shadowed; break;
		default:
			bb_show_usage();
		}
	}

	argv += optind;
	if (*argv) {
		G.arg_device_or_mountpoint = *argv++;
		if (*argv) {
			G.opt_source = G.arg_device_or_mountpoint;
			G.opt_target = *argv++;
			G.arg_device_or_mountpoint = NULL;
			if (*argv)
				bb_show_usage();
		}
	}

	if (G.opts & OPT_verify) {
		return do_verify(G.opt_tab_file);
	}

	if (G.opts & OPT_poll) {
		int timeout_ms = -1;
		if (G.opt_timeout)
			timeout_ms = xatoi_positive(G.opt_timeout);
		do_poll(G.opt_tab_file, timeout_ms);
		return EXIT_SUCCESS;
	}

	/* Determine input file and format */
	format_type = 0; /* 0=mountinfo, 1=mounts, 2=fstab */
	if (G.opt_task) {
		filename = xasprintf("/proc/%s/mountinfo", G.opt_task);
	} else if (G.opt_tab_file) {
		filename = G.opt_tab_file;
		if (G.opts & OPT_fstab)
			format_type = 2;
		else if (G.opts & OPT_mtab)
			format_type = 1;
		else
			format_type = 0;
	} else if (G.opts & OPT_fstab) {
		filename = (char *)"/etc/fstab";
		format_type = 2;
	} else if (G.opts & OPT_mtab) {
		filename = (char *)"/proc/mounts";
		format_type = 1;
	} else {
		filename = (char *)"/proc/self/mountinfo";
		format_type = 0;
	}

	read_mount_table(filename, format_type);

	/* Setup output columns */
	if (G.opts & OPT_df) {
		G.out_cols[0] = COL_SOURCE;
		G.out_cols[1] = COL_FSTYPE;
		G.out_cols[2] = COL_SIZE;
		G.out_cols[3] = COL_USED;
		G.out_cols[4] = COL_AVAIL;
		G.out_cols[5] = COL_USE_PCT;
		G.out_cols[6] = COL_TARGET;
		G.num_out_cols = 7;
	} else {
		G.out_cols[0] = COL_TARGET;
		G.out_cols[1] = COL_SOURCE;
		G.out_cols[2] = COL_FSTYPE;
		G.out_cols[3] = COL_OPTIONS;
		G.num_out_cols = 4;
	}

	if (G.opt_output) {
		parse_output_cols(G.opt_output);
	}

	if (needs_statvfs()) {
		do_statvfs_entries();
	}

	/* Detect shadowed and duplicate entries */
	for (i = 0; i < G.count; i++) {
		for (j = i + 1; j < G.count; j++) {
			if (strcmp(G.entries[i]->target, G.entries[j]->target) == 0) {
				G.entries[i]->shadowed = 1;
				G.entries[i]->duplicate = 1;
			}
		}
	}

	/* Link parent-child relationships */
	for (i = 0; i < G.count; i++) {
		e = G.entries[i];
		if (format_type == 0 && e->parent_id > 0) {
			for (j = 0; j < G.count; j++) {
				p = G.entries[j];
				if (p->id == e->parent_id && p != e) {
					e->parent = p;
					e->next_sibling = p->first_child;
					p->first_child = e;
					break;
				}
			}
		} else if (format_type != 0) {
			int best_len = 0;
			struct mount_entry *best_p = NULL;
			for (j = 0; j < G.count; j++) {
				p = G.entries[j];
				if (p != e && is_prefixed_with(e->target, p->target)) {
					int plen = strlen(p->target);
					if (plen > best_len) {
						best_len = plen;
						best_p = p;
					}
				}
			}
			if (best_p) {
				e->parent = best_p;
				e->next_sibling = best_p->first_child;
				best_p->first_child = e;
			}
		}
	}

	/* Apply filters to determine matches */
	for (i = 0; i < G.count; i++) {
		int m = 1;
		e = G.entries[i];

		if (G.opt_types && !match_types(e->fstype, G.opt_types))
			m = 0;
		if (G.opt_options && !match_options(e->combined_options, G.opt_options))
			m = 0;
		if (G.opt_mountpoint) {
			if (strcmp(e->target, G.opt_mountpoint) != 0) {
				if (!(G.opts & OPT_nocanonicalize)) {
					char *r1 = realpath(e->target, NULL);
					char *r2 = realpath(G.opt_mountpoint, NULL);
					if (!r1 || !r2 || strcmp(r1, r2) != 0)
						m = 0;
					free(r1);
					free(r2);
				} else {
					m = 0;
				}
			}
		}
		if (G.opt_source) {
			if (!match_target_or_source(e, G.opt_source, G.opts & OPT_nocanonicalize))
				m = 0;
		}
		if (G.opt_target) {
			if (!match_target_path(e, G.opt_target))
				m = 0;
		}
		if (G.arg_device_or_mountpoint) {
			if (!match_target_or_source(e, G.arg_device_or_mountpoint, G.opts & OPT_nocanonicalize))
				m = 0;
		}
		if (G.opts & OPT_shadowed) {
			if (!e->shadowed)
				m = 0;
		}
		if ((G.opts & OPT_df) && !(G.opts & OPT_all)) {
			if (e->size == 0)
				m = 0;
		}
		if ((G.opts & OPT_uniq) && e->duplicate) {
			m = 0;
		}

		if (G.opts & OPT_invert)
			m = !m;

		e->match = m;
	}

	/* Handle -R, --submounts */
	if (G.opts & OPT_submounts) {
		for (i = 0; i < G.count; i++) {
			if (G.entries[i]->match) {
				mark_descendants_match(G.entries[i]);
			}
		}
	}

	/* Set visibility */
	for (i = 0; i < G.count; i++) {
		G.entries[i]->visible = G.entries[i]->match;
	}

	/* Handle direction: 'backward' or 'desc' */
	if (G.opt_direction && (strcmp(G.opt_direction, "backward") == 0 || strcmp(G.opt_direction, "desc") == 0)) {
		for (i = 0; i < G.count / 2; i++) {
			struct mount_entry *tmp = G.entries[i];
			G.entries[i] = G.entries[G.count - 1 - i];
			G.entries[G.count - 1 - i] = tmp;
		}
	}

	/* Build display tree */
	for (i = 0; i < G.count; i++) {
		e = G.entries[i];
		e->disp_parent = NULL;
		e->disp_first_child = NULL;
		e->disp_next_sibling = NULL;
		e->disp_last_child = NULL;
	}
	G.disp_roots = NULL;
	G.disp_roots_last = NULL;

	for (i = 0; i < G.count; i++) {
		e = G.entries[i];
		if (!e->visible)
			continue;
		anc = e->parent;
		while (anc && !anc->visible)
			anc = anc->parent;
		e->disp_parent = anc;
		if (anc) {
			if (anc->disp_last_child) {
				anc->disp_last_child->disp_next_sibling = e;
				anc->disp_last_child = e;
			} else {
				anc->disp_first_child = e;
				anc->disp_last_child = e;
			}
		} else {
			if (G.disp_roots_last) {
				G.disp_roots_last->disp_next_sibling = e;
				G.disp_roots_last = e;
			} else {
				G.disp_roots = e;
				G.disp_roots_last = e;
			}
		}
	}

	/* Determine tree mode: default unless -l, -P, -r, -D, or fstab */
	tree_mode = 1;
	if (G.opts & (OPT_list | OPT_pairs | OPT_raw | OPT_fstab | OPT_df))
		tree_mode = 0;

	/* In JSON mode without -l, tree format is used (unless -D or -s) */
	if ((G.opts & OPT_json) && !(G.opts & (OPT_list | OPT_df | OPT_fstab)))
		tree_mode = 1;

	/* Check if any visible entry has a displayed parent */
	has_matching_ancestor = 0;
	for (i = 0; i < G.count; i++) {
		if (G.entries[i]->visible && G.entries[i]->disp_parent) {
			has_matching_ancestor = 1;
			break;
		}
	}
	if (!has_matching_ancestor && !(G.opts & OPT_json))
		tree_mode = 0;

	/* Calculate display depths */
	for (node = G.disp_roots; node; node = node->disp_next_sibling) {
		calc_tree_depths(node, 0);
	}

	/* Calculate column widths */
	for (k = 0; k < G.num_out_cols; k++) {
		G.col_width[k] = strlen(col_names[G.out_cols[k]]);
	}

	tree_col_k = -1;
	for (k = 0; k < G.num_out_cols; k++) {
		if (G.out_cols[k] == COL_TARGET) {
			tree_col_k = k;
			break;
		}
	}
	if (tree_col_k < 0)
		tree_col_k = 0;

	for (i = 0; i < G.count; i++) {
		e = G.entries[i];
		if (!e->visible)
			continue;
		for (k = 0; k < G.num_out_cols; k++) {
			val = get_column_value(e, G.out_cols[k],
					G.opts & OPT_bytes,
					G.opts & OPT_canonicalize,
					G.opts & OPT_nofsroot,
					G.opts & OPT_evaluate);
			vis_width = strlen(val);
			if (tree_mode && k == tree_col_k)
				vis_width += e->depth * 2;
			if (vis_width > G.col_width[k])
				G.col_width[k] = vis_width;
			free(val);
		}
	}

	visible_count = 0;
	for (i = 0; i < G.count; i++) {
		if (G.entries[i]->visible)
			visible_count++;
	}

	if (visible_count == 0) {
		if (G.opts & OPT_json) {
			puts("{\n   \"filesystems\": [\n\n   ]\n}");
		}
		if (G.arg_device_or_mountpoint || G.opt_source || G.opt_target || G.opt_mountpoint)
			return EXIT_FAILURE;
		return EXIT_SUCCESS;
	}

	/* Format outputs */
	if (G.opts & OPT_json) {
		fputs("{\n   \"filesystems\": [\n", stdout);
		if (tree_mode) {
			is_first = 1;
			for (node = G.disp_roots; node; node = node->disp_next_sibling) {
				print_json_tree(node, 6, is_first);
				is_first = 0;
				if (G.opts & OPT_first_only)
					break;
			}
		} else {
			int count = 0;
			for (i = 0; i < G.count; i++) {
				if (!G.entries[i]->visible)
					continue;
				if (count > 0)
					fputs(",\n", stdout);
				fputs("      {\n", stdout);
				for (k = 0; k < G.num_out_cols; k++) {
					val = get_column_value(G.entries[i], G.out_cols[k],
							G.opts & OPT_bytes,
							G.opts & OPT_canonicalize,
							G.opts & OPT_nofsroot,
							G.opts & OPT_evaluate);
					printf("         \"%s\": ", col_json_names[G.out_cols[k]]);
					json_print_escaped(val);
					if (k < G.num_out_cols - 1)
						putchar(',');
					putchar('\n');
					free(val);
				}
				fputs("      }", stdout);
				count++;
				if (G.opts & OPT_first_only)
					break;
			}
		}
		fputs("\n   ]\n}\n", stdout);
	} else if (G.opts & OPT_pairs) {
		for (i = 0; i < G.count; i++) {
			if (!G.entries[i]->visible)
				continue;
			for (k = 0; k < G.num_out_cols; k++) {
				val = get_column_value(G.entries[i], G.out_cols[k],
						G.opts & OPT_bytes,
						G.opts & OPT_canonicalize,
						G.opts & OPT_nofsroot,
						G.opts & OPT_evaluate);
				printf("%s=", col_names[G.out_cols[k]]);
				pairs_print_escaped(val);
				if (k < G.num_out_cols - 1)
					putchar(' ');
				free(val);
			}
			putchar('\n');
			if (G.opts & OPT_first_only)
				break;
		}
	} else if (G.opts & OPT_raw) {
		if (!(G.opts & OPT_noheadings)) {
			for (k = 0; k < G.num_out_cols; k++) {
				fputs(col_names[G.out_cols[k]], stdout);
				if (k < G.num_out_cols - 1)
					putchar(' ');
			}
			putchar('\n');
		}
		for (i = 0; i < G.count; i++) {
			if (!G.entries[i]->visible)
				continue;
			for (k = 0; k < G.num_out_cols; k++) {
				val = get_column_value(G.entries[i], G.out_cols[k],
						G.opts & OPT_bytes,
						G.opts & OPT_canonicalize,
						G.opts & OPT_nofsroot,
						G.opts & OPT_evaluate);
				fputs(val, stdout);
				if (k < G.num_out_cols - 1)
					putchar(' ');
				free(val);
			}
			putchar('\n');
			if (G.opts & OPT_first_only)
				break;
		}
	} else if (tree_mode) {
		if (!(G.opts & OPT_noheadings)) {
			for (k = 0; k < G.num_out_cols; k++) {
				fputs(col_names[G.out_cols[k]], stdout);
				pad = G.col_width[k] - strlen(col_names[G.out_cols[k]]);
				if (k < G.num_out_cols - 1) {
					while (pad-- >= 0)
						putchar(' ');
				}
			}
			putchar('\n');
		}
		for (node = G.disp_roots; node; node = node->disp_next_sibling) {
			is_last_flags[0] = (node->disp_next_sibling == NULL);
			render_tree_node(node, 0, is_last_flags, tree_col_k);
			if (G.opts & OPT_first_only)
				break;
		}
	} else {
		/* List mode */
		if (!(G.opts & OPT_noheadings)) {
			for (k = 0; k < G.num_out_cols; k++) {
				fputs(col_names[G.out_cols[k]], stdout);
				pad = G.col_width[k] - strlen(col_names[G.out_cols[k]]);
				if (k < G.num_out_cols - 1) {
					while (pad-- >= 0)
						putchar(' ');
				}
			}
			putchar('\n');
		}
		for (i = 0; i < G.count; i++) {
			if (!G.entries[i]->visible)
				continue;
			for (k = 0; k < G.num_out_cols; k++) {
				val = get_column_value(G.entries[i], G.out_cols[k],
						G.opts & OPT_bytes,
						G.opts & OPT_canonicalize,
						G.opts & OPT_nofsroot,
						G.opts & OPT_evaluate);
				fputs(val, stdout);
				pad = G.col_width[k] - strlen(val);
				if (k < G.num_out_cols - 1) {
					while (pad-- >= 0)
						putchar(' ');
				}
				free(val);
			}
			putchar('\n');
			if (G.opts & OPT_first_only)
				break;
		}
	}

	fflush_stdout_and_exit(EXIT_SUCCESS);
}
