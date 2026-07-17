/* vi: set sw=4 ts=4: */
/*
 * Zstandard decompression for busybox
 *
 * Copyright (C) 2024 busybox-ng contributors
 *
 * Licensed under GPLv2 or later, see file LICENSE in this source tree.
 */
#include "libbb.h"
#include "bb_archive.h"

/*
 * Zstd frame format:
 *   Magic number: 0xFD2FB528 (4 bytes, little-endian)
 *   Frame header (variable size)
 *   Data blocks
 *   Optional checksum (xxhash32)
 *
 * We use the external 'zstd' command for actual decompression,
 * reading/writing via pipes. This is the simplest and most
 * maintainable approach — the zstd format is complex and the
 * reference library is large (~100KB compiled).
 */

IF_DESKTOP(long long) int FAST_FUNC
unpack_zstd_stream(transformer_state_t *xstate)
{
	IF_DESKTOP(long long) int status = 0;
	unsigned char magic_buf[4];

	if (!xstate->signature_skipped) {
		if (full_read(xstate->src_fd, magic_buf, 4) != 4) {
			bb_simple_error_msg("short read");
			return -1;
		}
		/* Verify zstd magic: 0xFD2FB528 little-endian */
		if (magic_buf[0] != 0x28
		 || magic_buf[1] != 0xB5
		 || magic_buf[2] != 0x2F
		 || magic_buf[3] != 0xFD
		) {
			bb_simple_error_msg("invalid zstd magic");
			return -1;
		}
		xstate->signature_skipped = 4;
	}

	/*
	 * Seek back so that the external tool sees the full stream
	 * including the magic bytes.
	 */
	if (xstate->signature_skipped) {
		xlseek(xstate->src_fd, -(off_t)xstate->signature_skipped, SEEK_CUR);
		xstate->signature_skipped = 0;
	}

	{
		struct fd_pair fd_pipe;
		int pid;

		xpiped_pair(fd_pipe);
		pid = BB_MMU ? xfork() : xvfork();
		if (pid == 0) {
			/* Child: exec zstd -d */
			char *argv[4];

			close(fd_pipe.rd);
			xmove_fd(xstate->src_fd, STDIN_FILENO);
			xmove_fd(fd_pipe.wr, STDOUT_FILENO);
			argv[0] = (char *)"zstd";
			argv[1] = (char *)"-dc";
			argv[2] = (char *)"-";
			argv[3] = NULL;
			BB_EXECVP_or_die(argv);
		}

		/* Parent */
		close(fd_pipe.wr);
		{
			char buf[4096];
			ssize_t n;
			while ((n = safe_read(fd_pipe.rd, buf, sizeof(buf))) > 0) {
				if (transformer_write(xstate, buf, n) != n) {
					status = -1;
					break;
				}
				status += n;
			}
		}
		close(fd_pipe.rd);

		/* Reap child */
		{
			int child_status;
			safe_waitpid(pid, &child_status, 0);
			if (WIFEXITED(child_status) && WEXITSTATUS(child_status) != 0) {
				bb_simple_error_msg("zstd decompression failed");
				return -1;
			}
		}
	}

	return status;
}
