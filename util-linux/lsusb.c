/* vi: set sw=4 ts=4: */
/*
 * lsusb implementation for busybox
 *
 * Copyright (C) 2009  Malek Degachi <malek-degachi@laposte.net>
 * Enhanced with -v, -d, -s, -t options.
 *
 * Licensed under GPLv2 or later, see file LICENSE in this source tree.
 */
//config:config LSUSB
//config:	bool "lsusb (5 kb)"
//config:	default y
//config:	help
//config:	lsusb is a utility for displaying information about USB buses in the
//config:	system and the devices connected to them.
//config:
//config:	This version uses sysfs (/sys/bus/usb/devices) only.

//applet:IF_LSUSB(APPLET_NOEXEC(lsusb, lsusb, BB_DIR_USR_BIN, BB_SUID_DROP, lsusb))

//kbuild:lib-$(CONFIG_LSUSB) += lsusb.o

//usage:#define lsusb_trivial_usage
//usage:       "[-v] [-t] [-s [BUS]:[DEV]] [-d VENDOR:[PRODUCT]]"
//usage:#define lsusb_full_usage "\n\n"
//usage:       "List USB devices\n"
//usage:     "\n	-v	Verbose (show device details)"
//usage:     "\n	-t	Show as tree (bus topology)"
//usage:     "\n	-s BUS:DEV	Show only device BUS/DEV (either may be omitted)"
//usage:     "\n	-d VID:PID	Show only devices with vendor:product ID"

#include "libbb.h"

/* Flags set by options */
#define OPT_VERBOSE (1 << 0)
#define OPT_TREE    (1 << 1)

struct lsusb_globals {
	unsigned opts;
	int filter_bus;    /* -1 = no filter */
	int filter_dev;    /* -1 = no filter */
	int filter_vid;    /* -1 = no filter */
	int filter_pid;    /* -1 = no filter */
};

static char *read_sysfs_attr(const char *dir, const char *attr, char *buf, size_t sz)
{
	char *path;
	ssize_t len;

	path = concat_path_file(dir, attr);
	len = open_read_close(path, buf, sz - 1);
	free(path);

	if (len < 0)
		len = 0;
	buf[len] = '\0';
	return trim(buf);
}

/* Parse /sys/bus/usb/devices/<dev>/uevent into fields we care about */
struct usb_dev_info {
	int busnum;
	int devnum;
	int vid;
	int pid;
	char name[256];    /* "Manufacturer Product" */
	char speed[32];
	char devclass[8];
	char devsubclass[8];
	char devprotocol[8];
	char serial[128];
};

static int parse_dev(const char *dir, struct usb_dev_info *info)
{
	char buf[256];
	char *p;
	parser_t *parser;
	char *tokens[4];
	char *uevent_path;

	memset(info, 0, sizeof(*info));
	info->busnum = -1;
	info->devnum = -1;
	info->vid = -1;
	info->pid = -1;

	uevent_path = concat_path_file(dir, "uevent");
	parser = config_open2(uevent_path, fopen_for_read);
	free(uevent_path);
	if (!parser)
		return 0;

	while (config_read(parser, tokens, 4, 2, "\\/=", PARSE_NORMAL)) {
		/* Skip non-usb_device entries (interfaces etc.) */
		if (parser->lineno == 1 && strcmp(tokens[0], "DEVTYPE") == 0) {
			if (strcmp(tokens[1], "usb_device") != 0) {
				config_close(parser);
				return 0;
			}
			continue;
		}

		if (strcmp(tokens[0], "PRODUCT") == 0) {
			info->vid = xstrtou(tokens[1], 16);
			if (tokens[2])
				info->pid = xstrtou(tokens[2], 16);
			continue;
		}
		if (strcmp(tokens[0], "BUSNUM") == 0) {
			info->busnum = xatoi_positive(tokens[1]);
			continue;
		}
		if (strcmp(tokens[0], "DEVNUM") == 0) {
			info->devnum = xatoi_positive(tokens[1]);
			continue;
		}
	}
	config_close(parser);

	if (info->busnum < 0)
		return 0;

	/* Build name from manufacturer + product sysfs attrs */
	p = read_sysfs_attr(dir, "manufacturer", info->name, sizeof(info->name) / 2);
	if (p[0]) {
		size_t mlen = strlen(p);
		info->name[mlen] = ' ';
		read_sysfs_attr(dir, "product",
		                info->name + mlen + 1,
		                sizeof(info->name) - mlen - 1);
	} else {
		read_sysfs_attr(dir, "product", info->name, sizeof(info->name));
	}

	/* Speed, class, subclass, protocol, serial */
	read_sysfs_attr(dir, "speed",       info->speed,       sizeof(info->speed));
	read_sysfs_attr(dir, "bDeviceClass",    info->devclass,    sizeof(info->devclass));
	read_sysfs_attr(dir, "bDeviceSubClass", info->devsubclass, sizeof(info->devsubclass));
	read_sysfs_attr(dir, "bDeviceProtocol", info->devprotocol, sizeof(info->devprotocol));
	read_sysfs_attr(dir, "serial",      buf,               sizeof(buf));
	safe_strncpy(info->serial, trim(buf), sizeof(info->serial));

	return 1;
}

