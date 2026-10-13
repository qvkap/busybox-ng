/* vi: set sw=4 ts=4: */
/*
 * lsblk implementation for busybox
 *
 * Licensed under GPLv2 or later, see file LICENSE in this source tree.
 */
//config:config LSBLK
//config:	bool "lsblk (2.5 kb)"
//config:	default y
//config:	help
//config:	List information about all available or specified block devices.

//applet:IF_LSBLK(APPLET(lsblk, BB_DIR_USR_BIN, BB_SUID_DROP))

//kbuild:lib-$(CONFIG_LSBLK) += lsblk.o

//usage:#define lsblk_trivial_usage
//usage:       "[OPTIONS] [BLOCKDEV]..."
//usage:#define lsblk_full_usage "\n\n"
//usage:       "List information about block devices\n"
//usage:     "\n	-a, --all		Print all devices"
//usage:     "\n	-b, --bytes		Print SIZE in bytes"
//usage:     "\n	-d, --nodeps		Don't print partitions/holders"
//usage:     "\n	-f, --fs		Output filesystem info"
//usage:     "\n	-i, --ascii		Use ASCII characters only"
//usage:     "\n	-J, --json		Use JSON output format"
//usage:     "\n	-l, --list		Use list format output"
//usage:     "\n	-m, --perms		Output info about permissions"
//usage:     "\n	-n, --noheadings	Don't print headings"
//usage:     "\n	-o, --output LIST	Output columns"
//usage:     "\n	-O, --output-all	Output all columns"
//usage:     "\n	-p, --paths		Print complete device path"
//usage:     "\n	-P, --pairs		Use key=\"value\" output format"
//usage:     "\n	-r, --raw		Use raw output format"
//usage:     "\n	-s, --inverse		Inverse dependencies"
//usage:     "\n	-t, --topology		Output info about topology"
//usage:     "\n	-x, --sort COL		Sort output by <column>"
//usage:     "\n	-z, --zoned		Print zone information"

#include "libbb.h"
#include <sys/statvfs.h>

enum {
	COL_NAME = 0,
	COL_KNAME,
	COL_PATH,
	COL_MAJ_MIN,
	COL_FSAVAIL,
	COL_FSROOTS,
	COL_FSSIZE,
	COL_FSTYPE,
	COL_FSUSED,
	COL_FSUSE_PCT,
	COL_FSVER,
	COL_MOUNTPOINT,
	COL_MOUNTPOINTS,
	COL_LABEL,
	COL_UUID,
	COL_PTTYPE,
	COL_PARTTYPE,
	COL_PARTLABEL,
	COL_PARTUUID,
	COL_RA,
	COL_RO,
	COL_RM,
	COL_HOTPLUG,
	COL_MODEL,
	COL_SERIAL,
	COL_SIZE,
	COL_STATE,
	COL_OWNER,
	COL_GROUP,
	COL_MODE,
	COL_ALIGNMENT,
	COL_MIN_IO,
	COL_OPT_IO,
	COL_PHY_SEC,
	COL_LOG_SEC,
	COL_ROTA,
	COL_SCHED,
	COL_RQ_SIZE,
	COL_TYPE,
	COL_DISCALN,
	COL_DISCGRAN,
	COL_DISCMAX,
	COL_DISCZERO,
	COL_WSAME,
	COL_ZONED,
	COL_DAX,
	NUM_COLUMNS
};

struct column_info {
	const char *name;
	const char *alias;
	const char *json_name;
	uint8_t is_right;
	uint8_t is_bool;
	uint8_t is_byte_size;
	uint8_t is_numeric;
};

static const struct column_info columns_def[NUM_COLUMNS] = {
	[COL_NAME]        = { "NAME",        NULL,        "name",        0, 0, 0, 0 },
	[COL_KNAME]       = { "KNAME",       NULL,        "kname",       0, 0, 0, 0 },
	[COL_PATH]        = { "PATH",        NULL,        "path",        0, 0, 0, 0 },
	[COL_MAJ_MIN]     = { "MAJ:MIN",     "MAJMIN",    "maj:min",     0, 0, 0, 0 },
	[COL_FSAVAIL]     = { "FSAVAIL",     NULL,        "fsavail",     1, 0, 1, 1 },
	[COL_FSROOTS]     = { "FSROOTS",     NULL,        "fsroots",     0, 0, 0, 0 },
	[COL_FSSIZE]      = { "FSSIZE",      NULL,        "fssize",      1, 0, 1, 1 },
	[COL_FSTYPE]      = { "FSTYPE",      NULL,        "fstype",      0, 0, 0, 0 },
	[COL_FSUSED]      = { "FSUSED",      NULL,        "fsused",      1, 0, 1, 1 },
	[COL_FSUSE_PCT]   = { "FSUSE%",      "FSUSE_PCT", "fsuse%",      1, 0, 0, 1 },
	[COL_FSVER]       = { "FSVER",       NULL,        "fsver",       0, 0, 0, 0 },
	[COL_MOUNTPOINT]  = { "MOUNTPOINT",  NULL,        "mountpoint",  0, 0, 0, 0 },
	[COL_MOUNTPOINTS] = { "MOUNTPOINTS", NULL,        "mountpoints", 0, 0, 0, 0 },
	[COL_LABEL]       = { "LABEL",       NULL,        "label",       0, 0, 0, 0 },
	[COL_UUID]        = { "UUID",        NULL,        "uuid",        0, 0, 0, 0 },
	[COL_PTTYPE]      = { "PTTYPE",      NULL,        "pttype",      0, 0, 0, 0 },
	[COL_PARTTYPE]    = { "PARTTYPE",    NULL,        "parttype",    0, 0, 0, 0 },
	[COL_PARTLABEL]   = { "PARTLABEL",   NULL,        "partlabel",   0, 0, 0, 0 },
	[COL_PARTUUID]    = { "PARTUUID",    NULL,        "partuuid",    0, 0, 0, 0 },
	[COL_RA]          = { "RA",          NULL,        "ra",          1, 0, 0, 1 },
	[COL_RO]          = { "RO",          NULL,        "ro",          1, 1, 0, 1 },
	[COL_RM]          = { "RM",          NULL,        "rm",          1, 1, 0, 1 },
	[COL_HOTPLUG]     = { "HOTPLUG",     NULL,        "hotplug",     1, 1, 0, 1 },
	[COL_MODEL]       = { "MODEL",       NULL,        "model",       0, 0, 0, 0 },
	[COL_SERIAL]      = { "SERIAL",      NULL,        "serial",      0, 0, 0, 0 },
	[COL_SIZE]        = { "SIZE",        NULL,        "size",        1, 0, 1, 1 },
	[COL_STATE]       = { "STATE",       NULL,        "state",       0, 0, 0, 0 },
	[COL_OWNER]       = { "OWNER",       NULL,        "owner",       0, 0, 0, 0 },
	[COL_GROUP]       = { "GROUP",       NULL,        "group",       0, 0, 0, 0 },
	[COL_MODE]        = { "MODE",        NULL,        "mode",        0, 0, 0, 0 },
	[COL_ALIGNMENT]   = { "ALIGNMENT",   NULL,        "alignment",   1, 0, 0, 1 },
	[COL_MIN_IO]      = { "MIN-IO",      NULL,        "min-io",      1, 0, 0, 1 },
	[COL_OPT_IO]      = { "OPT-IO",      NULL,        "opt-io",      1, 0, 0, 1 },
	[COL_PHY_SEC]     = { "PHY-SEC",     NULL,        "phy-sec",     1, 0, 0, 1 },
	[COL_LOG_SEC]     = { "LOG-SEC",     NULL,        "log-sec",     1, 0, 0, 1 },
	[COL_ROTA]        = { "ROTA",        NULL,        "rota",        1, 1, 0, 1 },
	[COL_SCHED]       = { "SCHED",       NULL,        "sched",       0, 0, 0, 0 },
	[COL_RQ_SIZE]     = { "RQ-SIZE",     NULL,        "rq-size",     1, 0, 0, 1 },
	[COL_TYPE]        = { "TYPE",        NULL,        "type",        0, 0, 0, 0 },
	[COL_DISCALN]     = { "DISCALN",     "DISC-ALN",  "disc-aln",    1, 0, 0, 1 },
	[COL_DISCGRAN]    = { "DISCGRAN",    "DISC-GRAN", "disc-gran",   1, 0, 1, 1 },
	[COL_DISCMAX]     = { "DISCMAX",     "DISC-MAX",  "disc-max",    1, 0, 1, 1 },
	[COL_DISCZERO]    = { "DISCZERO",    "DISC-ZERO", "disc-zero",   1, 1, 0, 1 },
	[COL_WSAME]       = { "WSAME",       NULL,        "wsame",       1, 0, 1, 1 },
	[COL_ZONED]       = { "ZONED",       NULL,        "zoned",       0, 0, 0, 0 },
	[COL_DAX]         = { "DAX",         NULL,        "dax",         1, 1, 0, 1 },
};

