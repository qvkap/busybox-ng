/* vi: set sw=4 ts=4: */
/*
 * zstd compression support for BusyBox
 *
 * Licensed under GPLv2 or later, see file LICENSE in this source tree.
 */
//kbuild:lib-$(CONFIG_ZSTD) += compress_zstd.o zstd_declib.o
//kbuild:lib-$(CONFIG_FEATURE_TAR_ZSTD) += compress_zstd.o zstd_declib.o

#include "libbb.h"
#include "bb_archive.h"

/* Global compression level (1-19, default 3) */
int zstd_level = 3;

/* Forward declare ZSTD types since we compile zstd_declib.c separately */
typedef struct ZSTD_CStream_s ZSTD_CStream;
typedef struct {
	const void *src;
	size_t size;
	size_t pos;
} ZSTD_inBuffer;
typedef struct {
	void *dst;
	size_t size;
	size_t pos;
} ZSTD_outBuffer;

ZSTD_CStream* ZSTD_createCStream(void);
size_t ZSTD_freeCStream(ZSTD_CStream *zcs);
size_t ZSTD_initCStream(ZSTD_CStream *zcs, int compressionLevel);
size_t ZSTD_compressStream(ZSTD_CStream *zcs, ZSTD_outBuffer *output, ZSTD_inBuffer *input);
size_t ZSTD_endStream(ZSTD_CStream *zcs, ZSTD_outBuffer *output);
unsigned ZSTD_isError(size_t code);
const char* ZSTD_getErrorName(size_t code);
size_t ZSTD_CStreamInSize(void);
size_t ZSTD_CStreamOutSize(void);

IF_DESKTOP(long long) int FAST_FUNC
pack_zstd_stream(transformer_state_t *xstate)
{
	ZSTD_inBuffer input;
	ZSTD_outBuffer output;
	size_t in_size;
	size_t out_size;
	char *in_buf;
	char *out_buf;
	int level;
	size_t ret;
	IF_DESKTOP(long long) int total_out = 0;
	ZSTD_CStream *cstream;

	cstream = ZSTD_createCStream();
	if (!cstream) {
		bb_simple_error_msg("zstd alloc failed");
		return -1;
	}

	level = zstd_level;
	if (level < 1)
		level = 3;
	else if (level > 19)
		level = 19;

	ret = ZSTD_initCStream(cstream, level);
	if (ZSTD_isError(ret)) {
		bb_error_msg("zstd init error: %s", ZSTD_getErrorName(ret));
		ZSTD_freeCStream(cstream);
		return -1;
	}

	in_size = ZSTD_CStreamInSize();
	out_size = ZSTD_CStreamOutSize();
	in_buf = xmalloc(in_size);
	out_buf = xmalloc(out_size);

	input.src = in_buf;
	input.size = 0;
	input.pos = 0;

	output.dst = out_buf;
	output.size = out_size;
	output.pos = 0;

	while (1) {
		if (input.pos == input.size) {
			ssize_t n = safe_read(xstate->src_fd, in_buf, in_size);
			if (n < 0) {
				bb_simple_perror_msg(bb_msg_read_error);
				total_out = -1;
				goto done;
			}
			if (n == 0)
				break;
			input.size = n;
			input.pos = 0;
		}

		while (input.pos < input.size) {
			output.pos = 0;
			ret = ZSTD_compressStream(cstream, &output, &input);
			if (ZSTD_isError(ret)) {
				bb_error_msg("zstd error: %s", ZSTD_getErrorName(ret));
				total_out = -1;
				goto done;
			}
			if (output.pos > 0) {
				if (transformer_write(xstate, output.dst, output.pos) != (ssize_t)output.pos) {
					total_out = -1;
					goto done;
				}
				total_out += output.pos;
			}
		}
	}

	/* Flush remaining data and write frame footer */
	while (1) {
		output.pos = 0;
		ret = ZSTD_endStream(cstream, &output);
		if (ZSTD_isError(ret)) {
			bb_error_msg("zstd error: %s", ZSTD_getErrorName(ret));
			total_out = -1;
			goto done;
		}
		if (output.pos > 0) {
			if (transformer_write(xstate, output.dst, output.pos) != (ssize_t)output.pos) {
				total_out = -1;
				goto done;
			}
			total_out += output.pos;
		}
		if (ret == 0)
			break;
	}

 done:
	free(in_buf);
	free(out_buf);
	ZSTD_freeCStream(cstream);
	if (total_out >= 0)
		xstate->bytes_out = total_out;
	return total_out;
}

IF_DESKTOP(long long) int FAST_FUNC
compress_zstd_stream(transformer_state_t *xstate)
{
	return pack_zstd_stream(xstate);
}
