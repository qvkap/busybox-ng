/*
 * Wrapper for external full kmod tool.
 * Replaces internal modutils with execv to /bin/kmod.
 */

//config:config KMOD_WRAPPER
//config:	bool "kmod wrapper (replaces internal modutils)"
//config:	default y
//config:	help
//config:	Replaces internal modprobe/insmod/etc with wrappers
//config:	that execute the external full-featured kmod binary.

//applet:IF_KMOD_WRAPPER(APPLET(kmod, BB_DIR_BIN, BB_SUID_DROP))
//applet:IF_KMOD_WRAPPER(APPLET_ODDNAME(insmod, kmod, BB_DIR_SBIN, BB_SUID_DROP, insmod))
//applet:IF_KMOD_WRAPPER(APPLET_ODDNAME(rmmod, kmod, BB_DIR_SBIN, BB_SUID_DROP, rmmod))
//applet:IF_KMOD_WRAPPER(APPLET_ODDNAME(lsmod, kmod, BB_DIR_SBIN, BB_SUID_DROP, lsmod))
//applet:IF_KMOD_WRAPPER(APPLET_ODDNAME(modprobe, kmod, BB_DIR_SBIN, BB_SUID_DROP, modprobe))
//applet:IF_KMOD_WRAPPER(APPLET_ODDNAME(modinfo, kmod, BB_DIR_SBIN, BB_SUID_DROP, modinfo))
//applet:IF_KMOD_WRAPPER(APPLET_ODDNAME(depmod, kmod, BB_DIR_SBIN, BB_SUID_DROP, depmod))

//kbuild:lib-$(CONFIG_KMOD_WRAPPER) += kmod.o

//usage:#define kmod_trivial_usage "[OPTIONS] [COMMAND]"
//usage:#define kmod_full_usage "\n\nWrapper to execute external kmod tool"
//usage:#define insmod_trivial_usage "FILE [ARGS]"
//usage:#define insmod_full_usage "\n\nWrapper to external kmod"
//usage:#define rmmod_trivial_usage "MODULE"
//usage:#define rmmod_full_usage "\n\nWrapper to external kmod"
//usage:#define lsmod_trivial_usage ""
//usage:#define lsmod_full_usage "\n\nWrapper to external kmod"
//usage:#define modprobe_trivial_usage "MODULE"
//usage:#define modprobe_full_usage "\n\nWrapper to external kmod"
//usage:#define modinfo_trivial_usage "MODULE"
//usage:#define modinfo_full_usage "\n\nWrapper to external kmod"
//usage:#define depmod_trivial_usage ""
//usage:#define depmod_full_usage "\n\nWrapper to external kmod"

#include "libbb.h"

int kmod_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int kmod_main(int argc UNUSED_PARAM, char **argv)
{
	/* 
	 * If called as "kmod", we just pass all arguments to the external kmod.
	 * If called as "insmod", we execute "kmod insmod ...".
	 */
	char *applet = applet_name;
	const char *kmod_bin = "/bin/kmod";
	char **new_argv;
	int i;

	/* Fallback: if /bin/kmod doesn't exist, try looking in PATH */
	if (access(kmod_bin, X_OK) != 0) {
		kmod_bin = "kmod";
	}

	if (strcmp(applet, "kmod") == 0) {
		execvp(kmod_bin, argv);
	} else {
		/* Allocate new argv array. External kmod expects argv[0] to be the applet name. */
		new_argv = xmalloc(sizeof(char *) * (argc + 2));
		new_argv[0] = applet;
		for (i = 1; i <= argc; i++) {
			new_argv[i] = argv[i];
		}
		execvp(kmod_bin, new_argv);
	}

	bb_perror_msg_and_die("can't execute %s", kmod_bin);
}