struct mount_entry {
	char *majmin;
	char *root;
	char *mountpoint;
	char *fstype;
	char *devname;
};

struct blockdev_info {
	char *name;
	char *kname;
	char *path;
	char *majmin;
	unsigned major;
	unsigned minor;
	unsigned long long size;
	char *type;
	char *ro;
	char *rm;
	char *hotplug;
	char *model;
	char *serial;
	char *state;
	char *owner;
	char *group;
	char *mode;
	char *alignment;
	char *min_io;
	char *opt_io;
	char *phy_sec;
	char *log_sec;
	char *rota;
	char *sched;
	char *rq_size;
	char *discaln;
	char *discgran;
	char *discmax;
	char *disczero;
	char *wsame;
	char *zoned;
	char *dax;
	char *ra;

	/* Filesystem info */
	char *fstype;
	char *fsver;
	char *label;
	char *uuid;
	char *pttype;
	char *parttype;
	char *partlabel;
	char *partuuid;

	/* Mount info */
	char **mountpoints;
	char **fsroots;
	unsigned nmounts;
	unsigned long long fsavail;
	unsigned long long fssize;
	unsigned long long fsused;
	char *fsuse_pct;
	int has_statvfs;

	/* Formatted column values for printing & width calculation */
	char *col_values[NUM_COLUMNS];

	/* Tree relationships */
	struct blockdev_info **children;
	unsigned nchildren;
	struct blockdev_info **parents;
	unsigned nparents;

	int is_child;
	int is_cli_arg;
	int depth;
};

struct globals {
	struct blockdev_info **devices;
	unsigned num_devices;

	struct mount_entry *mounts;
	unsigned num_mounts;

	int *columns;
	unsigned num_columns;

	unsigned opt;
	int sort_col;
	unsigned exitcode;
};
#define G (*ptr_to_globals)
#define INIT_G() do { \
	SET_PTR_TO_GLOBALS(xzalloc(sizeof(G))); \
} while (0)

enum {
	OPT_a = (1 << 0),  /* all */
	OPT_b = (1 << 1),  /* bytes */
	OPT_d = (1 << 2),  /* nodeps */
	OPT_f = (1 << 3),  /* fs */
	OPT_i = (1 << 4),  /* ascii */
	OPT_J = (1 << 5),  /* json */
	OPT_l = (1 << 6),  /* list */
	OPT_m = (1 << 7),  /* perms */
	OPT_n = (1 << 8),  /* noheadings */
	OPT_o = (1 << 9),  /* output */
	OPT_O = (1 << 10), /* output-all */
	OPT_p = (1 << 11), /* paths */
	OPT_P = (1 << 12), /* pairs */
	OPT_r = (1 << 13), /* raw */
	OPT_s = (1 << 14), /* inverse */
	OPT_t = (1 << 15), /* topology */
	OPT_x = (1 << 16), /* sort */
	OPT_z = (1 << 17), /* zoned */
};

#if ENABLE_LONG_OPTS
static const char lsblk_longopts[] ALIGN1 =
	"all\0"         No_argument       "a"
	"bytes\0"       No_argument       "b"
	"nodeps\0"      No_argument       "d"
	"fs\0"          No_argument       "f"
	"ascii\0"       No_argument       "i"
	"json\0"        No_argument       "J"
	"list\0"        No_argument       "l"
	"perms\0"       No_argument       "m"
	"noheadings\0"  No_argument       "n"
	"output\0"      Required_argument "o"
	"output-all\0"  No_argument       "O"
	"paths\0"       No_argument       "p"
	"pairs\0"       No_argument       "P"
	"raw\0"         No_argument       "r"
	"inverse\0"     No_argument       "s"
	"topology\0"    No_argument       "t"
	"sort\0"        Required_argument "x"
	"zoned\0"       No_argument       "z"
	;
#endif

/* Unescape \ooo octal sequences in-place */
static void unescape_octal(char *str)
{
	char *src;
	char *dst;

	src = str;
	dst = str;
	while (*src) {
		if (src[0] == '\\' && src[1] >= '0' && src[1] <= '7'
		 && src[2] >= '0' && src[2] <= '7'
		 && src[3] >= '0' && src[3] <= '7') {
			*dst++ = (char)((src[1] - '0') * 64 + (src[2] - '0') * 8 + (src[3] - '0'));
			src += 4;
		} else {
			*dst++ = *src++;
		}
	}
	*dst = '\0';
}

static unsigned long long read_ull(const char *path, const char *name)
{
	char *filename;
	ssize_t len;
	unsigned long long val;
	char buf[sizeof(val) * 3];

	val = ~0ULL;
	filename = concat_path_file(path, name);
	len = open_read_close(filename, buf, sizeof(buf) - 1);
	free(filename);
	if (len > 0 && len < (ssize_t)(sizeof(buf) - 1)) {
		buf[len] = '\0';
		val = bb_strtoull(buf, NULL, 10);
	}
	return val;
}

static char *read_str(const char *path, const char *name)
{
	char *filename;
	char *res;

	filename = concat_path_file(path, name);
	res = xmalloc_open_read_close(filename, NULL);
	free(filename);

	if (res) {
		strchrnul(res, '\n')[0] = '\0';
		trim(res);
	}
	return res;
}

static char *read_scheduler(const char *queue_path)
{
	char *str;
	char *open_bracket;
	char *close_bracket;
	char *sp;

	str = read_str(queue_path, "scheduler");
	if (!str)
		return NULL;
	open_bracket = strchr(str, '[');
	if (open_bracket) {
		close_bracket = strchr(open_bracket + 1, ']');
		if (close_bracket) {
			*close_bracket = '\0';
			sp = xstrdup(open_bracket + 1);
			free(str);
			return sp;
		}
	}
	if (str[0]) {
		sp = strchr(str, ' ');
		if (sp)
			*sp = '\0';
		return str;
	}
	free(str);
	return NULL;
}

static char *format_size(unsigned long long bytes, int bytes_opt)
{
	static const char units[] = "BKMGTPEZY";
	unsigned long long val;
	unsigned idx;
	unsigned long long rem;
	unsigned frac;

	if (bytes_opt)
		return xasprintf("%llu", bytes);
	if (bytes == 0)
		return xstrdup("0B");

	val = bytes;
	idx = 0;
	rem = 0;
	while (val >= 1024 && idx < sizeof(units) - 2) {
		rem = val % 1024;
		val /= 1024;
		idx++;
	}
	if (idx == 0)
		return xasprintf("%lluB", val);

	frac = (unsigned)((rem * 10 + 512) / 1024);
	if (frac >= 10) {
		val++;
		frac = 0;
	}
	if (frac > 0)
		return xasprintf("%llu.%u%c", val, frac, units[idx]);
	return xasprintf("%llu%c", val, units[idx]);
}

static struct blockdev_info *get_device_by_name(const char *name)
{
	unsigned i;

	for (i = 0; i < G.num_devices; i++) {
		if (strcmp(G.devices[i]->name, name) == 0)
			return G.devices[i];
	}
	return NULL;
}

static struct blockdev_info *add_device(const char *name)
{
	struct blockdev_info *dev;

	dev = get_device_by_name(name);
	if (dev)
		return dev;

	dev = xzalloc(sizeof(*dev));
	dev->name = xstrdup(name);
	dev->kname = xstrdup(name);
	dev->path = xasprintf("/dev/%s", name);

	G.devices = xrealloc_vector(G.devices, 4, G.num_devices);
	G.devices[G.num_devices++] = dev;
	return dev;
}

