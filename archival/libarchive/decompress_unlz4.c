/* vi: set sw=4 ts=4: */
#include "libbb.h"
#include "bb_archive.h"

typedef struct LZ4F_dctx_s LZ4F_dctx;
typedef size_t LZ4F_errorCode_t;

unsigned LZ4F_isError(LZ4F_errorCode_t code);
const char* LZ4F_getErrorName(LZ4F_errorCode_t code);
LZ4F_errorCode_t LZ4F_createDecompressionContext(LZ4F_dctx** dctxPtr, unsigned version);
LZ4F_errorCode_t LZ4F_freeDecompressionContext(LZ4F_dctx* dctx);
size_t LZ4F_decompress(LZ4F_dctx* dctx, void* dstBuffer, size_t* dstSizePtr, const void* srcBuffer, size_t* srcSizePtr, const void* dOptPtr);

IF_DESKTOP(long long) int FAST_FUNC
unpack_lz4_stream(transformer_state_t *xstate)
{
	LZ4F_dctx* dctx;
	if (LZ4F_isError(LZ4F_createDecompressionContext(&dctx, 100))) {
		bb_simple_error_msg("lz4 alloc failed");
		return -1;
	}

	char in_buf[4096];
	char out_buf[4096];
	size_t in_pos = 0;
	size_t in_size = 0;
	IF_DESKTOP(long long) int total_out = 0;

	if (xstate->signature_skipped) {
		memcpy(in_buf, &xstate->magic, xstate->signature_skipped);
		in_size = xstate->signature_skipped;
		xstate->signature_skipped = 0;
	}

	while (1) {
		if (in_pos == in_size) {
			ssize_t n = safe_read(xstate->src_fd, in_buf, sizeof(in_buf));
			if (n < 0) {
				bb_simple_error_msg("lz4 read error");
				total_out = -1;
				break;
			}
			if (n == 0) break;
			in_size = n;
			in_pos = 0;
		}

		size_t src_len = in_size - in_pos;
		size_t dst_len = sizeof(out_buf);
		size_t ret = LZ4F_decompress(dctx, out_buf, &dst_len, in_buf + in_pos, &src_len, NULL);

		if (LZ4F_isError(ret)) {
			bb_error_msg("lz4 error: %s", LZ4F_getErrorName(ret));
			total_out = -1;
			break;
		}

		in_pos += src_len;

		if (dst_len > 0) {
			if (transformer_write(xstate, out_buf, dst_len) != (ssize_t)dst_len) {
				total_out = -1;
				break;
			}
			total_out += dst_len;
		}
	}

	LZ4F_freeDecompressionContext(dctx);
	return total_out;
}
