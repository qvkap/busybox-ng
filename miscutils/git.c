/* vi: set sw=4 ts=4: */
/*
 * Minimal Git implementation for BusyBox-ng
 *
 * Copyright (C) 2026 qvkap <qvkapp@gmail.com>
 * Licensed under GPLv2 or later.
 */

//config:config GIT
//config:	bool "git (mini version control)"
//config:	default n
//config:	help
//config:	Minimal implementation of Git revision control system.

//applet:IF_GIT(APPLET(git, BB_DIR_USR_BIN, BB_SUID_DROP))

//kbuild:lib-$(CONFIG_GIT) += git.o

//usage:#define git_trivial_usage
//usage:       "COMMAND [ARGS]"
//usage:#define git_full_usage "\n\n"
//usage:       "Minimal Git revision control\n"
//usage:       "\nCommands:"
//usage:       "\n\tinit [DIR]          Initialize repository"
//usage:       "\n\tclone URL [DIR]     Clone repository"
//usage:       "\n\tstatus              Show working tree status"
//usage:       "\n\tadd FILE...         Add files to index"
//usage:       "\n\tcommit -m MSG       Record changes"
//usage:       "\n\tdiff [FILE...]      Show unstaged changes"
//usage:       "\n\tlog [-n N]          Show commit log"
//usage:       "\n\tbranch [NAME]       List or create branches"
//usage:       "\n\tcheckout BRANCH     Switch branches"
//usage:       "\n\tmerge BRANCH        Merge branch (fast-forward)"
//usage:       "\n\tpull [REMOTE]       Fetch and merge"
//usage:       "\n\tfetch [REMOTE]      Download objects"
//usage:       "\n\tremote SUBCMD       Manage remotes"
//usage:       "\n\tstash [push|pop]    Stash changes"
//usage:       "\n\ttag [NAME]          List or create tags"
//usage:       "\n\treset [--hard] REF  Reset HEAD"
//usage:       "\n\tshow [OBJ]          Show object"
//usage:       "\n\thash-object [-w] F  Compute SHA1"
//usage:       "\n\tcat-file -p|-t|-s OBJ  Show object"

#include <fnmatch.h>
#include "libbb.h"

/* --- TINF (Tiny Inflate for Git zlib decompress) --- */
typedef struct {
	const uint8_t *source;
	const uint8_t *source_end;
	unsigned int tag;
	unsigned int bitcount;
} TINF_DATA;

typedef struct {
	unsigned short table[16];
	unsigned short trans[288];
} TINF_TREE;

static const uint8_t tinf_clc[] = {
	16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

static const unsigned short tinf_len_base[31] = {
	3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
	35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258, 0, 0
};
static const uint8_t tinf_len_ext[31] = {
	0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
	3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0, 0, 0
};
static const unsigned short tinf_dist_base[32] = {
	1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
	257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
	8193, 12289, 16385, 24577, 0, 0
};
static const uint8_t tinf_dist_ext[32] = {
	0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
	7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 0, 0
};

static unsigned int tinf_getbit(TINF_DATA *d)
{
	unsigned int bit;
	if (d->bitcount == 0) {
		if (d->source >= d->source_end)
			return 0;
		d->tag = *d->source++;
		d->bitcount = 8;
	}
	bit = d->tag & 1;
	d->tag >>= 1;
	--d->bitcount;
	return bit;
}

static unsigned int tinf_read_bits(TINF_DATA *d, int num, int base)
{
	unsigned int val = 0;
	if (num) {
		unsigned int mask = 1;
		while (num--) {
			if (tinf_getbit(d)) val |= mask;
			mask <<= 1;
		}
	}
	return base + val;
}

static void tinf_build_tree(TINF_TREE *t, const uint8_t *lengths, unsigned int num)
{
	unsigned short offs[16];
	unsigned int i, sum = 0;
	for (i = 0; i < 16; ++i) t->table[i] = 0;
	for (i = 0; i < num; ++i) t->table[lengths[i]]++;
	t->table[0] = 0;
	for (i = 0; i < 16; ++i) { offs[i] = sum; sum += t->table[i]; }
	for (i = 0; i < num; ++i) {
		if (lengths[i]) t->trans[offs[lengths[i]]++] = i;
	}
}

static int tinf_decode_symbol(TINF_DATA *d, const TINF_TREE *t)
{
	int len, cur = 0, sum = 0;
	for (len = 1; len < 16; ++len) {
		cur |= tinf_getbit(d);
		sum += t->table[len];
		cur -= t->table[len];
		if (cur < 0) {
			int idx = sum + cur;
			if (idx < 0 || idx >= 288)
				return -1;
			return t->trans[idx];
		}
		cur <<= 1;
	}
	return -1;
}

static void tinf_build_fixed_trees(TINF_TREE *lt, TINF_TREE *dt)
{
	uint8_t lengths[288];
	int i;
	for (i = 0; i <= 143; ++i) lengths[i] = 8;
	for (i = 144; i <= 255; ++i) lengths[i] = 9;
	for (i = 256; i <= 279; ++i) lengths[i] = 7;
	for (i = 280; i <= 287; ++i) lengths[i] = 8;
	tinf_build_tree(lt, lengths, 288);
	for (i = 0; i < 32; ++i) lengths[i] = 5;
	tinf_build_tree(dt, lengths, 32);
}

typedef struct {
	uint8_t *dest;
	size_t dest_len;
	size_t dest_cap;
} TINF_OUT;

static void tinf_put(TINF_OUT *out, uint8_t c)
{
	if (out->dest_len >= out->dest_cap) {
		out->dest_cap = out->dest_cap ? out->dest_cap * 2 : 1024;
		out->dest = xrealloc(out->dest, out->dest_cap);
	}
	out->dest[out->dest_len++] = c;
}

static int tinf_inflate_block(TINF_DATA *d, TINF_OUT *out, const TINF_TREE *lt, const TINF_TREE *dt)
{
	while (1) {
		int sym = tinf_decode_symbol(d, lt);
		if (sym < 0) return -1;
		if (sym == 256) return 0;
		if (sym < 256) {
			tinf_put(out, (uint8_t)sym);
		} else {
			int length, dist_sym, dist;
			sym -= 257;
			if (sym < 0 || sym >= 29) return -1;
			length = tinf_read_bits(d, tinf_len_ext[sym], tinf_len_base[sym]);
			dist_sym = tinf_decode_symbol(d, dt);
			if (dist_sym < 0 || dist_sym >= 30) return -1;
			dist = tinf_read_bits(d, tinf_dist_ext[dist_sym], tinf_dist_base[dist_sym]);
			if (dist <= 0 || (size_t)dist > out->dest_len) return -1;
			while (length--) {
				uint8_t c = out->dest[out->dest_len - dist];
				tinf_put(out, c);
			}
		}
	}
}

static int tinf_uncompress(const uint8_t *source, size_t source_len, uint8_t **dest, size_t *dest_len)
{
	TINF_DATA d;
	TINF_OUT out;
	int bfinal;

	if (source_len < 6) return -1;
	d.source = source + 2;
	d.source_end = source + source_len;
	d.tag = 0;
	d.bitcount = 0;

	out.dest = NULL;
	out.dest_len = 0;
	out.dest_cap = 0;

	do {
		int btype;
		bfinal = tinf_getbit(&d);
		btype = tinf_read_bits(&d, 2, 0);
		if (btype == 0) {
			const uint8_t *src;
			uint16_t i, len, nlen;

			d.bitcount = 0;
			d.tag = 0;
			src = d.source;
			if (src + 4 > d.source_end) { free(out.dest); return -1; }
			len = src[0] | (src[1] << 8);
			nlen = src[2] | (src[3] << 8);
			if ((uint16_t)len != (uint16_t)~nlen) { free(out.dest); return -1; }
			src += 4;
			if (src + len > d.source_end) { free(out.dest); return -1; }
			for (i = 0; i < len; ++i) tinf_put(&out, src[i]);
			d.source = src + len;
			d.bitcount = 0;
			d.tag = 0;
		} else if (btype == 1) {
			TINF_TREE lt, dt;
			tinf_build_fixed_trees(&lt, &dt);
			if (tinf_inflate_block(&d, &out, &lt, &dt) < 0) { free(out.dest); return -1; }
		} else if (btype == 2) {
			int i, num, hlit, hdist, hclen;
			uint8_t clc_lengths[19];
			uint8_t lengths[288 + 32];
			TINF_TREE ct, lt, dt;

			hlit = tinf_read_bits(&d, 5, 257);
			hdist = tinf_read_bits(&d, 5, 1);
			hclen = tinf_read_bits(&d, 4, 4);
			memset(clc_lengths, 0, sizeof(clc_lengths));
			for (i = 0; i < hclen; ++i) clc_lengths[tinf_clc[i]] = tinf_read_bits(&d, 3, 0);
			tinf_build_tree(&ct, clc_lengths, 19);

			memset(lengths, 0, sizeof(lengths));
			num = 0;
			while (num < hlit + hdist) {
				int sym = tinf_decode_symbol(&d, &ct);
				if (sym < 0) { free(out.dest); return -1; }
				if (sym < 16) {
					lengths[num++] = sym;
				} else if (sym == 16) {
					int rep = tinf_read_bits(&d, 2, 3);
					uint8_t prev;
					if (num == 0 || num + rep > (int)sizeof(lengths)) { free(out.dest); return -1; }
					prev = lengths[num - 1];
					while (rep--) lengths[num++] = prev;
				} else if (sym == 17) {
					int rep = tinf_read_bits(&d, 3, 3);
					if (num + rep > (int)sizeof(lengths)) { free(out.dest); return -1; }
					while (rep--) lengths[num++] = 0;
				} else if (sym == 18) {
					int rep = tinf_read_bits(&d, 7, 11);
					if (num + rep > (int)sizeof(lengths)) { free(out.dest); return -1; }
					while (rep--) lengths[num++] = 0;
				} else {
					free(out.dest); return -1;
				}
			}
			tinf_build_tree(&lt, lengths, hlit);
			tinf_build_tree(&dt, lengths + hlit, hdist);
			if (tinf_inflate_block(&d, &out, &lt, &dt) < 0) { free(out.dest); return -1; }
		} else {
			free(out.dest); return -1;
		}
	} while (!bfinal);

	*dest = out.dest;
	*dest_len = out.dest_len;
	return 0;
}

/* --- Adler32 and zlib compression (uncompressed deflate format) --- */
static uint32_t git_adler32(const uint8_t *buf, size_t len)
{
	uint32_t s1 = 1;
	uint32_t s2 = 0;
	size_t i;
	for (i = 0; i < len; i++) {
		s1 = (s1 + buf[i]) % 65521;
		s2 = (s2 + s1) % 65521;
	}
	return (s2 << 16) | s1;
}

static uint8_t *git_zlib_compress(const uint8_t *src, size_t src_len, size_t *out_len)
{
	size_t max_blocks = (src_len + 65534) / 65535 + 1;
	size_t cap = 2 + max_blocks * 5 + src_len + 4;
	uint8_t *dst = xmalloc(cap);
	size_t dst_pos = 0;
	size_t remaining = src_len;
	size_t src_pos = 0;
	uint32_t adler;

	dst[dst_pos++] = 0x78;
	dst[dst_pos++] = 0x01;

	do {
		size_t chunk = remaining > 65535 ? 65535 : remaining;
		int is_final = (remaining <= 65535);
		uint16_t nlen = (uint16_t)~chunk;

		dst[dst_pos++] = is_final ? 0x01 : 0x00;
		dst[dst_pos++] = chunk & 0xff;
		dst[dst_pos++] = (chunk >> 8) & 0xff;
		dst[dst_pos++] = nlen & 0xff;
		dst[dst_pos++] = (nlen >> 8) & 0xff;

		if (chunk > 0) {
			memcpy(dst + dst_pos, src + src_pos, chunk);
			dst_pos += chunk;
			src_pos += chunk;
			remaining -= chunk;
		}
	} while (remaining > 0);

	adler = git_adler32(src, src_len);
	dst[dst_pos++] = (adler >> 24) & 0xff;
	dst[dst_pos++] = (adler >> 16) & 0xff;
	dst[dst_pos++] = (adler >> 8) & 0xff;
	dst[dst_pos++] = adler & 0xff;

	*out_len = dst_pos;
	return dst;
}

/* --- SHA-1 and Object Storage Helpers --- */
static int is_valid_sha1(const char *s)
{
	int i;
	if (!s || strlen(s) != 40)
		return 0;
	for (i = 0; i < 40; i++) {
		if (!isxdigit(s[i]))
			return 0;
	}
	return 1;
}

static void calc_sha1(const void *data, size_t len, uint8_t bin[20], char hex[41])
{
	sha1_ctx_t ctx;
	uint8_t local_bin[20];
	uint8_t *dest_bin = bin ? bin : local_bin;
	sha1_begin(&ctx);
	sha1_hash(&ctx, data, len);
	sha1_end(&ctx, dest_bin);
	if (hex) {
		bin2hex(hex, (char*)dest_bin, 20);
		hex[40] = '\0';
	}
}

static char *git_write_object(const char *type, const void *content, size_t content_len, char out_hex[41])
{
	char header[64];
	char obj_dir[64];
	char obj_path[128];
	char tmp_path[128];
	int hdr_len, fd;
	size_t total_len, comp_len;
	uint8_t *raw, *comp;
	uint8_t bin[20];

	hdr_len = sprintf(header, "%s %zu", type, content_len) + 1;
	total_len = hdr_len + content_len;

	raw = xmalloc(total_len);
	memcpy(raw, header, hdr_len);
	memcpy(raw + hdr_len, content, content_len);

	calc_sha1(raw, total_len, bin, out_hex);

	comp = git_zlib_compress(raw, total_len, &comp_len);
	free(raw);

	sprintf(obj_dir, ".git/objects/%.2s", out_hex);
	mkdir(obj_dir, 0777);

	sprintf(obj_path, "%s/%s", obj_dir, out_hex + 2);
	if (access(obj_path, F_OK) != 0) {
		sprintf(tmp_path, "%s/tmp_%u_%ld", obj_dir, (unsigned)getpid(), (long)random());
		fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0444);
		if (fd >= 0) {
			xwrite(fd, comp, comp_len);
			close(fd);
			rename(tmp_path, obj_path);
		}
	}
	free(comp);
	return out_hex;
}