static void add_child(struct blockdev_info *parent, struct blockdev_info *child)
{
	unsigned i;

	for (i = 0; i < parent->nchildren; i++) {
		if (parent->children[i] == child)
			return;
	}
	parent->children = xrealloc_vector(parent->children, 2, parent->nchildren);
	parent->children[parent->nchildren++] = child;
}

static void add_parent(struct blockdev_info *child, struct blockdev_info *parent)
{
	unsigned i;

	for (i = 0; i < child->nparents; i++) {
		if (child->parents[i] == parent)
			return;
	}
	child->parents = xrealloc_vector(child->parents, 2, child->nparents);
	child->parents[child->nparents++] = parent;
}

static void parse_mountinfo(void)
{
	char *buf;
	char *p;

	buf = xmalloc_open_read_close("/proc/self/mountinfo", NULL);
	if (!buf)
		return;

	p = buf;
	while (*p) {
		char *line_end;
		char *m_majmin;
		char *m_root;
		char *m_mnt;
		char *m_fstype;
		char *m_dev;
		char *sep;

		line_end = strchr(p, '\n');
		if (line_end)
			*line_end = '\0';

		p = skip_whitespace(p);
		p = skip_non_whitespace(p); /* 1: mount ID */
		p = skip_whitespace(p);
		p = skip_non_whitespace(p); /* 2: parent ID */

		p = skip_whitespace(p);
		m_majmin = p;               /* 3: maj:min */
		p = skip_non_whitespace(p);
		if (*p) *p++ = '\0';

		p = skip_whitespace(p);
		m_root = p;                 /* 4: root */
		p = skip_non_whitespace(p);
		if (*p) *p++ = '\0';

		p = skip_whitespace(p);
		m_mnt = p;                  /* 5: mount point */
		p = skip_non_whitespace(p);
		if (*p) *p++ = '\0';

		sep = strstr(p, " - ");
		if (sep) {
			p = sep + 3;
			p = skip_whitespace(p);
			m_fstype = p;           /* fstype */
			p = skip_non_whitespace(p);
			if (*p) *p++ = '\0';

			p = skip_whitespace(p);
			m_dev = p;              /* devname */
			p = skip_non_whitespace(p);
			if (*p) *p++ = '\0';

			unescape_octal(m_mnt);
			unescape_octal(m_root);

			G.mounts = xrealloc_vector(G.mounts, 4, G.num_mounts);
			G.mounts[G.num_mounts].majmin = xstrdup(m_majmin);
			G.mounts[G.num_mounts].root = xstrdup(m_root);
			G.mounts[G.num_mounts].mountpoint = xstrdup(m_mnt);
			G.mounts[G.num_mounts].fstype = xstrdup(m_fstype);
			G.mounts[G.num_mounts].devname = xstrdup(m_dev);
			G.num_mounts++;
		}

		if (!line_end)
			break;
		p = line_end + 1;
	}
	free(buf);
}

static void parse_swaps(void)
{
	char *buf;
	char *p;

	buf = xmalloc_open_read_close("/proc/swaps", NULL);
	if (!buf)
		return;

	p = buf;
	p = strchr(p, '\n'); /* skip header line */
	if (p) {
		p++;
		while (*p) {
			char *line_end;
			char *devname;

			line_end = strchr(p, '\n');
			if (line_end)
				*line_end = '\0';

			p = skip_whitespace(p);
			devname = p;
			p = skip_non_whitespace(p);
			if (*p) *p++ = '\0';

			if (devname[0]) {
				G.mounts = xrealloc_vector(G.mounts, 4, G.num_mounts);
				G.mounts[G.num_mounts].majmin = xstrdup("");
				G.mounts[G.num_mounts].root = xstrdup("");
				G.mounts[G.num_mounts].mountpoint = xstrdup("[SWAP]");
				G.mounts[G.num_mounts].fstype = xstrdup("swap");
				G.mounts[G.num_mounts].devname = xstrdup(devname);
				G.num_mounts++;
			}

			if (!line_end)
				break;
			p = line_end + 1;
		}
	}
	free(buf);
}

static void attach_mounts_and_statvfs(struct blockdev_info *dev)
{
	unsigned m;

	for (m = 0; m < G.num_mounts; m++) {
		struct mount_entry *mnt;
		int match;

		mnt = &G.mounts[m];
		match = 0;
		if (dev->majmin && dev->majmin[0] && mnt->majmin && strcmp(dev->majmin, mnt->majmin) == 0) {
			match = 1;
		} else if (mnt->devname) {
			if (strcmp(mnt->devname, dev->path) == 0
			 || strcmp(mnt->devname, dev->name) == 0
			 || (is_prefixed_with(mnt->devname, "/dev/") && strcmp(mnt->devname + 5, dev->name) == 0)) {
				match = 1;
			}
		}

		if (match) {
			dev->mountpoints = xrealloc_vector(dev->mountpoints, 2, dev->nmounts);
			dev->fsroots = xrealloc_vector(dev->fsroots, 2, dev->nmounts);
			dev->mountpoints[dev->nmounts] = xstrdup(mnt->mountpoint);
			dev->fsroots[dev->nmounts] = xstrdup(mnt->root);
			dev->nmounts++;

			if (!dev->fstype && mnt->fstype && mnt->fstype[0])
				dev->fstype = xstrdup(mnt->fstype);
		}
	}

	if (dev->nmounts > 0 && strcmp(dev->mountpoints[0], "[SWAP]") != 0) {
		struct statvfs sv;
		if (statvfs(dev->mountpoints[0], &sv) == 0) {
			unsigned pct;

			dev->has_statvfs = 1;
			dev->fssize = (unsigned long long)sv.f_blocks * sv.f_frsize;
			dev->fsavail = (unsigned long long)sv.f_bavail * sv.f_frsize;
			dev->fsused = (sv.f_blocks >= sv.f_bfree)
				? ((unsigned long long)(sv.f_blocks - sv.f_bfree) * sv.f_frsize) : 0;
			if (sv.f_blocks > 0) {
				pct = (unsigned)(((unsigned long long)(sv.f_blocks - sv.f_bfree) * 100
					+ sv.f_blocks / 2) / sv.f_blocks);
				dev->fsuse_pct = xasprintf("%u%%", pct);
			}
		}
	}
}

static void scan_disk_by_dir(const char *dirpath, int col_target)
{
	DIR *dir;
	struct dirent *entry;

	dir = opendir(dirpath);
	if (!dir)
		return;

	while ((entry = readdir(dir)) != NULL) {
		char *linkpath;
		char *target;
		struct blockdev_info *dev;

		if (DOT_OR_DOTDOT(entry->d_name))
			continue;

		linkpath = concat_path_file(dirpath, entry->d_name);
		target = xmalloc_readlink(linkpath);
		free(linkpath);
		if (!target)
			continue;

		dev = get_device_by_name(bb_basename(target));
		if (dev) {
			if (col_target == COL_UUID && !dev->uuid)
				dev->uuid = xstrdup(entry->d_name);
			else if (col_target == COL_LABEL && !dev->label)
				dev->label = xstrdup(entry->d_name);
			else if (col_target == COL_PARTUUID && !dev->partuuid)
				dev->partuuid = xstrdup(entry->d_name);
			else if (col_target == COL_PARTLABEL && !dev->partlabel)
				dev->partlabel = xstrdup(entry->d_name);
		}
		free(target);
	}
	closedir(dir);
}

