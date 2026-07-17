/*
 * HTTP/2 protocol implementation for busybox-ng
 */
//config:config FEATURE_HTTP2
//config:	bool "HTTP/2 protocol support"
//config:	default y
//config:	help
//config:	Enable HTTP/2 protocol support for curl and wget.

//kbuild:lib-$(CONFIG_FEATURE_HTTP2) += http2.o

#include "libbb.h"
#include "http2.h"
#include <string.h>

static const char h2_client_preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

const struct hpack_static_entry hpack_static_table[62] = {
	{ NULL, NULL },
	{ ":authority", "" },
	{ ":method", "GET" },
	{ ":method", "POST" },
	{ ":path", "/" },
	{ ":path", "/index.html" },
	{ ":scheme", "http" },
	{ ":scheme", "https" },
	{ ":status", "200" },
	{ ":status", "204" },
	{ ":status", "206" },
	{ ":status", "304" },
	{ ":status", "400" },
	{ ":status", "404" },
	{ ":status", "500" },
	{ "accept-charset", "" },
	{ "accept-encoding", "gzip, deflate" },
	{ "accept-language", "" },
	{ "accept-ranges", "" },
	{ "accept", "" },
	{ "access-control-allow-origin", "" },
	{ "age", "" },
	{ "allow", "" },
	{ "authorization", "" },
	{ "cache-control", "" },
	{ "content-disposition", "" },
	{ "content-encoding", "" },
	{ "content-language", "" },
	{ "content-length", "" },
	{ "content-location", "" },
	{ "content-range", "" },
	{ "content-type", "" },
	{ "cookie", "" },
	{ "date", "" },
	{ "etag", "" },
	{ "expect", "" },
	{ "expires", "" },
	{ "from", "" },
	{ "host", "" },
	{ "if-match", "" },
	{ "if-modified-since", "" },
	{ "if-none-match", "" },
	{ "if-range", "" },
	{ "if-unmodified-since", "" },
	{ "last-modified", "" },
	{ "link", "" },
	{ "location", "" },
	{ "max-forwards", "" },
	{ "proxy-authenticate", "" },
	{ "proxy-authorization", "" },
	{ "range", "" },
	{ "referer", "" },
	{ "refresh", "" },
	{ "retry-after", "" },
	{ "server", "" },
	{ "set-cookie", "" },
	{ "strict-transport-security", "" },
	{ "transfer-encoding", "" },
	{ "user-agent", "" },
	{ "vary", "" },
	{ "via", "" },
	{ "www-authenticate", "" }
};

void h2_init(h2_state_t *h2, int fd) {
	memset(h2, 0, sizeof(*h2));
	h2->fd = fd;
	h2->peer_window = 65535;
	h2->our_window = 65535;
	h2->peer_max_frame_size = 16384;
	h2->next_stream_id = 1;
	h2->status_code = 0;
	h2->content_length = -1;
	h2->end_stream_reached = 0;
}

static void h2_write_frame(h2_state_t *h2, uint8_t type, uint8_t flags, uint32_t stream_id, const void *payload, uint32_t payload_len) {
	uint8_t header[9];
	header[0] = (payload_len >> 16) & 0xff;
	header[1] = (payload_len >> 8) & 0xff;
	header[2] = payload_len & 0xff;
	header[3] = type;
	header[4] = flags;
	header[5] = (stream_id >> 24) & 0x7f;
	header[6] = (stream_id >> 16) & 0xff;
	header[7] = (stream_id >> 8) & 0xff;
	header[8] = stream_id & 0xff;
	full_write(h2->fd, header, 9);
	if (payload_len > 0 && payload) {
		full_write(h2->fd, payload, payload_len);
	}
}

static int h2_read_frame(h2_state_t *h2, uint8_t *type_out, uint8_t *flags_out, uint32_t *stream_id_out, uint8_t **payload_out, uint32_t *payload_len_out) {
	uint8_t header[9];
	if (full_read(h2->fd, header, 9) != 9) return -1;
	*payload_len_out = (header[0] << 16) | (header[1] << 8) | header[2];
	*type_out = header[3];
	*flags_out = header[4];
	*stream_id_out = ((header[5] & 0x7f) << 24) | (header[6] << 16) | (header[7] << 8) | header[8];
	
	if (*payload_len_out > 0) {
		*payload_out = xmalloc(*payload_len_out);
		if (full_read(h2->fd, *payload_out, *payload_len_out) != *payload_len_out) {
			free(*payload_out);
			return -1;
		}
	} else {
		*payload_out = NULL;
	}
	return 0;
}

static void encode_int(uint8_t **buf, size_t *len, uint32_t val, uint8_t prefix_mask, uint8_t bits) {
	uint8_t max_val = (1 << bits) - 1;
	uint8_t *p = *buf;
	if (val < max_val) {
		*p++ = prefix_mask | val;
	} else {
		*p++ = prefix_mask | max_val;
		val -= max_val;
		while (val >= 128) {
			*p++ = (val & 0x7f) | 0x80;
			val >>= 7;
		}
		*p++ = val;
	}
	*len += (p - *buf);
	*buf = p;
}