static uint8_t *git_read_object(const char *hex, char **out_type, size_t *out_size)
{
	char obj_path[128];
	int fd;
	struct stat st;
	uint8_t *comp, *raw, *content;
	size_t raw_len, content_size, hdr_len;
	char *null_byte, *space;

	if (!is_valid_sha1(hex)) {
		return NULL;
	}

	sprintf(obj_path, ".git/objects/%.2s/%s", hex, hex + 2);
	fd = open(obj_path, O_RDONLY);
	if (fd < 0)
		return NULL;

	if (fstat(fd, &st) != 0 || st.st_size == 0) {
		close(fd);
		return NULL;
	}
	comp = xmalloc(st.st_size);
	xread(fd, comp, st.st_size);
	close(fd);

	raw = NULL;
	raw_len = 0;
	if (tinf_uncompress(comp, st.st_size, &raw, &raw_len) != 0) {
		free(comp);
		return NULL;
	}
	free(comp);

	null_byte = memchr(raw, '\0', raw_len);
	if (!null_byte) {
		free(raw);
		return NULL;
	}

	space = memchr(raw, ' ', null_byte - (char*)raw);
	if (!space) {
		free(raw);
		return NULL;
	}

	hdr_len = (null_byte - (char*)raw) + 1;
	content_size = bb_strtou(space + 1, NULL, 10);
	if (content_size > raw_len - hdr_len) {
		free(raw);
		bb_error_msg("corrupt object %s", hex);
		return NULL;
	}

	if (out_type)
		*out_type = xstrndup((char*)raw, space - (char*)raw);
	if (out_size)
		*out_size = content_size;

	content = xmalloc(content_size + 1);
	memcpy(content, raw + hdr_len, content_size);
	content[content_size] = '\0';
	free(raw);
	return content;
}

static int calc_blob_sha1(const char *path, char out_hex[41])
{
	struct stat st;
	int fd, hdr_len;
	char *buf;
	char header[64];
	size_t total;
	char *raw;

	fd = open(path, O_RDONLY);
	if (fd < 0) return -1;
	if (fstat(fd, &st) != 0) {
		close(fd);
		return -1;
	}
	buf = xmalloc(st.st_size);
	xread(fd, buf, st.st_size);
	close(fd);

	hdr_len = sprintf(header, "blob %zu", (size_t)st.st_size) + 1;
	total = hdr_len + st.st_size;
	raw = xmalloc(total);
	memcpy(raw, header, hdr_len);
	memcpy(raw + hdr_len, buf, st.st_size);
	calc_sha1(raw, total, NULL, out_hex);
	free(raw);
	free(buf);
	return 0;
}

/* --- Git Config Parser --- */
static char *git_config_get(const char *config_path, const char *section, const char *subsection, const char *key)
{
	FILE *f;
	char line[512];
	char cur_sec[64];
	char cur_subsec[64];
	int in_matching_sec = 0;
	char *res = NULL;

	cur_sec[0] = '\0';
	cur_subsec[0] = '\0';

	f = fopen(config_path, "r");
	if (!f) return NULL;

	while (fgets(line, sizeof(line), f)) {
		char *p = skip_whitespace(line);
		chomp(p);
		if (*p == '#' || *p == ';' || *p == '\0')
			continue;

		if (*p == '[') {
			char *end = strrchr(p, ']');
			if (end) {
				char *sub;
				*end = '\0';
				p++;
				p = skip_whitespace(p);
				sub = strchr(p, '"');
				if (sub) {
					char *sub_end;
					*sub = '\0';
					safe_strncpy(cur_sec, p, sizeof(cur_sec));
					trim(cur_sec);
					sub++;
					sub_end = strchr(sub, '"');
					if (sub_end) *sub_end = '\0';
					safe_strncpy(cur_subsec, sub, sizeof(cur_subsec));
				} else {
					safe_strncpy(cur_sec, p, sizeof(cur_sec));
					trim(cur_sec);
					cur_subsec[0] = '\0';
				}

				if (strcasecmp(cur_sec, section) == 0) {
					if (!subsection && cur_subsec[0] == '\0')
						in_matching_sec = 1;
					else if (subsection && strcmp(cur_subsec, subsection) == 0)
						in_matching_sec = 1;
					else
						in_matching_sec = 0;
				} else {
					in_matching_sec = 0;
				}
			}
			continue;
		}

		if (in_matching_sec) {
			char *eq = strchr(p, '=');
			if (eq) {
				char *k = p;
				char *v = eq + 1;
				*eq = '\0';
				trim(k);
				if (strcasecmp(k, key) == 0) {
					v = skip_whitespace(v);
					trim(v);
					if (*v == '"' || *v == '\'') {
						size_t len = strlen(v);
						if (len >= 2 && v[len - 1] == *v) {
							v[len - 1] = '\0';
							v++;
						}
					}
					res = xstrdup(v);
					break;
				}
			}
		}
	}

	fclose(f);
	return res;
}

static void git_config_set(const char *config_path, const char *section, const char *subsection, const char *key, const char *value)
{
	FILE *fin, *fout;
	char *tmp_path;
	char line[512];
	char cur_sec[64];
	char cur_subsec[64];
	int in_matching_sec = 0;
	int key_set = 0;
	int sec_found = 0;

	cur_sec[0] = '\0';
	cur_subsec[0] = '\0';

	tmp_path = xasprintf("%s.tmp_%u_%ld", config_path, (unsigned)getpid(), (long)random());
	fout = xfopen(tmp_path, "w");

	fin = fopen(config_path, "r");
	if (fin) {
		while (fgets(line, sizeof(line), fin)) {
			char *p = skip_whitespace(line);
			char orig_line[512];
			strcpy(orig_line, line);
			chomp(p);

			if (*p == '[') {
				char *end;
				if (in_matching_sec && !key_set) {
					fprintf(fout, "\t%s = %s\n", key, value);
					key_set = 1;
				}

				end = strrchr(p, ']');
				if (end) {
					char *sub;
					*end = '\0';
					p++;
					p = skip_whitespace(p);
					sub = strchr(p, '"');
					if (sub) {
						char *sub_end;
						*sub = '\0';
						safe_strncpy(cur_sec, p, sizeof(cur_sec));
						trim(cur_sec);
						sub++;
						sub_end = strchr(sub, '"');
						if (sub_end) *sub_end = '\0';
						safe_strncpy(cur_subsec, sub, sizeof(cur_subsec));
					} else {
						safe_strncpy(cur_sec, p, sizeof(cur_sec));
						trim(cur_sec);
						cur_subsec[0] = '\0';
					}

					if (strcasecmp(cur_sec, section) == 0) {
						if (!subsection && cur_subsec[0] == '\0') {
							in_matching_sec = 1;
							sec_found = 1;
						} else if (subsection && strcmp(cur_subsec, subsection) == 0) {
							in_matching_sec = 1;
							sec_found = 1;
						} else {
							in_matching_sec = 0;
						}
					} else {
						in_matching_sec = 0;
					}
				}
				fputs(orig_line, fout);
				continue;
			}

			if (in_matching_sec && !key_set) {
				char copy[512];
				char *eq;
				strcpy(copy, p);
				eq = strchr(copy, '=');
				if (eq) {
					char *k = copy;
					*eq = '\0';
					trim(k);
					if (strcasecmp(k, key) == 0) {
						fprintf(fout, "\t%s = %s\n", key, value);
						key_set = 1;
						continue;
					}
				}
			}

			fputs(orig_line, fout);
		}
		fclose(fin);
	}

	if (!sec_found) {
		if (subsection)
			fprintf(fout, "[%s \"%s\"]\n\t%s = %s\n", section, subsection, key, value);
		else
			fprintf(fout, "[%s]\n\t%s = %s\n", section, key, value);
	} else if (!key_set) {
		fprintf(fout, "\t%s = %s\n", key, value);
	}

	fclose(fout);
	rename(tmp_path, config_path);
	free(tmp_path);
}

static void git_config_remove_section(const char *config_path, const char *section, const char *subsection)
{
	FILE *fin, *fout;
	char *tmp_path;
	char line[512];
	char cur_sec[64];
	char cur_subsec[64];
	int in_matching_sec = 0;

	cur_sec[0] = '\0';
	cur_subsec[0] = '\0';

	fin = fopen(config_path, "r");
	if (!fin) return;

	tmp_path = xasprintf("%s.tmp_%u_%ld", config_path, (unsigned)getpid(), (long)random());
	fout = xfopen(tmp_path, "w");

	while (fgets(line, sizeof(line), fin)) {
		char *p = skip_whitespace(line);
		char orig_line[512];
		strcpy(orig_line, line);
		chomp(p);

		if (*p == '[') {
			char *end = strrchr(p, ']');
			if (end) {
				char *sub;
				*end = '\0';
				p++;
				p = skip_whitespace(p);
				sub = strchr(p, '"');
				if (sub) {
					char *sub_end;
					*sub = '\0';
					safe_strncpy(cur_sec, p, sizeof(cur_sec));
					trim(cur_sec);
					sub++;
					sub_end = strchr(sub, '"');
					if (sub_end) *sub_end = '\0';
					safe_strncpy(cur_subsec, sub, sizeof(cur_subsec));
				} else {
					safe_strncpy(cur_sec, p, sizeof(cur_sec));
					trim(cur_sec);
					cur_subsec[0] = '\0';
				}

				if (strcasecmp(cur_sec, section) == 0) {
					if (!subsection && cur_subsec[0] == '\0') {
						in_matching_sec = 1;
					} else if (subsection && strcmp(cur_subsec, subsection) == 0) {
						in_matching_sec = 1;
					} else {
						in_matching_sec = 0;
					}
				} else {
					in_matching_sec = 0;
				}
			}
			if (in_matching_sec)
				continue;
		}

		if (!in_matching_sec)
			fputs(orig_line, fout);
	}

	fclose(fin);
	fclose(fout);
	rename(tmp_path, config_path);
	free(tmp_path);
}

static char *git_get_author_name(void)
{
	char *val = getenv("GIT_AUTHOR_NAME");
	char *home;
	if (val && *val)
		return xstrdup(val);
	val = git_config_get(".git/config", "user", NULL, "name");
	if (val)
		return val;
	home = getenv("HOME");
	if (home) {
		char *path = xasprintf("%s/.gitconfig", home);
		val = git_config_get(path, "user", NULL, "name");
		free(path);
		if (val)
			return val;
	}
	val = getenv("USER");
	if (val && *val)
		return xstrdup(val);
	return xstrdup("User");
}

static char *git_get_author_email(void)
{
	char *val = getenv("GIT_AUTHOR_EMAIL");
	char *home;
	char host[64];
	const char *u;

	if (val && *val)
		return xstrdup(val);
	val = git_config_get(".git/config", "user", NULL, "email");
	if (val)
		return val;
	home = getenv("HOME");
	if (home) {
		char *path = xasprintf("%s/.gitconfig", home);
		val = git_config_get(path, "user", NULL, "email");
		free(path);
		if (val)
			return val;
	}
	u = getenv("USER");
	if (!u || !*u) u = "user";
	host[0] = '\0';
	gethostname(host, sizeof(host) - 1);
	if (!host[0]) strcpy(host, "local");
	return xasprintf("%s@%s", u, host);
}

/* --- .gitignore Matching --- */
static char **load_gitignore(int *out_count)
{
	FILE *f;
	char line[256];
	char **patterns = NULL;
	int count = 0;

	*out_count = 0;
	f = fopen(".gitignore", "r");
	if (!f) return NULL;

	while (fgets(line, sizeof(line), f)) {
		char *p = skip_whitespace(line);
		chomp(p);
		if (*p == '#' || *p == '\0')
			continue;
		patterns = xrealloc_vector(patterns, 4, count);
		patterns[count++] = xstrdup(p);
	}
	fclose(f);
	*out_count = count;
	return patterns;
}