static void scan_udev_data(struct blockdev_info *dev)
{
	char *udev_path;
	char *buf;
	char *p;

	if (!dev->majmin || !dev->majmin[0])
		return;

	udev_path = xasprintf("/run/udev/data/b%s", dev->majmin);
	buf = xmalloc_open_read_close(udev_path, NULL);
	free(udev_path);
	if (!buf)
		return;

	p = buf;
	while (*p) {
		char *line_end;

		line_end = strchr(p, '\n');
		if (line_end)
			*line_end = '\0';

		if (is_prefixed_with(p, "E:ID_PART_TABLE_TYPE=")) {
			if (!dev->pttype)
				dev->pttype = xstrdup(p + 21);
		} else if (is_prefixed_with(p, "E:ID_PART_ENTRY_TYPE=")) {
			if (!dev->parttype)
				dev->parttype = xstrdup(p + 21);
		} else if (is_prefixed_with(p, "E:ID_PART_ENTRY_NAME=")) {
			if (!dev->partlabel)
				dev->partlabel = xstrdup(p + 21);
		} else if (is_prefixed_with(p, "E:ID_PART_ENTRY_UUID=")) {
			if (!dev->partuuid)
				dev->partuuid = xstrdup(p + 21);
		} else if (is_prefixed_with(p, "E:ID_FS_VERSION=")) {
			if (!dev->fsver)
				dev->fsver = xstrdup(p + 16);
		} else if (is_prefixed_with(p, "E:ID_FS_LABEL=")) {
			if (!dev->label)
				dev->label = xstrdup(p + 14);
		} else if (is_prefixed_with(p, "E:ID_FS_UUID=")) {
			if (!dev->uuid)
				dev->uuid = xstrdup(p + 13);
		} else if (is_prefixed_with(p, "E:ID_FS_TYPE=")) {
			if (!dev->fstype)
				dev->fstype = xstrdup(p + 13);
		} else if (is_prefixed_with(p, "E:ID_MODEL=")) {
			if (!dev->model)
				dev->model = xstrdup(p + 11);
		} else if (is_prefixed_with(p, "E:ID_SERIAL_SHORT=")) {
			if (!dev->serial)
				dev->serial = xstrdup(p + 18);
		}

		if (!line_end)
			break;
		p = line_end + 1;
	}
	free(buf);
}

static void scan_uevent(struct blockdev_info *dev, const char *sys_path)
{
	char *buf;
	char *p;

	buf = read_str(sys_path, "uevent");
	if (!buf)
		return;
	p = buf;
	while (*p) {
		char *line_end;

		line_end = strchr(p, '\n');
		if (line_end)
			*line_end = '\0';

		if (is_prefixed_with(p, "PARTUUID=")) {
			if (!dev->partuuid)
				dev->partuuid = xstrdup(p + 9);
		} else if (is_prefixed_with(p, "PARTLABEL=")) {
			if (!dev->partlabel)
				dev->partlabel = xstrdup(p + 10);
		} else if (is_prefixed_with(p, "PARTTYPE=")) {
			if (!dev->parttype)
				dev->parttype = xstrdup(p + 9);
		}

		if (!line_end)
			break;
		p = line_end + 1;
	}
	free(buf);
}

static void load_device_info(struct blockdev_info *dev, struct blockdev_info *parent)
{
	char *sys_path;
	char *queue_path;
	unsigned long long sectors;
	struct stat st;

	sys_path = xasprintf("/sys/class/block/%s", dev->name);

	/* Major:Minor */
	dev->majmin = read_str(sys_path, "dev");
	if (dev->majmin) {
		unsigned maj, min;
		if (sscanf(dev->majmin, "%u:%u", &maj, &min) == 2) {
			dev->major = maj;
			dev->minor = min;
		}
	}

	/* Size */
	sectors = read_ull(sys_path, "size");
	if (sectors != ~0ULL)
		dev->size = sectors * 512;

	/* Read-only */
	dev->ro = read_str(sys_path, "ro");
	if (!dev->ro)
		dev->ro = xstrdup("0");

	/* Removable */
	dev->rm = read_str(sys_path, "removable");
	if (!dev->rm && parent)
		dev->rm = xstrdup(parent->rm ? parent->rm : "0");
	if (!dev->rm)
		dev->rm = xstrdup("0");

	/* Hotplug */
	dev->hotplug = (strcmp(dev->rm, "1") == 0) ? xstrdup("1") : xstrdup("0");

	/* Type */
	if (access(concat_path_file(sys_path, "partition"), F_OK) == 0) {
		dev->type = xstrdup("part");
	} else if (is_prefixed_with(dev->name, "loop")) {
		dev->type = xstrdup("loop");
	} else if (is_prefixed_with(dev->name, "dm-") || access(concat_path_file(sys_path, "dm"), F_OK) == 0) {
		char *dm_uuid;
		dm_uuid = read_str(sys_path, "dm/uuid");
		if (dm_uuid && is_prefixed_with(dm_uuid, "LVM-"))
			dev->type = xstrdup("lvm");
		else if (dm_uuid && is_prefixed_with(dm_uuid, "CRYPT-"))
			dev->type = xstrdup("crypt");
		else
			dev->type = xstrdup("lvm");
		free(dm_uuid);
	} else if (is_prefixed_with(dev->name, "sr") || read_ull(sys_path, "device/type") == 5) {
		dev->type = xstrdup("rom");
	} else {
		dev->type = xstrdup("disk");
	}

	/* Model, Serial, State */
	dev->model = read_str(sys_path, "device/model");
	if (!dev->model && parent)
		dev->model = xstrdup(parent->model ? parent->model : "");

	dev->serial = read_str(sys_path, "device/serial");
	if (!dev->serial)
		dev->serial = read_str(sys_path, "serial");
	if (!dev->serial && parent)
		dev->serial = xstrdup(parent->serial ? parent->serial : "");

	dev->state = read_str(sys_path, "device/state");

	/* Alignment */
	dev->alignment = read_str(sys_path, "alignment_offset");
	dev->discaln = read_str(sys_path, "discard_alignment");

	/* Queue attributes */
	queue_path = concat_path_file(sys_path, "queue");
	if (access(queue_path, F_OK) != 0 && parent) {
		free(queue_path);
		queue_path = xasprintf("/sys/class/block/%s/queue", parent->name);
	}

	if (access(queue_path, F_OK) == 0) {
		dev->rota = read_str(queue_path, "rotational");
		dev->sched = read_scheduler(queue_path);
		dev->rq_size = read_str(queue_path, "nr_requests");
		dev->ra = read_str(queue_path, "read_ahead_kb");
		dev->phy_sec = read_str(queue_path, "physical_block_size");
		dev->log_sec = read_str(queue_path, "logical_block_size");
		dev->min_io = read_str(queue_path, "minimum_io_size");
		dev->opt_io = read_str(queue_path, "optimal_io_size");
		dev->discgran = read_str(queue_path, "discard_granularity");
		dev->discmax = read_str(queue_path, "discard_max_bytes");
		dev->disczero = read_str(queue_path, "discard_zeroes_data");
		dev->wsame = read_str(queue_path, "write_same_max_bytes");
		dev->zoned = read_str(queue_path, "zoned");
		dev->dax = read_str(queue_path, "dax");
	}
	free(queue_path);

	/* Stat /dev/node for owner, group, mode */
	if (stat(dev->path, &st) == 0) {
		char mode_buf[12];
		dev->mode = xstrdup(bb_mode_string(mode_buf, st.st_mode));
		dev->owner = xstrdup(get_cached_username(st.st_uid));
		dev->group = xstrdup(get_cached_groupname(st.st_gid));
	}

	/* Uevent */
	scan_uevent(dev, sys_path);

	/* Udev DB */
	scan_udev_data(dev);

	/* Mount info & statvfs */
	attach_mounts_and_statvfs(dev);

	free(sys_path);
}

static void scan_holders(struct blockdev_info *dev)
{
	DIR *dir;
	struct dirent *entry;
	char *holders_path;

	holders_path = xasprintf("/sys/class/block/%s/holders", dev->name);
	dir = opendir(holders_path);
	free(holders_path);
	if (!dir)
		return;

	while ((entry = readdir(dir)) != NULL) {
		struct blockdev_info *holder_dev;

		if (DOT_OR_DOTDOT(entry->d_name))
			continue;

		holder_dev = add_device(entry->d_name);
		add_child(dev, holder_dev);
		add_parent(holder_dev, dev);
		holder_dev->is_child = 1;
		load_device_info(holder_dev, dev);
		scan_holders(holder_dev);
	}
	closedir(dir);
}

