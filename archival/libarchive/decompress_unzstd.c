/* vi: set sw=4 ts=4: */
#include "libbb.h"
#include "bb_archive.h"

/* Forward declare ZSTD types since we compile zstd_declib.c separately */
typedef struct ZSTD_DStream_s ZSTD_DStream;
typedef struct {
    const void* src;
    size_t size;
    size_t pos;
} ZSTD_inBuffer;
typedef struct {
    void*  dst;
    size_t size;
    size_t pos;
} ZSTD_outBuffer;

ZSTD_DStream* ZSTD_createDStream(void);
size_t ZSTD_freeDStream(ZSTD_DStream* zds);
size_t ZSTD_initDStream(ZSTD_DStream* zds);
size_t ZSTD_decompressStream(ZSTD_DStream* zds, ZSTD_outBuffer* output, ZSTD_inBuffer* input);
unsigned ZSTD_isError(size_t code);
const char* ZSTD_getErrorName(size_t code);
size_t ZSTD_DStreamInSize(void);
size_t ZSTD_DStreamOutSize(void);

IF_DESKTOP(long long) int FAST_FUNC
unpack_zstd_stream(transformer_state_t *xstate)
{
	ZSTD_DStream* dctx = ZSTD_createDStream();
	if (!dctx) {
		bb_simple_error_msg("zstd alloc failed");
		return -1;
	}

	ZSTD_initDStream(dctx);

	ZSTD_inBuffer input = { NULL, 0, 0 };
	ZSTD_outBuffer output = { NULL, 0, 0 };

	size_t in_size = ZSTD_DStreamInSize();
	size_t out_size = ZSTD_DStreamOutSize();
	char *in_buf = xmalloc(in_size);
	char *out_buf = xmalloc(out_size);

	output.dst = out_buf;
	output.size = out_size;

	IF_DESKTOP(long long) int total_out = 0;

	if (xstate->signature_skipped) {
		memcpy(in_buf, &xstate->magic, xstate->signature_skipped);
		input.src = in_buf;
		input.size = xstate->signature_skipped;
		input.pos = 0;
		xstate->signature_skipped = 0;
	}

	while (1) {
		if (input.pos == input.size) {
			ssize_t n = safe_read(xstate->src_fd, in_buf, in_size);
			if (n < 0) {
				bb_simple_error_msg("zstd read error");
				total_out = -1;
				break;
			}
			if (n == 0) break;
			input.src = in_buf;
			input.size = n;
			input.pos = 0;
		}

		output.pos = 0;
		size_t ret = ZSTD_decompressStream(dctx, &output, &input);
		if (ZSTD_isError(ret)) {
			bb_error_msg("zstd error: %s", ZSTD_getErrorName(ret));
			total_out = -1;
			break;
		}

		if (output.pos > 0) {
			if (transformer_write(xstate, output.dst, output.pos) != (ssize_t)output.pos) {
				total_out = -1;
				break;
			}
			total_out += output.pos;
		}
	}

	free(in_buf);
	free(out_buf);
	ZSTD_freeDStream(dctx);
	return total_out;
}
