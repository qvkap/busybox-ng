/* vi: set sw=4 ts=4: */
/*
 * Minimal Git implementation for BusyBox-ng
 *
 * Copyright (C) 2026 qvkap <qvkapp@gmail.com>
 * Licensed under GPLv2 or later.
 */

//config:config GIT
//config:	bool "git (mini version control)"
//config:	default y
//config:	help
//config:	Minimal implementation of Git revision control system.
//config:	Supports: init, status, add, commit, log, branch, checkout,
//config:	hash-object, cat-file.

//applet:IF_GIT(APPLET(git, BB_DIR_USR_BIN, BB_SUID_DROP))

//kbuild:lib-$(CONFIG_GIT) += git.o

//usage:#define git_trivial_usage
//usage:       "COMMAND [ARGS]"
//usage:#define git_full_usage "\n\n"
//usage:       "Minimal Git revision control system\n"
//usage:       "\nCommands:"
//usage:       "\n	init [DIRECTORY]     Initialize a new repository"
//usage:       "\n	status               Show working tree status"
//usage:       "\n	add FILE...          Add file contents to index"
//usage:       "\n	commit -m MSG        Record changes to repository"
//usage:       "\n	log [-n NUM]         Show commit logs"
//usage:       "\n	branch [NAME]        List or create branches"
//usage:       "\n	checkout BRANCH      Switch branches"
//usage:       "\n	hash-object [-w] FILE Compute object ID"
//usage:       "\n	cat-file -p|-t|-s ID Display object content/type/size"

#include "libbb.h"

