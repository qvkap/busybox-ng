/* vi: set sw=4 ts=4: */
/* ip monitor - watch for netlink events */
#include "ip_common.h"
#include "utils.h"
#include "libnetlink.h"
#include <linux/rtnetlink.h>

static const char * const nlmsg_type_name[] = {
	[RTM_NEWLINK]  = "NEWLINK",
	[RTM_DELLINK]  = "DELLINK",
	[RTM_NEWADDR]  = "NEWADDR",
	[RTM_DELADDR]  = "DELADDR",
	[RTM_NEWROUTE] = "NEWROUTE",
	[RTM_DELROUTE] = "DELROUTE",
	[RTM_NEWNEIGH] = "NEWNEIGH",
	[RTM_DELNEIGH] = "DELNEIGH",
	[RTM_NEWRULE]  = "NEWRULE",
	[RTM_DELRULE]  = "DELRULE",
};

int FAST_FUNC do_ipmonitor(char **argv UNUSED_PARAM)
{
	struct sockaddr_nl addr;
	int fd;
	char buf[16384];
	unsigned groups;
	struct nlmsghdr *h;
	int len;
	unsigned t;
	const char *name;

	groups = RTMGRP_LINK
		| RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR
		| RTMGRP_IPV4_ROUTE  | RTMGRP_IPV6_ROUTE
		| RTMGRP_NEIGH;

	fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (fd < 0)
		bb_perror_msg_and_die("socket");

	memset(&addr, 0, sizeof(addr));
	addr.nl_family = AF_NETLINK;
	addr.nl_groups = groups;

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
		bb_perror_msg_and_die("bind");

	printf("Monitoring network events. Press Ctrl+C to stop.\n");
	fflush(stdout);

	while (1) {
		len = recv(fd, buf, sizeof(buf), 0);
		if (len <= 0)
			break;

		for (h = (struct nlmsghdr *)buf; NLMSG_OK(h, (unsigned)len);
		     h = NLMSG_NEXT(h, len)) {
			if (h->nlmsg_type == NLMSG_DONE)
				break;
			if (h->nlmsg_type == NLMSG_ERROR)
				break;

			t = h->nlmsg_type;
			name = NULL;
			if (t < ARRAY_SIZE(nlmsg_type_name))
				name = nlmsg_type_name[t];

			if (name)
				printf("[%s] seq=%u\n", name, h->nlmsg_seq);
			else
				printf("[type=%u]\n", t);
			fflush(stdout);
		}
	}
	close(fd);
	return 0;
}