static int is_ignored(const char *path, int is_dir, char **patterns, int pcount)
{
	int i;
	const char *base;

	if (strcmp(path, ".git") == 0 || is_prefixed_with(path, ".git/") || strcmp(path, "./.git") == 0)
		return 1;

	base = strrchr(path, '/');
	base = base ? base + 1 : path;

	for (i = 0; i < pcount; i++) {
		const char *pat = patterns[i];
		int pat_dir = 0;
		char pat_buf[256];

		safe_strncpy(pat_buf, pat, sizeof(pat_buf));
		if (pat_buf[0] && pat_buf[strlen(pat_buf) - 1] == '/') {
			pat_dir = 1;
			pat_buf[strlen(pat_buf) - 1] = '\0';
		}

		if (pat_dir && !is_dir)
			continue;

		if (strchr(pat_buf, '/')) {
			const char *match_path = path;
			if (pat_buf[0] == '/') {
				if (match_path[0] == '/') match_path++;
				if (match_path[0] == '.' && match_path[1] == '/') match_path += 2;
				if (fnmatch(pat_buf + 1, match_path, 0) == 0)
					return 1;
			} else {
				if (fnmatch(pat_buf, match_path, 0) == 0)
					return 1;
			}
		} else {
			if (fnmatch(pat_buf, base, 0) == 0)
				return 1;
		}
	}
	return 0;
}

/* --- Index Structure and Helpers --- */
typedef struct {
	char *path;
	char hex[41];
} index_entry_t;

static index_entry_t *read_index(int *count)
{
	char line[512];
	index_entry_t *entries = NULL;
	FILE *f;

	*count = 0;
	f = fopen(".git/index.txt", "r");
	if (!f) return NULL;
	while (fgets(line, sizeof(line), f)) {
		char *sp;
		chomp(line);
		sp = strchr(line, ' ');
		if (!sp || (sp - line) != 40) continue;
		*sp = '\0';
		entries = xrealloc_vector(entries, 4, *count);
		entries[*count].path = xstrdup(sp + 1);
		strcpy(entries[*count].hex, line);
		(*count)++;
	}
	fclose(f);
	return entries;
}

static void write_index(index_entry_t *entries, int count)
{
	int i;
	FILE *f = xfopen(".git/index.txt", "w");
	for (i = 0; i < count; i++) {
		fprintf(f, "%s %s\n", entries[i].hex, entries[i].path);
	}
	fclose(f);
}

static void free_index(index_entry_t *entries, int count)
{
	int i;
	for (i = 0; i < count; i++)
		free(entries[i].path);
	free(entries);
}

/* --- Tree Object Handling (Recursive) --- */
typedef struct {
	char mode[8];
	char *name;
	uint8_t sha1[20];
	int is_dir;
} tree_child_t;

typedef struct {
	char *path;
	char hex[41];
	char mode[8];
} flat_tree_entry_t;

static int tree_entry_cmp(const char *name1, int is_dir1, const char *name2, int is_dir2)
{
	size_t len1 = strlen(name1);
	size_t len2 = strlen(name2);
	size_t min_len = len1 < len2 ? len1 : len2;
	int cmp = memcmp(name1, name2, min_len);
	if (cmp != 0)
		return cmp;
	if (len1 == len2)
		return 0;
	if (len1 < len2) {
		int c1 = is_dir1 ? '/' : '\0';
		int c2 = (unsigned char)name2[len1];
		return c1 - c2;
	} else {
		int c1 = (unsigned char)name1[len2];
		int c2 = is_dir2 ? '/' : '\0';
		return c1 - c2;
	}
}

static int compare_tree_children(const void *a, const void *b)
{
	const tree_child_t *c1 = (const tree_child_t *)a;
	const tree_child_t *c2 = (const tree_child_t *)b;
	return tree_entry_cmp(c1->name, c1->is_dir, c2->name, c2->is_dir);
}

static void build_tree_recursive(index_entry_t *entries, int count, const char *prefix, char out_hex[41])
{
	tree_child_t *children = NULL;
	int num_children = 0;
	size_t plen = strlen(prefix);
	size_t total_size = 0;
	size_t pos = 0;
	uint8_t *buf;
	int i;

	for (i = 0; i < count; i++) {
		const char *p = entries[i].path;
		const char *rem;
		char *slash;

		if (strncmp(p, prefix, plen) != 0)
			continue;
		rem = p + plen;
		slash = strchr(rem, '/');

		if (!slash) {
			/* Direct file entry */
			struct stat st;
			children = xrealloc_vector(children, 4, num_children);
			if (stat(entries[i].path, &st) == 0 && (st.st_mode & 0111))
				strcpy(children[num_children].mode, "100755");
			else
				strcpy(children[num_children].mode, "100644");
			children[num_children].name = xstrdup(rem);
			children[num_children].is_dir = 0;
			hex2bin((char*)children[num_children].sha1, entries[i].hex, 20);
			num_children++;
		} else {
			/* Subdirectory */
			char *dirname = xstrndup(rem, slash - rem);
			int j, found = 0;
			for (j = 0; j < num_children; j++) {
				if (children[j].is_dir && strcmp(children[j].name, dirname) == 0) {
					found = 1;
					break;
				}
			}
			if (!found) {
				char *sub_prefix = xasprintf("%s%s/", prefix, dirname);
				char sub_tree_hex[41];
				build_tree_recursive(entries, count, sub_prefix, sub_tree_hex);
				free(sub_prefix);

				children = xrealloc_vector(children, 4, num_children);
				strcpy(children[num_children].mode, "40000");
				children[num_children].name = dirname;
				children[num_children].is_dir = 1;
				hex2bin((char*)children[num_children].sha1, sub_tree_hex, 20);
				num_children++;
			} else {
				free(dirname);
			}
		}
	}

	qsort(children, num_children, sizeof(tree_child_t), compare_tree_children);

	for (i = 0; i < num_children; i++) {
		total_size += strlen(children[i].mode) + 1 + strlen(children[i].name) + 1 + 20;
	}

	buf = xmalloc(total_size);
	for (i = 0; i < num_children; i++) {
		int len = sprintf((char*)buf + pos, "%s %s", children[i].mode, children[i].name) + 1;
		pos += len;
		memcpy(buf + pos, children[i].sha1, 20);
		pos += 20;
		free(children[i].name);
	}
	free(children);

	git_write_object("tree", buf, pos, out_hex);
	free(buf);
}

static void read_tree_recursive(const char *tree_hex, const char *prefix, flat_tree_entry_t **out_entries, int *out_count)
{
	char *type = NULL;
	size_t sz = 0;
	uint8_t *content = git_read_object(tree_hex, &type, &sz);
	size_t pos = 0;

	if (!content || !type || strcmp(type, "tree") != 0) {
		free(content);
		free(type);
		return;
	}

	while (pos < sz) {
		char *sp = memchr((char*)content + pos, ' ', sz - pos);
		char *nul;
		char mode_buf[8];
		const char *name;
		char child_hex[41];
		char *full_path;
		int is_dir;

		if (!sp) break;
		nul = memchr(sp + 1, '\0', sz - (sp + 1 - (char*)content));
		if (!nul || nul + 1 + 20 > (char*)content + sz) break;

		safe_strncpy(mode_buf, (char*)content + pos, sizeof(mode_buf));
		if (sp - ((char*)content + pos) < (int)sizeof(mode_buf))
			mode_buf[sp - ((char*)content + pos)] = '\0';
		name = sp + 1;

		bin2hex(child_hex, nul + 1, 20);
		child_hex[40] = '\0';

		full_path = xasprintf("%s%s", prefix, name);
		is_dir = (strcmp(mode_buf, "40000") == 0 || strcmp(mode_buf, "040000") == 0);
		if (is_dir) {
			char *sub_prefix = xasprintf("%s/", full_path);
			read_tree_recursive(child_hex, sub_prefix, out_entries, out_count);
			free(sub_prefix);
			free(full_path);
		} else {
			*out_entries = xrealloc_vector(*out_entries, 4, *out_count);
			(*out_entries)[*out_count].path = full_path;
			strcpy((*out_entries)[*out_count].hex, child_hex);
			safe_strncpy((*out_entries)[*out_count].mode, mode_buf, sizeof((*out_entries)[*out_count].mode));
			(*out_count)++;
		}
		pos = (nul - (char*)content) + 1 + 20;
	}

	free(content);
	free(type);
}

/* --- Ref Resolution and Graph Helpers --- */
static char *git_resolve_ref(const char *ref)
{
	char path[128];
	FILE *f;
	char line[128];

	if (!ref || !*ref)
		return NULL;

	if (strcmp(ref, "HEAD") == 0) {
		f = fopen(".git/HEAD", "r");
		if (!f) return NULL;
		if (fgets(line, sizeof(line), f)) {
			fclose(f);
			chomp(line);
			if (is_prefixed_with(line, "ref: "))
				return git_resolve_ref(line + 5);
			if (is_valid_sha1(line))
				return xstrdup(line);
		} else {
			fclose(f);
		}
		return NULL;
	}

	if (is_valid_sha1(ref))
		return xstrdup(ref);

	snprintf(path, sizeof(path), ".git/%s", ref);
	f = fopen(path, "r");
	if (!f) {
		snprintf(path, sizeof(path), ".git/refs/heads/%s", ref);
		f = fopen(path, "r");
	}
	if (!f) {
		snprintf(path, sizeof(path), ".git/refs/tags/%s", ref);
		f = fopen(path, "r");
	}
	if (!f) {
		snprintf(path, sizeof(path), ".git/refs/remotes/%s", ref);
		f = fopen(path, "r");
	}
	if (!f) {
		snprintf(path, sizeof(path), ".git/refs/remotes/origin/%s", ref);
		f = fopen(path, "r");
	}

	if (f) {
		if (fgets(line, sizeof(line), f)) {
			fclose(f);
			chomp(line);
			if (is_valid_sha1(line))
				return xstrdup(line);
			if (is_prefixed_with(line, "ref: "))
				return git_resolve_ref(line + 5);
		} else {
			fclose(f);
		}
	}

	if (strlen(ref) >= 4) {
		int i, is_hex = 1;
		for (i = 0; ref[i]; i++) {
			if (!isxdigit(ref[i])) {
				is_hex = 0;
				break;
			}
		}
		if (is_hex) {
			char obj_dir[64];
			DIR *d;
			snprintf(obj_dir, sizeof(obj_dir), ".git/objects/%.2s", ref);
			d = opendir(obj_dir);
			if (d) {
				struct dirent *de;
				char *match = NULL;
				int matches = 0;
				while ((de = readdir(d)) != NULL) {
					if (de->d_name[0] == '.') continue;
					if (is_prefixed_with(de->d_name, ref + 2)) {
						matches++;
						free(match);
						match = xasprintf("%.2s%s", ref, de->d_name);
					}
				}
				closedir(d);
				if (matches == 1 && is_valid_sha1(match))
					return match;
				free(match);
			}
		}
	}

	return NULL;
}

static char *git_current_branch(void)
{
	char line[128];
	FILE *f = fopen(".git/HEAD", "r");
	if (!f) return NULL;
	if (!fgets(line, sizeof(line), f)) {
		fclose(f);
		return NULL;
	}
	fclose(f);
	chomp(line);
	if (is_prefixed_with(line, "ref: refs/heads/"))
		return xstrdup(line + 16);
	return NULL;
}

static char *git_get_head_commit(void)
{
	return git_resolve_ref("HEAD");
}

static char *get_commit_tree_sha(const char *commit_sha)
{
	char *type = NULL;
	size_t sz = 0;
	char *data = (char*)git_read_object(commit_sha, &type, &sz);
	char *tree_sha = NULL;

	if (data && type && strcmp(type, "commit") == 0) {
		char *p = data;
		while (p && *p) {
			char *nl = strchr(p, '\n');
			if (nl) *nl = '\0';
			if (is_prefixed_with(p, "tree ")) {
				tree_sha = xstrdup(p + 5);
				break;
			}
			if (!nl) break;
			p = nl + 1;
		}
	}
	free(data);
	free(type);
	return tree_sha;
}

static char *get_commit_subject(const char *commit_sha)
{
	char *type = NULL;
	size_t sz = 0;
	char *data = (char*)git_read_object(commit_sha, &type, &sz);
	char *subject = NULL;

	if (data && type && strcmp(type, "commit") == 0) {
		char *p = data;
		int in_body = 0;
		while (p && *p) {
			char *nl = strchr(p, '\n');
			if (nl) *nl = '\0';
			if (in_body) {
				if (*p) {
					subject = xstrdup(p);
					break;
				}
			} else if (*p == '\0') {
				in_body = 1;
			}
			if (!nl) break;
			p = nl + 1;
		}
	}
	free(data);
	free(type);
	return subject ? subject : xstrdup("");
}