/* --- TINF (Tiny Inflate for Git zlib decompress) --- */
typedef struct {
	const uint8_t *source;
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
	unsigned int bit = d->tag & 1;
	d->tag >>= 1;
	if (--d->bitcount == 0) {
		d->tag = *d->source++;
		d->bitcount = 8;
	}
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
		if (cur < 0) return t->trans[sum + cur];
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
			length = tinf_read_bits(d, tinf_len_ext[sym], tinf_len_base[sym]);
			dist_sym = tinf_decode_symbol(d, dt);
			dist = tinf_read_bits(d, tinf_dist_ext[dist_sym], tinf_dist_base[dist_sym]);
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
	d.tag = *d.source++;
	d.bitcount = 8;

	out.dest = NULL;
	out.dest_len = 0;
	out.dest_cap = 0;

	do {
		int btype;
		bfinal = tinf_getbit(&d);
		btype = tinf_read_bits(&d, 2, 0);
		if (btype == 0) {
			const uint8_t *src;
			uint16_t i, len;
			src = d.source;
			len = src[0] | (src[1] << 8);
			src += 4;
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
				if (sym < 16) lengths[num++] = sym;
				else if (sym == 16) {
					int rep = tinf_read_bits(&d, 2, 3);
					uint8_t prev = lengths[num - 1];
					while (rep--) lengths[num++] = prev;
				} else if (sym == 17) {
					int rep = tinf_read_bits(&d, 3, 3);
					while (rep--) lengths[num++] = 0;
				} else if (sym == 18) {
					int rep = tinf_read_bits(&d, 7, 11);
					while (rep--) lengths[num++] = 0;
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

/* --- SHA-1 object helpers --- */
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

	fd = open(obj_path, O_WRONLY | O_CREAT | O_EXCL, 0444);
	if (fd >= 0) {
		xwrite(fd, comp, comp_len);
		close(fd);
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

	sprintf(obj_path, ".git/objects/%.2s/%s", hex, hex + 2);
	fd = open(obj_path, O_RDONLY);
	if (fd < 0)
		return NULL;

	fstat(fd, &st);
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

	if (out_type) {
		*out_type = xstrndup((char*)raw, space - (char*)raw);
	}
	content_size = bb_strtou(space + 1, NULL, 10);
	if (out_size)
		*out_size = content_size;

	hdr_len = (null_byte - (char*)raw) + 1;
	content = xmalloc(content_size + 1);
	memcpy(content, raw + hdr_len, content_size);
	content[content_size] = '\0';
	free(raw);
	return content;
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
	return xstrdup(line);
}

static char *git_get_head_commit(void)
{
	char line[64];
	char ref_path[128];
	char *br;
	FILE *f;

	br = git_current_branch();
	if (!br) return NULL;
	snprintf(ref_path, sizeof(ref_path), ".git/refs/heads/%s", br);
	free(br);
	f = fopen(ref_path, "r");
	if (!f) return NULL;
	if (!fgets(line, sizeof(line), f)) {
		fclose(f);
		return NULL;
	}
	fclose(f);
	chomp(line);
	return xstrdup(line);
}

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

static int git_add(char **argv)
{
	int count = 0;
	index_entry_t *entries;

	if (!argv[0])
		bb_show_usage();

	entries = read_index(&count);

	while (*argv) {
		const char *path = *argv++;
		int fd, found, i;
		struct stat st;
		char *buf;
		char hex[41];

		fd = open(path, O_RDONLY);
		if (fd < 0) {
			bb_perror_msg("fatal: pathspec '%s' did not match any files", path);
			return 1;
		}
		fstat(fd, &st);
		buf = xmalloc(st.st_size);
		xread(fd, buf, st.st_size);
		close(fd);

		git_write_object("blob", buf, st.st_size, hex);
		free(buf);

		found = 0;
		for (i = 0; i < count; i++) {
			if (strcmp(entries[i].path, path) == 0) {
				strcpy(entries[i].hex, hex);
				found = 1;
				break;
			}
		}
		if (!found) {
			entries = xrealloc_vector(entries, 4, count);
			entries[count].path = xstrdup(path);
			strcpy(entries[count].hex, hex);
			count++;
		}
	}

	write_index(entries, count);
	return 0;
}

static int git_status(void)
{
	int count = 0;
	index_entry_t *entries;
	char *head;
	char *br = git_current_branch();

	if (!br) {
		bb_error_msg("fatal: not a git repository");
		return 1;
	}
	printf("On branch %s\n", br);
	free(br);

	entries = read_index(&count);
	head = git_get_head_commit();
	if (!head) {
		printf("\nNo commits yet\n\n");
	}

	if (count > 0) {
		int i;
		printf("Changes to be committed:\n");
		for (i = 0; i < count; i++) {
			printf("\tmodified:   %s\n", entries[i].path);
		}
	} else {
		printf("nothing to commit, working tree clean\n");
	}
	free(head);
	return 0;
}

static int git_commit(int argc, char **argv)
{
	const char *msg = NULL;
	int i, count = 0, cpos;
	index_entry_t *entries;
	size_t tree_sz = 0, tree_pos = 0;
	uint8_t *tree_buf;
	char tree_hex[41], commit_hex[41], commit_body[2048], ref_path[128];
	char *parent, *br;
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

	for (i = 0; i < count; i++) {
		tree_sz += strlen("100644 ") + strlen(entries[i].path) + 1 + 20;
	}
	tree_buf = xmalloc(tree_sz);
	for (i = 0; i < count; i++) {
		int prefix_len = sprintf((char*)tree_buf + tree_pos, "100644 %s", entries[i].path) + 1;
		uint8_t bin[20];
		tree_pos += prefix_len;
		hex2bin((char*)bin, entries[i].hex, 20);
		memcpy(tree_buf + tree_pos, bin, 20);
		tree_pos += 20;
	}

	git_write_object("tree", tree_buf, tree_pos, tree_hex);
	free(tree_buf);

	parent = git_get_head_commit();
	now = time(NULL);
	cpos = sprintf(commit_body, "tree %s\n", tree_hex);
	if (parent) {
		cpos += sprintf(commit_body + cpos, "parent %s\n", parent);
	}
	cpos += sprintf(commit_body + cpos, "author User <user@local> %ld +0000\n", (long)now);
	cpos += sprintf(commit_body + cpos, "committer User <user@local> %ld +0000\n\n%s\n", (long)now, msg);

	git_write_object("commit", commit_body, cpos, commit_hex);

	br = git_current_branch();
	snprintf(ref_path, sizeof(ref_path), ".git/refs/heads/%s", br ? br : "main");
	f = xfopen(ref_path, "w");
	fprintf(f, "%s\n", commit_hex);
	fclose(f);

	unlink(".git/index.txt");

	printf("[%s %s] %s\n", br ? br : "main", commit_hex, msg);
	free(br);
	free(parent);
	return 0;
}

static int git_log(int argc, char **argv)
{
	int limit = 10, shown = 0, i;
	char *curr;

	for (i = 0; i < argc; i++) {
		if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
			limit = bb_strtou(argv[++i], NULL, 10);
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

		if (!data || strcmp(type, "commit") != 0) {
			free(type);
			free(data);
			break;
		}

		printf("\033[33mcommit %s\033[0m\n", curr);
		line = data;
		while (line && *line) {
			char *nl = strchr(line, '\n');
			if (nl) *nl = '\0';
			if (is_prefixed_with(line, "parent ")) {
				next_parent = xstrdup(line + 7);
			} else if (is_prefixed_with(line, "author ")) {
				printf("Author: %s\n", line + 7);
			} else if (is_prefixed_with(line, "committer ")) {
				/* skip */
			} else if (line[0] == '\0') {
				if (nl) {
					printf("\n    %s\n\n", nl + 1);
				}
				break;
			}
			if (!nl) break;
			line = nl + 1;
		}

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
		if (d) {
			struct dirent *de;
			while ((de = readdir(d)) != NULL) {
				if (de->d_name[0] == '.') continue;
				if (cur && strcmp(cur, de->d_name) == 0)
					printf("* \033[32m%s\033[0m\n", de->d_name);
				else
					printf("  %s\n", de->d_name);
			}
			closedir(d);
		}
		free(cur);
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
	const char *br;
	char ref[128];
	FILE *f;

	if (!argv[0])
		bb_show_usage();
	br = argv[0];
	snprintf(ref, sizeof(ref), ".git/refs/heads/%s", br);
	if (access(ref, F_OK) != 0) {
		bb_error_msg("error: pathspec '%s' did not match any file(s) known to git", br);
		return 1;
	}
	f = xfopen(".git/HEAD", "w");
	fprintf(f, "ref: refs/heads/%s\n", br);
	fclose(f);
	printf("Switched to branch '%s'\n", br);
	return 0;
}

static int git_cat_file(int argc, char **argv)
{
	const char *mode, *hex;
	char *type = NULL;
	size_t sz = 0;
	uint8_t *content;

	if (argc < 2)
		bb_show_usage();
	mode = argv[0];
	hex = argv[1];

	content = git_read_object(hex, &type, &sz);
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
				char *sp = strchr((char*)content + pos, ' ');
				char *nul = strchr((char*)content + pos, '\0');
				char hex_str[41];
				if (!sp || !nul) break;
				bin2hex(hex_str, (char*)content + (nul - (char*)content) + 1, 20);
				hex_str[40] = '\0';
				printf("%s blob %s\t%s\n", (char*)content + pos, hex_str, sp + 1);
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
	if (strcmp(cmd, "log") == 0)
		return git_log(argc, argv);
	if (strcmp(cmd, "branch") == 0)
		return git_branch(argc, argv);
	if (strcmp(cmd, "checkout") == 0)
		return git_checkout(argv);
	if (strcmp(cmd, "hash-object") == 0)
		return git_hash_object(argc, argv);
	if (strcmp(cmd, "cat-file") == 0)
		return git_cat_file(argc, argv);

	bb_error_msg_and_die("git: '%s' is not a git command. See 'git --help'.", cmd);
}