static void encode_string(uint8_t **buf, size_t *len, const char *str) {
	size_t slen = strlen(str);
	encode_int(buf, len, slen, 0, 7);
	memcpy(*buf, str, slen);
	*buf += slen;
	*len += slen;
}

int h2_send_request(h2_state_t *h2, const char *method, const char *host, const char *path, const char *user_agent) {
	uint8_t settings_payload[6];
	uint8_t *headers, *p;
	size_t headers_len = 0;
	
	full_write(h2->fd, h2_client_preface, sizeof(h2_client_preface) - 1);
	
	settings_payload[0] = 0;
	settings_payload[1] = H2_SETTINGS_ENABLE_PUSH;
	settings_payload[2] = 0; settings_payload[3] = 0; settings_payload[4] = 0; settings_payload[5] = 0;
	h2_write_frame(h2, H2_FRAME_SETTINGS, 0, 0, settings_payload, 6);
	
	headers = xmalloc(4096);
	p = headers;
	
	/* :method */
	if (!strcmp(method, "GET")) {
		encode_int(&p, &headers_len, 2, 0x80, 7);
	} else {
		encode_int(&p, &headers_len, 3, 0x80, 7); /* Assuming indexed POST */
	}
	
	/* :scheme https */
	encode_int(&p, &headers_len, 7, 0x80, 7);
	
	/* :path */
	if (!strcmp(path, "/")) {
		encode_int(&p, &headers_len, 4, 0x80, 7);
	} else {
		encode_int(&p, &headers_len, 4, 0x00, 4);
		encode_string(&p, &headers_len, path);
	}
	
	/* :authority */
	encode_int(&p, &headers_len, 1, 0x00, 4);
	encode_string(&p, &headers_len, host);
	
	/* user-agent */
	encode_int(&p, &headers_len, 58, 0x00, 4);
	encode_string(&p, &headers_len, user_agent);
	
	h2_write_frame(h2, H2_FRAME_HEADERS, H2_FLAG_END_STREAM | H2_FLAG_END_HEADERS, 1, headers, headers_len);
	free(headers);
	
	while (1) {
		uint8_t type, flags;
		uint32_t stream_id, payload_len;
		uint8_t *payload;
		if (h2_read_frame(h2, &type, &flags, &stream_id, &payload, &payload_len) < 0) {
			return -1;
		}
		
		if (type == H2_FRAME_SETTINGS && !(flags & H2_FLAG_ACK)) {
			h2_write_frame(h2, H2_FRAME_SETTINGS, H2_FLAG_ACK, 0, NULL, 0);
		} else if (type == H2_FRAME_HEADERS) {
			h2->status_code = 200; /* Minimal placeholder decoding */
			free(payload);
			break;
		} else if (type == H2_FRAME_GOAWAY) {
			free(payload);
			return -1;
		}
		free(payload);
	}
	
	return 0;
}

ssize_t h2_read_body(h2_state_t *h2, void *buf, size_t bufsize) {
	if (h2->end_stream_reached) return 0;
	while (1) {
		uint8_t type, flags;
		uint32_t stream_id, payload_len;
		uint8_t *payload;
		if (h2_read_frame(h2, &type, &flags, &stream_id, &payload, &payload_len) < 0) return -1;
		
		if (type == H2_FRAME_DATA) {
			size_t to_copy = payload_len < bufsize ? payload_len : bufsize;
			if (to_copy > 0) memcpy(buf, payload, to_copy);
			free(payload);
			
			if (payload_len > 0) {
				uint8_t wu[4];
				wu[0] = (payload_len >> 24) & 0x7f;
				wu[1] = (payload_len >> 16) & 0xff;
				wu[2] = (payload_len >> 8) & 0xff;
				wu[3] = payload_len & 0xff;
				/* Window update for connection (stream 0) */
				h2_write_frame(h2, H2_FRAME_WINDOW_UPDATE, 0, 0, wu, 4);
				/* Window update for stream */
				h2_write_frame(h2, H2_FRAME_WINDOW_UPDATE, 0, stream_id, wu, 4);
			}
			
			if (flags & H2_FLAG_END_STREAM) {
				h2->end_stream_reached = 1;
			}
			return to_copy;
		}
		/* If it's a WINDOW_UPDATE or PING, ignore or ACK it */
		if (type == H2_FRAME_PING && !(flags & H2_FLAG_ACK)) {
			h2_write_frame(h2, H2_FRAME_PING, H2_FLAG_ACK, 0, payload, payload_len);
		}
		free(payload);
	}
	return -1;
}

void h2_close(h2_state_t *h2) {
	/* no-op */
}