static int is_ancestor(const char *ancestor_hex, const char *descendant_hex)
{
	char *curr;

	if (!ancestor_hex || !descendant_hex)
		return 0;
	if (strcmp(ancestor_hex, descendant_hex) == 0)
		return 1;

	curr = xstrdup(descendant_hex);
	while (curr) {
		char *type = NULL;
		size_t sz = 0;
		char *data = (char*)git_read_object(curr, &type, &sz);
		char *next_parent = NULL;
		char *line;

		if (!data || !type || strcmp(type, "commit") != 0) {
			free(data);
			free(type);
			free(curr);
			break;
		}

		line = data;
		while (line && *line) {
			char *nl = strchr(line, '\n');
			if (nl) *nl = '\0';
			if (is_prefixed_with(line, "parent ")) {
				char *parent_sha = line + 7;
				if (strcmp(parent_sha, ancestor_hex) == 0) {
					free(data);
					free(type);
					free(curr);
					return 1;
				}
				if (!next_parent)
					next_parent = xstrdup(parent_sha);
			} else if (*line == '\0') {
				break;
			}
			if (!nl) break;
			line = nl + 1;
		}

		free(data);
		free(type);
		free(curr);
		curr = next_parent;
	}
	return 0;
}

/* --- Myers Diff Algorithm --- */
typedef struct {
	char **lines;
	int count;
} line_array_t;

typedef struct {
	char type;
	const char *line;
} diff_edit_t;

static line_array_t split_lines(const char *buf, size_t len)
{
	line_array_t la;
	size_t i = 0, start = 0;
	la.lines = NULL;
	la.count = 0;

	if (!buf || len == 0)
		return la;

	while (i < len) {
		if (buf[i] == '\n') {
			la.lines = xrealloc_vector(la.lines, 4, la.count);
			la.lines[la.count++] = xstrndup(buf + start, i - start);
			start = i + 1;
		}
		i++;
	}
	if (start < len) {
		la.lines = xrealloc_vector(la.lines, 4, la.count);
		la.lines[la.count++] = xstrndup(buf + start, len - start);
	}
	return la;
}

static void free_lines(line_array_t *la)
{
	int i;
	for (i = 0; i < la->count; i++)
		free(la->lines[i]);
	free(la->lines);
	la->lines = NULL;
	la->count = 0;
}

static int diff_buffers(const char *buf_a, size_t len_a, const char *name_a,
                        const char *buf_b, size_t len_b, const char *name_b,
                        FILE *out, int use_color_flag)
{
	line_array_t la, lb;
	int N, M, MAX, i, d, k, found_d;
	int *V;
	int **v_history;
	diff_edit_t *rev_edits;
	diff_edit_t *edits;
	int rev_count, edit_count;
	int cur_x, cur_y;
	uint8_t *mark;
	int has_changes, old_lineno, new_lineno, idx;

	la = split_lines(buf_a, len_a);
	lb = split_lines(buf_b, len_b);

	if (la.count == lb.count) {
		int same = 1;
		for (i = 0; i < la.count; i++) {
			if (strcmp(la.lines[i], lb.lines[i]) != 0) {
				same = 0;
				break;
			}
		}
		if (same) {
			free_lines(&la);
			free_lines(&lb);
			return 0;
		}
	}

	N = la.count;
	M = lb.count;
	MAX = N + M;

	if (MAX == 0) {
		free_lines(&la);
		free_lines(&lb);
		return 0;
	}

	found_d = -1;
	V = xzalloc((2 * MAX + 1) * sizeof(int));
	v_history = xmalloc((MAX + 1) * sizeof(int*));

	for (d = 0; d <= MAX && d <= 4000; d++) {
		v_history[d] = xmalloc((2 * d + 1) * sizeof(int));
		for (k = -d; k <= d; k += 2) {
			int x, y;
			if (k == -d || (k != d && V[k - 1 + MAX] < V[k + 1 + MAX]))
				x = V[k + 1 + MAX];
			else
				x = V[k - 1 + MAX] + 1;
			y = x - k;
			while (x < N && y < M && strcmp(la.lines[x], lb.lines[y]) == 0) {
				x++;
				y++;
			}
			V[k + MAX] = x;
			v_history[d][k + d] = x;
			if (x >= N && y >= M) {
				found_d = d;
				break;
			}
		}
		if (found_d >= 0)
			break;
	}

	rev_edits = xmalloc((N + M + 1) * sizeof(diff_edit_t));
	rev_count = 0;
	cur_x = N;
	cur_y = M;

	if (found_d >= 0) {
		for (d = found_d; d > 0; d--) {
			int cur_k = cur_x - cur_y;
			int prev_k;
			int prev_x, prev_y;

			if (cur_k == -d || (cur_k != d && v_history[d - 1][cur_k - 1 + (d - 1)] < v_history[d - 1][cur_k + 1 + (d - 1)]))
				prev_k = cur_k + 1;
			else
				prev_k = cur_k - 1;

			prev_x = v_history[d - 1][prev_k + (d - 1)];
			prev_y = prev_x - prev_k;

			if (prev_k == cur_k + 1) {
				while (cur_x > prev_x && cur_y > prev_y + 1) {
					cur_x--; cur_y--;
					rev_edits[rev_count].type = ' ';
					rev_edits[rev_count].line = la.lines[cur_x];
					rev_count++;
				}
				cur_y--;
				rev_edits[rev_count].type = '+';
				rev_edits[rev_count].line = lb.lines[cur_y];
				rev_count++;
			} else {
				while (cur_x > prev_x + 1 && cur_y > prev_y) {
					cur_x--; cur_y--;
					rev_edits[rev_count].type = ' ';
					rev_edits[rev_count].line = la.lines[cur_x];
					rev_count++;
				}
				cur_x--;
				rev_edits[rev_count].type = '-';
				rev_edits[rev_count].line = la.lines[cur_x];
				rev_count++;
			}
		}
		while (cur_x > 0 && cur_y > 0) {
			cur_x--; cur_y--;
			rev_edits[rev_count].type = ' ';
			rev_edits[rev_count].line = la.lines[cur_x];
			rev_count++;
		}
		for (d = 0; d <= found_d; d++)
			free(v_history[d]);
	} else {
		for (i = M - 1; i >= 0; i--) {
			rev_edits[rev_count].type = '+';
			rev_edits[rev_count].line = lb.lines[i];
			rev_count++;
		}
		for (i = N - 1; i >= 0; i--) {
			rev_edits[rev_count].type = '-';
			rev_edits[rev_count].line = la.lines[i];
			rev_count++;
		}
	}
	free(v_history);
	free(V);

	edits = xmalloc(rev_count * sizeof(diff_edit_t));
	edit_count = rev_count;
	for (i = 0; i < rev_count; i++)
		edits[i] = rev_edits[rev_count - 1 - i];
	free(rev_edits);

	has_changes = 0;
	for (i = 0; i < edit_count; i++) {
		if (edits[i].type != ' ') {
			has_changes = 1;
			break;
		}
	}

	if (!has_changes) {
		free(edits);
		free_lines(&la);
		free_lines(&lb);
		return 0;
	}

	if (use_color_flag) {
		fprintf(out, "\033[1mdiff --git a/%s b/%s\033[0m\n", name_a, name_b);
		fprintf(out, "\033[1m--- a/%s\033[0m\n", name_a);
		fprintf(out, "\033[1m+++ b/%s\033[0m\n", name_b);
	} else {
		fprintf(out, "diff --git a/%s b/%s\n", name_a, name_b);
		fprintf(out, "--- a/%s\n", name_a);
		fprintf(out, "+++ b/%s\n", name_b);
	}

	mark = xzalloc(edit_count);
	for (i = 0; i < edit_count; i++) {
		if (edits[i].type != ' ') {
			int j;
			int start_m = (i - 3 < 0) ? 0 : i - 3;
			int end_m = (i + 3 >= edit_count) ? edit_count - 1 : i + 3;
			for (j = start_m; j <= end_m; j++)
				mark[j] = 1;
		}
	}

	old_lineno = 1;
	new_lineno = 1;
	idx = 0;

	while (idx < edit_count) {
		int hunk_start, hunk_end, hunk_old_start, hunk_new_start, hunk_old_count, hunk_new_count, h;

		while (idx < edit_count && !mark[idx]) {
			if (edits[idx].type == ' ' || edits[idx].type == '-') old_lineno++;
			if (edits[idx].type == ' ' || edits[idx].type == '+') new_lineno++;
			idx++;
		}
		if (idx >= edit_count) break;

		hunk_start = idx;
		hunk_old_start = old_lineno;
		hunk_new_start = new_lineno;
		hunk_old_count = 0;
		hunk_new_count = 0;

		h = idx;
		while (h < edit_count && mark[h]) {
			if (edits[h].type == ' ' || edits[h].type == '-') hunk_old_count++;
			if (edits[h].type == ' ' || edits[h].type == '+') hunk_new_count++;
			h++;
		}
		hunk_end = h;

		if (use_color_flag)
			fprintf(out, "\033[36m@@ -%d,%d +%d,%d @@\033[0m\n", hunk_old_start, hunk_old_count, hunk_new_start, hunk_new_count);
		else
			fprintf(out, "@@ -%d,%d +%d,%d @@\n", hunk_old_start, hunk_old_count, hunk_new_start, hunk_new_count);

		for (h = hunk_start; h < hunk_end; h++) {
			if (edits[h].type == '-') {
				if (use_color_flag) fprintf(out, "\033[31m-%s\033[0m\n", edits[h].line);
				else fprintf(out, "-%s\n", edits[h].line);
				old_lineno++;
			} else if (edits[h].type == '+') {
				if (use_color_flag) fprintf(out, "\033[32m+%s\033[0m\n", edits[h].line);
				else fprintf(out, "+%s\n", edits[h].line);
				new_lineno++;
			} else {
				fprintf(out, " %s\n", edits[h].line);
				old_lineno++;
				new_lineno++;
			}
		}
		idx = hunk_end;
	}

	free(mark);
	free(edits);
	free_lines(&la);
	free_lines(&lb);
	return 1;
}

/* --- Remote Download Helpers --- */
static int download_single_object(const char *source, const char *hex)
{
	char obj_dir[64];
	char obj_path[128];
	char tmp_path[128];
	int ret;

	if (!is_valid_sha1(hex))
		return -1;

	snprintf(obj_dir, sizeof(obj_dir), ".git/objects/%.2s", hex);
	mkdir(obj_dir, 0777);
	snprintf(obj_path, sizeof(obj_path), "%s/%s", obj_dir, hex + 2);

	if (access(obj_path, F_OK) == 0)
		return 0;

	snprintf(tmp_path, sizeof(tmp_path), "%s/tmp_%u_%ld", obj_dir, (unsigned)getpid(), (long)random());

	if (is_prefixed_with(source, "http://") || is_prefixed_with(source, "https://")) {
		char *cmd = xasprintf("curl -s -f -o '%s' '%s/objects/%.2s/%s' 2>/dev/null || wget -q -O '%s' '%s/objects/%.2s/%s' 2>/dev/null",
		                      tmp_path, source, hex, hex + 2,
		                      tmp_path, source, hex, hex + 2);
		ret = system(cmd);
		free(cmd);
	} else {
		const char *src_clean = source;
		char *src_check;
		char *src_file;
		char *cmd;

		if (is_prefixed_with(src_clean, "file://"))
			src_clean += 7;

		src_check = xasprintf("%s/.git/objects", src_clean);
		if (access(src_check, F_OK) == 0)
			src_file = xasprintf("%s/.git/objects/%.2s/%s", src_clean, hex, hex + 2);
		else
			src_file = xasprintf("%s/objects/%.2s/%s", src_clean, hex, hex + 2);
		free(src_check);

		cmd = xasprintf("cp '%s' '%s' 2>/dev/null", src_file, tmp_path);
		ret = system(cmd);
		free(cmd);
		free(src_file);
	}

	if (ret == 0 && access(tmp_path, F_OK) == 0) {
		rename(tmp_path, obj_path);
		return 0;
	}
	unlink(tmp_path);
	return -1;
}

static void download_object_graph(const char *source, const char *hex)
{
	char *type = NULL;
	size_t sz = 0;
	uint8_t *content;

	if (download_single_object(source, hex) != 0)
		return;

	content = git_read_object(hex, &type, &sz);
	if (!content || !type) {
		free(content);
		free(type);
		return;
	}

	if (strcmp(type, "commit") == 0) {
		char *p = (char*)content;
		while (p && *p) {
			char *nl = strchr(p, '\n');
			if (nl) *nl = '\0';
			if (is_prefixed_with(p, "tree ")) {
				download_object_graph(source, p + 5);
			} else if (is_prefixed_with(p, "parent ")) {
				download_object_graph(source, p + 7);
			} else if (*p == '\0') {
				break;
			}
			if (!nl) break;
			p = nl + 1;
		}
	} else if (strcmp(type, "tree") == 0) {
		size_t pos = 0;
		while (pos < sz) {
			char *sp = memchr((char*)content + pos, ' ', sz - pos);
			char *nul;
			char child_hex[41];
			if (!sp) break;
			nul = memchr(sp + 1, '\0', sz - (sp + 1 - (char*)content));
			if (!nul || nul + 1 + 20 > (char*)content + sz) break;
			bin2hex(child_hex, nul + 1, 20);
			child_hex[40] = '\0';
			download_object_graph(source, child_hex);
			pos = (nul - (char*)content) + 1 + 20;
		}
	}

	free(content);
	free(type);
}

/* --- Subcommands --- */

