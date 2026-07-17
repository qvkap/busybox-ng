#ifndef HTTP2_H
#define HTTP2_H

#include <stdint.h>
#include <sys/types.h>

/* Frame type constants */
#define H2_FRAME_DATA 0x0
#define H2_FRAME_HEADERS 0x1
#define H2_FRAME_PRIORITY 0x2
#define H2_FRAME_RST_STREAM 0x3
#define H2_FRAME_SETTINGS 0x4
#define H2_FRAME_PUSH_PROMISE 0x5
#define H2_FRAME_PING 0x6
#define H2_FRAME_GOAWAY 0x7
#define H2_FRAME_WINDOW_UPDATE 0x8
#define H2_FRAME_CONTINUATION 0x9

/* Frame flag constants */
#define H2_FLAG_END_STREAM 0x1
#define H2_FLAG_END_HEADERS 0x4
#define H2_FLAG_PADDED 0x8
#define H2_FLAG_PRIORITY 0x20
#define H2_FLAG_ACK 0x1

/* Settings IDs */
#define H2_SETTINGS_HEADER_TABLE_SIZE 0x1
#define H2_SETTINGS_ENABLE_PUSH 0x2
#define H2_SETTINGS_MAX_CONCURRENT_STREAMS 0x3
#define H2_SETTINGS_INITIAL_WINDOW_SIZE 0x4
#define H2_SETTINGS_MAX_FRAME_SIZE 0x5
#define H2_SETTINGS_MAX_HEADER_LIST_SIZE 0x6

struct hpack_static_entry {
	const char *name;
	const char *value;
};

extern const struct hpack_static_entry hpack_static_table[62];

typedef struct h2_state {
	int fd;
	uint32_t peer_window;
	uint32_t our_window;
	uint32_t peer_max_frame_size;
	uint32_t next_stream_id;
	int status_code;
	off_t content_length;
	int end_stream_reached;
} h2_state_t;

void h2_init(h2_state_t *h2, int fd);
int h2_send_request(h2_state_t *h2, const char *method, const char *host, const char *path, const char *user_agent);
ssize_t h2_read_body(h2_state_t *h2, void *buf, size_t bufsize);
void h2_close(h2_state_t *h2);

#endif /* HTTP2_H */
