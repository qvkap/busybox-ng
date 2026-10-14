/* vi: set sw=4 ts=4: */
/*
 * zstd implementation for BusyBox
 *
 * Licensed under GPLv2 or later, see file LICENSE in this source tree.
 */
//config:config ZSTD
//config:	bool "zstd (zstd compressor)"
//config:	default y
//config:	help
//config:	zstd compresses or decompresses files using Zstandard algorithm.
//config:	Compression levels 1-19 supported.

//applet:IF_ZSTD(APPLET(zstd, BB_DIR_USR_BIN, BB_SUID_DROP))

//kbuild:lib-$(CONFIG_ZSTD) += zstd.o

//usage:#define zstd_trivial_usage
//usage:       "[-cfkvdq] [-1..19] [FILE]..."
//usage:#define zstd_full_usage "\n\n"
//usage:       "Compress or decompress FILEs (or stdin) using Zstandard\n"
//usage:     "\n	-d		Decompress"
//usage:     "\n	-c		Write to stdout"
//usage:     "\n	-f		Force overwrite"
//usage:     "\n	-k		Keep original file"
//usage:     "\n	-t		Test integrity"
//usage:     "\n	-v		Verbose"
//usage:     "\n	-q		Quiet"
//usage:     "\n	-1..-19		Compression level (default 3)"
//usage:     "\n	-T N		Number of threads (ignored)"
//usage:     "\n	--fast[=N]	Use fast compression (level 1)"
//usage:     "\n	--best		Use best compression (level 19)"
//usage:     "\n	--check		Verify checksum"
//usage:     "\n	--no-check	Skip checksum"
//usage:     "\n	--rm		Remove source file"

#include "libbb.h"
#include "bb_archive.h"

#if ENABLE_LONG_OPTS
static const char zstd_longopts[] ALIGN1 =
	"stdout\0"              No_argument       "c"
	"to-stdout\0"           No_argument       "c"
	"force\0"               No_argument       "f"
	"keep\0"                No_argument       "k"
	"verbose\0"             No_argument       "v"
	"quiet\0"               No_argument       "q"
	"decompress\0"          No_argument       "d"
	"uncompress\0"          No_argument       "d"
	"test\0"                No_argument       "t"
	"threads\0"             Required_argument "T"
	"fast\0"                Optional_argument "\xff"
	"best\0"                No_argument       "\xfe"
	"check\0"               No_argument       "\xfd"
	"no-check\0"            No_argument       "\xfc"
	"rm\0"                  No_argument       "\xfb"
	;

enum {
	OPTBIT_T = 7,
	OPTBIT_FAST,
	OPTBIT_BEST,
	OPTBIT_CHECK,
	OPTBIT_NO_CHECK,
	OPTBIT_RM,

	OPT_T        = 1 << OPTBIT_T,
	OPT_FAST     = 1 << OPTBIT_FAST,
	OPT_BEST     = 1 << OPTBIT_BEST,
	OPT_CHECK    = 1 << OPTBIT_CHECK,
	OPT_NO_CHECK = 1 << OPTBIT_NO_CHECK,
	OPT_RM       = 1 << OPTBIT_RM,
};
#endif

static char* FAST_FUNC make_new_name_zstd(char *filename, const char *expected_ext)
{
	char *extension = strrchr(filename, '.');
	if (!extension || strcmp(extension + 1, expected_ext) != 0) {
		return NULL;
	}
	*extension = '\0';
	return filename;
}

int zstd_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int zstd_main(int argc, char **argv)
{
	int i, j;
	int level = 3;
	unsigned opt;
	char *opt_T = NULL;
#if ENABLE_LONG_OPTS
	char *opt_fast = NULL;
#endif

	/* Parse and extract numeric options -1..-19 */
	for (i = 1; i < argc; ) {
		if (argv[i][0] == '-' && isdigit(argv[i][1])) {
			char *endp;
			long val = strtol(argv[i] + 1, &endp, 10);
			if (*endp == '\0' && val >= 1 && val <= 19) {
				level = (int)val;
				for (j = i; j < argc; j++)
					argv[j] = argv[j + 1];
				argc--;
				continue;
			}
		}
		i++;
	}

#if ENABLE_LONG_OPTS
	opt = getopt32long(argv, BBUNPK_OPTSTR "dtT:", zstd_longopts, &opt_T, &opt_fast);
	if (opt & OPT_FAST) {
		level = 1;
		if (opt_fast && isdigit(opt_fast[0])) {
			int fast_lvl = xatoi_positive(opt_fast);
			if (fast_lvl >= 1)
				level = fast_lvl;
		}
	}
	if (opt & OPT_BEST)
		level = 19;
	if (opt & OPT_RM)
		option_mask32 &= ~BBUNPK_OPT_KEEP;
#else
	opt = getopt32(argv, BBUNPK_OPTSTR "dtT:", &opt_T);
#endif

	/* If called as unzstd or zstdcat */
	if (ENABLE_UNZSTD && applet_name[0] == 'u')
		opt |= BBUNPK_OPT_DECOMPRESS;
	if (ENABLE_ZSTDCAT && applet_name[0] == 'z' && applet_name[4] == 'c') {
		opt |= BBUNPK_OPT_DECOMPRESS;
		option_mask32 |= BBUNPK_OPT_STDOUT;
	}

	zstd_level = level;
	argv += optind;

	if (opt & (BBUNPK_OPT_DECOMPRESS | BBUNPK_OPT_TEST))
		return bbunpack(argv, unpack_zstd_stream, make_new_name_zstd, "zst");

	option_mask32 &= BBUNPK_OPTSTRMASK;
	return bbunpack(argv, pack_zstd_stream, append_ext, "zst");
}