static void precompute_columns(struct blockdev_info *dev, int bytes_opt, int paths_opt)
{
	dev->col_values[COL_NAME]        = xstrdup(paths_opt ? dev->path : dev->name);
	dev->col_values[COL_KNAME]       = xstrdup(dev->kname);
	dev->col_values[COL_PATH]        = xstrdup(dev->path);
	dev->col_values[COL_MAJ_MIN]     = xstrdup(dev->majmin ? dev->majmin : "");
	dev->col_values[COL_FSAVAIL]     = dev->has_statvfs ? format_size(dev->fsavail, bytes_opt) : xstrdup("");
	dev->col_values[COL_FSROOTS]     = (dev->nmounts > 0 && dev->fsroots) ? xstrdup(dev->fsroots[0]) : xstrdup("");
	dev->col_values[COL_FSSIZE]      = dev->has_statvfs ? format_size(dev->fssize, bytes_opt) : xstrdup("");
	dev->col_values[COL_FSTYPE]      = xstrdup(dev->fstype ? dev->fstype : "");
	dev->col_values[COL_FSUSED]      = dev->has_statvfs ? format_size(dev->fsused, bytes_opt) : xstrdup("");
	dev->col_values[COL_FSUSE_PCT]   = xstrdup(dev->fsuse_pct ? dev->fsuse_pct : "");
	dev->col_values[COL_FSVER]       = xstrdup(dev->fsver ? dev->fsver : "");
	dev->col_values[COL_MOUNTPOINT]  = (dev->nmounts > 0) ? xstrdup(dev->mountpoints[0]) : xstrdup("");
	dev->col_values[COL_MOUNTPOINTS] = (dev->nmounts > 0) ? xstrdup(dev->mountpoints[0]) : xstrdup("");
	dev->col_values[COL_LABEL]       = xstrdup(dev->label ? dev->label : "");
	dev->col_values[COL_UUID]        = xstrdup(dev->uuid ? dev->uuid : "");
	dev->col_values[COL_PTTYPE]      = xstrdup(dev->pttype ? dev->pttype : "");
	dev->col_values[COL_PARTTYPE]    = xstrdup(dev->parttype ? dev->parttype : "");
	dev->col_values[COL_PARTLABEL]   = xstrdup(dev->partlabel ? dev->partlabel : "");
	dev->col_values[COL_PARTUUID]    = xstrdup(dev->partuuid ? dev->partuuid : "");
	dev->col_values[COL_RA]          = xstrdup(dev->ra ? dev->ra : "");
	dev->col_values[COL_RO]          = xstrdup(dev->ro ? dev->ro : "0");
	dev->col_values[COL_RM]          = xstrdup(dev->rm ? dev->rm : "0");
	dev->col_values[COL_HOTPLUG]     = xstrdup(dev->hotplug ? dev->hotplug : "0");
	dev->col_values[COL_MODEL]       = xstrdup(dev->model ? dev->model : "");
	dev->col_values[COL_SERIAL]      = xstrdup(dev->serial ? dev->serial : "");
	dev->col_values[COL_SIZE]        = format_size(dev->size, bytes_opt);
	dev->col_values[COL_STATE]       = xstrdup(dev->state ? dev->state : "");
	dev->col_values[COL_OWNER]       = xstrdup(dev->owner ? dev->owner : "");
	dev->col_values[COL_GROUP]       = xstrdup(dev->group ? dev->group : "");
	dev->col_values[COL_MODE]        = xstrdup(dev->mode ? dev->mode : "");
	dev->col_values[COL_ALIGNMENT]   = xstrdup(dev->alignment ? dev->alignment : "0");
	dev->col_values[COL_MIN_IO]      = xstrdup(dev->min_io ? dev->min_io : "");
	dev->col_values[COL_OPT_IO]      = xstrdup(dev->opt_io ? dev->opt_io : "");
	dev->col_values[COL_PHY_SEC]     = xstrdup(dev->phy_sec ? dev->phy_sec : "");
	dev->col_values[COL_LOG_SEC]     = xstrdup(dev->log_sec ? dev->log_sec : "");
	dev->col_values[COL_ROTA]        = xstrdup(dev->rota ? dev->rota : "0");
	dev->col_values[COL_SCHED]       = xstrdup(dev->sched ? dev->sched : "");
	dev->col_values[COL_RQ_SIZE]     = xstrdup(dev->rq_size ? dev->rq_size : "");
	dev->col_values[COL_TYPE]        = xstrdup(dev->type ? dev->type : "");
	dev->col_values[COL_DISCALN]     = xstrdup(dev->discaln ? dev->discaln : "0");
	dev->col_values[COL_DISCGRAN]    = dev->discgran
		? (bytes_opt ? xstrdup(dev->discgran) : format_size(bb_strtoull(dev->discgran, NULL, 10), 0))
		: xstrdup("");
	dev->col_values[COL_DISCMAX]     = dev->discmax
		? (bytes_opt ? xstrdup(dev->discmax) : format_size(bb_strtoull(dev->discmax, NULL, 10), 0))
		: xstrdup("");
	dev->col_values[COL_DISCZERO]    = xstrdup(dev->disczero ? dev->disczero : "0");
	dev->col_values[COL_WSAME]       = dev->wsame
		? (bytes_opt ? xstrdup(dev->wsame) : format_size(bb_strtoull(dev->wsame, NULL, 10), 0))
		: xstrdup("");
	dev->col_values[COL_ZONED]       = xstrdup(dev->zoned ? dev->zoned : "");
	dev->col_values[COL_DAX]         = xstrdup(dev->dax ? dev->dax : "0");
}

static int col_name_matches(const char *def_name, const char *input_name)
{
	while (*def_name && *input_name) {
		char c1 = *def_name;
		char c2 = *input_name;

		if (c1 == '-' || c1 == '_') {
			if (c2 == '-' || c2 == '_') {
				def_name++;
				input_name++;
				continue;
			}
			def_name++;
			continue;
		}
		if (c2 == '-' || c2 == '_') {
			input_name++;
			continue;
		}
		if (tolower((unsigned char)c1) != tolower((unsigned char)c2))
			return 0;
		def_name++;
		input_name++;
	}
	while (*def_name == '-' || *def_name == '_') def_name++;
	while (*input_name == '-' || *input_name == '_') input_name++;
	return *def_name == '\0' && *input_name == '\0';
}

static int find_column(const char *name)
{
	int i;

	for (i = 0; i < NUM_COLUMNS; i++) {
		if (strcasecmp(columns_def[i].name, name) == 0)
			return i;
		if (columns_def[i].alias && strcasecmp(columns_def[i].alias, name) == 0)
			return i;
		if (col_name_matches(columns_def[i].name, name))
			return i;
		if (columns_def[i].alias && col_name_matches(columns_def[i].alias, name))
			return i;
	}
	return -1;
}

static void add_selected_column(int col_id)
{
	G.columns = xrealloc_vector(G.columns, 2, G.num_columns);
	G.columns[G.num_columns++] = col_id;
}

static void parse_column_list(const char *list, int append)
{
	char *dup_list;
	char *token;
	char *p;

	if (!append)
		G.num_columns = 0;

	dup_list = xstrdup(list);
	p = dup_list;
	while ((token = strsep(&p, ",")) != NULL) {
		int col;

		if (!token[0])
			continue;
		col = find_column(token);
		if (col < 0)
			bb_error_msg_and_die("unknown column: %s", token);
		add_selected_column(col);
	}
	free(dup_list);
}

static int compare_by_name(const void *a, const void *b)
{
	const struct blockdev_info *da = *(const struct blockdev_info * const *)a;
	const struct blockdev_info *db = *(const struct blockdev_info * const *)b;
	return strcmp(da->name, db->name);
}

static int compare_by_col(const void *a, const void *b)
{
	const struct blockdev_info *da = *(const struct blockdev_info * const *)a;
	const struct blockdev_info *db = *(const struct blockdev_info * const *)b;
	int col = G.sort_col;

	if (col == COL_MAJ_MIN) {
		if (da->major != db->major)
			return (int)da->major - (int)db->major;
		if (da->minor != db->minor)
			return (int)da->minor - (int)db->minor;
	} else if (col == COL_SIZE) {
		if (da->size != db->size)
			return da->size < db->size ? -1 : 1;
	} else if (col == COL_FSAVAIL) {
		if (da->fsavail != db->fsavail)
			return da->fsavail < db->fsavail ? -1 : 1;
	} else if (col == COL_FSSIZE) {
		if (da->fssize != db->fssize)
			return da->fssize < db->fssize ? -1 : 1;
	} else if (col == COL_FSUSED) {
		if (da->fsused != db->fsused)
			return da->fsused < db->fsused ? -1 : 1;
	} else if (columns_def[col].is_numeric) {
		unsigned long long va, vb;
		va = da->col_values[col] ? bb_strtoull(da->col_values[col], NULL, 10) : 0;
		vb = db->col_values[col] ? bb_strtoull(db->col_values[col], NULL, 10) : 0;
		if (va != vb)
			return va < vb ? -1 : 1;
	} else {
		const char *sa = da->col_values[col] ? da->col_values[col] : "";
		const char *sb = db->col_values[col] ? db->col_values[col] : "";
		int cmp = strcmp(sa, sb);
		if (cmp != 0)
			return cmp;
	}
	return strcmp(da->name, db->name);
}

