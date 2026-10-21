/* vi: set sw=4 ts=4: */
#include "ip_common.h"
#include "utils.h"
#include <sched.h>

#ifndef CLONE_NEWNET
# define CLONE_NEWNET 0x40000000
#endif

family_t preferred_family = AF_UNSPEC;
smallint show_stats;
smallint show_details;
smallint show_raw;
smallint resolve_hosts;
smallint oneline;
smallint json_output;
smallint brief_output;
smallint pretty_output;
smallint numeric_output;
char _SL_;

char** FAST_FUNC ip_parse_common_args(char **argv)
{
	static const char ip_common_commands[] ALIGN1 =
		"oneline\0"    /* 0 */
		"family\0"     /* 1 */
		"4\0"          /* 2 */
		"6\0"          /* 3 */
		"0\0"          /* 4 */
		"json\0"       /* 5 */
		"j\0"          /* 6 */
		"brief\0"      /* 7 */
		"br\0"         /* 8 */
		"pretty\0"     /* 9 */
		"p\0"          /* 10 */
		"stats\0"      /* 11 */
		"statistics\0" /* 12 */
		"s\0"          /* 13 */
		"details\0"    /* 14 */
		"d\0"          /* 15 */
		"resolve\0"    /* 16 */
		"r\0"          /* 17 */
		"Numeric\0"    /* 18 */
		"N\0"          /* 19 */
		"color\0"      /* 20 */
		"c\0"          /* 21 */
		"human\0"      /* 22 */
		"h\0"          /* 23 */
		"Version\0"    /* 24 */
		"V\0"          /* 25 */
		"netns\0"      /* 26 */
		"n\0"          /* 27 */
		"loops\0"      /* 28 */
		"l\0"          /* 29 */
		"batch\0"      /* 30 */
		"b\0"          /* 31 */
		"rcvbuf\0"     /* 32 */
		"rc\0"         /* 33 */
		"timestamp\0"  /* 34 */
		"t\0"          /* 35 */
		"all\0"        /* 36 */
		"a\0"          /* 37 */
		;
	static const family_t af_numbers[] = { AF_INET, AF_INET6, AF_PACKET };
	int arg;
	char *opt;
	int fam;
	char path[64];
	int fd;

	while (*argv) {
		opt = *argv;

		if (opt[0] != '-')
			break;
		opt++;
		if (opt[0] == '-') {
			opt++;
			if (!opt[0]) { /* "--" */
				argv++;
				break;
			}
		}
		arg = index_in_substrings(ip_common_commands, opt);
		if (arg < 0) {
			/* Unknown global flag: skip silently (iproute2 behavior) */
			argv++;
			continue;
		}
		switch (arg) {
		case 0: /* oneline */
			oneline = 1;
			break;
		case 1: /* family */
		{
			static const char families[] ALIGN1 =
				"inet\0" "inet6\0" "link\0";
			argv++;
			if (!*argv)
				bb_show_usage();
			fam = index_in_strings(families, *argv);
			if (fam < 0)
				invarg_1_to_2(*argv, "family");
			preferred_family = af_numbers[fam < 3 ? fam : 0];
			break;
		}
		case 2: /* 4 */
			preferred_family = AF_INET;
			break;
		case 3: /* 6 */
			preferred_family = AF_INET6;
			break;
		case 4: /* 0 */
			preferred_family = AF_PACKET;
			break;
		case 5: /* json */
		case 6: /* j */
			json_output = 1;
			break;
		case 7: /* brief */
		case 8: /* br */
			brief_output = 1;
			break;
		case 9: /* pretty */
		case 10: /* p */
			pretty_output = 1;
			break;
		case 11: /* stats */
		case 12: /* statistics */
		case 13: /* s */
			show_stats++;
			break;
		case 14: /* details */
		case 15: /* d */
			show_details = 1;
			break;
		case 16: /* resolve */
		case 17: /* r */
			resolve_hosts = 1;
			break;
		case 18: /* Numeric */
		case 19: /* N */
			numeric_output = 1;
			break;
		case 20: /* color */
		case 21: /* c */
			/* only enable color on tty */
			/* color_output = isatty(STDOUT_FILENO); */
			break;
		case 22: /* human */
		case 23: /* h */
			/* no-op */
			break;
		case 24: /* Version */
		case 25: /* V */
			printf("ip utility, BusyBox-ng\n");
			exit(0);
		case 26: /* netns */
		case 27: /* n */
			argv++;
			if (!*argv)
				bb_show_usage();
			snprintf(path, sizeof(path), "/var/run/netns/%s", *argv);
			fd = open(path, O_RDONLY);
			if (fd < 0)
				bb_perror_msg_and_die("Cannot open network namespace '%s'", *argv);
			if (setns(fd, CLONE_NEWNET) < 0)
				bb_perror_msg_and_die("setns");
			close(fd);
			break;
		case 28: /* loops */
		case 29: /* l */
			argv++;
			/* skip argument */
			break;
		case 30: /* batch */
		case 31: /* b */
			argv++;
			/* skip argument */
			break;
		case 32: /* rcvbuf */
		case 33: /* rc */
			argv++;
			/* skip argument */
			break;
		case 34: /* timestamp */
		case 35: /* t */
			/* no-op */
			break;
		case 36: /* all */
		case 37: /* a */
			/* no-op for now */
			break;
		}
		argv++;
	}
	_SL_ = oneline ? '\\' : '\n';
	return argv;
}
