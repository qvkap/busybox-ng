/* vi: set sw=4 ts=4: */
/* ip tuntap - TUN/TAP interface management */
#include "ip_common.h"
#include "utils.h"
#include <net/if.h>
#include <linux/if_tun.h>

static int tuntap_list(void)
{
	FILE *f;
	char line[256];
	char path[128];
	char tun_flags[32];
	FILE *tf;
	unsigned flags;
	int n;

	/* /proc/net/dev lists all interfaces */
	/* For TUN/TAP specifically, check /sys/class/net/<dev>/tun_flags */
	f = popen("ls /sys/class/net/ 2>/dev/null", "r");
	if (!f)
		return 1;
	n = 0;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = '\0';
		snprintf(path, sizeof(path), "/sys/class/net/%s/tun_flags", line);
		tf = fopen(path, "r");
		if (!tf)
			continue;
		if (fgets(tun_flags, sizeof(tun_flags), tf)) {
			flags = strtoul(tun_flags, NULL, 16);
			if (flags & IFF_TUN)
				printf("%s: tun\n", line);
			else if (flags & IFF_TAP)
				printf("%s: tap\n", line);
			n++;
		}
		fclose(tf);
	}
	pclose(f);
	if (n == 0)
		printf("(no tun/tap interfaces)\n");
	return 0;
}

static int tuntap_add_del(char **argv, int is_add)
{
	char ifname[IFNAMSIZ];
	int mode;
	int fd;
	struct ifreq ifr;
	int persist;

	ifname[0] = '\0';
	mode = IFF_TUN | IFF_NO_PI; /* default: tun */
	persist = is_add ? 1 : 0;

	/* Parse: [dev NAME] mode tun|tap [user U] [group G] */
	while (*argv) {
		if (strcmp(*argv, "mode") == 0) {
			argv++;
			if (!*argv) break;
			if (strcmp(*argv, "tap") == 0)
				mode = (mode & ~IFF_TUN) | IFF_TAP;
			else
				mode = (mode & ~IFF_TAP) | IFF_TUN;
		} else if (strcmp(*argv, "name") == 0 || strcmp(*argv, "dev") == 0) {
			argv++;
			if (*argv)
				safe_strncpy(ifname, *argv, IFNAMSIZ);
		} else if (strcmp(*argv, "pi") == 0) {
			mode &= ~IFF_NO_PI;
		} else if (strcmp(*argv, "user") == 0 || strcmp(*argv, "group") == 0) {
			argv++; /* skip argument */
		} else {
			/* treat as interface name */
			safe_strncpy(ifname, *argv, IFNAMSIZ);
		}
		argv++;
	}

	fd = open("/dev/net/tun", O_RDWR);
	if (fd < 0)
		bb_perror_msg_and_die("open /dev/net/tun");

	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_flags = mode;
	if (ifname[0])
		safe_strncpy(ifr.ifr_name, ifname, IFNAMSIZ);

	if (ioctl(fd, TUNSETIFF, &ifr) < 0)
		bb_perror_msg_and_die("TUNSETIFF");

	if (ioctl(fd, TUNSETPERSIST, persist) < 0)
		bb_perror_msg_and_die("TUNSETPERSIST");

	if (is_add)
		printf("Created %s interface %s\n",
			(mode & IFF_TAP) ? "tap" : "tun", ifr.ifr_name);

	close(fd);
	return 0;
}

int FAST_FUNC do_iptuntap(char **argv)
{
	static const char subcmds[] ALIGN1 =
		"add\0" "del\0" "delete\0" "list\0" "show\0" "lst\0";
	int cmd;

	if (!*argv)
		return tuntap_list();

	cmd = index_in_substrings(subcmds, *argv);
	if (cmd < 0)
		bb_error_msg_and_die("unknown command \"%s\"", *argv);

	argv++;
	switch (cmd) {
	case 0: return tuntap_add_del(argv, 1);
	case 1:
	case 2: return tuntap_add_del(argv, 0);
	case 3:
	case 4:
	case 5: return tuntap_list();
	}
	return 0;
}