static int is_root_device(struct blockdev_info *dev)
{
	if (G.opt & OPT_s)
		return (dev->nchildren == 0);
	return (dev->is_child == 0);
}

static void compute_depths(struct blockdev_info *dev, int depth)
{
	unsigned i;
	struct blockdev_info **children;
	unsigned count;

	dev->depth = depth;
	if (depth >= 30 || (G.opt & OPT_d))
		return;

	if (G.opt & OPT_s) {
		children = dev->parents;
		count = dev->nparents;
	} else {
		children = dev->children;
		count = dev->nchildren;
	}

	for (i = 0; i < count; i++) {
		compute_depths(children[i], depth + 1);
	}
}

static void print_pairs_value(const char *s)
{
	putchar('"');
	if (s) {
		while (*s) {
			if (*s == '"')
				fputs("\\\"", stdout);
			else if (*s == '\\')
				fputs("\\\\", stdout);
			else if (*s == '\n')
				fputs("\\x0a", stdout);
			else if (*s == '\r')
				fputs("\\x0d", stdout);
			else if (*s == '\t')
				fputs("\\x09", stdout);
			else if ((unsigned char)*s < 32 || (unsigned char)*s >= 127)
				printf("\\x%02x", (unsigned char)*s);
			else
				putchar(*s);
			s++;
		}
	}
	putchar('"');
}

static void print_raw_value(const char *s)
{
	if (s) {
		while (*s) {
			if (*s == ' ')
				fputs("\\x20", stdout);
			else if (*s == '\n')
				fputs("\\x0a", stdout);
			else if (*s == '\r')
				fputs("\\x0d", stdout);
			else if (*s == '\t')
				fputs("\\x09", stdout);
			else if (*s == '\\')
				fputs("\\\\", stdout);
			else if ((unsigned char)*s < 32 || (unsigned char)*s >= 127)
				printf("\\x%02x", (unsigned char)*s);
			else
				putchar(*s);
			s++;
		}
	}
}

static void print_device_pairs(struct blockdev_info *dev)
{
	unsigned c;

	for (c = 0; c < G.num_columns; c++) {
		int col_id = G.columns[c];

		if (c > 0)
			putchar(' ');
		printf("%s=", columns_def[col_id].name);

		if (col_id == COL_MOUNTPOINTS) {
			unsigned m;
			char *joined = NULL;
			for (m = 0; m < dev->nmounts; m++) {
				xasprintf_inplace(joined, "%s%s%s", joined ? joined : "", (m > 0) ? "\n" : "", dev->mountpoints[m]);
			}
			print_pairs_value(joined ? joined : "");
			free(joined);
		} else {
			print_pairs_value(dev->col_values[col_id]);
		}
	}
	putchar('\n');
}

static void print_device_raw(struct blockdev_info *dev)
{
	unsigned c;

	for (c = 0; c < G.num_columns; c++) {
		int col_id = G.columns[c];

		if (c > 0)
			putchar(' ');

		if (col_id == COL_MOUNTPOINTS) {
			unsigned m;
			char *joined = NULL;
			for (m = 0; m < dev->nmounts; m++) {
				xasprintf_inplace(joined, "%s%s%s", joined ? joined : "", (m > 0) ? "\n" : "", dev->mountpoints[m]);
			}
			print_raw_value(joined ? joined : "");
			free(joined);
		} else {
			print_raw_value(dev->col_values[col_id]);
		}
	}
	putchar('\n');
}

static void print_json_string(const char *s)
{
	putchar('"');
	if (s) {
		while (*s) {
			if (*s == '"')
				fputs("\\\"", stdout);
			else if (*s == '\\')
				fputs("\\\\", stdout);
			else if (*s == '\b')
				fputs("\\b", stdout);
			else if (*s == '\f')
				fputs("\\f", stdout);
			else if (*s == '\n')
				fputs("\\n", stdout);
			else if (*s == '\r')
				fputs("\\r", stdout);
			else if (*s == '\t')
				fputs("\\t", stdout);
			else if ((unsigned char)*s < 32)
				printf("\\u%04x", (unsigned char)*s);
			else
				putchar(*s);
			s++;
		}
	}
	putchar('"');
}

static void print_json_indent(int level)
{
	int i;
	for (i = 0; i < level; i++)
		fputs("   ", stdout);
}

static void print_json_device(struct blockdev_info *dev, int level, int is_tree)
{
	unsigned c;
	int first_field;
	struct blockdev_info **children;
	unsigned count;

	print_json_indent(level);
	printf("{\n");

	first_field = 1;
	for (c = 0; c < G.num_columns; c++) {
		int col_id;
		const char *v;

		col_id = G.columns[c];
		v = dev->col_values[col_id];

		if (!first_field)
			printf(",\n");
		first_field = 0;

		print_json_indent(level + 1);
		printf("\"%s\": ", columns_def[col_id].json_name);

		if (col_id == COL_MOUNTPOINTS) {
			unsigned m;
			printf("[");
			if (dev->nmounts > 0) {
				printf("\n");
				for (m = 0; m < dev->nmounts; m++) {
					if (m > 0)
						printf(",\n");
					print_json_indent(level + 2);
					print_json_string(dev->mountpoints[m]);
				}
				printf("\n");
				print_json_indent(level + 1);
			}
			printf("]");
		} else if (columns_def[col_id].is_bool) {
			printf("%s", (v && v[0] == '1') ? "true" : "false");
		} else if ((G.opt & OPT_b) && columns_def[col_id].is_byte_size) {
			unsigned long long num;
			num = 0;
			if (col_id == COL_SIZE)
				num = dev->size;
			else if (col_id == COL_FSAVAIL)
				num = dev->fsavail;
			else if (col_id == COL_FSSIZE)
				num = dev->fssize;
			else if (col_id == COL_FSUSED)
				num = dev->fsused;
			else if (v && v[0])
				num = bb_strtoull(v, NULL, 10);
			printf("%llu", num);
		} else {
			if (v && v[0])
				print_json_string(v);
			else
				printf("null");
		}
	}

	if (is_tree && !(G.opt & OPT_d)) {
		if (G.opt & OPT_s) {
			children = dev->parents;
			count = dev->nparents;
		} else {
			children = dev->children;
			count = dev->nchildren;
		}

		if (count > 0 && level < 30) {
			unsigned i;
			printf(",\n");
			print_json_indent(level + 1);
			printf("\"children\": [");
			for (i = 0; i < count; i++) {
				if (i > 0)
					printf(",");
				printf("\n");
				print_json_device(children[i], level + 2, 1);
			}
			printf("\n");
			print_json_indent(level + 1);
			printf("]");
		}
	}

	printf("\n");
	print_json_indent(level);
	printf("}");
}