static int git_init(char **argv)
{
	const char *dir = argv[0] ? argv[0] : ".";
	char buf[256];
	FILE *f;

	if (argv[0])
		mkdir(dir, 0777);
	snprintf(buf, sizeof(buf), "%s/.git", dir);
	mkdir(buf, 0777);
	snprintf(buf, sizeof(buf), "%s/.git/objects", dir);
	mkdir(buf, 0777);
	snprintf(buf, sizeof(buf), "%s/.git/refs", dir);
	mkdir(buf, 0777);
	snprintf(buf, sizeof(buf), "%s/.git/refs/heads", dir);
	mkdir(buf, 0777);
	snprintf(buf, sizeof(buf), "%s/.git/refs/tags", dir);
	mkdir(buf, 0777);
	snprintf(buf, sizeof(buf), "%s/.git/refs/remotes", dir);
	mkdir(buf, 0777);

	snprintf(buf, sizeof(buf), "%s/.git/HEAD", dir);
	f = fopen(buf, "w");
	if (f) {
		fputs("ref: refs/heads/main\n", f);
		fclose(f);
	}

	snprintf(buf, sizeof(buf), "%s/.git/config", dir);
	f = fopen(buf, "w");
	if (f) {
		fputs("[core]\n\trepositoryformatversion = 0\n\tfilemode = true\n\tbare = false\n", f);
		fclose(f);
	}

	printf("Initialized empty Git repository in %s/.git/\n", dir);
	return 0;
}

static void add_path_recursive(const char *path, char **patterns, int pcount, index_entry_t **entries, int *count)
{
	struct stat st;
	if (lstat(path, &st) != 0)
		return;

	if (is_ignored(path, S_ISDIR(st.st_mode), patterns, pcount))
		return;

	if (S_ISDIR(st.st_mode)) {
		DIR *d = opendir(path);
		struct dirent *de;
		if (!d) return;
		while ((de = readdir(d)) != NULL) {
			char *sub;
			if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
				continue;
			if (strcmp(path, ".") == 0)
				sub = xstrdup(de->d_name);
			else
				sub = xasprintf("%s/%s", path, de->d_name);
			add_path_recursive(sub, patterns, pcount, entries, count);
			free(sub);
		}
		closedir(d);
	} else if (S_ISREG(st.st_mode)) {
		int fd, found, i;
		char *buf;
		char hex[41];
		const char *idx_path = path;

		if (is_prefixed_with(idx_path, "./"))
			idx_path += 2;

		fd = open(path, O_RDONLY);
		if (fd < 0) return;
		buf = xmalloc(st.st_size);
		xread(fd, buf, st.st_size);
		close(fd);

		git_write_object("blob", buf, st.st_size, hex);
		free(buf);

		found = 0;
		for (i = 0; i < *count; i++) {
			if (strcmp((*entries)[i].path, idx_path) == 0) {
				strcpy((*entries)[i].hex, hex);
				found = 1;
				break;
			}
		}
		if (!found) {
			*entries = xrealloc_vector(*entries, 4, *count);
			(*entries)[*count].path = xstrdup(idx_path);
			strcpy((*entries)[*count].hex, hex);
			(*count)++;
		}
	}
}

static int git_add(char **argv)
{
	int count = 0;
	index_entry_t *entries;
	char **patterns;
	int pcount = 0;

	if (!argv[0])
		bb_show_usage();

	entries = read_index(&count);
	patterns = load_gitignore(&pcount);

	while (*argv) {
		const char *path = *argv++;
		struct stat st;
		if (lstat(path, &st) != 0) {
			bb_perror_msg("fatal: pathspec '%s' did not match any files", path);
			free_index(entries, count);
			return 1;
		}
		add_path_recursive(path, patterns, pcount, &entries, &count);
	}

	write_index(entries, count);
	free_index(entries, count);
	if (patterns) {
		int i;
		for (i = 0; i < pcount; i++) free(patterns[i]);
		free(patterns);
	}
	return 0;
}

static void scan_untracked(const char *dir_path, char **patterns, int pcount,
                           index_entry_t *entries, int index_count,
                           char ***untracked, int *untracked_count)
{
	DIR *d = opendir(dir_path);
	struct dirent *de;

	if (!d) return;

	while ((de = readdir(d)) != NULL) {
		char *rel_path;
		struct stat st;

		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;

		if (strcmp(dir_path, ".") == 0)
			rel_path = xstrdup(de->d_name);
		else
			rel_path = xasprintf("%s/%s", dir_path, de->d_name);

		if (lstat(rel_path, &st) == 0) {
			if (is_ignored(rel_path, S_ISDIR(st.st_mode), patterns, pcount)) {
				free(rel_path);
				continue;
			}
			if (S_ISDIR(st.st_mode)) {
				scan_untracked(rel_path, patterns, pcount, entries, index_count, untracked, untracked_count);
			} else if (S_ISREG(st.st_mode)) {
				int i, in_index = 0;
				for (i = 0; i < index_count; i++) {
					if (strcmp(entries[i].path, rel_path) == 0) {
						in_index = 1;
						break;
					}
				}
				if (!in_index) {
					*untracked = xrealloc_vector(*untracked, 4, *untracked_count);
					(*untracked)[*untracked_count] = rel_path;
					(*untracked_count)++;
					continue;
				}
			}
		}
		free(rel_path);
	}
	closedir(d);
}

static int git_status(void)
{
	int index_count = 0, head_count = 0, pcount = 0, untracked_count = 0;
	index_entry_t *entries;
	flat_tree_entry_t *head_entries = NULL;
	char **patterns;
	char **untracked = NULL;
	char *head;
	char *br;
	int use_color_flag;
	int i;
	char **staged_new = NULL; int snew_c = 0;
	char **staged_mod = NULL; int smod_c = 0;
	char **staged_del = NULL; int sdel_c = 0;
	char **unstaged_mod = NULL; int umod_c = 0;
	char **unstaged_del = NULL; int udel_c = 0;

	br = git_current_branch();
	head = git_get_head_commit();
	use_color_flag = isatty(STDOUT_FILENO);

	if (br) {
		printf("On branch %s\n", br);
		free(br);
	} else if (head) {
		printf("HEAD detached at %.7s\n", head);
	} else {
		printf("On branch main\n");
	}

	if (!head) {
		printf("\nNo commits yet\n\n");
	} else {
		char *tree_hex = get_commit_tree_sha(head);
		if (tree_hex) {
			read_tree_recursive(tree_hex, "", &head_entries, &head_count);
			free(tree_hex);
		}
	}

	entries = read_index(&index_count);

	/* Staged changes */
	for (i = 0; i < index_count; i++) {
		int j, found = 0;
		for (j = 0; j < head_count; j++) {
			if (strcmp(entries[i].path, head_entries[j].path) == 0) {
				found = 1;
				if (strcmp(entries[i].hex, head_entries[j].hex) != 0) {
					staged_mod = xrealloc_vector(staged_mod, 4, smod_c);
					staged_mod[smod_c++] = entries[i].path;
				}
				break;
			}
		}
		if (!found) {
			staged_new = xrealloc_vector(staged_new, 4, snew_c);
			staged_new[snew_c++] = entries[i].path;
		}
	}
	for (i = 0; i < head_count; i++) {
		int j, found = 0;
		for (j = 0; j < index_count; j++) {
			if (strcmp(head_entries[i].path, entries[j].path) == 0) {
				found = 1;
				break;
			}
		}
		if (!found) {
			staged_del = xrealloc_vector(staged_del, 4, sdel_c);
			staged_del[sdel_c++] = head_entries[i].path;
		}
	}

	/* Unstaged changes */
	for (i = 0; i < index_count; i++) {
		struct stat st;
		if (lstat(entries[i].path, &st) != 0) {
			unstaged_del = xrealloc_vector(unstaged_del, 4, udel_c);
			unstaged_del[udel_c++] = entries[i].path;
		} else {
			char cur_hex[41];
			if (calc_blob_sha1(entries[i].path, cur_hex) == 0) {
				if (strcmp(cur_hex, entries[i].hex) != 0) {
					unstaged_mod = xrealloc_vector(unstaged_mod, 4, umod_c);
					unstaged_mod[umod_c++] = entries[i].path;
				}
			}
		}
	}

	/* Untracked files */
	patterns = load_gitignore(&pcount);
	scan_untracked(".", patterns, pcount, entries, index_count, &untracked, &untracked_count);

	if (snew_c + smod_c + sdel_c > 0) {
		printf("Changes to be committed:\n");
		printf("  (use \"git restore --staged <file>...\" to unstage)\n");
		for (i = 0; i < snew_c; i++) {
			if (use_color_flag) printf("\t\033[32mnew file:   %s\033[0m\n", staged_new[i]);
			else printf("\tnew file:   %s\n", staged_new[i]);
		}
		for (i = 0; i < smod_c; i++) {
			if (use_color_flag) printf("\t\033[32mmodified:   %s\033[0m\n", staged_mod[i]);
			else printf("\tmodified:   %s\n", staged_mod[i]);
		}
		for (i = 0; i < sdel_c; i++) {
			if (use_color_flag) printf("\t\033[32mdeleted:    %s\033[0m\n", staged_del[i]);
			else printf("\tdeleted:    %s\n", staged_del[i]);
		}
		printf("\n");
	}

	if (umod_c + udel_c > 0) {
		printf("Changes not staged for commit:\n");
		printf("  (use \"git add <file>...\" to update what will be committed)\n");
		for (i = 0; i < umod_c; i++) {
			if (use_color_flag) printf("\t\033[31mmodified:   %s\033[0m\n", unstaged_mod[i]);
			else printf("\tmodified:   %s\n", unstaged_mod[i]);
		}
		for (i = 0; i < udel_c; i++) {
			if (use_color_flag) printf("\t\033[31mdeleted:    %s\033[0m\n", unstaged_del[i]);
			else printf("\tdeleted:    %s\n", unstaged_del[i]);
		}
		printf("\n");
	}

	if (untracked_count > 0) {
		printf("Untracked files:\n");
		printf("  (use \"git add <file>...\" to include in what will be committed)\n");
		for (i = 0; i < untracked_count; i++) {
			if (use_color_flag) printf("\t\033[31m%s\033[0m\n", untracked[i]);
			else printf("\t%s\n", untracked[i]);
			free(untracked[i]);
		}
		free(untracked);
		printf("\n");
	}

	if (snew_c + smod_c + sdel_c == 0 && umod_c + udel_c == 0 && untracked_count == 0) {
		printf("nothing to commit, working tree clean\n");
	}

	free(staged_new);
	free(staged_mod);
	free(staged_del);
	free(unstaged_mod);
	free(unstaged_del);

	for (i = 0; i < head_count; i++) free(head_entries[i].path);
	free(head_entries);
	free_index(entries, index_count);
	free(head);
	if (patterns) {
		for (i = 0; i < pcount; i++) free(patterns[i]);
		free(patterns);
	}
	return 0;
}

static int git_commit(int argc, char **argv)
{
	const char *msg = NULL;
	int i, count = 0;
	index_entry_t *entries;
	char tree_hex[41], commit_hex[41], ref_path[128];
	char *parent, *br, *commit_body, *author_name, *author_email;
	time_t now;
	FILE *f;

	for (i = 0; i < argc; i++) {
		if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
			msg = argv[++i];
		}
	}
	if (!msg) {
		bb_error_msg("fatal: commit message required (-m)");
		return 1;
	}

	entries = read_index(&count);
	if (count == 0) {
		printf("nothing to commit, working tree clean\n");
		return 0;
	}

	build_tree_recursive(entries, count, "", tree_hex);
	free_index(entries, count);

	parent = git_get_head_commit();
	now = time(NULL);
	author_name = git_get_author_name();
	author_email = git_get_author_email();

	if (parent) {
		commit_body = xasprintf(
			"tree %s\n"
			"parent %s\n"
			"author %s <%s> %ld +0000\n"
			"committer %s <%s> %ld +0000\n\n"
			"%s\n",
			tree_hex, parent, author_name, author_email, (long)now,
			author_name, author_email, (long)now, msg);
	} else {
		commit_body = xasprintf(
			"tree %s\n"
			"author %s <%s> %ld +0000\n"
			"committer %s <%s> %ld +0000\n\n"
			"%s\n",
			tree_hex, author_name, author_email, (long)now,
			author_name, author_email, (long)now, msg);
	}

	git_write_object("commit", commit_body, strlen(commit_body), commit_hex);
	free(commit_body);
	free(author_name);
	free(author_email);

	br = git_current_branch();
	if (br) {
		snprintf(ref_path, sizeof(ref_path), ".git/refs/heads/%s", br);
		f = xfopen(ref_path, "w");
		fprintf(f, "%s\n", commit_hex);
		fclose(f);
	} else {
		f = xfopen(".git/HEAD", "w");
		fprintf(f, "%s\n", commit_hex);
		fclose(f);
	}

	printf("[%s %s] %s\n", br ? br : "detached", commit_hex, msg);
	free(br);
	free(parent);
	return 0;
}

