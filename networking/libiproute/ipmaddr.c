/* vi: set sw=4 ts=4: */
/* ip maddr - multicast address management */
#include "ip_common.h"
#include "utils.h"

static int maddr_show(char *filter_dev)
{
	FILE *f;
	char line[256];
	char cur_dev[32];
	int in_dev;
	char dev[32];
	char *col;
	unsigned grp;
	int users;
	struct in_addr a;

	cur_dev[0] = '\0';
	in_dev = 0;

	/* IPv4 multicast groups */
	f = fopen("/proc/net/igmp", "r");
	if (f) {
		while (fgets(line, sizeof(line), f)) {
			if (line[0] != '\t' && line[0] != ' ' && sscanf(line, "%*d %31s", dev) == 1) {
				/* Device line */
				col = strchr(dev, ':');
				if (col)
					*col = '\0';
				safe_strncpy(cur_dev, dev, sizeof(cur_dev));
				in_dev = !filter_dev || strcmp(cur_dev, filter_dev) == 0;
				if (in_dev)
					printf("%s\n", cur_dev);
			} else if (in_dev && sscanf(line, "%x %d", &grp, &users) == 2) {
				/* Group line */
				a.s_addr = htonl(grp);
				printf("\tinet %s\n", inet_ntoa(a));
			}
		}
		fclose(f);
	}
	return 0;
}

int FAST_FUNC do_ipmaddr(char **argv)
{
	static const char subcmds[] ALIGN1 =
		"show\0" "list\0" "add\0" "del\0" "delete\0";
	char *filter_dev = NULL;
	int cmd;

	if (!*argv)
		return maddr_show(NULL);

	cmd = index_in_substrings(subcmds, *argv);
	if (cmd < 0) {
		/* Maybe it's a device name */
		return maddr_show(*argv);
	}

	argv++;
	if (*argv && (strcmp(*argv, "dev") == 0)) {
		argv++;
		if (*argv) filter_dev = *argv++;
	} else if (*argv) {
		filter_dev = *argv++;
	}

	switch (cmd) {
	case 0:
	case 1:
		return maddr_show(filter_dev);
	case 2:
	case 3:
	case 4:
		/* add/del: use ip_mreq setsockopt */
		/* for now: stub */
		bb_error_msg("maddr add/del not yet implemented");
		break;
	}
	return 0;
}