static void print_table_row(struct blockdev_info *dev, int depth, int is_last,
	const int *last_child_at_depth, const int *col_widths)
{
	unsigned c;
	int vis_len;
	int pad;
	int has_multiline;

	for (c = 0; c < G.num_columns; c++) {
		int col_id = G.columns[c];
		int is_last_col = (c == G.num_columns - 1);
		const char *val = dev->col_values[col_id] ? dev->col_values[col_id] : "";

		if (col_id == COL_NAME && !(G.opt & OPT_l)) {
			int k;
			/* Print tree prefix */
			for (k = 0; k < depth - 1; k++) {
				if (last_child_at_depth[k])
					fputs("  ", stdout);
				else
					fputs((G.opt & OPT_i) ? "| " : "│ ", stdout);
			}
			if (depth > 0) {
				if (is_last)
					fputs((G.opt & OPT_i) ? "`-" : "└─", stdout);
				else
					fputs((G.opt & OPT_i) ? "|-" : "├─", stdout);
			}
			fputs(val, stdout);

			vis_len = 2 * depth + strlen(val);
			pad = col_widths[c] - vis_len;
			if (!is_last_col) {
				while (pad-- > 0)
					putchar(' ');
				putchar(' ');
			}
		} else {
			if (is_last_col) {
				printf("%s", val);
			} else if (columns_def[col_id].is_right) {
				printf("%*s ", col_widths[c], val);
			} else {
				printf("%-*s ", col_widths[c], val);
			}
		}
	}
	putchar('\n');

	/* Continuation lines for multiple mountpoints */
	has_multiline = 0;
	for (c = 0; c < G.num_columns; c++) {
		if (G.columns[c] == COL_MOUNTPOINTS || G.columns[c] == COL_FSROOTS)
			has_multiline = 1;
	}
	if (has_multiline && dev->nmounts > 1) {
		unsigned m;
		for (m = 1; m < dev->nmounts; m++) {
			for (c = 0; c < G.num_columns; c++) {
				int col_id = G.columns[c];
				int is_last_col = (c == G.num_columns - 1);

				if (col_id == COL_NAME && !(G.opt & OPT_l)) {
					int k;
					for (k = 0; k < depth - 1; k++) {
						if (last_child_at_depth[k])
							fputs("  ", stdout);
						else
							fputs((G.opt & OPT_i) ? "| " : "│ ", stdout);
					}
					if (depth > 0) {
						if (dev->nchildren > 0 || !is_last)
							fputs((G.opt & OPT_i) ? "| " : "│ ", stdout);
						else
							fputs("  ", stdout);
					}
					pad = col_widths[c] - 2 * depth;
					while (pad-- > 0)
						putchar(' ');
					putchar(' ');
				} else if (col_id == COL_MOUNTPOINTS) {
					const char *mnt = dev->mountpoints[m];
					if (is_last_col)
						printf("%s", mnt);
					else
						printf("%-*s ", col_widths[c], mnt);
				} else if (col_id == COL_FSROOTS && dev->fsroots && dev->fsroots[m]) {
					const char *root = dev->fsroots[m];
					if (is_last_col)
						printf("%s", root);
					else
						printf("%-*s ", col_widths[c], root);
				} else {
					if (!is_last_col)
						printf("%*s ", col_widths[c], "");
				}
			}
			putchar('\n');
		}
	}
}

static void print_table_tree(struct blockdev_info *dev, int depth, int is_last,
	int *last_child_at_depth, const int *col_widths)
{
	unsigned i;
	struct blockdev_info **children;
	unsigned count;

	if (depth >= 30)
		return;

	last_child_at_depth[depth] = is_last;
	print_table_row(dev, depth, is_last, last_child_at_depth, col_widths);

	if (G.opt & OPT_d)
		return;

	if (G.opt & OPT_s) {
		children = dev->parents;
		count = dev->nparents;
	} else {
		children = dev->children;
		count = dev->nchildren;
	}

	for (i = 0; i < count; i++) {
		print_table_tree(children[i], depth + 1, (i == count - 1),
			last_child_at_depth, col_widths);
	}
}