static int git_diff(int argc, char **argv)
{
	int count = 0;
	index_entry_t *entries = read_index(&count);
	int use_color_flag = isatty(STDOUT_FILENO);
	int i;

	for (i = 0; i < count; i++) {
		struct stat st;
		int match = (argc == 0);
		int a;

		for (a = 0; a < argc; a++) {
			if (strcmp(argv[a], entries[i].path) == 0) {
				match = 1;
				break;
			}
		}
		if (!match) continue;

		if (lstat(entries[i].path, &st) == 0) {
			int fd = open(entries[i].path, O_RDONLY);
			if (fd >= 0) {
				char *disk_buf = xmalloc(st.st_size);
				size_t obj_sz = 0;
				char *obj_buf = (char*)git_read_object(entries[i].hex, NULL, &obj_sz);
				xread(fd, disk_buf, st.st_size);
				close(fd);

				diff_buffers(obj_buf ? obj_buf : "", obj_sz, entries[i].path,
				             disk_buf, st.st_size, entries[i].path,
				             stdout, use_color_flag);
				free(obj_buf);
				free(disk_buf);
			}
		} else {
			/* Deleted unstaged */
			size_t obj_sz = 0;
			char *obj_buf = (char*)git_read_object(entries[i].hex, NULL, &obj_sz);
			diff_buffers(obj_buf ? obj_buf : "", obj_sz, entries[i].path,
			             "", 0, "/dev/null",
			             stdout, use_color_flag);
			free(obj_buf);
		}
	}

	free_index(entries, count);
	return 0;
}

static int git_log(int argc, char **argv)
{
	int limit = 10, shown = 0, oneline = 0, i;
	char *curr;
	int use_color_flag = isatty(STDOUT_FILENO);

	for (i = 0; i < argc; i++) {
		if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
			limit = bb_strtou(argv[++i], NULL, 10);
		} else if (strcmp(argv[i], "--oneline") == 0) {
			oneline = 1;
		}
	}

	curr = git_get_head_commit();
	if (!curr) {
		bb_error_msg("fatal: your current branch does not have any commits yet");
		return 1;
	}

	while (curr && shown < limit) {
		char *type = NULL;
		size_t sz = 0;
		char *data = (char*)git_read_object(curr, &type, &sz);
		char *next_parent = NULL;
		char *line;
		char *author = NULL;
		char *subject = NULL;

		if (!data || !type || strcmp(type, "commit") != 0) {
			free(type);
			free(data);
			break;
		}

		line = data;
		while (line && *line) {
			char *nl = strchr(line, '\n');
			if (nl) *nl = '\0';
			if (is_prefixed_with(line, "parent ") && !next_parent) {
				next_parent = xstrdup(line + 7);
			} else if (is_prefixed_with(line, "author ") && !author) {
				author = xstrdup(line + 7);
			} else if (*line == '\0') {
				if (nl && *(nl + 1)) {
					char *sub_end = strchr(nl + 1, '\n');
					if (sub_end) *sub_end = '\0';
					subject = xstrdup(nl + 1);
				}
				break;
			}
			if (!nl) break;
			line = nl + 1;
		}

		if (oneline) {
			if (use_color_flag)
				printf("\033[33m%.7s\033[0m %s\n", curr, subject ? subject : "");
			else
				printf("%.7s %s\n", curr, subject ? subject : "");
		} else {
			if (use_color_flag)
				printf("\033[33mcommit %s\033[0m\n", curr);
			else
				printf("commit %s\n", curr);
			if (author)
				printf("Author: %s\n", author);
			printf("\n    %s\n\n", subject ? subject : "");
		}

		free(author);
		free(subject);
		free(type);
		free(data);
		free(curr);
		curr = next_parent;
		shown++;
	}
	free(curr);
	return 0;
}

static int git_branch(int argc, char **argv)
{
	const char *name;
	char *head;
	char ref[128];
	FILE *f;

	if (argc == 0) {
		char *cur = git_current_branch();
		DIR *d = opendir(".git/refs/heads");
		int use_color_flag = isatty(STDOUT_FILENO);
		if (d) {
			struct dirent *de;
			while ((de = readdir(d)) != NULL) {
				if (de->d_name[0] == '.') continue;
				if (cur && strcmp(cur, de->d_name) == 0) {
					if (use_color_flag)
						printf("* \033[32m%s\033[0m\n", de->d_name);
					else
						printf("* %s\n", de->d_name);
				} else {
					printf("  %s\n", de->d_name);
				}
			}
			closedir(d);
		}
		free(cur);
		return 0;
	}

	if (strcmp(argv[0], "-d") == 0 || strcmp(argv[0], "-D") == 0) {
		if (argc < 2) bb_show_usage();
		snprintf(ref, sizeof(ref), ".git/refs/heads/%s", argv[1]);
		if (unlink(ref) == 0)
			printf("Deleted branch %s\n", argv[1]);
		else
			bb_perror_msg("error: branch '%s' not found", argv[1]);
		return 0;
	}

	name = argv[0];
	head = git_get_head_commit();
	if (!head) {
		bb_error_msg("fatal: not a valid object name: 'HEAD'");
		return 1;
	}
	snprintf(ref, sizeof(ref), ".git/refs/heads/%s", name);
	f = xfopen(ref, "w");
	fprintf(f, "%s\n", head);
	fclose(f);
	free(head);
	return 0;
}

static int git_checkout(char **argv)
{
	const char *target;
	char *target_commit;
	char ref[128];
	int is_branch = 0;
	FILE *f;
	flat_tree_entry_t *target_entries = NULL;
	int target_count = 0;
	int old_count = 0;
	index_entry_t *old_entries;
	char *tree_hex;
	int i;

	if (!argv[0])
		bb_show_usage();

	if (strcmp(argv[0], "-b") == 0) {
		if (!argv[1]) bb_show_usage();
		{
			char *br_argv[2];
			br_argv[0] = argv[1];
			br_argv[1] = NULL;
			if (git_branch(1, br_argv) != 0)
				return 1;
			target = argv[1];
		}
	} else {
		target = argv[0];
	}

	snprintf(ref, sizeof(ref), ".git/refs/heads/%s", target);
	if (access(ref, F_OK) == 0) {
		is_branch = 1;
		target_commit = git_resolve_ref(target);
	} else {
		target_commit = git_resolve_ref(target);
	}

	if (!target_commit) {
		bb_error_msg("error: pathspec '%s' did not match any file(s) known to git", target);
		return 1;
	}

	tree_hex = get_commit_tree_sha(target_commit);
	if (!tree_hex) {
		bb_error_msg("fatal: not a valid commit object: %s", target_commit);
		free(target_commit);
		return 1;
	}

	read_tree_recursive(tree_hex, "", &target_entries, &target_count);
	free(tree_hex);

	/* Delete old files not in target commit */
	old_entries = read_index(&old_count);
	for (i = 0; i < old_count; i++) {
		int j, found = 0;
		for (j = 0; j < target_count; j++) {
			if (strcmp(old_entries[i].path, target_entries[j].path) == 0) {
				found = 1;
				break;
			}
		}
		if (!found)
			unlink(old_entries[i].path);
	}
	free_index(old_entries, old_count);

	/* Write files from target commit */
	for (i = 0; i < target_count; i++) {
		size_t sz = 0;
		uint8_t *content = git_read_object(target_entries[i].hex, NULL, &sz);
		if (content) {
			char *slash = strrchr(target_entries[i].path, '/');
			int mode = target_entries[i].mode[0] ? (int)bb_strtou(target_entries[i].mode, NULL, 8) : 0644;
			int fd;
			if (mode == 0) mode = 0644;
			if (slash) {
				char *dir = xstrndup(target_entries[i].path, slash - target_entries[i].path);
				bb_make_directory(dir, 0777, FILEUTILS_RECUR);
				free(dir);
			}
			fd = open(target_entries[i].path, O_WRONLY | O_CREAT | O_TRUNC, mode);
			if (fd >= 0) {
				xwrite(fd, content, sz);
				close(fd);
				chmod(target_entries[i].path, mode);
			}
			free(content);
		}
	}

	/* Write target entries to .git/index.txt */
	f = xfopen(".git/index.txt", "w");
	for (i = 0; i < target_count; i++) {
		fprintf(f, "%s %s\n", target_entries[i].hex, target_entries[i].path);
		free(target_entries[i].path);
	}
	fclose(f);
	free(target_entries);

	/* Update .git/HEAD */
	f = xfopen(".git/HEAD", "w");
	if (is_branch) {
		fprintf(f, "ref: refs/heads/%s\n", target);
		printf("Switched to branch '%s'\n", target);
	} else {
		fprintf(f, "%s\n", target_commit);
		printf("Note: switching to '%s'.\nHEAD is now at %.7s\n", target, target_commit);
	}
	fclose(f);

	free(target_commit);
	return 0;
}

static int git_cat_file(int argc, char **argv)
{
	const char *mode, *hex;
	char *resolved;
	char *type = NULL;
	size_t sz = 0;
	uint8_t *content;

	if (argc < 2)
		bb_show_usage();
	mode = argv[0];
	hex = argv[1];

	resolved = git_resolve_ref(hex);
	if (!resolved) {
		bb_error_msg("fatal: Not a valid object name %s", hex);
		return 1;
	}

	content = git_read_object(resolved, &type, &sz);
	free(resolved);
	if (!content) {
		bb_error_msg("fatal: Not a valid object name %s", hex);
		return 1;
	}

	if (strcmp(mode, "-t") == 0) {
		puts(type);
	} else if (strcmp(mode, "-s") == 0) {
		printf("%zu\n", sz);
	} else if (strcmp(mode, "-p") == 0) {
		if (strcmp(type, "tree") == 0) {
			size_t pos = 0;
			while (pos < sz) {
				char *sp = memchr((char*)content + pos, ' ', sz - pos);
				char *nul;
				char mode_str[8];
				char hex_str[41];

				if (!sp) break;
				nul = memchr(sp + 1, '\0', sz - (sp + 1 - (char*)content));
				if (!nul || nul + 1 + 20 > (char*)content + sz) break;

				safe_strncpy(mode_str, (char*)content + pos, sizeof(mode_str));
				if (sp - ((char*)content + pos) < (int)sizeof(mode_str))
					mode_str[sp - ((char*)content + pos)] = '\0';

				bin2hex(hex_str, nul + 1, 20);
				hex_str[40] = '\0';

				if (strcmp(mode_str, "40000") == 0 || strcmp(mode_str, "040000") == 0) {
					printf("040000 tree %s\t%s\n", hex_str, sp + 1);
				} else {
					printf("%06u blob %s\t%s\n", (unsigned)bb_strtou(mode_str, NULL, 8), hex_str, sp + 1);
				}
				pos = (nul - (char*)content) + 1 + 20;
			}
		} else {
			full_write(STDOUT_FILENO, content, sz);
		}
	}
	free(type);
	free(content);
	return 0;
}

static int git_hash_object(int argc, char **argv)
{
	int write_obj = 0, i, fd;
	const char *file = NULL;
	struct stat st;
	char *buf;
	char hex[41];

	for (i = 0; i < argc; i++) {
		if (strcmp(argv[i], "-w") == 0) write_obj = 1;
		else file = argv[i];
	}
	if (!file) bb_show_usage();

	fd = open(file, O_RDONLY);
	if (fd < 0) {
		bb_perror_msg_and_die("fatal: Cannot open '%s'", file);
	}
	fstat(fd, &st);
	buf = xmalloc(st.st_size);
	xread(fd, buf, st.st_size);
	close(fd);

	if (write_obj) {
		git_write_object("blob", buf, st.st_size, hex);
	} else {
		char header[64];
		int hdr_len = sprintf(header, "blob %zu", (size_t)st.st_size) + 1;
		size_t total = hdr_len + st.st_size;
		char *raw = xmalloc(total);
		memcpy(raw, header, hdr_len);
		memcpy(raw + hdr_len, buf, st.st_size);
		calc_sha1(raw, total, NULL, hex);
		free(raw);
	}
	free(buf);
	puts(hex);
	return 0;
}

static int git_merge(char **argv)
{
	const char *target_ref_name;
	char *target_sha;
	char *head_sha;
	char *cur_br;

	if (!argv[0])
		bb_show_usage();

	target_ref_name = argv[0];
	target_sha = git_resolve_ref(target_ref_name);
	if (!target_sha) {
		bb_error_msg("fatal: %s - not something we can merge", target_ref_name);
		return 1;
	}

	head_sha = git_get_head_commit();
	if (!head_sha) {
		bb_error_msg("fatal: no commits yet on current branch");
		free(target_sha);
		return 1;
	}

	if (strcmp(head_sha, target_sha) == 0) {
		printf("Already up to date.\n");
		free(head_sha);
		free(target_sha);
		return 0;
	}

	if (is_ancestor(head_sha, target_sha)) {
		cur_br = git_current_branch();
		if (cur_br) {
			char *ref_path = xasprintf(".git/refs/heads/%s", cur_br);
			FILE *fl = xfopen(ref_path, "w");
			fprintf(fl, "%s\n", target_sha);
			fclose(fl);
			free(ref_path);
			free(cur_br);
		}
		{
			char *co_argv[2];
			co_argv[0] = (char*)target_ref_name;
			co_argv[1] = NULL;
			git_checkout(co_argv);
		}
		printf("Updating %.7s..%.7s\nFast-forward\n", head_sha, target_sha);
		free(head_sha);
		free(target_sha);
		return 0;
	}

	bb_error_msg("fatal: Merge: not a fast-forward, manual merge required");
	free(head_sha);
	free(target_sha);
	return 1;
}