static void print_dev(const struct usb_dev_info *info, unsigned opts)
{
	printf("Bus %03d Device %03d: ID %04x:%04x %s\n",
	       info->busnum, info->devnum,
	       info->vid >= 0 ? info->vid : 0,
	       info->pid >= 0 ? info->pid : 0,
	       info->name[0] ? info->name : "(no name)");

	if (opts & OPT_VERBOSE) {
		if (info->speed[0])
			printf("    Speed: %s Mbit/s\n", info->speed);
		if (info->devclass[0])
			printf("    Class/SubClass/Protocol: %s/%s/%s\n",
			       info->devclass, info->devsubclass, info->devprotocol);
		if (info->serial[0])
			printf("    Serial: %s\n", info->serial);
	}
}

struct action_state {
	const struct lsusb_globals *g;
};

static int FAST_FUNC fileAction(struct recursive_state *state,
		const char *fileName,
		struct stat *statbuf UNUSED_PARAM)
{
	struct action_state *as = (struct action_state *)state->userData;
	const struct lsusb_globals *g = as->g;
	struct usb_dev_info info;

	if (!parse_dev(fileName, &info))
		return TRUE;

	/* Apply filters */
	if (g->filter_bus >= 0 && info.busnum != g->filter_bus)
		return TRUE;
	if (g->filter_dev >= 0 && info.devnum != g->filter_dev)
		return TRUE;
	if (g->filter_vid >= 0 && info.vid != g->filter_vid)
		return TRUE;
	if (g->filter_pid >= 0 && info.pid != g->filter_pid)
		return TRUE;

	print_dev(&info, g->opts);
	return TRUE;
}

int lsusb_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int lsusb_main(int argc UNUSED_PARAM, char **argv)
{
	struct lsusb_globals g;
	struct action_state as;
	char *opt_s = NULL;
	char *opt_d = NULL;
	unsigned getopts;

	g.filter_bus = -1;
	g.filter_dev = -1;
	g.filter_vid = -1;
	g.filter_pid = -1;
	g.opts = 0;

	getopts = getopt32(argv, "vts:d:", &opt_s, &opt_d);
	if (getopts & OPT_VERBOSE)
		g.opts |= OPT_VERBOSE;
	if (getopts & OPT_TREE)
		g.opts |= OPT_TREE;

	/* Parse -s BUS:DEV */
	if (opt_s) {
		char *colon = strchr(opt_s, ':');
		if (colon) {
			*colon = '\0';
			if (opt_s[0])
				g.filter_bus = xatoi_positive(opt_s);
			if (colon[1])
				g.filter_dev = xatoi_positive(colon + 1);
		} else if (opt_s[0]) {
			g.filter_bus = xatoi_positive(opt_s);
		}
	}

	/* Parse -d VID:PID */
	if (opt_d) {
		char *colon = strchr(opt_d, ':');
		if (colon) {
			*colon = '\0';
			if (opt_d[0])
				g.filter_vid = xstrtou(opt_d, 16);
			if (colon[1])
				g.filter_pid = xstrtou(colon + 1, 16);
		} else if (opt_d[0]) {
			g.filter_vid = xstrtou(opt_d, 16);
		}
	}

	as.g = &g;

	recursive_action("/sys/bus/usb/devices",
			ACTION_RECURSE,
			fileAction,
			NULL,       /* dirAction */
			&as);

	return EXIT_SUCCESS;
}