int lsblk_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int lsblk_main(int argc UNUSED_PARAM, char **argv)
{
	char *opt_o = NULL;
	char *opt_x = NULL;
	int has_cli_args;
	unsigned i;
	int *col_widths;

	INIT_G();

	G.opt = getopt32long(argv,
		"abdfiJlmno:OpPrstx:z",
		IF_LONG_OPTS(lsblk_longopts,)
		&opt_o, &opt_x
	);
	argv += optind;
	has_cli_args = (*argv != NULL);

	/* Set up columns */
	if (G.opt & OPT_o) {
		if (opt_o[0] == '+') {
			/* Append to default set */
			add_selected_column(COL_NAME);
			add_selected_column(COL_MAJ_MIN);
			add_selected_column(COL_RM);
			add_selected_column(COL_SIZE);
			add_selected_column(COL_RO);
			add_selected_column(COL_TYPE);
			add_selected_column(COL_MOUNTPOINTS);
			parse_column_list(opt_o + 1, 1);
		} else {
			parse_column_list(opt_o, 0);
		}
	} else if (G.opt & OPT_O) {
		int c;
		for (c = 0; c < NUM_COLUMNS; c++)
			add_selected_column(c);
	} else if (G.opt & OPT_f) {
		add_selected_column(COL_NAME);
		add_selected_column(COL_FSTYPE);
		add_selected_column(COL_FSVER);
		add_selected_column(COL_LABEL);
		add_selected_column(COL_UUID);
		add_selected_column(COL_FSAVAIL);
		add_selected_column(COL_FSUSE_PCT);
		add_selected_column(COL_MOUNTPOINTS);
	} else if (G.opt & OPT_m) {
		add_selected_column(COL_NAME);
		add_selected_column(COL_SIZE);
		add_selected_column(COL_OWNER);
		add_selected_column(COL_GROUP);
		add_selected_column(COL_MODE);
	} else if (G.opt & OPT_t) {
		add_selected_column(COL_NAME);
		add_selected_column(COL_ALIGNMENT);
		add_selected_column(COL_MIN_IO);
		add_selected_column(COL_OPT_IO);
		add_selected_column(COL_PHY_SEC);
		add_selected_column(COL_LOG_SEC);
		add_selected_column(COL_ROTA);
		add_selected_column(COL_SCHED);
		add_selected_column(COL_RQ_SIZE);
		add_selected_column(COL_RA);
		add_selected_column(COL_WSAME);
	} else if (G.opt & OPT_z) {
		add_selected_column(COL_NAME);
		add_selected_column(COL_ZONED);
	} else {
		/* Default columns */
		add_selected_column(COL_NAME);
		add_selected_column(COL_MAJ_MIN);
		add_selected_column(COL_RM);
		add_selected_column(COL_SIZE);
		add_selected_column(COL_RO);
		add_selected_column(COL_TYPE);
		add_selected_column(COL_MOUNTPOINTS);
	}

	if (G.opt & OPT_x) {
		G.sort_col = find_column(opt_x);
		if (G.sort_col < 0)
			bb_error_msg_and_die("unknown column: %s", opt_x);
		G.opt |= OPT_l; /* sorting implies list format */
	}

	/* Read mount info */
	parse_mountinfo();
	parse_swaps();

	/* Discover block devices */
	if (has_cli_args) {
		while (*argv) {
			const char *arg = *argv++;
			struct stat st;
			char *majmin;
			char *sys_dev_path;
			char *target;

			if (stat(arg, &st) != 0) {
				bb_perror_msg("%s", arg);
				G.exitcode |= 32;
				continue;
			}
			if (!S_ISBLK(st.st_mode)) {
				bb_error_msg("%s: not a block device", arg);
				G.exitcode |= 64;
				continue;
			}

			majmin = xasprintf("%u:%u", (unsigned)major(st.st_rdev), (unsigned)minor(st.st_rdev));
			sys_dev_path = concat_path_file("/sys/dev/block", majmin);
			target = xmalloc_readlink(sys_dev_path);
			free(sys_dev_path);
			free(majmin);

			if (target) {
				char *devname;
				struct blockdev_info *dev;
				char *part_check;

				devname = bb_basename(target);
				dev = add_device(devname);
				dev->is_cli_arg = 1;
				load_device_info(dev, NULL);

				/* If this is a disk, scan partitions and holders */
				part_check = xasprintf("/sys/class/block/%s/partition", devname);
				if (access(part_check, F_OK) != 0) {
					/* Disk: scan partitions */
					DIR *pdir;
					struct dirent *pentry;
					char *disk_path;

					disk_path = xasprintf("/sys/block/%s", devname);
					pdir = opendir(disk_path);
					if (pdir) {
						while ((pentry = readdir(pdir)) != NULL) {
							char *pcheck;
							if (DOT_OR_DOTDOT(pentry->d_name))
								continue;
							pcheck = xasprintf("/sys/block/%s/%s/partition", devname, pentry->d_name);
							if (access(pcheck, F_OK) == 0) {
								struct blockdev_info *part_dev;
								part_dev = add_device(pentry->d_name);
								add_child(dev, part_dev);
								add_parent(part_dev, dev);
								part_dev->is_child = 1;
								load_device_info(part_dev, dev);
								scan_holders(part_dev);
							}
							free(pcheck);
						}
						closedir(pdir);
					}
					free(disk_path);
				}
				free(part_check);

				scan_holders(dev);
				free(target);
			}
		}
	} else {
		DIR *dir;
		struct dirent *entry;

		dir = xopendir("/sys/block");
		while ((entry = readdir(dir)) != NULL) {
			const char *diskname = entry->d_name;
			char *disk_path;
			char *hidden;
			struct blockdev_info *disk_dev;
			DIR *pdir;
			struct dirent *pentry;

			if (DOT_OR_DOTDOT(diskname))
				continue;

			disk_path = xasprintf("/sys/block/%s", diskname);

			if (!(G.opt & OPT_a)) {
				/* Skip hidden devices */
				hidden = read_str(disk_path, "hidden");
				if (hidden && strcmp(hidden, "1") == 0) {
					free(hidden);
					free(disk_path);
					continue;
				}
				free(hidden);

				/* Skip RAM disks */
				if (is_prefixed_with(diskname, "ram")) {
					free(disk_path);
					continue;
				}

				/* Skip empty loop devices */
				if (is_prefixed_with(diskname, "loop")) {
					unsigned long long sz = read_ull(disk_path, "size");
					if (sz == 0 || sz == ~0ULL) {
						free(disk_path);
						continue;
					}
				}
			}

			disk_dev = add_device(diskname);
			load_device_info(disk_dev, NULL);

			/* Scan partitions */
			pdir = opendir(disk_path);
			if (pdir) {
				while ((pentry = readdir(pdir)) != NULL) {
					char *part_path;
					if (DOT_OR_DOTDOT(pentry->d_name))
						continue;
					part_path = xasprintf("/sys/block/%s/%s/partition", diskname, pentry->d_name);
					if (access(part_path, F_OK) == 0) {
						struct blockdev_info *part_dev;
						part_dev = add_device(pentry->d_name);
						add_child(disk_dev, part_dev);
						add_parent(part_dev, disk_dev);
						part_dev->is_child = 1;
						load_device_info(part_dev, disk_dev);
						scan_holders(part_dev);
					}
					free(part_path);
				}
				closedir(pdir);
			}

			scan_holders(disk_dev);
			free(disk_path);
		}
		closedir(dir);
	}

	if (G.num_devices == 0) {
		if (has_cli_args)
			return G.exitcode ? G.exitcode : 32;
		return 0;
	}

	/* Scan /dev/disk symlinks */
	scan_disk_by_dir("/dev/disk/by-uuid", COL_UUID);
	scan_disk_by_dir("/dev/disk/by-label", COL_LABEL);
	scan_disk_by_dir("/dev/disk/by-partuuid", COL_PARTUUID);
	scan_disk_by_dir("/dev/disk/by-partlabel", COL_PARTLABEL);

	/* Sort devices and their children by name by default */
	qsort(G.devices, G.num_devices, sizeof(G.devices[0]), compare_by_name);
	for (i = 0; i < G.num_devices; i++) {
		struct blockdev_info *d = G.devices[i];
		if (d->nchildren > 1)
			qsort(d->children, d->nchildren, sizeof(d->children[0]), compare_by_name);
		if (d->nparents > 1)
			qsort(d->parents, d->nparents, sizeof(d->parents[0]), compare_by_name);
	}

	/* Precompute column values for all devices */
	for (i = 0; i < G.num_devices; i++) {
		precompute_columns(G.devices[i], (G.opt & OPT_b), (G.opt & OPT_p));
	}

	/* Sort if requested */
	if (G.opt & OPT_x) {
		qsort(G.devices, G.num_devices, sizeof(G.devices[0]), compare_by_col);
	}

	/* JSON output */
	if (G.opt & OPT_J) {
		int is_tree = !(G.opt & (OPT_l | OPT_d));
		int first_dev = 1;

		printf("{\n   \"blockdevices\": [");
		if (is_tree) {
			for (i = 0; i < G.num_devices; i++) {
				struct blockdev_info *d = G.devices[i];
				if (has_cli_args ? d->is_cli_arg : is_root_device(d)) {
					if (!first_dev)
						printf(",");
					first_dev = 0;
					printf("\n");
					print_json_device(d, 2, 1);
				}
			}
		} else {
			for (i = 0; i < G.num_devices; i++) {
				struct blockdev_info *d = G.devices[i];
				if (G.opt & OPT_d) {
					if (!(has_cli_args ? d->is_cli_arg : is_root_device(d)))
						continue;
				}
				if (!first_dev)
					printf(",");
				first_dev = 0;
				printf("\n");
				print_json_device(d, 2, 0);
			}
		}
		printf("\n   ]\n}\n");
		fflush_stdout_and_exit(G.exitcode);
	}

	/* Pairs output */
	if (G.opt & OPT_P) {
		for (i = 0; i < G.num_devices; i++) {
			struct blockdev_info *d = G.devices[i];
			if (G.opt & OPT_d) {
				if (!(has_cli_args ? d->is_cli_arg : is_root_device(d)))
					continue;
			}
			print_device_pairs(d);
		}
		fflush_stdout_and_exit(G.exitcode);
	}

	/* Raw output */
	if (G.opt & OPT_r) {
		if (!(G.opt & OPT_n)) {
			unsigned c;
			for (c = 0; c < G.num_columns; c++) {
				if (c > 0)
					putchar(' ');
				printf("%s", columns_def[G.columns[c]].name);
			}
			putchar('\n');
		}
		for (i = 0; i < G.num_devices; i++) {
			struct blockdev_info *d = G.devices[i];
			if (G.opt & OPT_d) {
				if (!(has_cli_args ? d->is_cli_arg : is_root_device(d)))
					continue;
			}
			print_device_raw(d);
		}
		fflush_stdout_and_exit(G.exitcode);
	}

	/* Table output (tree or list) */
	/* Compute device depths for tree alignment */
	for (i = 0; i < G.num_devices; i++) {
		struct blockdev_info *d = G.devices[i];
		if (has_cli_args ? d->is_cli_arg : is_root_device(d)) {
			compute_depths(d, 0);
		}
	}

	/* Calculate column widths */
	col_widths = xzalloc(sizeof(int) * G.num_columns);
	for (i = 0; i < G.num_columns; i++) {
		int col_id = G.columns[i];
		unsigned d_idx;

		col_widths[i] = strlen(columns_def[col_id].name);
		for (d_idx = 0; d_idx < G.num_devices; d_idx++) {
			struct blockdev_info *d = G.devices[d_idx];
			int vis_len;

			if (G.opt & OPT_d) {
				if (!(has_cli_args ? d->is_cli_arg : is_root_device(d)))
					continue;
			}

			if (col_id == COL_NAME && !(G.opt & OPT_l)) {
				vis_len = 2 * d->depth + strlen(d->col_values[COL_NAME]);
			} else {
				vis_len = strlen(d->col_values[col_id]);
			}
			if (vis_len > col_widths[i])
				col_widths[i] = vis_len;
		}
	}

	/* Print header */
	if (!(G.opt & OPT_n)) {
		unsigned c;
		for (c = 0; c < G.num_columns; c++) {
			int col_id = G.columns[c];
			int is_last_col = (c == G.num_columns - 1);
			const char *hname = columns_def[col_id].name;

			if (is_last_col) {
				printf("%s", hname);
			} else if (columns_def[col_id].is_right) {
				printf("%*s ", col_widths[c], hname);
			} else {
				printf("%-*s ", col_widths[c], hname);
			}
		}
		putchar('\n');
	}

	/* Print rows */
	if (G.opt & OPT_l) {
		/* Flat list */
		int dummy_last[32];
		memset(dummy_last, 0, sizeof(dummy_last));
		for (i = 0; i < G.num_devices; i++) {
			struct blockdev_info *d = G.devices[i];
			if (G.opt & OPT_d) {
				if (!(has_cli_args ? d->is_cli_arg : is_root_device(d)))
					continue;
			}
			print_table_row(d, 0, 0, dummy_last, col_widths);
		}
	} else {
		/* Tree */
		int last_child_at_depth[32];
		memset(last_child_at_depth, 0, sizeof(last_child_at_depth));
		for (i = 0; i < G.num_devices; i++) {
			struct blockdev_info *d = G.devices[i];
			if (has_cli_args ? d->is_cli_arg : is_root_device(d)) {
				print_table_tree(d, 0, 1, last_child_at_depth, col_widths);
			}
		}
	}

	free(col_widths);
	fflush_stdout_and_exit(G.exitcode);
}