static int git_remote(int argc, char **argv)
{
	const char *subcmd;

	if (argc == 0) {
		FILE *f = fopen(".git/config", "r");
		char line[512];
		if (!f) return 0;
		while (fgets(line, sizeof(line), f)) {
			char *p = skip_whitespace(line);
			chomp(p);
			if (is_prefixed_with(p, "[remote \"")) {
				char *end = strchr(p + 9, '"');
				if (end) {
					*end = '\0';
					puts(p + 9);
				}
			}
		}
		fclose(f);
		return 0;
	}

	subcmd = argv[0];
	if (strcmp(subcmd, "add") == 0) {
		char *fetch_val;
		if (argc < 3) bb_show_usage();
		git_config_set(".git/config", "remote", argv[1], "url", argv[2]);
		fetch_val = xasprintf("+refs/heads/*:refs/remotes/%s/*", argv[1]);
		git_config_set(".git/config", "remote", argv[1], "fetch", fetch_val);
		free(fetch_val);
		return 0;
	}

	if (strcmp(subcmd, "remove") == 0 || strcmp(subcmd, "rm") == 0) {
		if (argc < 2) bb_show_usage();
		git_config_remove_section(".git/config", "remote", argv[1]);
		return 0;
	}

	if (strcmp(subcmd, "show") == 0 || strcmp(subcmd, "get-url") == 0) {
		char *url;
		if (argc < 2) bb_show_usage();
		url = git_config_get(".git/config", "remote", argv[1], "url");
		if (!url) {
			bb_error_msg("fatal: No such remote '%s'", argv[1]);
			return 1;
		}
		if (strcmp(subcmd, "get-url") == 0) {
			puts(url);
		} else {
			printf("* remote %s\n  Fetch URL: %s\n  Push  URL: %s\n", argv[1], url, url);
		}
		free(url);
		return 0;
	}

	bb_show_usage();
}

static int git_fetch(int argc, char **argv)
{
	const char *remote_name = argc > 0 ? argv[0] : "origin";
	char *url = git_config_get(".git/config", "remote", remote_name, "url");
	char *rem_dir;
	int fetched = 0;

	if (!url) {
		if (argc > 0)
			url = xstrdup(remote_name);
		else {
			bb_error_msg("fatal: remote '%s' not configured", remote_name);
			return 1;
		}
	}

	rem_dir = xasprintf(".git/refs/remotes/%s", remote_name);
	bb_make_directory(rem_dir, 0777, FILEUTILS_RECUR);
	free(rem_dir);

	if (is_prefixed_with(url, "http://") || is_prefixed_with(url, "https://")) {
		char *cmd = xasprintf("curl -s -f '%s/info/refs' 2>/dev/null || wget -q -O - '%s/info/refs' 2>/dev/null", url, url);
		FILE *fp = popen(cmd, "r");
		free(cmd);
		if (fp) {
			char line[256];
			while (fgets(line, sizeof(line), fp)) {
				char *tab;
				chomp(line);
				tab = strchr(line, '\t');
				if (tab && (tab - line) == 40) {
					char *ref_name;
					*tab = '\0';
					ref_name = tab + 1;
					if (is_prefixed_with(ref_name, "refs/heads/")) {
						char *branch = ref_name + 11;
						char *rem_ref_file = xasprintf(".git/refs/remotes/%s/%s", remote_name, branch);
						FILE *fr;

						download_object_graph(url, line);

						fr = xfopen(rem_ref_file, "w");
						fprintf(fr, "%s\n", line);
						fclose(fr);
						printf(" * [new branch]      %s     -> %s/%s\n", branch, remote_name, branch);
						free(rem_ref_file);
						fetched++;
					}
				}
			}
			pclose(fp);
		}
	} else {
		const char *src_clean = url;
		char *src_heads;
		DIR *d;

		if (is_prefixed_with(src_clean, "file://"))
			src_clean += 7;

		src_heads = xasprintf("%s/.git/refs/heads", src_clean);
		d = opendir(src_heads);
		if (!d) {
			free(src_heads);
			src_heads = xasprintf("%s/refs/heads", src_clean);
			d = opendir(src_heads);
		}

		if (d) {
			struct dirent *de;
			while ((de = readdir(d)) != NULL) {
				char *ref_file;
				FILE *fr;
				char sha[64];

				if (de->d_name[0] == '.') continue;
				ref_file = xasprintf("%s/%s", src_heads, de->d_name);
				fr = fopen(ref_file, "r");
				if (fr) {
					if (fgets(sha, sizeof(sha), fr)) {
						chomp(sha);
						if (is_valid_sha1(sha)) {
							char *rem_ref_file = xasprintf(".git/refs/remotes/%s/%s", remote_name, de->d_name);
							FILE *fout;

							download_object_graph(url, sha);

							fout = xfopen(rem_ref_file, "w");
							fprintf(fout, "%s\n", sha);
							fclose(fout);
							printf(" * [new branch]      %s     -> %s/%s\n", de->d_name, remote_name, de->d_name);
							free(rem_ref_file);
							fetched++;
						}
					}
					fclose(fr);
				}
				free(ref_file);
			}
			closedir(d);
		}
		free(src_heads);
	}

	free(url);
	return fetched > 0 ? 0 : 1;
}

static int git_pull(int argc, char **argv)
{
	const char *remote = argc > 0 ? argv[0] : "origin";
	char *branch = NULL;
	char *remote_ref, *remote_sha, *head_sha, *cur_br;
	int ret;
	FILE *f;

	if (argc > 1) {
		branch = xstrdup(argv[1]);
	} else {
		branch = git_current_branch();
		if (!branch) branch = xstrdup("main");
	}

	{
		char *fetch_argv[2];
		fetch_argv[0] = (char*)remote;
		fetch_argv[1] = NULL;
		ret = git_fetch(1, fetch_argv);
		if (ret != 0) {
			free(branch);
			return ret;
		}
	}

	remote_ref = xasprintf(".git/refs/remotes/%s/%s", remote, branch);
	remote_sha = NULL;
	f = fopen(remote_ref, "r");
	if (f) {
		char line[64];
		if (fgets(line, sizeof(line), f)) {
			chomp(line);
			if (is_valid_sha1(line))
				remote_sha = xstrdup(line);
		}
		fclose(f);
	}
	free(remote_ref);

	if (!remote_sha) {
		bb_error_msg("fatal: couldn't find remote ref for %s", branch);
		free(branch);
		return 1;
	}

	head_sha = git_get_head_commit();
	if (!head_sha) {
		cur_br = git_current_branch();
		if (cur_br) {
			char *ref_path = xasprintf(".git/refs/heads/%s", cur_br);
			FILE *fl = xfopen(ref_path, "w");
			fprintf(fl, "%s\n", remote_sha);
			fclose(fl);
			free(ref_path);
			free(cur_br);
		}
		{
			char *co_argv[2];
			co_argv[0] = branch;
			co_argv[1] = NULL;
			git_checkout(co_argv);
		}
		free(remote_sha);
		free(branch);
		return 0;
	}

	if (strcmp(head_sha, remote_sha) == 0) {
		printf("Already up to date.\n");
		free(head_sha);
		free(remote_sha);
		free(branch);
		return 0;
	}

	if (is_ancestor(head_sha, remote_sha)) {
		cur_br = git_current_branch();
		if (cur_br) {
			char *ref_path = xasprintf(".git/refs/heads/%s", cur_br);
			FILE *fl = xfopen(ref_path, "w");
			fprintf(fl, "%s\n", remote_sha);
			fclose(fl);
			free(ref_path);
			free(cur_br);
		}
		{
			char *co_argv[2];
			co_argv[0] = branch;
			co_argv[1] = NULL;
			git_checkout(co_argv);
		}
		printf("Updating %.7s..%.7s\nFast-forward\n", head_sha, remote_sha);
	} else {
		bb_error_msg("fatal: Not possible to fast-forward, aborting.");
		free(head_sha);
		free(remote_sha);
		free(branch);
		return 1;
	}

	free(head_sha);
	free(remote_sha);
	free(branch);
	return 0;
}

static int git_clone(int argc, char **argv)
{
	const char *url;
	char *clean_url;
	char *dest_dir;
	char *init_argv[2];
	int ret;

	if (argc < 1)
		bb_show_usage();

	url = argv[0];
	if (argc > 1) {
		dest_dir = xstrdup(argv[1]);
	} else {
		const char *p;
		const char *slash;

		clean_url = xstrdup(url);
		while (strlen(clean_url) > 0 && clean_url[strlen(clean_url) - 1] == '/')
			clean_url[strlen(clean_url) - 1] = '\0';
		if (strlen(clean_url) > 4 && strcmp(clean_url + strlen(clean_url) - 4, ".git") == 0)
			clean_url[strlen(clean_url) - 4] = '\0';

		slash = strrchr(clean_url, '/');
		p = slash ? slash + 1 : clean_url;
		if (!*p) p = "repo";
		dest_dir = xstrdup(p);
		free(clean_url);
	}

	printf("Cloning into '%s'...\n", dest_dir);
	if (mkdir(dest_dir, 0777) != 0 && errno != EEXIST) {
		bb_perror_msg_and_die("fatal: could not create work tree dir '%s'", dest_dir);
	}

	if (chdir(dest_dir) != 0) {
		bb_perror_msg_and_die("fatal: could not switch to dir '%s'", dest_dir);
	}

	init_argv[0] = NULL;
	git_init(init_argv);

	git_config_set(".git/config", "remote", "origin", "url", url);
	git_config_set(".git/config", "remote", "origin", "fetch", "+refs/heads/*:refs/remotes/origin/*");

	{
		char *fetch_argv[2];
		fetch_argv[0] = (char*)"origin";
		fetch_argv[1] = NULL;
		ret = git_fetch(1, fetch_argv);
	}

	if (ret != 0) {
		bb_error_msg("warning: You appear to have cloned an empty repository or fetch failed.");
		free(dest_dir);
		return 0;
	}

	{
		char *remote_branch = NULL;

		if (access(".git/refs/remotes/origin/main", F_OK) == 0) {
			remote_branch = xstrdup("main");
		} else if (access(".git/refs/remotes/origin/master", F_OK) == 0) {
			remote_branch = xstrdup("master");
		} else {
			DIR *d = opendir(".git/refs/remotes/origin");
			if (d) {
				struct dirent *de;
				while ((de = readdir(d)) != NULL) {
					if (de->d_name[0] != '.') {
						remote_branch = xstrdup(de->d_name);
						break;
					}
				}
				closedir(d);
			}
		}

		if (remote_branch) {
			char *ref_path = xasprintf(".git/refs/remotes/origin/%s", remote_branch);
			FILE *f = fopen(ref_path, "r");
			if (f) {
				char line[64];
				if (fgets(line, sizeof(line), f)) {
					chomp(line);
					if (is_valid_sha1(line)) {
						char *local_ref = xasprintf(".git/refs/heads/%s", remote_branch);
						FILE *fl = xfopen(local_ref, "w");
						fprintf(fl, "%s\n", line);
						fclose(fl);
						free(local_ref);

						fl = xfopen(".git/HEAD", "w");
						fprintf(fl, "ref: refs/heads/%s\n", remote_branch);
						fclose(fl);

						{
							char *co_argv[2];
							co_argv[0] = remote_branch;
							co_argv[1] = NULL;
							git_checkout(co_argv);
						}
					}
				}
				fclose(f);
			}
			free(ref_path);
			free(remote_branch);
		}
	}

	free(dest_dir);
	return 0;
}

static int git_stash(int argc, char **argv)
{
	const char *subcmd = argc > 0 ? argv[0] : "push";

	if (strcmp(subcmd, "list") == 0) {
		if (access(".git/stash", F_OK) == 0) {
			char meta[256];
			FILE *f = fopen(".git/stash.meta", "r");
			meta[0] = '\0';
			if (f) {
				if (fgets(meta, sizeof(meta), f))
					chomp(meta);
				fclose(f);
			}
			printf("stash@{0}: WIP on %s\n", meta[0] ? meta : "main");
		}
		return 0;
	}

	if (strcmp(subcmd, "drop") == 0) {
		if (access(".git/stash", F_OK) != 0) {
			bb_error_msg("fatal: No stash entries found.");
			return 1;
		}
		unlink(".git/stash");
		unlink(".git/stash.meta");
		printf("Dropped stash@{0}\n");
		return 0;
	}

	if (strcmp(subcmd, "pop") == 0) {
		int ret;
		if (access(".git/stash", F_OK) != 0) {
			bb_error_msg("fatal: No stash entries found.");
			return 1;
		}
		ret = system("patch -p1 < .git/stash >/dev/null 2>&1");
		if (ret != 0) {
			ret = system("patch -p0 < .git/stash");
		}
		if (ret == 0) {
			unlink(".git/stash");
			unlink(".git/stash.meta");
			printf("Dropped stash@{0}\n");
		} else {
			bb_error_msg("error: could not apply stash cleanly");
			return 1;
		}
		return 0;
	}

	if (strcmp(subcmd, "push") == 0 || strcmp(subcmd, "save") == 0 || argc == 0) {
		FILE *fpatch;
		int count = 0;
		index_entry_t *entries;
		char *cur_br;
		char *head_sha;
		int diffs_found = 0;
		int i;

		entries = read_index(&count);
		if (count == 0) {
			printf("No local changes to save\n");
			return 0;
		}

		fpatch = fopen(".git/stash", "w");
		if (!fpatch) {
			bb_perror_msg("fatal: cannot write .git/stash");
			free_index(entries, count);
			return 1;
		}

		for (i = 0; i < count; i++) {
			struct stat st;
			if (stat(entries[i].path, &st) == 0) {
				char *disk_buf;
				int fd = open(entries[i].path, O_RDONLY);
				if (fd >= 0) {
					size_t obj_sz = 0;
					char *obj_buf = (char*)git_read_object(entries[i].hex, NULL, &obj_sz);
					disk_buf = xmalloc(st.st_size);
					xread(fd, disk_buf, st.st_size);
					close(fd);

					if (diff_buffers(obj_buf ? obj_buf : "", obj_sz, entries[i].path,
					                 disk_buf, st.st_size, entries[i].path,
					                 fpatch, 0) > 0) {
						diffs_found = 1;
					}
					free(obj_buf);
					free(disk_buf);
				}
			}
		}
		fclose(fpatch);

		if (!diffs_found) {
			unlink(".git/stash");
			free_index(entries, count);
			printf("No local changes to save\n");
			return 0;
		}

		cur_br = git_current_branch();
		head_sha = git_get_head_commit();
		{
			FILE *fm = fopen(".git/stash.meta", "w");
			if (fm) {
				char *subject = head_sha ? get_commit_subject(head_sha) : xstrdup("initial");
				fprintf(fm, "%s: %.7s %s\n", cur_br ? cur_br : "HEAD", head_sha ? head_sha : "", subject);
				fclose(fm);
				free(subject);
			}
		}

		for (i = 0; i < count; i++) {
			size_t sz = 0;
			uint8_t *content = git_read_object(entries[i].hex, NULL, &sz);
			if (content) {
				int fd = open(entries[i].path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
				if (fd >= 0) {
					xwrite(fd, content, sz);
					close(fd);
				}
				free(content);
			}
		}

		printf("Saved working directory and index state WIP on %s\n", cur_br ? cur_br : "HEAD");
		free(cur_br);
		free(head_sha);
		free_index(entries, count);
		return 0;
	}

	bb_show_usage();
}

static int git_tag(int argc, char **argv)
{
	const char *name;
	char *commit_sha;
	char ref_path[128];
	FILE *f;

	if (argc == 0) {
		DIR *d = opendir(".git/refs/tags");
		if (d) {
			struct dirent *de;
			char **tag_list = NULL;
			int tag_count = 0;
			int i;

			while ((de = readdir(d)) != NULL) {
				if (de->d_name[0] == '.') continue;
				tag_list = xrealloc_vector(tag_list, 4, tag_count);
				tag_list[tag_count++] = xstrdup(de->d_name);
			}
			closedir(d);

			qsort_string_vector(tag_list, tag_count);
			for (i = 0; i < tag_count; i++) {
				puts(tag_list[i]);
				free(tag_list[i]);
			}
			free(tag_list);
		}
		return 0;
	}

	name = argv[0];
	if (argc > 1) {
		commit_sha = git_resolve_ref(argv[1]);
		if (!commit_sha) {
			bb_error_msg("fatal: Failed to resolve '%s' as a valid revision.", argv[1]);
			return 1;
		}
	} else {
		commit_sha = git_get_head_commit();
		if (!commit_sha) {
			bb_error_msg("fatal: Failed to resolve 'HEAD' as a valid revision.");
			return 1;
		}
	}

	bb_make_directory((char*)".git/refs/tags", 0777, FILEUTILS_RECUR);
	snprintf(ref_path, sizeof(ref_path), ".git/refs/tags/%s", name);
	f = xfopen(ref_path, "w");
	fprintf(f, "%s\n", commit_sha);
	fclose(f);
	free(commit_sha);
	return 0;
}

static int git_reset(int argc, char **argv)
{
	int mode = 1;
	const char *ref = "HEAD";
	char *target_sha;
	int i;

	for (i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--soft") == 0)
			mode = 0;
		else if (strcmp(argv[i], "--mixed") == 0)
			mode = 1;
		else if (strcmp(argv[i], "--hard") == 0)
			mode = 2;
		else if (argv[i][0] != '-')
			ref = argv[i];
	}

	target_sha = git_resolve_ref(ref);
	if (!target_sha) {
		bb_error_msg("fatal: ambiguous argument '%s': unknown revision", ref);
		return 1;
	}

	{
		char *br = git_current_branch();
		if (br) {
			char *ref_path = xasprintf(".git/refs/heads/%s", br);
			FILE *f = xfopen(ref_path, "w");
			fprintf(f, "%s\n", target_sha);
			fclose(f);
			free(ref_path);
			free(br);
		} else {
			FILE *f = xfopen(".git/HEAD", "w");
			fprintf(f, "%s\n", target_sha);
			fclose(f);
		}
	}

	if (mode == 0) {
		free(target_sha);
		return 0;
	}

	{
		char *tree_hex = get_commit_tree_sha(target_sha);
		flat_tree_entry_t *new_entries = NULL;
		int new_count = 0;

		if (tree_hex) {
			read_tree_recursive(tree_hex, "", &new_entries, &new_count);
			free(tree_hex);
		}

		if (mode == 2) {
			int old_count = 0;
			index_entry_t *old_entries = read_index(&old_count);
			int j;

			for (j = 0; j < old_count; j++) {
				int k, found = 0;
				for (k = 0; k < new_count; k++) {
					if (strcmp(old_entries[j].path, new_entries[k].path) == 0) {
						found = 1;
						break;
					}
				}
				if (!found)
					unlink(old_entries[j].path);
			}
			free_index(old_entries, old_count);

			for (j = 0; j < new_count; j++) {
				size_t sz = 0;
				uint8_t *content = git_read_object(new_entries[j].hex, NULL, &sz);
				if (content) {
					char *slash = strrchr(new_entries[j].path, '/');
					int fd;
					if (slash) {
						char *d = xstrndup(new_entries[j].path, slash - new_entries[j].path);
						bb_make_directory(d, 0777, FILEUTILS_RECUR);
						free(d);
					}
					fd = open(new_entries[j].path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
					if (fd >= 0) {
						xwrite(fd, content, sz);
						close(fd);
					}
					free(content);
				}
			}
			{
				char *subj = get_commit_subject(target_sha);
				printf("HEAD is now at %.7s %s\n", target_sha, subj ? subj : "");
				free(subj);
			}
		}

		{
			FILE *f = xfopen(".git/index.txt", "w");
			int j;
			for (j = 0; j < new_count; j++) {
				fprintf(f, "%s %s\n", new_entries[j].hex, new_entries[j].path);
				free(new_entries[j].path);
			}
			fclose(f);
			free(new_entries);
		}
	}

	free(target_sha);
	return 0;
}

static int git_show(int argc, char **argv)
{
	const char *ref = argc > 0 ? argv[0] : "HEAD";
	char *sha = git_resolve_ref(ref);
	char *type = NULL;
	size_t sz = 0;
	uint8_t *content;

	if (!sha) {
		bb_error_msg("fatal: ambiguous argument '%s': unknown revision", ref);
		return 1;
	}

	content = git_read_object(sha, &type, &sz);
	if (!content) {
		bb_error_msg("fatal: object %s not found", sha);
		free(sha);
		return 1;
	}

	if (strcmp(type, "commit") == 0) {
		char *data = (char*)content;
		char *tree_sha = NULL;
		char *parent_sha = NULL;
		char *p = data;
		int use_color_flag = isatty(STDOUT_FILENO);

		if (use_color_flag)
			printf("\033[33mcommit %s\033[0m\n", sha);
		else
			printf("commit %s\n", sha);

		while (p && *p) {
			char *nl = strchr(p, '\n');
			if (nl) *nl = '\0';
			if (is_prefixed_with(p, "tree ")) {
				tree_sha = xstrdup(p + 5);
			} else if (is_prefixed_with(p, "parent ") && !parent_sha) {
				parent_sha = xstrdup(p + 7);
			} else if (is_prefixed_with(p, "author ")) {
				printf("Author: %s\n", p + 7);
			} else if (*p == '\0') {
				if (nl && *(nl + 1)) {
					printf("\n    %s\n\n", nl + 1);
				}
				break;
			}
			if (!nl) break;
			p = nl + 1;
		}

		if (tree_sha) {
			flat_tree_entry_t *c_entries = NULL, *p_entries = NULL;
			int c_count = 0, p_count = 0;
			int i;

			read_tree_recursive(tree_sha, "", &c_entries, &c_count);
			if (parent_sha) {
				char *p_tree = get_commit_tree_sha(parent_sha);
				if (p_tree) {
					read_tree_recursive(p_tree, "", &p_entries, &p_count);
					free(p_tree);
				}
			}

			for (i = 0; i < c_count; i++) {
				int j, found = 0;
				for (j = 0; j < p_count; j++) {
					if (strcmp(c_entries[i].path, p_entries[j].path) == 0) {
						found = 1;
						if (strcmp(c_entries[i].hex, p_entries[j].hex) != 0) {
							size_t s1 = 0, s2 = 0;
							char *b1 = (char*)git_read_object(p_entries[j].hex, NULL, &s1);
							char *b2 = (char*)git_read_object(c_entries[i].hex, NULL, &s2);
							diff_buffers(b1 ? b1 : "", s1, c_entries[i].path,
							             b2 ? b2 : "", s2, c_entries[i].path,
							             stdout, use_color_flag);
							free(b1);
							free(b2);
						}
						break;
					}
				}
				if (!found) {
					size_t s2 = 0;
					char *b2 = (char*)git_read_object(c_entries[i].hex, NULL, &s2);
					diff_buffers("", 0, "/dev/null",
					             b2 ? b2 : "", s2, c_entries[i].path,
					             stdout, use_color_flag);
					free(b2);
				}
			}

			for (i = 0; i < p_count; i++) {
				int j, found = 0;
				for (j = 0; j < c_count; j++) {
					if (strcmp(p_entries[i].path, c_entries[j].path) == 0) {
						found = 1;
						break;
					}
				}
				if (!found) {
					size_t s1 = 0;
					char *b1 = (char*)git_read_object(p_entries[i].hex, NULL, &s1);
					diff_buffers(b1 ? b1 : "", s1, p_entries[i].path,
					             "", 0, "/dev/null",
					             stdout, use_color_flag);
					free(b1);
				}
			}

			for (i = 0; i < c_count; i++) free(c_entries[i].path);
			free(c_entries);
			for (i = 0; i < p_count; i++) free(p_entries[i].path);
			free(p_entries);
			free(tree_sha);
		}
		free(parent_sha);
	} else if (strcmp(type, "tree") == 0) {
		char *cat_argv[3];
		cat_argv[0] = (char*)"-p";
		cat_argv[1] = sha;
		cat_argv[2] = NULL;
		git_cat_file(2, cat_argv);
	} else {
		full_write(STDOUT_FILENO, content, sz);
	}

	free(type);
	free(content);
	free(sha);
	return 0;
}

int git_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int git_main(int argc, char **argv)
{
	const char *cmd;

	if (argc < 2)
		bb_show_usage();

	cmd = argv[1];
	argv += 2;
	argc -= 2;

	if (strcmp(cmd, "init") == 0)
		return git_init(argv);
	if (strcmp(cmd, "status") == 0)
		return git_status();
	if (strcmp(cmd, "add") == 0)
		return git_add(argv);
	if (strcmp(cmd, "commit") == 0)
		return git_commit(argc, argv);
	if (strcmp(cmd, "diff") == 0)
		return git_diff(argc, argv);
	if (strcmp(cmd, "log") == 0)
		return git_log(argc, argv);
	if (strcmp(cmd, "branch") == 0)
		return git_branch(argc, argv);
	if (strcmp(cmd, "checkout") == 0)
		return git_checkout(argv);
	if (strcmp(cmd, "merge") == 0)
		return git_merge(argv);
	if (strcmp(cmd, "pull") == 0)
		return git_pull(argc, argv);
	if (strcmp(cmd, "fetch") == 0)
		return git_fetch(argc, argv);
	if (strcmp(cmd, "clone") == 0)
		return git_clone(argc, argv);
	if (strcmp(cmd, "remote") == 0)
		return git_remote(argc, argv);
	if (strcmp(cmd, "stash") == 0)
		return git_stash(argc, argv);
	if (strcmp(cmd, "tag") == 0)
		return git_tag(argc, argv);
	if (strcmp(cmd, "reset") == 0)
		return git_reset(argc, argv);
	if (strcmp(cmd, "show") == 0)
		return git_show(argc, argv);
	if (strcmp(cmd, "hash-object") == 0)
		return git_hash_object(argc, argv);
	if (strcmp(cmd, "cat-file") == 0)
		return git_cat_file(argc, argv);

	bb_error_msg_and_die("git: '%s' is not a git command. See 'git --help'.", cmd);
}
