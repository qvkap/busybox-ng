/* vi: set sw=4 ts=4: */
/*
 * Lightweight jq implementation for BusyBox-ng
 *
 * Copyright (C) 2026 BusyBox-ng Authors
 * Licensed under GPLv2 or later.
 */

//config:config JQ
//config:	bool "jq (JSON processor)"
//config:	default y
//config:	help
//config:	Lightweight JSON processor. Supports a subset of jq filters.

//applet:IF_JQ(APPLET_NOEXEC(jq, jq, BB_DIR_USR_BIN, BB_SUID_DROP, jq))

//kbuild:lib-$(CONFIG_JQ) += jq.o

//usage:#define jq_trivial_usage
//usage:       "[-crnRse] [-f FILE] [--arg K V] FILTER [FILE...]"
//usage:#define jq_full_usage "\n\n"
//usage:       "JSON processor\n"
//usage:     "\n\t-c\tCompact output"
//usage:     "\n\t-r\tRaw string output"
//usage:     "\n\t-n\tNull input"
//usage:     "\n\t-R\tRaw input (lines as strings)"
//usage:     "\n\t-s\tSlurp into array"
//usage:     "\n\t-e\tExit status based on last value"
//usage:     "\n\t-a\tASCII output"
//usage:     "\n\t-S\tSort keys"
//usage:     "\n\t-f FILE\tRead filter from file"
//usage:     "\n\t--arg NAME VAL\tBind $NAME to string VAL"
//usage:     "\n\t--argjson NAME JSON\tBind $NAME to parsed JSON"
//usage:     "\n\t--tab\tUse tab indentation"
//usage:     "\n\t--indent N\tUse N-space indentation"

#include "libbb.h"
#include <math.h>

#ifndef NAN
#define NAN (0.0 / 0.0)
#endif
#ifndef INFINITY
#define INFINITY (1.0 / 0.0)
#endif

/* --- Type Definitions --- */

typedef enum {
	JV_NULL,
	JV_BOOL,
	JV_NUM,
	JV_STR,
	JV_ARR,
	JV_OBJ
} jv_type;

typedef struct jv jv;

typedef struct jv_kv {
	char *k;
	jv *v;
} jv_kv;

struct jv {
	jv_type type;
	union {
		int bval;
		double nval;
		struct {
			char *s;
			int len;
		} str;
		struct {
			jv **a;
			int len;
			int cap;
		} arr;
		struct {
			jv_kv *kv;
			int len;
			int cap;
		} obj;
	} u;
};

typedef struct {
	jv **items;
	int len;
	int cap;
} jv_list;

typedef struct {
	char *buf;
	int len;
	int cap;
} str_buf;

struct globals {
	int compact_out;
	int raw_out;
	int null_in;
	int raw_in;
	int slurp_in;
	int exit_status_opt;
	int join_out;
	int ascii_out;
	int sort_keys_opt;
	int indent_width;
	int use_tabs;
	int had_output;
	int last_truthy;
	int error_flag;
	char *error_msg;
};
#define G (*ptr_to_globals)
#define INIT_G() do { \
	SET_PTR_TO_GLOBALS(xzalloc(sizeof(G))); \
	G.indent_width = 2; \
} while (0)

/* --- String Buffer Helpers --- */

static void str_buf_init(str_buf *sb)
{
	sb->buf = NULL;
	sb->len = 0;
	sb->cap = 0;
}

static void str_buf_append_len(str_buf *sb, const char *s, int len)
{
	if (len <= 0 || !s)
		return;
	if (sb->len + len + 1 > sb->cap) {
		sb->cap = (sb->len + len + 1) * 2;
		if (sb->cap < 32)
			sb->cap = 32;
		sb->buf = xrealloc(sb->buf, sb->cap);
	}
	memcpy(sb->buf + sb->len, s, len);
	sb->len += len;
	sb->buf[sb->len] = '\0';
}

static void str_buf_append_str(str_buf *sb, const char *s)
{
	if (s)
		str_buf_append_len(sb, s, strlen(s));
}

static void str_buf_append_char(str_buf *sb, char c)
{
	str_buf_append_len(sb, &c, 1);
}

static void str_buf_free(str_buf *sb)
{
	free(sb->buf);
	sb->buf = NULL;
	sb->len = sb->cap = 0;
}

static void str_buf_append_utf8(str_buf *sb, unsigned cp)
{
	char b[4];
	if (cp < 0x80) {
		b[0] = (char)cp;
		str_buf_append_len(sb, b, 1);
	} else if (cp < 0x800) {
		b[0] = (char)(0xC0 | (cp >> 6));
		b[1] = (char)(0x80 | (cp & 0x3F));
		str_buf_append_len(sb, b, 2);
	} else if (cp < 0x10000) {
		b[0] = (char)(0xE0 | (cp >> 12));
		b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		b[2] = (char)(0x80 | (cp & 0x3F));
		str_buf_append_len(sb, b, 3);
	} else if (cp <= 0x10FFFF) {
		b[0] = (char)(0xF0 | (cp >> 18));
		b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
		b[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
		b[3] = (char)(0x80 | (cp & 0x3F));
		str_buf_append_len(sb, b, 4);
	}
}

/* --- JSON Value Construction and Memory --- */

static jv* jv_null(void)
{
	jv *v = xzalloc(sizeof(*v));
	v->type = JV_NULL;
	return v;
}

static jv* jv_bool(int b)
{
	jv *v = xzalloc(sizeof(*v));
	v->type = JV_BOOL;
	v->u.bval = !!b;
	return v;
}

static jv* jv_number(double d)
{
	jv *v = xzalloc(sizeof(*v));
	v->type = JV_NUM;
	v->u.nval = d;
	return v;
}

static jv* jv_string_take(char *s, int len)
{
	jv *v = xzalloc(sizeof(*v));
	v->type = JV_STR;
	v->u.str.s = s;
	v->u.str.len = len;
	return v;
}

static jv* jv_string_sized(const char *s, int len)
{
	char *copy;
	if (!s)
		return jv_string_take(xstrdup(""), 0);
	copy = xstrndup(s, len);
	return jv_string_take(copy, len);
}

static jv* jv_string(const char *s)
{
	if (!s)
		s = "";
	return jv_string_sized(s, strlen(s));
}

static jv* jv_array(void)
{
	jv *v = xzalloc(sizeof(*v));
	v->type = JV_ARR;
	return v;
}

static jv* jv_object(void)
{
	jv *v = xzalloc(sizeof(*v));
	v->type = JV_OBJ;
	return v;
}

static void jv_free(jv *v)
{
	int i;
	if (!v)
		return;
	switch (v->type) {
	case JV_STR:
		free(v->u.str.s);
		break;
	case JV_ARR:
		for (i = 0; i < v->u.arr.len; i++)
			jv_free(v->u.arr.a[i]);
		free(v->u.arr.a);
		break;
	case JV_OBJ:
		for (i = 0; i < v->u.obj.len; i++) {
			free(v->u.obj.kv[i].k);
			jv_free(v->u.obj.kv[i].v);
		}
		free(v->u.obj.kv);
		break;
	default:
		break;
	}
	free(v);
}

static jv* jv_copy(const jv *v)
{
	jv *copy;
	int i;
	if (!v)
		return NULL;
	copy = xzalloc(sizeof(*copy));
	copy->type = v->type;
	switch (v->type) {
	case JV_NULL:
		break;
	case JV_BOOL:
		copy->u.bval = v->u.bval;
		break;
	case JV_NUM:
		copy->u.nval = v->u.nval;
		break;
	case JV_STR:
		copy->u.str.s = xstrdup(v->u.str.s);
		copy->u.str.len = v->u.str.len;
		break;
	case JV_ARR:
		copy->u.arr.len = v->u.arr.len;
		copy->u.arr.cap = v->u.arr.len;
		if (copy->u.arr.len > 0) {
			copy->u.arr.a = xmalloc(copy->u.arr.len * sizeof(jv*));
			for (i = 0; i < copy->u.arr.len; i++)
				copy->u.arr.a[i] = jv_copy(v->u.arr.a[i]);
		}
		break;
	case JV_OBJ:
		copy->u.obj.len = v->u.obj.len;
		copy->u.obj.cap = v->u.obj.len;
		if (copy->u.obj.len > 0) {
			copy->u.obj.kv = xmalloc(copy->u.obj.len * sizeof(jv_kv));
			for (i = 0; i < copy->u.obj.len; i++) {
				copy->u.obj.kv[i].k = xstrdup(v->u.obj.kv[i].k);
				copy->u.obj.kv[i].v = jv_copy(v->u.obj.kv[i].v);
			}
		}
		break;
	}
	return copy;
}

static void jv_array_append(jv *arr, jv *val)
{
	if (!arr || arr->type != JV_ARR || !val)
		return;
	if (arr->u.arr.len >= arr->u.arr.cap) {
		arr->u.arr.cap = arr->u.arr.cap ? arr->u.arr.cap * 2 : 4;
		arr->u.arr.a = xrealloc(arr->u.arr.a, arr->u.arr.cap * sizeof(jv*));
	}
	arr->u.arr.a[arr->u.arr.len++] = val;
}

static jv* jv_array_get(const jv *arr, int idx)
{
	if (!arr || arr->type != JV_ARR)
		return NULL;
	if (idx < 0)
		idx += arr->u.arr.len;
	if (idx < 0 || idx >= arr->u.arr.len)
		return NULL;
	return arr->u.arr.a[idx];
}

static void jv_array_set(jv *arr, int idx, jv *val)
{
	int i;
	if (!arr || arr->type != JV_ARR)
		return;
	if (idx < 0)
		idx += arr->u.arr.len;
	if (idx < 0)
		return;
	if (idx >= arr->u.arr.len) {
		if (idx >= arr->u.arr.cap) {
			arr->u.arr.cap = (idx + 1) * 2;
			arr->u.arr.a = xrealloc(arr->u.arr.a, arr->u.arr.cap * sizeof(jv*));
		}
		for (i = arr->u.arr.len; i < idx; i++)
			arr->u.arr.a[i] = jv_null();
		arr->u.arr.len = idx + 1;
		arr->u.arr.a[idx] = val;
	} else {
		jv_free(arr->u.arr.a[idx]);
		arr->u.arr.a[idx] = val;
	}
}

static void jv_array_delete(jv *arr, int idx)
{
	int i;
	if (!arr || arr->type != JV_ARR)
		return;
	if (idx < 0)
		idx += arr->u.arr.len;
	if (idx < 0 || idx >= arr->u.arr.len)
		return;
	jv_free(arr->u.arr.a[idx]);
	for (i = idx; i < arr->u.arr.len - 1; i++)
		arr->u.arr.a[i] = arr->u.arr.a[i + 1];
	arr->u.arr.len--;
}

static jv* jv_object_get(const jv *obj, const char *key)
{
	int i;
	if (!obj || obj->type != JV_OBJ || !key)
		return NULL;
	for (i = 0; i < obj->u.obj.len; i++) {
		if (strcmp(obj->u.obj.kv[i].k, key) == 0)
			return obj->u.obj.kv[i].v;
	}
	return NULL;
}

static void jv_object_set(jv *obj, const char *key, jv *val)
{
	int i;
	if (!obj || obj->type != JV_OBJ || !key) {
		jv_free(val);
		return;
	}
	for (i = 0; i < obj->u.obj.len; i++) {
		if (strcmp(obj->u.obj.kv[i].k, key) == 0) {
			jv_free(obj->u.obj.kv[i].v);
			obj->u.obj.kv[i].v = val;
			return;
		}
	}
	if (obj->u.obj.len >= obj->u.obj.cap) {
		obj->u.obj.cap = obj->u.obj.cap ? obj->u.obj.cap * 2 : 4;
		obj->u.obj.kv = xrealloc(obj->u.obj.kv, obj->u.obj.cap * sizeof(jv_kv));
	}
	obj->u.obj.kv[obj->u.obj.len].k = xstrdup(key);
	obj->u.obj.kv[obj->u.obj.len].v = val;
	obj->u.obj.len++;
}

static int jv_object_delete(jv *obj, const char *key)
{
	int i;
	if (!obj || obj->type != JV_OBJ || !key)
		return 0;
	for (i = 0; i < obj->u.obj.len; i++) {
		if (strcmp(obj->u.obj.kv[i].k, key) == 0) {
			free(obj->u.obj.kv[i].k);
			jv_free(obj->u.obj.kv[i].v);
			for (; i < obj->u.obj.len - 1; i++)
				obj->u.obj.kv[i] = obj->u.obj.kv[i + 1];
			obj->u.obj.len--;
			return 1;
		}
	}
	return 0;
}

/* --- JV List --- */

static void jv_list_init(jv_list *l)
{
	l->items = NULL;
	l->len = 0;
	l->cap = 0;
}

static void jv_list_append(jv_list *l, jv *val)
{
	if (!val)
		return;
	if (l->len >= l->cap) {
		l->cap = l->cap ? l->cap * 2 : 4;
		l->items = xrealloc(l->items, l->cap * sizeof(jv*));
	}
	l->items[l->len++] = val;
}

static void jv_list_concat(jv_list *dst, jv_list *src)
{
	int i;
	if (!src || src->len == 0) {
		if (src)
			free(src->items);
		return;
	}
	if (dst->len + src->len > dst->cap) {
		dst->cap = dst->len + src->len + 4;
		dst->items = xrealloc(dst->items, dst->cap * sizeof(jv*));
	}
	for (i = 0; i < src->len; i++)
		dst->items[dst->len++] = src->items[i];
	free(src->items);
	src->items = NULL;
	src->len = src->cap = 0;
}

static void jv_list_free(jv_list *l)
{
	int i;
	if (!l)
		return;
	for (i = 0; i < l->len; i++)
		jv_free(l->items[i]);
	free(l->items);
	l->items = NULL;
	l->len = l->cap = 0;
}

/* --- Comparison and Truthiness --- */

static int jv_is_truthy(const jv *v)
{
	if (!v)
		return 0;
	if (v->type == JV_NULL)
		return 0;
	if (v->type == JV_BOOL && !v->u.bval)
		return 0;
	return 1;
}

static int jv_compare(const jv *a, const jv *b)
{
	int i, diff, min_len;
	if (!a && !b)
		return 0;
	if (!a)
		return -1;
	if (!b)
		return 1;
	if (a->type != b->type)
		return (int)a->type - (int)b->type;
	switch (a->type) {
	case JV_NULL:
		return 0;
	case JV_BOOL:
		return a->u.bval - b->u.bval;
	case JV_NUM:
		if (a->u.nval < b->u.nval) return -1;
		if (a->u.nval > b->u.nval) return 1;
		return 0;
	case JV_STR:
		return strcmp(a->u.str.s, b->u.str.s);
	case JV_ARR:
		min_len = a->u.arr.len < b->u.arr.len ? a->u.arr.len : b->u.arr.len;
		for (i = 0; i < min_len; i++) {
			diff = jv_compare(a->u.arr.a[i], b->u.arr.a[i]);
			if (diff != 0)
				return diff;
		}
		return a->u.arr.len - b->u.arr.len;
	case JV_OBJ:
		if (a->u.obj.len != b->u.obj.len)
			return a->u.obj.len - b->u.obj.len;
		for (i = 0; i < a->u.obj.len; i++) {
			jv *bv = jv_object_get(b, a->u.obj.kv[i].k);
			if (!bv)
				return 1;
			diff = jv_compare(a->u.obj.kv[i].v, bv);
			if (diff != 0)
				return diff;
		}
		return 0;
	}
	return 0;
}

static int jv_equal(const jv *a, const jv *b)
{
	return jv_compare(a, b) == 0;
}

/* --- JSON Serializer --- */

static int key_cmp(const void *p1, const void *p2)
{
	const jv_kv *k1 = p1;
	const jv_kv *k2 = p2;
	return strcmp(k1->k, k2->k);
}

static void append_indent(str_buf *sb, int depth, int indent_width, int use_tabs)
{
	int i, count;
	if (use_tabs) {
		for (i = 0; i < depth; i++)
			str_buf_append_char(sb, '\t');
	} else {
		count = depth * indent_width;
		for (i = 0; i < count; i++)
			str_buf_append_char(sb, ' ');
	}
}

static void dump_string(str_buf *sb, const char *s, int len, int ascii_only)
{
	int i;
	const unsigned char *us = (const unsigned char*)s;
	str_buf_append_char(sb, '"');
	for (i = 0; i < len; i++) {
		unsigned char c = us[i];
		switch (c) {
		case '"':  str_buf_append_str(sb, "\\\""); break;
		case '\\': str_buf_append_str(sb, "\\\\"); break;
		case '\b': str_buf_append_str(sb, "\\b"); break;
		case '\f': str_buf_append_str(sb, "\\f"); break;
		case '\n': str_buf_append_str(sb, "\\n"); break;
		case '\r': str_buf_append_str(sb, "\\r"); break;
		case '\t': str_buf_append_str(sb, "\\t"); break;
		default:
			if (c < 0x20) {
				char h[8];
				snprintf(h, sizeof(h), "\\u%04x", c);
				str_buf_append_str(sb, h);
			} else if (ascii_only && c >= 0x80) {
				char h[8];
				snprintf(h, sizeof(h), "\\u%04x", c);
				str_buf_append_str(sb, h);
			} else {
				str_buf_append_char(sb, (char)c);
			}
			break;
		}
	}
	str_buf_append_char(sb, '"');
}

static void jv_dump_to_sb(str_buf *sb, const jv *v, int compact, int raw,
                          int indent_width, int use_tabs, int ascii_only,
                          int sort_keys, int depth)
{
	int i;
	char num_buf[64];
	if (!v) {
		str_buf_append_str(sb, "null");
		return;
	}
	switch (v->type) {
	case JV_NULL:
		str_buf_append_str(sb, "null");
		break;
	case JV_BOOL:
		str_buf_append_str(sb, v->u.bval ? "true" : "false");
		break;
	case JV_NUM:
		if (isnan(v->u.nval) || isinf(v->u.nval)) {
			str_buf_append_str(sb, "null");
		} else if (v->u.nval == (double)(long long)v->u.nval && fabs(v->u.nval) < 1e15) {
			snprintf(num_buf, sizeof(num_buf), "%lld", (long long)v->u.nval);
			str_buf_append_str(sb, num_buf);
		} else {
			snprintf(num_buf, sizeof(num_buf), "%.14g", v->u.nval);
			str_buf_append_str(sb, num_buf);
		}
		break;
	case JV_STR:
		if (raw && depth == 0) {
			str_buf_append_len(sb, v->u.str.s, v->u.str.len);
		} else {
			dump_string(sb, v->u.str.s, v->u.str.len, ascii_only);
		}
		break;
	case JV_ARR:
		if (v->u.arr.len == 0) {
			str_buf_append_str(sb, "[]");
			break;
		}
		if (compact) {
			str_buf_append_char(sb, '[');
			for (i = 0; i < v->u.arr.len; i++) {
				if (i > 0)
					str_buf_append_char(sb, ',');
				jv_dump_to_sb(sb, v->u.arr.a[i], 1, 0, indent_width, use_tabs, ascii_only, sort_keys, depth + 1);
			}
			str_buf_append_char(sb, ']');
		} else {
			str_buf_append_str(sb, "[\n");
			for (i = 0; i < v->u.arr.len; i++) {
				append_indent(sb, depth + 1, indent_width, use_tabs);
				jv_dump_to_sb(sb, v->u.arr.a[i], 0, 0, indent_width, use_tabs, ascii_only, sort_keys, depth + 1);
				if (i < v->u.arr.len - 1)
					str_buf_append_str(sb, ",\n");
				else
					str_buf_append_char(sb, '\n');
			}
			append_indent(sb, depth, indent_width, use_tabs);
			str_buf_append_char(sb, ']');
		}
		break;
	case JV_OBJ:
		if (v->u.obj.len == 0) {
			str_buf_append_str(sb, "{}");
			break;
		}
		if (sort_keys && v->u.obj.len > 1) {
			qsort(v->u.obj.kv, v->u.obj.len, sizeof(jv_kv), key_cmp);
		}
		if (compact) {
			str_buf_append_char(sb, '{');
			for (i = 0; i < v->u.obj.len; i++) {
				if (i > 0)
					str_buf_append_char(sb, ',');
				dump_string(sb, v->u.obj.kv[i].k, strlen(v->u.obj.kv[i].k), ascii_only);
				str_buf_append_char(sb, ':');
				jv_dump_to_sb(sb, v->u.obj.kv[i].v, 1, 0, indent_width, use_tabs, ascii_only, sort_keys, depth + 1);
			}
			str_buf_append_char(sb, '}');
		} else {
			str_buf_append_str(sb, "{\n");
			for (i = 0; i < v->u.obj.len; i++) {
				append_indent(sb, depth + 1, indent_width, use_tabs);
				dump_string(sb, v->u.obj.kv[i].k, strlen(v->u.obj.kv[i].k), ascii_only);
				str_buf_append_str(sb, ": ");
				jv_dump_to_sb(sb, v->u.obj.kv[i].v, 0, 0, indent_width, use_tabs, ascii_only, sort_keys, depth + 1);
				if (i < v->u.obj.len - 1)
					str_buf_append_str(sb, ",\n");
				else
					str_buf_append_char(sb, '\n');
			}
			append_indent(sb, depth, indent_width, use_tabs);
			str_buf_append_char(sb, '}');
		}
		break;
	}
}

static char* jv_to_string(const jv *v)
{
	str_buf sb;
	str_buf_init(&sb);
	jv_dump_to_sb(&sb, v, 1, 0, 0, 0, 0, 0, 0);
	return sb.buf ? sb.buf : xstrdup("");
}

/* --- JSON Parser --- */

static int parse_hex4(const char *s)
{
	int val = 0, i;
	for (i = 0; i < 4; i++) {
		char c = s[i];
		val <<= 4;
		if (c >= '0' && c <= '9') val |= (c - '0');
		else if (c >= 'a' && c <= 'f') val |= (c - 'a' + 10);
		else if (c >= 'A' && c <= 'F') val |= (c - 'A' + 10);
		else return -1;
	}
	return val;
}

static void skip_json_ws(const char **p, const char *end)
{
	while (*p < end) {
		char c = **p;
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
			(*p)++;
		} else if (c == '#') {
			while (*p < end && **p != '\n')
				(*p)++;
		} else {
			break;
		}
	}
}

static char* parse_json_string_raw(const char **p, const char *end, int *out_len, char **err)
{
	str_buf sb;
	str_buf_init(&sb);
	(*p)++; /* skip opening quote */
	while (*p < end) {
		char c = **p;
		if (c == '"') {
			(*p)++;
			if (out_len) *out_len = sb.len;
			return sb.buf ? sb.buf : xstrdup("");
		}
		if (c == '\\') {
			(*p)++;
			if (*p >= end) {
				*err = xstrdup("unexpected EOF in string escape");
				str_buf_free(&sb);
				return NULL;
			}
			c = *(*p)++;
			switch (c) {
			case '"':  str_buf_append_char(&sb, '"'); break;
			case '\\': str_buf_append_char(&sb, '\\'); break;
			case '/':  str_buf_append_char(&sb, '/'); break;
			case 'b':  str_buf_append_char(&sb, '\b'); break;
			case 'f':  str_buf_append_char(&sb, '\f'); break;
			case 'n':  str_buf_append_char(&sb, '\n'); break;
			case 'r':  str_buf_append_char(&sb, '\r'); break;
			case 't':  str_buf_append_char(&sb, '\t'); break;
			case 'u': {
				int cp, cp2;
				if (*p + 4 > end) {
					*err = xstrdup("truncated \\u escape");
					str_buf_free(&sb);
					return NULL;
				}
				cp = parse_hex4(*p);
				if (cp < 0) {
					*err = xstrdup("invalid hex in \\u escape");
					str_buf_free(&sb);
					return NULL;
				}
				*p += 4;
				if (cp >= 0xD800 && cp <= 0xDBFF && *p + 6 <= end) {
					if ((*p)[0] == '\\' && (*p)[1] == 'u') {
						cp2 = parse_hex4(*p + 2);
						if (cp2 >= 0xDC00 && cp2 <= 0xDFFF) {
							cp = 0x10000 + ((cp - 0xD800) << 10) + (cp2 - 0xDC00);
							*p += 6;
						}
					}
				}
				str_buf_append_utf8(&sb, cp);
				break;
			}
			default:
				str_buf_append_char(&sb, c);
				break;
			}
		} else {
			str_buf_append_char(&sb, c);
			(*p)++;
		}
	}
	*err = xstrdup("unterminated string");
	str_buf_free(&sb);
	return NULL;
}

static jv* parse_json_value(const char **p, const char *end, char **err);

static jv* parse_json_value(const char **p, const char *end, char **err)
{
	skip_json_ws(p, end);
	if (*p >= end)
		return NULL;

	if (**p == 'n') {
		if (*p + 4 <= end && strncmp(*p, "null", 4) == 0) {
			*p += 4;
			return jv_null();
		}
		*err = xstrdup("expected 'null'");
		return NULL;
	}
	if (**p == 't') {
		if (*p + 4 <= end && strncmp(*p, "true", 4) == 0) {
			*p += 4;
			return jv_bool(1);
		}
		*err = xstrdup("expected 'true'");
		return NULL;
	}
	if (**p == 'f') {
		if (*p + 5 <= end && strncmp(*p, "false", 5) == 0) {
			*p += 5;
			return jv_bool(0);
		}
		*err = xstrdup("expected 'false'");
		return NULL;
	}
	if (**p == '"') {
		int slen = 0;
		char *s = parse_json_string_raw(p, end, &slen, err);
		if (!s)
			return NULL;
		return jv_string_take(s, slen);
	}
	if (**p == '-' || (**p >= '0' && **p <= '9')) {
		char *num_end;
		double d = strtod(*p, &num_end);
		if (num_end == *p) {
			*err = xstrdup("invalid number");
			return NULL;
		}
		*p = num_end;
		return jv_number(d);
	}
	if (**p == '[') {
		jv *arr = jv_array();
		(*p)++;
		skip_json_ws(p, end);
		if (*p < end && **p == ']') {
			(*p)++;
			return arr;
		}
		while (*p < end) {
			jv *elem = parse_json_value(p, end, err);
			if (!elem && *err) {
				jv_free(arr);
				return NULL;
			}
			jv_array_append(arr, elem);
			skip_json_ws(p, end);
			if (*p < end && **p == ',') {
				(*p)++;
				skip_json_ws(p, end);
			} else if (*p < end && **p == ']') {
				(*p)++;
				return arr;
			} else {
				*err = xstrdup("expected ',' or ']' in array");
				jv_free(arr);
				return NULL;
			}
		}
		*err = xstrdup("unexpected EOF in array");
		jv_free(arr);
		return NULL;
	}
	if (**p == '{') {
		jv *obj = jv_object();
		(*p)++;
		skip_json_ws(p, end);
		if (*p < end && **p == '}') {
			(*p)++;
			return obj;
		}
		while (*p < end) {
			char *k;
			jv *val;
			skip_json_ws(p, end);
			if (*p >= end || **p != '"') {
				*err = xstrdup("expected string key in object");
				jv_free(obj);
				return NULL;
			}
			k = parse_json_string_raw(p, end, NULL, err);
			if (!k) {
				jv_free(obj);
				return NULL;
			}
			skip_json_ws(p, end);
			if (*p >= end || **p != ':') {
				*err = xstrdup("expected ':' after object key");
				free(k);
				jv_free(obj);
				return NULL;
			}
			(*p)++;
			val = parse_json_value(p, end, err);
			if (!val && *err) {
				free(k);
				jv_free(obj);
				return NULL;
			}
			jv_object_set(obj, k, val);
			free(k);
			skip_json_ws(p, end);
			if (*p < end && **p == ',') {
				(*p)++;
				skip_json_ws(p, end);
			} else if (*p < end && **p == '}') {
				(*p)++;
				return obj;
			} else {
				*err = xstrdup("expected ',' or '}' in object");
				jv_free(obj);
				return NULL;
			}
		}
		*err = xstrdup("unexpected EOF in object");
		jv_free(obj);
		return NULL;
	}

	*err = xasprintf("unexpected character '%c'", **p);
	return NULL;
}

/* --- Built-in String and Utility Helpers --- */

static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static jv* builtin_base64_encode(const jv *v)
{
	const unsigned char *src;
	char *out;
	int len, out_len, i, j;

	if (!v || v->type != JV_STR)
		return jv_null();
	src = (const unsigned char*)v->u.str.s;
	len = v->u.str.len;
	out_len = 4 * ((len + 2) / 3);
	out = xmalloc(out_len + 1);
	j = 0;
	for (i = 0; i < len; i += 3) {
		unsigned b0 = src[i];
		unsigned b1 = (i + 1 < len) ? src[i + 1] : 0;
		unsigned b2 = (i + 2 < len) ? src[i + 2] : 0;
		unsigned triple = (b0 << 16) | (b1 << 8) | b2;
		out[j++] = b64_table[(triple >> 18) & 0x3F];
		out[j++] = b64_table[(triple >> 12) & 0x3F];
		out[j++] = (i + 1 < len) ? b64_table[(triple >> 6) & 0x3F] : '=';
		out[j++] = (i + 2 < len) ? b64_table[triple & 0x3F] : '=';
	}
	out[j] = '\0';
	return jv_string_take(out, j);
}

static int b64_val(char c)
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}

static jv* builtin_base64_decode(const jv *v)
{
	const char *s;
	str_buf sb;
	int val = 0, valb = -8;

	if (!v || v->type != JV_STR)
		return jv_null();
	s = v->u.str.s;
	str_buf_init(&sb);
	while (*s) {
		int c = b64_val(*s++);
		if (c == -1)
			continue;
		val = (val << 6) | c;
		valb += 6;
		if (valb >= 0) {
			str_buf_append_char(&sb, (char)((val >> valb) & 0xFF));
			valb -= 8;
		}
	}
	return jv_string_take(sb.buf, sb.len);
}

static jv* builtin_uri_encode(const jv *v)
{
	const unsigned char *s;
	str_buf sb;

	if (!v || v->type != JV_STR)
		return jv_null();
	s = (const unsigned char*)v->u.str.s;
	str_buf_init(&sb);
	while (*s) {
		unsigned char c = *s++;
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		    (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
			str_buf_append_char(&sb, c);
		} else {
			char hex[4];
			snprintf(hex, sizeof(hex), "%%%02X", c);
			str_buf_append_str(&sb, hex);
		}
	}
	return jv_string_take(sb.buf, sb.len);
}

static jv* builtin_sh_quote(const jv *v)
{
	str_buf sb;
	const char *s;
	int i;

	if (!v)
		return jv_null();
	str_buf_init(&sb);
	if (v->type == JV_STR) {
		str_buf_append_char(&sb, '\'');
		s = v->u.str.s;
		while (*s) {
			if (*s == '\'')
				str_buf_append_str(&sb, "'\\''");
			else
				str_buf_append_char(&sb, *s);
			s++;
		}
		str_buf_append_char(&sb, '\'');
	} else if (v->type == JV_ARR) {
		for (i = 0; i < v->u.arr.len; i++) {
			jv *elem_sh = builtin_sh_quote(v->u.arr.a[i]);
			if (i > 0)
				str_buf_append_char(&sb, ' ');
			if (elem_sh && elem_sh->type == JV_STR)
				str_buf_append_str(&sb, elem_sh->u.str.s);
			jv_free(elem_sh);
		}
	} else {
		char *dump = jv_to_string(v);
		str_buf_append_str(&sb, dump);
		free(dump);
	}
	return jv_string_take(sb.buf, sb.len);
}

static jv* builtin_html_escape(const jv *v)
{
	str_buf sb;
	const char *s;

	if (!v || v->type != JV_STR)
		return jv_null();
	str_buf_init(&sb);
	s = v->u.str.s;
	while (*s) {
		switch (*s) {
		case '&':  str_buf_append_str(&sb, "&amp;"); break;
		case '<':  str_buf_append_str(&sb, "&lt;"); break;
		case '>':  str_buf_append_str(&sb, "&gt;"); break;
		case '"':  str_buf_append_str(&sb, "&quot;"); break;
		case '\'': str_buf_append_str(&sb, "&apos;"); break;
		default:   str_buf_append_char(&sb, *s); break;
		}
		s++;
	}
	return jv_string_take(sb.buf, sb.len);
}

static jv* builtin_csv(const jv *v)
{
	str_buf sb;
	int i;

	if (!v || v->type != JV_ARR)
		return jv_null();
	str_buf_init(&sb);
	for (i = 0; i < v->u.arr.len; i++) {
		jv *item = v->u.arr.a[i];
		if (i > 0)
			str_buf_append_char(&sb, ',');
		if (item->type == JV_STR) {
			const char *s = item->u.str.s;
			str_buf_append_char(&sb, '"');
			while (*s) {
				if (*s == '"')
					str_buf_append_str(&sb, "\"\"");
				else
					str_buf_append_char(&sb, *s);
				s++;
			}
			str_buf_append_char(&sb, '"');
		} else {
			char *str = jv_to_string(item);
			str_buf_append_str(&sb, str);
			free(str);
		}
	}
	return jv_string_take(sb.buf, sb.len);
}

static jv* builtin_tsv(const jv *v)
{
	str_buf sb;
	int i;

	if (!v || v->type != JV_ARR)
		return jv_null();
	str_buf_init(&sb);
	for (i = 0; i < v->u.arr.len; i++) {
		jv *item = v->u.arr.a[i];
		if (i > 0)
			str_buf_append_char(&sb, '\t');
		if (item->type == JV_STR) {
			const char *s = item->u.str.s;
			while (*s) {
				if (*s == '\t')
					str_buf_append_str(&sb, "\\t");
				else if (*s == '\n')
					str_buf_append_str(&sb, "\\n");
				else if (*s == '\r')
					str_buf_append_str(&sb, "\\r");
				else if (*s == '\\')
					str_buf_append_str(&sb, "\\\\");
				else
					str_buf_append_char(&sb, *s);
				s++;
			}
		} else {
			char *str = jv_to_string(item);
			str_buf_append_str(&sb, str);
			free(str);
		}
	}
	return jv_string_take(sb.buf, sb.len);
}

static jv* builtin_split(const jv *v, const jv *delim)
{
	jv *res;
	const char *s, *sep, *p;
	int seplen;

	if (!v || v->type != JV_STR || !delim || delim->type != JV_STR)
		return jv_null();
	res = jv_array();
	s = v->u.str.s;
	sep = delim->u.str.s;
	seplen = delim->u.str.len;
	if (seplen == 0) {
		while (*s) {
			jv_array_append(res, jv_string_sized(s, 1));
			s++;
		}
		return res;
	}
	while ((p = strstr(s, sep)) != NULL) {
		jv_array_append(res, jv_string_sized(s, p - s));
		s = p + seplen;
	}
	jv_array_append(res, jv_string(s));
	return res;
}

static jv* builtin_join(const jv *v, const jv *delim)
{
	str_buf sb;
	int i;
	const char *sep;

	if (!v || v->type != JV_ARR || !delim || delim->type != JV_STR)
		return jv_null();
	sep = delim->u.str.s;
	str_buf_init(&sb);
	for (i = 0; i < v->u.arr.len; i++) {
		jv *elem = v->u.arr.a[i];
		if (i > 0)
			str_buf_append_str(&sb, sep);
		if (elem->type == JV_STR) {
			str_buf_append_str(&sb, elem->u.str.s);
		} else {
			char *s = jv_to_string(elem);
			str_buf_append_str(&sb, s);
			free(s);
		}
	}
	return jv_string_take(sb.buf, sb.len);
}

static jv* builtin_ltrimstr(const jv *v, const jv *prefix)
{
	if (!v || v->type != JV_STR) return jv_copy(v);
	if (!prefix || prefix->type != JV_STR) return jv_copy(v);
	if (strncmp(v->u.str.s, prefix->u.str.s, prefix->u.str.len) == 0)
		return jv_string(v->u.str.s + prefix->u.str.len);
	return jv_copy(v);
}

static jv* builtin_rtrimstr(const jv *v, const jv *suffix)
{
	int vlen, slen;
	if (!v || v->type != JV_STR) return jv_copy(v);
	if (!suffix || suffix->type != JV_STR) return jv_copy(v);
	vlen = v->u.str.len;
	slen = suffix->u.str.len;
	if (vlen >= slen && strcmp(v->u.str.s + vlen - slen, suffix->u.str.s) == 0)
		return jv_string_sized(v->u.str.s, vlen - slen);
	return jv_copy(v);
}

static jv* builtin_startswith(const jv *v, const jv *prefix)
{
	if (!v || v->type != JV_STR || !prefix || prefix->type != JV_STR)
		return jv_bool(0);
	return jv_bool(strncmp(v->u.str.s, prefix->u.str.s, prefix->u.str.len) == 0);
}

static jv* builtin_endswith(const jv *v, const jv *suffix)
{
	int vlen, slen;
	if (!v || v->type != JV_STR || !suffix || suffix->type != JV_STR)
		return jv_bool(0);
	vlen = v->u.str.len;
	slen = suffix->u.str.len;
	if (vlen < slen)
		return jv_bool(0);
	return jv_bool(strcmp(v->u.str.s + vlen - slen, suffix->u.str.s) == 0);
}

static jv* builtin_ascii_case(const jv *v, int upcase)
{
	char *s;
	int i;

	if (!v || v->type != JV_STR)
		return jv_null();
	s = xstrdup(v->u.str.s);
	for (i = 0; s[i]; i++)
		s[i] = upcase ? toupper((unsigned char)s[i]) : tolower((unsigned char)s[i]);
	return jv_string_take(s, v->u.str.len);
}

static jv* builtin_ascii(const jv *v)
{
	char b[2];
	if (!v || v->type != JV_NUM)
		return jv_null();
	b[0] = (char)((int)v->u.nval & 0x7F);
	b[1] = '\0';
	return jv_string(b);
}

static jv* builtin_explode(const jv *v)
{
	jv *res;
	const unsigned char *s;

	if (!v || v->type != JV_STR)
		return jv_null();
	res = jv_array();
	s = (const unsigned char*)v->u.str.s;
	while (*s) {
		unsigned cp;
		if (*s < 0x80) {
			cp = *s++;
		} else if ((*s & 0xE0) == 0xC0) {
			cp = (*s++ & 0x1F) << 6;
			if (*s) cp |= (*s++ & 0x3F);
		} else if ((*s & 0xF0) == 0xE0) {
			cp = (*s++ & 0x0F) << 12;
			if (*s) cp |= (*s++ & 0x3F) << 6;
			if (*s) cp |= (*s++ & 0x3F);
		} else if ((*s & 0xF8) == 0xF0) {
			cp = (*s++ & 0x07) << 18;
			if (*s) cp |= (*s++ & 0x3F) << 12;
			if (*s) cp |= (*s++ & 0x3F) << 6;
			if (*s) cp |= (*s++ & 0x3F);
		} else {
			cp = *s++;
		}
		jv_array_append(res, jv_number(cp));
	}
	return res;
}

static jv* builtin_implode(const jv *v)
{
	str_buf sb;
	int i;

	if (!v || v->type != JV_ARR)
		return jv_null();
	str_buf_init(&sb);
	for (i = 0; i < v->u.arr.len; i++) {
		if (v->u.arr.a[i]->type == JV_NUM)
			str_buf_append_utf8(&sb, (unsigned)v->u.arr.a[i]->u.nval);
	}
	return jv_string_take(sb.buf, sb.len);
}

static void flatten_helper(jv *dst, const jv *src, int cur_depth, int max_depth)
{
	int i;
	if (!src || src->type != JV_ARR) {
		jv_array_append(dst, jv_copy(src));
		return;
	}
	for (i = 0; i < src->u.arr.len; i++) {
		jv *elem = src->u.arr.a[i];
		if (elem && elem->type == JV_ARR && cur_depth < max_depth) {
			flatten_helper(dst, elem, cur_depth + 1, max_depth);
		} else {
			jv_array_append(dst, jv_copy(elem));
		}
	}
}

static jv* builtin_flatten(const jv *v, int depth)
{
	jv *res;
	if (!v || v->type != JV_ARR)
		return jv_null();
	if (depth < 0)
		depth = 1000000;
	res = jv_array();
	flatten_helper(res, v, 0, depth);
	return res;
}

static int jv_contains(const jv *a, const jv *b)
{
	int i, j;
	if (!a || !b)
		return (a == b);
	if (a->type != b->type)
		return 0;
	switch (a->type) {
	case JV_NULL:
		return 1;
	case JV_BOOL:
		return a->u.bval == b->u.bval;
	case JV_NUM:
		return a->u.nval == b->u.nval;
	case JV_STR:
		return strstr(a->u.str.s, b->u.str.s) != NULL;
	case JV_ARR:
		for (i = 0; i < b->u.arr.len; i++) {
			int found = 0;
			for (j = 0; j < a->u.arr.len; j++) {
				if (jv_contains(a->u.arr.a[j], b->u.arr.a[i])) {
					found = 1;
					break;
				}
			}
			if (!found)
				return 0;
		}
		return 1;
	case JV_OBJ:
		for (i = 0; i < b->u.obj.len; i++) {
			jv *av = jv_object_get(a, b->u.obj.kv[i].k);
			if (!av || !jv_contains(av, b->u.obj.kv[i].v))
				return 0;
		}
		return 1;
	}
	return 0;
}

/* --- AST Definitions and Parser --- */

typedef enum {
	AST_ID,
	AST_LITERAL,
	AST_VAR,
	AST_FIELD,
	AST_INDEX,
	AST_SLICE,
	AST_ITER,
	AST_PIPE,
	AST_COMMA,
	AST_ALT,
	AST_ASSIGN,
	AST_UPDATE,
	AST_BINOP,
	AST_UNOP_NEG,
	AST_ARR_CONSTR,
	AST_OBJ_CONSTR,
	AST_IF,
	AST_BIND,
	AST_TRY_CATCH,
	AST_CALL,
	AST_REDUCE,
	AST_FOREACH,
	AST_FORMAT,
	AST_PATH,
	AST_DEL
} ast_type;

enum {
	OP_ADD = 1,
	OP_SUB,
	OP_MUL,
	OP_DIV,
	OP_MOD,
	OP_EQ,
	OP_NEQ,
	OP_LT,
	OP_LE,
	OP_GT,
	OP_GE,
	OP_AND,
	OP_OR
};

typedef struct ast_node ast_node;

typedef struct ast_kv {
	ast_node *key;
	ast_node *val;
} ast_kv;

struct ast_node {
	ast_type type;
	int optional;
	int binop;
	char *sval;
	jv *literal;
	ast_node *left;
	ast_node *right;
	ast_node *cond;
	ast_node *init;
	ast_node **args;
	int num_args;
	ast_kv *obj_pairs;
	int num_pairs;
};

static ast_node* ast_new(ast_type type)
{
	ast_node *n = xzalloc(sizeof(*n));
	n->type = type;
	return n;
}

static void ast_free(ast_node *n)
{
	int i;
	if (!n)
		return;
	free(n->sval);
	jv_free(n->literal);
	ast_free(n->left);
	ast_free(n->right);
	ast_free(n->cond);
	ast_free(n->init);
	if (n->args) {
		for (i = 0; i < n->num_args; i++)
			ast_free(n->args[i]);
		free(n->args);
	}
	if (n->obj_pairs) {
		for (i = 0; i < n->num_pairs; i++) {
			ast_free(n->obj_pairs[i].key);
			ast_free(n->obj_pairs[i].val);
		}
		free(n->obj_pairs);
	}
	free(n);
}

typedef enum {
	TOK_EOF = 0,
	TOK_DOT,
	TOK_DOT_DOT,
	TOK_FIELD,
	TOK_IDENT,
	TOK_VAR,
	TOK_NUM,
	TOK_STR,
	TOK_LBRACKET,
	TOK_RBRACKET,
	TOK_LBRACE,
	TOK_RBRACE,
	TOK_LPAREN,
	TOK_RPAREN,
	TOK_PIPE,
	TOK_COMMA,
	TOK_SEMI,
	TOK_COLON,
	TOK_QUESTION,
	TOK_ALT,
	TOK_ASSIGN,
	TOK_SET_PIPE,
	TOK_SET_PLUS,
	TOK_SET_MINUS,
	TOK_SET_MULT,
	TOK_SET_DIV,
	TOK_SET_MOD,
	TOK_SET_ALT,
	TOK_EQ,
	TOK_NEQ,
	TOK_LE,
	TOK_GE,
	TOK_LT,
	TOK_GT,
	TOK_PLUS,
	TOK_MINUS,
	TOK_STAR,
	TOK_SLASH,
	TOK_PERCENT,
	TOK_FORMAT
} token_type;

typedef struct {
	const char *src;
	const char *p;
	token_type curr_tok;
	char *sval;
	double nval;
} lexer_state;

static void lexer_skip_ws(lexer_state *l)
{
	while (*l->p) {
		char c = *l->p;
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
			l->p++;
		} else if (c == '#') {
			while (*l->p && *l->p != '\n')
				l->p++;
		} else {
			break;
		}
	}
}

static token_type lexer_next(lexer_state *l)
{
	free(l->sval);
	l->sval = NULL;
	lexer_skip_ws(l);

	if (!*l->p)
		return l->curr_tok = TOK_EOF;

	if (*l->p == '.') {
		if (l->p[1] == '.') {
			l->p += 2;
			return l->curr_tok = TOK_DOT_DOT;
		}
		if (isalpha(l->p[1]) || l->p[1] == '_') {
			const char *start;
			l->p++;
			start = l->p;
			while (isalnum(*l->p) || *l->p == '_')
				l->p++;
			l->sval = xstrndup(start, l->p - start);
			return l->curr_tok = TOK_FIELD;
		}
		if (l->p[1] == '"') {
			int slen = 0;
			char *err = NULL;
			l->p++;
			l->sval = parse_json_string_raw(&l->p, l->p + strlen(l->p), &slen, &err);
			free(err);
			return l->curr_tok = TOK_FIELD;
		}
		l->p++;
		return l->curr_tok = TOK_DOT;
	}

	if (*l->p == '$') {
		const char *start;
		l->p++;
		start = l->p;
		while (isalnum(*l->p) || *l->p == '_')
			l->p++;
		l->sval = xstrndup(start, l->p - start);
		return l->curr_tok = TOK_VAR;
	}

	if (*l->p == '@') {
		const char *start;
		l->p++;
		start = l->p;
		while (isalnum(*l->p) || *l->p == '_')
			l->p++;
		l->sval = xstrndup(start, l->p - start);
		return l->curr_tok = TOK_FORMAT;
	}

	if (*l->p == '"') {
		int slen = 0;
		char *err = NULL;
		l->sval = parse_json_string_raw(&l->p, l->p + strlen(l->p), &slen, &err);
		free(err);
		return l->curr_tok = TOK_STR;
	}

	if (isdigit(*l->p)) {
		char *endp;
		l->nval = strtod(l->p, &endp);
		l->p = endp;
		return l->curr_tok = TOK_NUM;
	}

	if (isalpha(*l->p) || *l->p == '_') {
		const char *start = l->p;
		while (isalnum(*l->p) || *l->p == '_')
			l->p++;
		l->sval = xstrndup(start, l->p - start);
		return l->curr_tok = TOK_IDENT;
	}

	if (*l->p == '|') {
		if (l->p[1] == '=') {
			l->p += 2;
			return l->curr_tok = TOK_SET_PIPE;
		}
		l->p++;
		return l->curr_tok = TOK_PIPE;
	}

	if (*l->p == '/') {
		if (l->p[1] == '/') {
			if (l->p[2] == '=') {
				l->p += 3;
				return l->curr_tok = TOK_SET_ALT;
			}
			l->p += 2;
			return l->curr_tok = TOK_ALT;
		}
		if (l->p[1] == '=') {
			l->p += 2;
			return l->curr_tok = TOK_SET_DIV;
		}
		l->p++;
		return l->curr_tok = TOK_SLASH;
	}

	if (*l->p == '+') {
		if (l->p[1] == '=') {
			l->p += 2;
			return l->curr_tok = TOK_SET_PLUS;
		}
		l->p++;
		return l->curr_tok = TOK_PLUS;
	}

	if (*l->p == '-') {
		if (l->p[1] == '=') {
			l->p += 2;
			return l->curr_tok = TOK_SET_MINUS;
		}
		l->p++;
		return l->curr_tok = TOK_MINUS;
	}

	if (*l->p == '*') {
		if (l->p[1] == '=') {
			l->p += 2;
			return l->curr_tok = TOK_SET_MULT;
		}
		l->p++;
		return l->curr_tok = TOK_STAR;
	}

	if (*l->p == '%') {
		if (l->p[1] == '=') {
			l->p += 2;
			return l->curr_tok = TOK_SET_MOD;
		}
		l->p++;
		return l->curr_tok = TOK_PERCENT;
	}

	if (*l->p == '=') {
		if (l->p[1] == '=') {
			l->p += 2;
			return l->curr_tok = TOK_EQ;
		}
		l->p++;
		return l->curr_tok = TOK_ASSIGN;
	}

	if (*l->p == '!') {
		if (l->p[1] == '=') {
			l->p += 2;
			return l->curr_tok = TOK_NEQ;
		}
	}

	if (*l->p == '<') {
		if (l->p[1] == '=') {
			l->p += 2;
			return l->curr_tok = TOK_LE;
		}
		l->p++;
		return l->curr_tok = TOK_LT;
	}

	if (*l->p == '>') {
		if (l->p[1] == '=') {
			l->p += 2;
			return l->curr_tok = TOK_GE;
		}
		l->p++;
		return l->curr_tok = TOK_GT;
	}

	switch (*l->p) {
	case '[': l->p++; return l->curr_tok = TOK_LBRACKET;
	case ']': l->p++; return l->curr_tok = TOK_RBRACKET;
	case '{': l->p++; return l->curr_tok = TOK_LBRACE;
	case '}': l->p++; return l->curr_tok = TOK_RBRACE;
	case '(': l->p++; return l->curr_tok = TOK_LPAREN;
	case ')': l->p++; return l->curr_tok = TOK_RPAREN;
	case ',': l->p++; return l->curr_tok = TOK_COMMA;
	case ';': l->p++; return l->curr_tok = TOK_SEMI;
	case ':': l->p++; return l->curr_tok = TOK_COLON;
	case '?': l->p++; return l->curr_tok = TOK_QUESTION;
	}

	l->p++;
	return l->curr_tok = TOK_EOF;
}

static ast_node* parse_pipe_ext(lexer_state *l, int stop_at_as);
static ast_node* parse_pipe(lexer_state *l);
static ast_node* parse_pipe_no_comma(lexer_state *l);

static ast_node* parse_primary(lexer_state *l)
{
	ast_node *n;

	if (l->curr_tok == TOK_DOT) {
		lexer_next(l);
		n = ast_new(AST_ID);
		return n;
	}
	if (l->curr_tok == TOK_DOT_DOT) {
		lexer_next(l);
		n = ast_new(AST_CALL);
		n->sval = xstrdup("recurse");
		return n;
	}
	if (l->curr_tok == TOK_FIELD) {
		n = ast_new(AST_FIELD);
		n->sval = xstrdup(l->sval);
		n->left = ast_new(AST_ID);
		lexer_next(l);
		return n;
	}
	if (l->curr_tok == TOK_VAR) {
		n = ast_new(AST_VAR);
		n->sval = xstrdup(l->sval);
		lexer_next(l);
		return n;
	}
	if (l->curr_tok == TOK_FORMAT) {
		n = ast_new(AST_FORMAT);
		n->sval = xstrdup(l->sval);
		lexer_next(l);
		return n;
	}
	if (l->curr_tok == TOK_STR) {
		n = ast_new(AST_LITERAL);
		n->literal = jv_string(l->sval);
		lexer_next(l);
		return n;
	}
	if (l->curr_tok == TOK_NUM) {
		n = ast_new(AST_LITERAL);
		n->literal = jv_number(l->nval);
		lexer_next(l);
		return n;
	}
	if (l->curr_tok == TOK_LPAREN) {
		lexer_next(l);
		n = parse_pipe(l);
		if (l->curr_tok == TOK_RPAREN)
			lexer_next(l);
		return n;
	}
	if (l->curr_tok == TOK_LBRACKET) {
		lexer_next(l);
		if (l->curr_tok == TOK_RBRACKET) {
			lexer_next(l);
			return ast_new(AST_ARR_CONSTR);
		}
		n = ast_new(AST_ARR_CONSTR);
		n->left = parse_pipe(l);
		if (l->curr_tok == TOK_RBRACKET)
			lexer_next(l);
		return n;
	}
	if (l->curr_tok == TOK_LBRACE) {
		ast_kv *pairs = NULL;
		int count = 0, cap = 0;
		lexer_next(l);
		if (l->curr_tok == TOK_RBRACE) {
			lexer_next(l);
			return ast_new(AST_OBJ_CONSTR);
		}
		while (l->curr_tok != TOK_EOF && l->curr_tok != TOK_RBRACE) {
			ast_node *k = NULL, *v = NULL;
			if (l->curr_tok == TOK_LPAREN) {
				lexer_next(l);
				k = parse_pipe(l);
				if (l->curr_tok == TOK_RPAREN)
					lexer_next(l);
				if (l->curr_tok == TOK_COLON)
					lexer_next(l);
				v = parse_pipe_no_comma(l);
			} else if (l->curr_tok == TOK_IDENT || l->curr_tok == TOK_FIELD || l->curr_tok == TOK_STR) {
				char *name = xstrdup(l->sval);
				lexer_next(l);
				k = ast_new(AST_LITERAL);
				k->literal = jv_string(name);
				if (l->curr_tok == TOK_COLON) {
					lexer_next(l);
					v = parse_pipe_no_comma(l);
				} else {
					v = ast_new(AST_FIELD);
					v->sval = xstrdup(name);
					v->left = ast_new(AST_ID);
				}
				free(name);
			} else {
				break;
			}
			if (count >= cap) {
				cap = cap ? cap * 2 : 4;
				pairs = xrealloc(pairs, cap * sizeof(ast_kv));
			}
			pairs[count].key = k;
			pairs[count].val = v;
			count++;
			if (l->curr_tok == TOK_COMMA)
				lexer_next(l);
			else
				break;
		}
		if (l->curr_tok == TOK_RBRACE)
			lexer_next(l);
		n = ast_new(AST_OBJ_CONSTR);
		n->obj_pairs = pairs;
		n->num_pairs = count;
		return n;
	}
	if (l->curr_tok == TOK_IDENT) {
		char *id = xstrdup(l->sval);
		lexer_next(l);
		if (strcmp(id, "null") == 0) {
			free(id);
			n = ast_new(AST_LITERAL);
			n->literal = jv_null();
			return n;
		}
		if (strcmp(id, "true") == 0) {
			free(id);
			n = ast_new(AST_LITERAL);
			n->literal = jv_bool(1);
			return n;
		}
		if (strcmp(id, "false") == 0) {
			free(id);
			n = ast_new(AST_LITERAL);
			n->literal = jv_bool(0);
			return n;
		}
		if (strcmp(id, "if") == 0) {
			ast_node *cond = parse_pipe(l);
			ast_node *then_br = NULL, *else_br = NULL;
			if (l->curr_tok == TOK_IDENT && strcmp(l->sval, "then") == 0)
				lexer_next(l);
			then_br = parse_pipe(l);
			while (l->curr_tok == TOK_IDENT && strcmp(l->sval, "elif") == 0) {
				ast_node *elif_cond, *elif_then, *elif_node;
				lexer_next(l);
				elif_cond = parse_pipe(l);
				if (l->curr_tok == TOK_IDENT && strcmp(l->sval, "then") == 0)
					lexer_next(l);
				elif_then = parse_pipe(l);
				elif_node = ast_new(AST_IF);
				elif_node->cond = elif_cond;
				elif_node->left = elif_then;
				else_br = elif_node;
			}
			if (l->curr_tok == TOK_IDENT && strcmp(l->sval, "else") == 0) {
				lexer_next(l);
				if (else_br) {
					ast_node *cur = else_br;
					while (cur->right) cur = cur->right;
					cur->right = parse_pipe(l);
				} else {
					else_br = parse_pipe(l);
				}
			} else if (!else_br) {
				else_br = ast_new(AST_ID);
			}
			if (l->curr_tok == TOK_IDENT && strcmp(l->sval, "end") == 0)
				lexer_next(l);
			free(id);
			n = ast_new(AST_IF);
			n->cond = cond;
			n->left = then_br;
			n->right = else_br;
			return n;
		}
		if (strcmp(id, "try") == 0) {
			ast_node *body = parse_pipe(l);
			ast_node *handler = NULL;
			free(id);
			if (l->curr_tok == TOK_IDENT && strcmp(l->sval, "catch") == 0) {
				lexer_next(l);
				handler = parse_pipe(l);
			}
			n = ast_new(AST_TRY_CATCH);
			n->left = body;
			n->right = handler;
			return n;
		}
		if (strcmp(id, "reduce") == 0 || strcmp(id, "foreach") == 0) {
			int is_reduce = (strcmp(id, "reduce") == 0);
			ast_node *gen, *init_node, *update_node;
			char *var_name = NULL;
			free(id);
			gen = parse_pipe_ext(l, 1);
			if (l->curr_tok == TOK_IDENT && strcmp(l->sval, "as") == 0)
				lexer_next(l);
			if (l->curr_tok == TOK_VAR) {
				var_name = xstrdup(l->sval);
				lexer_next(l);
			}
			if (l->curr_tok == TOK_LPAREN)
				lexer_next(l);
			init_node = parse_pipe(l);
			if (l->curr_tok == TOK_SEMI)
				lexer_next(l);
			update_node = parse_pipe(l);
			if (l->curr_tok == TOK_RPAREN)
				lexer_next(l);
			n = ast_new(is_reduce ? AST_REDUCE : AST_FOREACH);
			n->sval = var_name;
			n->left = gen;
			n->init = init_node;
			n->right = update_node;
			return n;
		}
		if (strcmp(id, "del") == 0) {
			free(id);
			if (l->curr_tok == TOK_LPAREN) {
				lexer_next(l);
				n = ast_new(AST_DEL);
				n->left = parse_pipe(l);
				if (l->curr_tok == TOK_RPAREN)
					lexer_next(l);
				return n;
			}
			n = ast_new(AST_CALL);
			n->sval = xstrdup("del");
			return n;
		}
		if (strcmp(id, "path") == 0) {
			free(id);
			if (l->curr_tok == TOK_LPAREN) {
				lexer_next(l);
				n = ast_new(AST_PATH);
				n->left = parse_pipe(l);
				if (l->curr_tok == TOK_RPAREN)
					lexer_next(l);
				return n;
			}
			n = ast_new(AST_CALL);
			n->sval = xstrdup("path");
			return n;
		}

		/* Function call: id(...) or 0-arg call */
		if (l->curr_tok == TOK_LPAREN) {
			ast_node **args = NULL;
			int count = 0, cap = 0;
			lexer_next(l);
			if (l->curr_tok != TOK_RPAREN) {
				while (l->curr_tok != TOK_EOF && l->curr_tok != TOK_RPAREN) {
					ast_node *arg = parse_pipe(l);
					if (count >= cap) {
						cap = cap ? cap * 2 : 2;
						args = xrealloc(args, cap * sizeof(ast_node*));
					}
					args[count++] = arg;
					if (l->curr_tok == TOK_SEMI)
						lexer_next(l);
					else
						break;
				}
			}
			if (l->curr_tok == TOK_RPAREN)
				lexer_next(l);
			n = ast_new(AST_CALL);
			n->sval = id;
			n->args = args;
			n->num_args = count;
			return n;
		}

		n = ast_new(AST_CALL);
		n->sval = id;
		return n;
	}

	return ast_new(AST_ID);
}

static ast_node* parse_postfix(lexer_state *l)
{
	ast_node *n = parse_primary(l);

	while (1) {
		if (l->curr_tok == TOK_FIELD) {
			ast_node *f = ast_new(AST_FIELD);
			f->sval = xstrdup(l->sval);
			f->left = n;
			lexer_next(l);
			if (l->curr_tok == TOK_QUESTION) {
				f->optional = 1;
				lexer_next(l);
			}
			n = f;
			continue;
		}
		if (l->curr_tok == TOK_LBRACKET) {
			lexer_next(l);
			if (l->curr_tok == TOK_RBRACKET) {
				ast_node *it = ast_new(AST_ITER);
				it->left = n;
				lexer_next(l);
				if (l->curr_tok == TOK_QUESTION) {
					it->optional = 1;
					lexer_next(l);
				}
				n = it;
				continue;
			}
			if (l->curr_tok == TOK_COLON) {
				ast_node *sl = ast_new(AST_SLICE);
				sl->left = n;
				lexer_next(l);
				if (l->curr_tok != TOK_RBRACKET)
					sl->right = parse_pipe(l);
				if (l->curr_tok == TOK_RBRACKET)
					lexer_next(l);
				if (l->curr_tok == TOK_QUESTION) {
					sl->optional = 1;
					lexer_next(l);
				}
				n = sl;
				continue;
			}
			{
				ast_node *sub = parse_pipe(l);
				if (l->curr_tok == TOK_COLON) {
					ast_node *sl = ast_new(AST_SLICE);
					sl->left = n;
					sl->init = sub; /* start */
					lexer_next(l);
					if (l->curr_tok != TOK_RBRACKET)
						sl->right = parse_pipe(l); /* end */
					if (l->curr_tok == TOK_RBRACKET)
						lexer_next(l);
					if (l->curr_tok == TOK_QUESTION) {
						sl->optional = 1;
						lexer_next(l);
					}
					n = sl;
					continue;
				}
				if (l->curr_tok == TOK_RBRACKET)
					lexer_next(l);
				if (sub->type == AST_LITERAL && sub->literal && sub->literal->type == JV_NUM) {
					ast_node *idx = ast_new(AST_INDEX);
					idx->left = n;
					idx->literal = jv_copy(sub->literal);
					ast_free(sub);
					if (l->curr_tok == TOK_QUESTION) {
						idx->optional = 1;
						lexer_next(l);
					}
					n = idx;
				} else {
					ast_node *f = ast_new(AST_INDEX);
					f->left = n;
					f->right = sub;
					if (l->curr_tok == TOK_QUESTION) {
						f->optional = 1;
						lexer_next(l);
					}
					n = f;
				}
				continue;
			}
		}
		if (l->curr_tok == TOK_QUESTION) {
			n->optional = 1;
			lexer_next(l);
			continue;
		}
		break;
	}
	return n;
}

static ast_node* parse_unary(lexer_state *l)
{
	if (l->curr_tok == TOK_MINUS) {
		ast_node *n;
		lexer_next(l);
		n = ast_new(AST_UNOP_NEG);
		n->left = parse_unary(l);
		return n;
	}
	return parse_postfix(l);
}

static ast_node* parse_mul(lexer_state *l)
{
	ast_node *n = parse_unary(l);
	while (l->curr_tok == TOK_STAR || l->curr_tok == TOK_SLASH || l->curr_tok == TOK_PERCENT) {
		int op = (l->curr_tok == TOK_STAR ? OP_MUL : (l->curr_tok == TOK_SLASH ? OP_DIV : OP_MOD));
		ast_node *b = ast_new(AST_BINOP);
		b->binop = op;
		b->left = n;
		lexer_next(l);
		b->right = parse_unary(l);
		n = b;
	}
	return n;
}

static ast_node* parse_add(lexer_state *l)
{
	ast_node *n = parse_mul(l);
	while (l->curr_tok == TOK_PLUS || l->curr_tok == TOK_MINUS) {
		int op = (l->curr_tok == TOK_PLUS ? OP_ADD : OP_SUB);
		ast_node *b = ast_new(AST_BINOP);
		b->binop = op;
		b->left = n;
		lexer_next(l);
		b->right = parse_mul(l);
		n = b;
	}
	return n;
}

static ast_node* parse_cmp(lexer_state *l)
{
	ast_node *n = parse_add(l);
	if (l->curr_tok == TOK_EQ || l->curr_tok == TOK_NEQ ||
	    l->curr_tok == TOK_LT || l->curr_tok == TOK_LE ||
	    l->curr_tok == TOK_GT || l->curr_tok == TOK_GE) {
		int op;
		ast_node *b;
		switch (l->curr_tok) {
		case TOK_EQ:  op = OP_EQ; break;
		case TOK_NEQ: op = OP_NEQ; break;
		case TOK_LT:  op = OP_LT; break;
		case TOK_LE:  op = OP_LE; break;
		case TOK_GT:  op = OP_GT; break;
		default:      op = OP_GE; break;
		}
		b = ast_new(AST_BINOP);
		b->binop = op;
		b->left = n;
		lexer_next(l);
		b->right = parse_add(l);
		n = b;
	}
	return n;
}

static ast_node* parse_and(lexer_state *l)
{
	ast_node *n = parse_cmp(l);
	while (l->curr_tok == TOK_IDENT && strcmp(l->sval, "and") == 0) {
		ast_node *b = ast_new(AST_BINOP);
		b->binop = OP_AND;
		b->left = n;
		lexer_next(l);
		b->right = parse_cmp(l);
		n = b;
	}
	return n;
}

static ast_node* parse_or(lexer_state *l)
{
	ast_node *n = parse_and(l);
	while (l->curr_tok == TOK_IDENT && strcmp(l->sval, "or") == 0) {
		ast_node *b = ast_new(AST_BINOP);
		b->binop = OP_OR;
		b->left = n;
		lexer_next(l);
		b->right = parse_and(l);
		n = b;
	}
	return n;
}

static ast_node* parse_alt(lexer_state *l)
{
	ast_node *n = parse_or(l);
	while (l->curr_tok == TOK_ALT) {
		ast_node *a = ast_new(AST_ALT);
		a->left = n;
		lexer_next(l);
		a->right = parse_or(l);
		n = a;
	}
	return n;
}

static ast_node* parse_assign(lexer_state *l)
{
	ast_node *n = parse_alt(l);
	if (l->curr_tok == TOK_ASSIGN) {
		ast_node *as = ast_new(AST_ASSIGN);
		as->left = n;
		lexer_next(l);
		as->right = parse_alt(l);
		return as;
	}
	if (l->curr_tok == TOK_SET_PIPE) {
		ast_node *as = ast_new(AST_UPDATE);
		as->left = n;
		lexer_next(l);
		as->right = parse_alt(l);
		return as;
	}
	if (l->curr_tok == TOK_SET_PLUS || l->curr_tok == TOK_SET_MINUS ||
	    l->curr_tok == TOK_SET_MULT || l->curr_tok == TOK_SET_DIV ||
	    l->curr_tok == TOK_SET_MOD || l->curr_tok == TOK_SET_ALT) {
		token_type t = l->curr_tok;
		ast_node *as = ast_new(AST_UPDATE);
		ast_node *b = ast_new((t == TOK_SET_ALT) ? AST_ALT : AST_BINOP);
		as->left = n;
		b->left = ast_new(AST_ID);
		if (t == TOK_SET_PLUS) b->binop = OP_ADD;
		else if (t == TOK_SET_MINUS) b->binop = OP_SUB;
		else if (t == TOK_SET_MULT) b->binop = OP_MUL;
		else if (t == TOK_SET_DIV) b->binop = OP_DIV;
		else if (t == TOK_SET_MOD) b->binop = OP_MOD;
		lexer_next(l);
		b->right = parse_alt(l);
		as->right = b;
		return as;
	}
	return n;
}

static ast_node* parse_pipe_no_comma(lexer_state *l)
{
	ast_node *n = parse_assign(l);
	while (l->curr_tok == TOK_PIPE) {
		ast_node *p = ast_new(AST_PIPE);
		p->left = n;
		lexer_next(l);
		p->right = parse_pipe_no_comma(l);
		n = p;
	}
	return n;
}

static ast_node* parse_comma(lexer_state *l)
{
	ast_node *n = parse_assign(l);
	while (l->curr_tok == TOK_COMMA) {
		ast_node *c = ast_new(AST_COMMA);
		c->left = n;
		lexer_next(l);
		c->right = parse_assign(l);
		n = c;
	}
	return n;
}

static ast_node* parse_pipe_ext(lexer_state *l, int stop_at_as)
{
	ast_node *n = parse_comma(l);

	if (!stop_at_as && l->curr_tok == TOK_IDENT && strcmp(l->sval, "as") == 0) {
		char *var = NULL;
		ast_node *b;
		lexer_next(l);
		if (l->curr_tok == TOK_VAR) {
			var = xstrdup(l->sval);
			lexer_next(l);
		}
		if (l->curr_tok == TOK_PIPE)
			lexer_next(l);
		b = ast_new(AST_BIND);
		b->sval = var;
		b->left = n;
		b->right = parse_pipe_ext(l, 0);
		return b;
	}

	while (l->curr_tok == TOK_PIPE) {
		ast_node *p = ast_new(AST_PIPE);
		p->left = n;
		lexer_next(l);
		p->right = parse_pipe_ext(l, stop_at_as);
		n = p;
	}
	return n;
}

static ast_node* parse_pipe(lexer_state *l)
{
	return parse_pipe_ext(l, 0);
}

static ast_node* jq_compile(const char *filter)
{
	lexer_state l;
	ast_node *ast;

	l.src = filter;
	l.p = filter;
	l.sval = NULL;
	l.curr_tok = TOK_EOF;
	lexer_next(&l);

	ast = parse_pipe(&l);
	free(l.sval);
	return ast;
}

/* --- Evaluator Environment and Implementation --- */

typedef struct env_var env_var;
struct env_var {
	char *name;
	jv *val;
	env_var *next;
};

typedef struct env_scope env_scope;
struct env_scope {
	env_var *vars;
	env_scope *parent;
	int error;
	char *error_msg;
};

static void env_set(env_scope *env, const char *name, jv *val)
{
	env_var *v = xzalloc(sizeof(*v));
	v->name = xstrdup(name);
	v->val = val;
	v->next = env->vars;
	env->vars = v;
}

static jv* env_get(env_scope *env, const char *name)
{
	env_scope *s = env;
	while (s) {
		env_var *v = s->vars;
		while (v) {
			if (strcmp(v->name, name) == 0)
				return v->val;
			v = v->next;
		}
		s = s->parent;
	}
	return NULL;
}

static void env_free(env_scope *env)
{
	env_var *v = env->vars;
	while (v) {
		env_var *next = v->next;
		free(v->name);
		jv_free(v->val);
		free(v);
		v = next;
	}
	env->vars = NULL;
	free(env->error_msg);
	env->error_msg = NULL;
}

static jv* get_env_object(void)
{
	extern char **environ;
	jv *obj = jv_object();
	char **env = environ;
	if (env) {
		while (*env) {
			char *eq = strchr(*env, '=');
			if (eq) {
				char *k = xstrndup(*env, eq - *env);
				jv_object_set(obj, k, jv_string(eq + 1));
				free(k);
			}
			env++;
		}
	}
	return obj;
}

typedef struct {
	jv *elem;
	jv *key;
} sort_pair;

static int sort_pair_cmp(const void *p1, const void *p2)
{
	const sort_pair *a = p1;
	const sort_pair *b = p2;
	return jv_compare(a->key, b->key);
}

static jv_list eval_expr(const ast_node *node, const jv *input, env_scope *env);

/* Path navigation for assignment and deletion */

static void extract_paths(const ast_node *node, const jv *input, env_scope *env, jv_list *paths_out)
{
	int i;
	jv_list sub_paths;

	if (!node)
		return;

	if (node->type == AST_ID) {
		jv_list_append(paths_out, jv_array());
		return;
	}

	if (node->type == AST_FIELD) {
		jv_list_init(&sub_paths);
		extract_paths(node->left, input, env, &sub_paths);
		for (i = 0; i < sub_paths.len; i++) {
			jv *p = sub_paths.items[i];
			jv_array_append(p, jv_string(node->sval));
			jv_list_append(paths_out, p);
		}
		free(sub_paths.items);
		return;
	}

	if (node->type == AST_INDEX) {
		jv_list_init(&sub_paths);
		extract_paths(node->left, input, env, &sub_paths);
		for (i = 0; i < sub_paths.len; i++) {
			jv *p = sub_paths.items[i];
			if (node->literal) {
				jv_array_append(p, jv_copy(node->literal));
			} else if (node->right) {
				jv_list idx_res = eval_expr(node->right, input, env);
				if (idx_res.len > 0)
					jv_array_append(p, jv_copy(idx_res.items[0]));
				jv_list_free(&idx_res);
			}
			jv_list_append(paths_out, p);
		}
		free(sub_paths.items);
		return;
	}

	if (node->type == AST_ITER) {
		jv_list_init(&sub_paths);
		extract_paths(node->left, input, env, &sub_paths);
		for (i = 0; i < sub_paths.len; i++) {
			jv *p = sub_paths.items[i];
			const jv *target = input;
			int j, ok = 1;
			for (j = 0; j < p->u.arr.len; j++) {
				jv *elem = p->u.arr.a[j];
				if (elem->type == JV_STR && target->type == JV_OBJ) {
					target = jv_object_get(target, elem->u.str.s);
				} else if (elem->type == JV_NUM && target->type == JV_ARR) {
					target = jv_array_get(target, (int)elem->u.nval);
				} else {
					ok = 0;
					break;
				}
				if (!target) {
					ok = 0;
					break;
				}
			}
			if (ok && target) {
				if (target->type == JV_ARR) {
					int k;
					for (k = 0; k < target->u.arr.len; k++) {
						jv *cp = jv_copy(p);
						jv_array_append(cp, jv_number(k));
						jv_list_append(paths_out, cp);
					}
				} else if (target->type == JV_OBJ) {
					int k;
					for (k = 0; k < target->u.obj.len; k++) {
						jv *cp = jv_copy(p);
						jv_array_append(cp, jv_string(target->u.obj.kv[k].k));
						jv_list_append(paths_out, cp);
					}
				}
			}
			jv_free(p);
		}
		free(sub_paths.items);
		return;
	}

	if (node->type == AST_COMMA) {
		extract_paths(node->left, input, env, paths_out);
		extract_paths(node->right, input, env, paths_out);
		return;
	}

	/* Fallback: evaluate expression as a path */
	{
		jv_list res = eval_expr(node, input, env);
		for (i = 0; i < res.len; i++) {
			if (res.items[i]->type == JV_ARR)
				jv_list_append(paths_out, res.items[i]);
			else
				jv_free(res.items[i]);
		}
		free(res.items);
	}
}

static jv* jv_set_path(jv *target, const jv *path, int path_idx, jv *new_val)
{
	jv *elem;

	if (path_idx >= path->u.arr.len) {
		jv_free(target);
		return new_val;
	}

	elem = path->u.arr.a[path_idx];

	if (elem->type == JV_STR) {
		jv *child, *updated;
		if (!target || target->type != JV_OBJ) {
			jv_free(target);
			target = jv_object();
		}
		child = jv_object_get(target, elem->u.str.s);
		updated = jv_set_path(child ? jv_copy(child) : NULL, path, path_idx + 1, new_val);
		jv_object_set(target, elem->u.str.s, updated);
		return target;
	}

	if (elem->type == JV_NUM) {
		int idx = (int)elem->u.nval;
		jv *child, *updated;
		if (!target || target->type != JV_ARR) {
			jv_free(target);
			target = jv_array();
		}
		child = jv_array_get(target, idx);
		updated = jv_set_path(child ? jv_copy(child) : NULL, path, path_idx + 1, new_val);
		jv_array_set(target, idx, updated);
		return target;
	}

	jv_free(new_val);
	return target;
}

static jv* jv_del_path(jv *target, const jv *path, int path_idx)
{
	jv *elem;

	if (!target || path_idx >= path->u.arr.len)
		return target;

	elem = path->u.arr.a[path_idx];

	if (path_idx == path->u.arr.len - 1) {
		if (elem->type == JV_STR && target->type == JV_OBJ)
			jv_object_delete(target, elem->u.str.s);
		else if (elem->type == JV_NUM && target->type == JV_ARR)
			jv_array_delete(target, (int)elem->u.nval);
		return target;
	}

	if (elem->type == JV_STR && target->type == JV_OBJ) {
		jv *child = jv_object_get(target, elem->u.str.s);
		if (child) {
			jv *updated = jv_del_path(jv_copy(child), path, path_idx + 1);
			jv_object_set(target, elem->u.str.s, updated);
		}
	} else if (elem->type == JV_NUM && target->type == JV_ARR) {
		int idx = (int)elem->u.nval;
		jv *child = jv_array_get(target, idx);
		if (child) {
			jv *updated = jv_del_path(jv_copy(child), path, path_idx + 1);
			jv_array_set(target, idx, updated);
		}
	}
	return target;
}

static void recurse_helper(jv_list *out, const jv *cur, const ast_node *f_expr, env_scope *env)
{
	int i;
	jv_list children;

	jv_list_append(out, jv_copy(cur));

	jv_list_init(&children);
	if (f_expr) {
		children = eval_expr(f_expr, cur, env);
	} else {
		/* default: .[]? */
		if (cur && cur->type == JV_ARR) {
			for (i = 0; i < cur->u.arr.len; i++)
				jv_list_append(&children, jv_copy(cur->u.arr.a[i]));
		} else if (cur && cur->type == JV_OBJ) {
			for (i = 0; i < cur->u.obj.len; i++)
				jv_list_append(&children, jv_copy(cur->u.obj.kv[i].v));
		}
	}

	for (i = 0; i < children.len; i++) {
		jv *c = children.items[i];
		if (c && c->type != JV_NULL)
			recurse_helper(out, c, f_expr, env);
		jv_free(c);
	}
	free(children.items);
}

static jv* eval_binop(int op, const jv *l, const jv *r, env_scope *env)
{
	if (!l || !r)
		return jv_null();

	switch (op) {
	case OP_ADD:
		if (l->type == JV_NUM && r->type == JV_NUM)
			return jv_number(l->u.nval + r->u.nval);
		if (l->type == JV_STR && r->type == JV_STR) {
			str_buf sb;
			str_buf_init(&sb);
			str_buf_append_len(&sb, l->u.str.s, l->u.str.len);
			str_buf_append_len(&sb, r->u.str.s, r->u.str.len);
			return jv_string_take(sb.buf, sb.len);
		}
		if (l->type == JV_ARR && r->type == JV_ARR) {
			jv *res = jv_array();
			int i;
			for (i = 0; i < l->u.arr.len; i++)
				jv_array_append(res, jv_copy(l->u.arr.a[i]));
			for (i = 0; i < r->u.arr.len; i++)
				jv_array_append(res, jv_copy(r->u.arr.a[i]));
			return res;
		}
		if (l->type == JV_OBJ && r->type == JV_OBJ) {
			jv *res = jv_copy(l);
			int i;
			for (i = 0; i < r->u.obj.len; i++)
				jv_object_set(res, r->u.obj.kv[i].k, jv_copy(r->u.obj.kv[i].v));
			return res;
		}
		break;
	case OP_SUB:
		if (l->type == JV_NUM && r->type == JV_NUM)
			return jv_number(l->u.nval - r->u.nval);
		if (l->type == JV_ARR && r->type == JV_ARR) {
			jv *res = jv_array();
			int i, j;
			for (i = 0; i < l->u.arr.len; i++) {
				int found = 0;
				for (j = 0; j < r->u.arr.len; j++) {
					if (jv_equal(l->u.arr.a[i], r->u.arr.a[j])) {
						found = 1;
						break;
					}
				}
				if (!found)
					jv_array_append(res, jv_copy(l->u.arr.a[i]));
			}
			return res;
		}
		break;
	case OP_MUL:
		if (l->type == JV_NUM && r->type == JV_NUM)
			return jv_number(l->u.nval * r->u.nval);
		if (l->type == JV_STR && r->type == JV_NUM) {
			int times = (int)r->u.nval;
			str_buf sb;
			int i;
			str_buf_init(&sb);
			for (i = 0; i < times; i++)
				str_buf_append_len(&sb, l->u.str.s, l->u.str.len);
			return jv_string_take(sb.buf, sb.len);
		}
		if (l->type == JV_OBJ && r->type == JV_OBJ) {
			jv *res = jv_copy(l);
			int i;
			for (i = 0; i < r->u.obj.len; i++) {
				jv *orig = jv_object_get(res, r->u.obj.kv[i].k);
				if (orig && orig->type == JV_OBJ && r->u.obj.kv[i].v->type == JV_OBJ) {
					jv *merged = eval_binop(OP_MUL, orig, r->u.obj.kv[i].v, NULL);
					jv_object_set(res, r->u.obj.kv[i].k, merged);
				} else {
					jv_object_set(res, r->u.obj.kv[i].k, jv_copy(r->u.obj.kv[i].v));
				}
			}
			return res;
		}
		break;
	case OP_DIV:
		if (l->type == JV_NUM && r->type == JV_NUM) {
			if (r->u.nval == 0.0) {
				if (env) {
					env->error = 1;
					free(env->error_msg);
					env->error_msg = xstrdup("division by zero");
				}
				return jv_null();
			}
			return jv_number(l->u.nval / r->u.nval);
		}
		if (l->type == JV_STR && r->type == JV_STR)
			return builtin_split(l, r);
		break;
	case OP_MOD:
		if (l->type == JV_NUM && r->type == JV_NUM) {
			long long rval = (long long)r->u.nval;
			if (rval == 0) {
				if (env) {
					env->error = 1;
					free(env->error_msg);
					env->error_msg = xstrdup("modulo by zero");
				}
				return jv_null();
			}
			return jv_number((long long)l->u.nval % rval);
		}
		break;
	case OP_EQ:  return jv_bool(jv_equal(l, r));
	case OP_NEQ: return jv_bool(!jv_equal(l, r));
	case OP_LT:  return jv_bool(jv_compare(l, r) < 0);
	case OP_LE:  return jv_bool(jv_compare(l, r) <= 0);
	case OP_GT:  return jv_bool(jv_compare(l, r) > 0);
	case OP_GE:  return jv_bool(jv_compare(l, r) >= 0);
	case OP_AND: return jv_bool(jv_is_truthy(l) && jv_is_truthy(r));
	case OP_OR:  return jv_bool(jv_is_truthy(l) || jv_is_truthy(r));
	}
	return jv_null();
}

static jv_list eval_expr(const ast_node *node, const jv *input, env_scope *env)
{
	jv_list out;
	int i, j, k;

	jv_list_init(&out);
	if (!node)
		return out;

	switch (node->type) {
	case AST_ID:
		jv_list_append(&out, jv_copy(input));
		break;

	case AST_LITERAL:
		jv_list_append(&out, jv_copy(node->literal));
		break;

	case AST_VAR: {
		if (strcmp(node->sval, "ENV") == 0) {
			jv_list_append(&out, get_env_object());
		} else {
			jv *val = env_get(env, node->sval);
			if (val)
				jv_list_append(&out, jv_copy(val));
			else
				jv_list_append(&out, jv_null());
		}
		break;
	}

	case AST_FIELD: {
		jv_list base_res = eval_expr(node->left, input, env);
		for (i = 0; i < base_res.len; i++) {
			jv *b = base_res.items[i];
			if (b && b->type == JV_OBJ) {
				jv *f = jv_object_get(b, node->sval);
				jv_list_append(&out, f ? jv_copy(f) : jv_null());
			} else if (b && b->type == JV_NULL) {
				jv_list_append(&out, jv_null());
			} else if (!node->optional) {
				/* error or null in jq */
				jv_list_append(&out, jv_null());
			}
			jv_free(b);
		}
		free(base_res.items);
		break;
	}

	case AST_INDEX: {
		jv_list base_res = eval_expr(node->left, input, env);
		int has_idx = (node->literal && node->literal->type == JV_NUM);
		int static_idx = has_idx ? (int)node->literal->u.nval : 0;

		for (i = 0; i < base_res.len; i++) {
			jv *b = base_res.items[i];
			if (has_idx) {
				if (b && b->type == JV_ARR) {
					jv *elem = jv_array_get(b, static_idx);
					jv_list_append(&out, elem ? jv_copy(elem) : jv_null());
				} else if (b && b->type == JV_NULL) {
					jv_list_append(&out, jv_null());
				} else if (!node->optional) {
					jv_list_append(&out, jv_null());
				}
			} else if (node->right) {
				jv_list idx_res = eval_expr(node->right, b, env);
				for (j = 0; j < idx_res.len; j++) {
					jv *idx_val = idx_res.items[j];
					if (b && b->type == JV_ARR && idx_val->type == JV_NUM) {
						jv *elem = jv_array_get(b, (int)idx_val->u.nval);
						jv_list_append(&out, elem ? jv_copy(elem) : jv_null());
					} else if (b && b->type == JV_OBJ && idx_val->type == JV_STR) {
						jv *elem = jv_object_get(b, idx_val->u.str.s);
						jv_list_append(&out, elem ? jv_copy(elem) : jv_null());
					} else {
						jv_list_append(&out, jv_null());
					}
					jv_free(idx_val);
				}
				free(idx_res.items);
			}
			jv_free(b);
		}
		free(base_res.items);
		break;
	}

	case AST_SLICE: {
		jv_list base_res = eval_expr(node->left, input, env);
		for (i = 0; i < base_res.len; i++) {
			jv *b = base_res.items[i];
			int start = 0, end = 0, len = 0;
			if (b && b->type == JV_ARR) {
				len = b->u.arr.len;
				end = len;
				if (node->init) {
					jv_list r1 = eval_expr(node->init, b, env);
					if (r1.len > 0 && r1.items[0]->type == JV_NUM)
						start = (int)r1.items[0]->u.nval;
					jv_list_free(&r1);
				}
				if (node->right) {
					jv_list r2 = eval_expr(node->right, b, env);
					if (r2.len > 0 && r2.items[0]->type == JV_NUM)
						end = (int)r2.items[0]->u.nval;
					jv_list_free(&r2);
				}
				if (start < 0) start += len;
				if (end < 0) end += len;
				if (start < 0) start = 0;
				if (end > len) end = len;
				{
					jv *slice = jv_array();
					for (j = start; j < end; j++)
						jv_array_append(slice, jv_copy(b->u.arr.a[j]));
					jv_list_append(&out, slice);
				}
			} else if (b && b->type == JV_STR) {
				len = b->u.str.len;
				end = len;
				if (node->init) {
					jv_list r1 = eval_expr(node->init, b, env);
					if (r1.len > 0 && r1.items[0]->type == JV_NUM)
						start = (int)r1.items[0]->u.nval;
					jv_list_free(&r1);
				}
				if (node->right) {
					jv_list r2 = eval_expr(node->right, b, env);
					if (r2.len > 0 && r2.items[0]->type == JV_NUM)
						end = (int)r2.items[0]->u.nval;
					jv_list_free(&r2);
				}
				if (start < 0) start += len;
				if (end < 0) end += len;
				if (start < 0) start = 0;
				if (end > len) end = len;
				if (end > start)
					jv_list_append(&out, jv_string_sized(b->u.str.s + start, end - start));
				else
					jv_list_append(&out, jv_string(""));
			} else if (b && b->type == JV_NULL) {
				jv_list_append(&out, jv_null());
			}
			jv_free(b);
		}
		free(base_res.items);
		break;
	}

	case AST_ITER: {
		jv_list base_res = eval_expr(node->left, input, env);
		for (i = 0; i < base_res.len; i++) {
			jv *b = base_res.items[i];
			if (b && b->type == JV_ARR) {
				for (j = 0; j < b->u.arr.len; j++)
					jv_list_append(&out, jv_copy(b->u.arr.a[j]));
			} else if (b && b->type == JV_OBJ) {
				for (j = 0; j < b->u.obj.len; j++)
					jv_list_append(&out, jv_copy(b->u.obj.kv[j].v));
			}
			jv_free(b);
		}
		free(base_res.items);
		break;
	}

	case AST_PIPE: {
		jv_list l_res = eval_expr(node->left, input, env);
		for (i = 0; i < l_res.len; i++) {
			jv_list r_res = eval_expr(node->right, l_res.items[i], env);
			jv_list_concat(&out, &r_res);
			jv_free(l_res.items[i]);
		}
		free(l_res.items);
		break;
	}

	case AST_COMMA: {
		jv_list l_res = eval_expr(node->left, input, env);
		jv_list r_res = eval_expr(node->right, input, env);
		jv_list_concat(&out, &l_res);
		jv_list_concat(&out, &r_res);
		break;
	}

	case AST_ALT: {
		jv_list l_res = eval_expr(node->left, input, env);
		int has_truthy = 0;
		for (i = 0; i < l_res.len; i++) {
			if (jv_is_truthy(l_res.items[i])) {
				jv_list_append(&out, l_res.items[i]);
				has_truthy = 1;
			} else {
				jv_free(l_res.items[i]);
			}
		}
		free(l_res.items);
		if (!has_truthy) {
			jv_list r_res = eval_expr(node->right, input, env);
			jv_list_concat(&out, &r_res);
		}
		break;
	}

	case AST_ASSIGN: {
		jv_list paths;
		jv_list rhs_res = eval_expr(node->right, input, env);
		jv *rhs_val = (rhs_res.len > 0) ? rhs_res.items[0] : jv_null();
		jv *target = jv_copy(input);

		jv_list_init(&paths);
		extract_paths(node->left, input, env, &paths);
		for (i = 0; i < paths.len; i++) {
			target = jv_set_path(target, paths.items[i], 0, jv_copy(rhs_val));
			jv_free(paths.items[i]);
		}
		free(paths.items);
		jv_list_free(&rhs_res);
		jv_list_append(&out, target);
		break;
	}

	case AST_UPDATE: {
		jv_list paths;
		jv *target = jv_copy(input);

		jv_list_init(&paths);
		extract_paths(node->left, input, env, &paths);
		for (i = 0; i < paths.len; i++) {
			jv *p = paths.items[i];
			const jv *cur = target;
			int ok = 1;
			for (j = 0; j < p->u.arr.len; j++) {
				jv *elem = p->u.arr.a[j];
				if (elem->type == JV_STR && cur->type == JV_OBJ) {
					cur = jv_object_get(cur, elem->u.str.s);
				} else if (elem->type == JV_NUM && cur->type == JV_ARR) {
					cur = jv_array_get(cur, (int)elem->u.nval);
				} else {
					ok = 0;
					break;
				}
				if (!cur) {
					ok = 0;
					break;
				}
			}
			if (ok) {
				jv_list upd = eval_expr(node->right, cur, env);
				if (upd.len > 0) {
					target = jv_set_path(target, p, 0, jv_copy(upd.items[0]));
				}
				jv_list_free(&upd);
			}
			jv_free(p);
		}
		free(paths.items);
		jv_list_append(&out, target);
		break;
	}

	case AST_DEL: {
		jv_list paths;
		jv *target = jv_copy(input);
		jv_list_init(&paths);
		extract_paths(node->left, input, env, &paths);
		for (i = 0; i < paths.len; i++) {
			target = jv_del_path(target, paths.items[i], 0);
			jv_free(paths.items[i]);
		}
		free(paths.items);
		jv_list_append(&out, target);
		break;
	}

	case AST_PATH: {
		jv_list paths;
		jv_list_init(&paths);
		extract_paths(node->left, input, env, &paths);
		for (i = 0; i < paths.len; i++)
			jv_list_append(&out, paths.items[i]);
		free(paths.items);
		break;
	}

	case AST_BINOP: {
		jv_list l_res = eval_expr(node->left, input, env);
		jv_list r_res = eval_expr(node->right, input, env);
		for (i = 0; i < l_res.len; i++) {
			for (j = 0; j < r_res.len; j++) {
				jv *res = eval_binop(node->binop, l_res.items[i], r_res.items[j], env);
				jv_list_append(&out, res);
			}
		}
		jv_list_free(&l_res);
		jv_list_free(&r_res);
		break;
	}

	case AST_UNOP_NEG: {
		jv_list l_res = eval_expr(node->left, input, env);
		for (i = 0; i < l_res.len; i++) {
			if (l_res.items[i]->type == JV_NUM)
				jv_list_append(&out, jv_number(-l_res.items[i]->u.nval));
			else
				jv_list_append(&out, jv_null());
			jv_free(l_res.items[i]);
		}
		free(l_res.items);
		break;
	}

	case AST_ARR_CONSTR: {
		jv *arr = jv_array();
		if (node->left) {
			jv_list items = eval_expr(node->left, input, env);
			for (i = 0; i < items.len; i++)
				jv_array_append(arr, items.items[i]);
			free(items.items);
		}
		jv_list_append(&out, arr);
		break;
	}

	case AST_OBJ_CONSTR: {
		jv *obj = jv_object();
		for (i = 0; i < node->num_pairs; i++) {
			jv_list k_res = eval_expr(node->obj_pairs[i].key, input, env);
			jv_list v_res = eval_expr(node->obj_pairs[i].val, input, env);
			char *key_str = (k_res.len > 0 && k_res.items[0]->type == JV_STR) ?
			                k_res.items[0]->u.str.s : "";
			jv *val = (v_res.len > 0) ? jv_copy(v_res.items[0]) : jv_null();
			jv_object_set(obj, key_str, val);
			jv_list_free(&k_res);
			jv_list_free(&v_res);
		}
		jv_list_append(&out, obj);
		break;
	}

	case AST_IF: {
		jv_list cond_res = eval_expr(node->cond, input, env);
		int cond_true = (cond_res.len > 0 && jv_is_truthy(cond_res.items[0]));
		jv_list_free(&cond_res);
		if (cond_true) {
			jv_list then_res = eval_expr(node->left, input, env);
			jv_list_concat(&out, &then_res);
		} else if (node->right) {
			jv_list else_res = eval_expr(node->right, input, env);
			jv_list_concat(&out, &else_res);
		} else {
			jv_list_append(&out, jv_copy(input));
		}
		break;
	}

	case AST_BIND: {
		jv_list gen_res = eval_expr(node->left, input, env);
		for (i = 0; i < gen_res.len; i++) {
			env_scope sub_env;
			jv_list body_res;
			sub_env.vars = NULL;
			sub_env.parent = env;
			sub_env.error = 0;
			sub_env.error_msg = NULL;
			if (node->sval)
				env_set(&sub_env, node->sval, jv_copy(gen_res.items[i]));
			body_res = eval_expr(node->right, input, &sub_env);
			jv_list_concat(&out, &body_res);
			env_free(&sub_env);
			jv_free(gen_res.items[i]);
		}
		free(gen_res.items);
		break;
	}

	case AST_TRY_CATCH: {
		jv_list body_res = eval_expr(node->left, input, env);
		if (env->error) {
			env->error = 0;
			jv_list_free(&body_res);
			if (node->right) {
				jv *err_val = jv_string(env->error_msg ? env->error_msg : "error");
				free(env->error_msg);
				env->error_msg = NULL;
				body_res = eval_expr(node->right, err_val, env);
				jv_free(err_val);
				jv_list_concat(&out, &body_res);
			}
		} else {
			jv_list_concat(&out, &body_res);
		}
		break;
	}

	case AST_REDUCE:
	case AST_FOREACH: {
		int is_reduce = (node->type == AST_REDUCE);
		jv_list init_res = eval_expr(node->init, input, env);
		jv *acc = (init_res.len > 0) ? init_res.items[0] : jv_null();
		jv_list gen_res;

		for (j = 1; j < init_res.len; j++)
			jv_free(init_res.items[j]);
		free(init_res.items);

		gen_res = eval_expr(node->left, input, env);
		for (i = 0; i < gen_res.len; i++) {
			env_scope sub_env;
			jv_list upd_res;
			sub_env.vars = NULL;
			sub_env.parent = env;
			sub_env.error = 0;
			sub_env.error_msg = NULL;
			if (node->sval)
				env_set(&sub_env, node->sval, jv_copy(gen_res.items[i]));
			upd_res = eval_expr(node->right, acc, &sub_env);
			jv_free(acc);
			acc = (upd_res.len > 0) ? upd_res.items[0] : jv_null();
			for (k = 1; k < upd_res.len; k++)
				jv_free(upd_res.items[k]);
			free(upd_res.items);
			env_free(&sub_env);
			if (!is_reduce)
				jv_list_append(&out, jv_copy(acc));
			jv_free(gen_res.items[i]);
		}
		free(gen_res.items);
		if (is_reduce)
			jv_list_append(&out, acc);
		else
			jv_free(acc);
		break;
	}

	case AST_FORMAT:
	case AST_CALL: {
		const char *fn = node->sval;
		if (!fn)
			break;

		if (strcmp(fn, "length") == 0) {
			if (input && input->type == JV_STR)
				jv_list_append(&out, jv_number(input->u.str.len));
			else if (input && input->type == JV_ARR)
				jv_list_append(&out, jv_number(input->u.arr.len));
			else if (input && input->type == JV_OBJ)
				jv_list_append(&out, jv_number(input->u.obj.len));
			else if (input && input->type == JV_NUM)
				jv_list_append(&out, jv_number(fabs(input->u.nval)));
			else
				jv_list_append(&out, jv_number(0));
		}
		else if (strcmp(fn, "keys") == 0 || strcmp(fn, "keys_unsorted") == 0) {
			int sort_it = (strcmp(fn, "keys") == 0);
			if (input && input->type == JV_OBJ) {
				jv *res = jv_array();
				for (i = 0; i < input->u.obj.len; i++)
					jv_array_append(res, jv_string(input->u.obj.kv[i].k));
				if (sort_it && res->u.arr.len > 1)
					qsort(res->u.arr.a, res->u.arr.len, sizeof(jv*), (int(*)(const void*,const void*))jv_compare);
				jv_list_append(&out, res);
			} else if (input && input->type == JV_ARR) {
				jv *res = jv_array();
				for (i = 0; i < input->u.arr.len; i++)
					jv_array_append(res, jv_number(i));
				jv_list_append(&out, res);
			} else {
				jv_list_append(&out, jv_array());
			}
		}
		else if (strcmp(fn, "values") == 0) {
			if (input && input->type == JV_OBJ) {
				jv *res = jv_array();
				for (i = 0; i < input->u.obj.len; i++)
					jv_array_append(res, jv_copy(input->u.obj.kv[i].v));
				jv_list_append(&out, res);
			} else if (input && input->type == JV_ARR) {
				jv_list_append(&out, jv_copy(input));
			} else {
				jv_list_append(&out, jv_array());
			}
		}
		else if (strcmp(fn, "has") == 0) {
			int ok = 0;
			if (node->num_args > 0) {
				jv_list arg_res = eval_expr(node->args[0], input, env);
				if (arg_res.len > 0) {
					jv *arg = arg_res.items[0];
					if (input && input->type == JV_OBJ && arg->type == JV_STR)
						ok = (jv_object_get(input, arg->u.str.s) != NULL);
					else if (input && input->type == JV_ARR && arg->type == JV_NUM) {
						int idx = (int)arg->u.nval;
						ok = (idx >= 0 && idx < input->u.arr.len);
					}
				}
				jv_list_free(&arg_res);
			}
			jv_list_append(&out, jv_bool(ok));
		}
		else if (strcmp(fn, "in") == 0) {
			int ok = 0;
			if (node->num_args > 0) {
				jv_list target_res = eval_expr(node->args[0], input, env);
				if (target_res.len > 0) {
					jv *t = target_res.items[0];
					if (t && t->type == JV_OBJ && input && input->type == JV_STR)
						ok = (jv_object_get(t, input->u.str.s) != NULL);
					else if (t && t->type == JV_ARR && input && input->type == JV_NUM) {
						int idx = (int)input->u.nval;
						ok = (idx >= 0 && idx < t->u.arr.len);
					}
				}
				jv_list_free(&target_res);
			}
			jv_list_append(&out, jv_bool(ok));
		}
		else if (strcmp(fn, "map") == 0) {
			if (input && input->type == JV_ARR && node->num_args > 0) {
				jv *res = jv_array();
				for (i = 0; i < input->u.arr.len; i++) {
					jv_list r = eval_expr(node->args[0], input->u.arr.a[i], env);
					for (j = 0; j < r.len; j++)
						jv_array_append(res, r.items[j]);
					free(r.items);
				}
				jv_list_append(&out, res);
			} else {
				jv_list_append(&out, jv_array());
			}
		}
		else if (strcmp(fn, "map_values") == 0) {
			if (input && input->type == JV_OBJ && node->num_args > 0) {
				jv *res = jv_object();
				for (i = 0; i < input->u.obj.len; i++) {
					jv_list r = eval_expr(node->args[0], input->u.obj.kv[i].v, env);
					if (r.len > 0)
						jv_object_set(res, input->u.obj.kv[i].k, jv_copy(r.items[0]));
					jv_list_free(&r);
				}
				jv_list_append(&out, res);
			} else if (input && input->type == JV_ARR && node->num_args > 0) {
				jv *res = jv_array();
				for (i = 0; i < input->u.arr.len; i++) {
					jv_list r = eval_expr(node->args[0], input->u.arr.a[i], env);
					if (r.len > 0)
						jv_array_append(res, jv_copy(r.items[0]));
					jv_list_free(&r);
				}
				jv_list_append(&out, res);
			}
		}
		else if (strcmp(fn, "select") == 0) {
			if (node->num_args > 0) {
				jv_list cond_res = eval_expr(node->args[0], input, env);
				int ok = 0;
				for (i = 0; i < cond_res.len; i++) {
					if (jv_is_truthy(cond_res.items[i]))
						ok = 1;
				}
				jv_list_free(&cond_res);
				if (ok)
					jv_list_append(&out, jv_copy(input));
			}
		}
		else if (strcmp(fn, "empty") == 0) {
			/* emit nothing */
		}
		else if (strcmp(fn, "add") == 0) {
			if (input && input->type == JV_ARR) {
				if (input->u.arr.len == 0) {
					jv_list_append(&out, jv_null());
				} else {
					jv *sum = jv_copy(input->u.arr.a[0]);
					for (i = 1; i < input->u.arr.len; i++) {
						jv *tmp = eval_binop(OP_ADD, sum, input->u.arr.a[i], env);
						jv_free(sum);
						sum = tmp;
					}
					jv_list_append(&out, sum);
				}
			}
		}
		else if (strcmp(fn, "any") == 0) {
			int ok = 0;
			if (node->num_args == 0 && input && input->type == JV_ARR) {
				for (i = 0; i < input->u.arr.len; i++) {
					if (jv_is_truthy(input->u.arr.a[i])) {
						ok = 1;
						break;
					}
				}
			} else if (node->num_args == 1 && input && input->type == JV_ARR) {
				for (i = 0; i < input->u.arr.len; i++) {
					jv_list r = eval_expr(node->args[0], input->u.arr.a[i], env);
					for (j = 0; j < r.len; j++) {
						if (jv_is_truthy(r.items[j]))
							ok = 1;
					}
					jv_list_free(&r);
					if (ok) break;
				}
			}
			jv_list_append(&out, jv_bool(ok));
		}
		else if (strcmp(fn, "all") == 0) {
			int ok = 1;
			if (node->num_args == 0 && input && input->type == JV_ARR) {
				for (i = 0; i < input->u.arr.len; i++) {
					if (!jv_is_truthy(input->u.arr.a[i])) {
						ok = 0;
						break;
					}
				}
			} else if (node->num_args == 1 && input && input->type == JV_ARR) {
				for (i = 0; i < input->u.arr.len; i++) {
					jv_list r = eval_expr(node->args[0], input->u.arr.a[i], env);
					for (j = 0; j < r.len; j++) {
						if (!jv_is_truthy(r.items[j]))
							ok = 0;
					}
					jv_list_free(&r);
					if (!ok) break;
				}
			}
			jv_list_append(&out, jv_bool(ok));
		}
		else if (strcmp(fn, "flatten") == 0) {
			int depth = -1;
			if (node->num_args > 0) {
				jv_list d_res = eval_expr(node->args[0], input, env);
				if (d_res.len > 0 && d_res.items[0]->type == JV_NUM)
					depth = (int)d_res.items[0]->u.nval;
				jv_list_free(&d_res);
			}
			jv_list_append(&out, builtin_flatten(input, depth));
		}
		else if (strcmp(fn, "range") == 0) {
			int from = 0, to = 0;
			if (node->num_args == 1) {
				jv_list r = eval_expr(node->args[0], input, env);
				if (r.len > 0 && r.items[0]->type == JV_NUM)
					to = (int)r.items[0]->u.nval;
				jv_list_free(&r);
			} else if (node->num_args >= 2) {
				jv_list r1 = eval_expr(node->args[0], input, env);
				jv_list r2 = eval_expr(node->args[1], input, env);
				if (r1.len > 0 && r1.items[0]->type == JV_NUM)
					from = (int)r1.items[0]->u.nval;
				if (r2.len > 0 && r2.items[0]->type == JV_NUM)
					to = (int)r2.items[0]->u.nval;
				jv_list_free(&r1);
				jv_list_free(&r2);
			}
			for (i = from; i < to; i++)
				jv_list_append(&out, jv_number(i));
		}
		else if (strcmp(fn, "floor") == 0) {
			if (input && input->type == JV_NUM)
				jv_list_append(&out, jv_number(floor(input->u.nval)));
		}
		else if (strcmp(fn, "ceil") == 0) {
			if (input && input->type == JV_NUM)
				jv_list_append(&out, jv_number(ceil(input->u.nval)));
		}
		else if (strcmp(fn, "round") == 0) {
			if (input && input->type == JV_NUM)
				jv_list_append(&out, jv_number(round(input->u.nval)));
		}
		else if (strcmp(fn, "sqrt") == 0) {
			if (input && input->type == JV_NUM)
				jv_list_append(&out, jv_number(sqrt(input->u.nval)));
		}
		else if (strcmp(fn, "fabs") == 0) {
			if (input && input->type == JV_NUM)
				jv_list_append(&out, jv_number(fabs(input->u.nval)));
		}
		else if (strcmp(fn, "nan") == 0) {
			jv_list_append(&out, jv_number(NAN));
		}
		else if (strcmp(fn, "infinite") == 0) {
			jv_list_append(&out, jv_number(INFINITY));
		}
		else if (strcmp(fn, "isinfinite") == 0) {
			jv_list_append(&out, jv_bool(input && input->type == JV_NUM && isinf(input->u.nval)));
		}
		else if (strcmp(fn, "isnan") == 0) {
			jv_list_append(&out, jv_bool(input && input->type == JV_NUM && isnan(input->u.nval)));
		}
		else if (strcmp(fn, "isnormal") == 0) {
			jv_list_append(&out, jv_bool(input && input->type == JV_NUM && isnormal(input->u.nval)));
		}
		else if (strcmp(fn, "isfinite") == 0) {
			jv_list_append(&out, jv_bool(input && input->type == JV_NUM && isfinite(input->u.nval)));
		}
		else if (strcmp(fn, "sort") == 0) {
			if (input && input->type == JV_ARR) {
				jv *res = jv_copy(input);
				if (res->u.arr.len > 1)
					qsort(res->u.arr.a, res->u.arr.len, sizeof(jv*), (int(*)(const void*,const void*))jv_compare);
				jv_list_append(&out, res);
			}
		}
		else if (strcmp(fn, "sort_by") == 0) {
			if (input && input->type == JV_ARR && node->num_args > 0) {
				int len = input->u.arr.len;
				sort_pair *pairs = xmalloc(len * sizeof(sort_pair));
				jv *res = jv_array();
				for (i = 0; i < len; i++) {
					jv_list kl = eval_expr(node->args[0], input->u.arr.a[i], env);
					pairs[i].elem = input->u.arr.a[i];
					pairs[i].key = (kl.len > 0) ? kl.items[0] : jv_null();
					for (j = 1; j < kl.len; j++) jv_free(kl.items[j]);
					free(kl.items);
				}
				qsort(pairs, len, sizeof(sort_pair), sort_pair_cmp);
				for (i = 0; i < len; i++) {
					jv_array_append(res, jv_copy(pairs[i].elem));
					jv_free(pairs[i].key);
				}
				free(pairs);
				jv_list_append(&out, res);
			}
		}
		else if (strcmp(fn, "group_by") == 0) {
			if (input && input->type == JV_ARR && node->num_args > 0) {
				int len = input->u.arr.len;
				sort_pair *pairs = xmalloc(len * sizeof(sort_pair));
				jv *res = jv_array();
				for (i = 0; i < len; i++) {
					jv_list kl = eval_expr(node->args[0], input->u.arr.a[i], env);
					pairs[i].elem = input->u.arr.a[i];
					pairs[i].key = (kl.len > 0) ? kl.items[0] : jv_null();
					for (j = 1; j < kl.len; j++) jv_free(kl.items[j]);
					free(kl.items);
				}
				qsort(pairs, len, sizeof(sort_pair), sort_pair_cmp);
				if (len > 0) {
					jv *grp = jv_array();
					jv_array_append(grp, jv_copy(pairs[0].elem));
					for (i = 1; i < len; i++) {
						if (jv_compare(pairs[i].key, pairs[i-1].key) == 0) {
							jv_array_append(grp, jv_copy(pairs[i].elem));
						} else {
							jv_array_append(res, grp);
							grp = jv_array();
							jv_array_append(grp, jv_copy(pairs[i].elem));
						}
					}
					jv_array_append(res, grp);
				}
				for (i = 0; i < len; i++)
					jv_free(pairs[i].key);
				free(pairs);
				jv_list_append(&out, res);
			}
		}
		else if (strcmp(fn, "unique") == 0) {
			if (input && input->type == JV_ARR) {
				jv *res = jv_array();
				jv *sorted = jv_copy(input);
				if (sorted->u.arr.len > 1)
					qsort(sorted->u.arr.a, sorted->u.arr.len, sizeof(jv*), (int(*)(const void*,const void*))jv_compare);
				for (i = 0; i < sorted->u.arr.len; i++) {
					if (i == 0 || !jv_equal(sorted->u.arr.a[i], sorted->u.arr.a[i-1]))
						jv_array_append(res, jv_copy(sorted->u.arr.a[i]));
				}
				jv_free(sorted);
				jv_list_append(&out, res);
			}
		}
		else if (strcmp(fn, "unique_by") == 0) {
			if (input && input->type == JV_ARR && node->num_args > 0) {
				int len = input->u.arr.len;
				sort_pair *pairs = xmalloc(len * sizeof(sort_pair));
				jv *res = jv_array();
				for (i = 0; i < len; i++) {
					jv_list kl = eval_expr(node->args[0], input->u.arr.a[i], env);
					pairs[i].elem = input->u.arr.a[i];
					pairs[i].key = (kl.len > 0) ? kl.items[0] : jv_null();
					for (j = 1; j < kl.len; j++) jv_free(kl.items[j]);
					free(kl.items);
				}
				qsort(pairs, len, sizeof(sort_pair), sort_pair_cmp);
				for (i = 0; i < len; i++) {
					if (i == 0 || jv_compare(pairs[i].key, pairs[i-1].key) != 0)
						jv_array_append(res, jv_copy(pairs[i].elem));
				}
				for (i = 0; i < len; i++)
					jv_free(pairs[i].key);
				free(pairs);
				jv_list_append(&out, res);
			}
		}
		else if (strcmp(fn, "reverse") == 0) {
			if (input && input->type == JV_ARR) {
				jv *res = jv_array();
				for (i = input->u.arr.len - 1; i >= 0; i--)
					jv_array_append(res, jv_copy(input->u.arr.a[i]));
				jv_list_append(&out, res);
			}
		}
		else if (strcmp(fn, "contains") == 0) {
			if (node->num_args > 0) {
				jv_list arg_res = eval_expr(node->args[0], input, env);
				int ok = 0;
				if (arg_res.len > 0)
					ok = jv_contains(input, arg_res.items[0]);
				jv_list_free(&arg_res);
				jv_list_append(&out, jv_bool(ok));
			}
		}
		else if (strcmp(fn, "inside") == 0) {
			if (node->num_args > 0) {
				jv_list arg_res = eval_expr(node->args[0], input, env);
				int ok = 0;
				if (arg_res.len > 0)
					ok = jv_contains(arg_res.items[0], input);
				jv_list_free(&arg_res);
				jv_list_append(&out, jv_bool(ok));
			}
		}
		else if (strcmp(fn, "split") == 0) {
			if (node->num_args > 0) {
				jv_list d_res = eval_expr(node->args[0], input, env);
				if (d_res.len > 0)
					jv_list_append(&out, builtin_split(input, d_res.items[0]));
				jv_list_free(&d_res);
			}
		}
		else if (strcmp(fn, "join") == 0) {
			if (node->num_args > 0) {
				jv_list d_res = eval_expr(node->args[0], input, env);
				if (d_res.len > 0)
					jv_list_append(&out, builtin_join(input, d_res.items[0]));
				jv_list_free(&d_res);
			}
		}
		else if (strcmp(fn, "ltrimstr") == 0) {
			if (node->num_args > 0) {
				jv_list p_res = eval_expr(node->args[0], input, env);
				if (p_res.len > 0)
					jv_list_append(&out, builtin_ltrimstr(input, p_res.items[0]));
				jv_list_free(&p_res);
			}
		}
		else if (strcmp(fn, "rtrimstr") == 0) {
			if (node->num_args > 0) {
				jv_list s_res = eval_expr(node->args[0], input, env);
				if (s_res.len > 0)
					jv_list_append(&out, builtin_rtrimstr(input, s_res.items[0]));
				jv_list_free(&s_res);
			}
		}
		else if (strcmp(fn, "startswith") == 0) {
			if (node->num_args > 0) {
				jv_list p_res = eval_expr(node->args[0], input, env);
				if (p_res.len > 0)
					jv_list_append(&out, builtin_startswith(input, p_res.items[0]));
				jv_list_free(&p_res);
			}
		}
		else if (strcmp(fn, "endswith") == 0) {
			if (node->num_args > 0) {
				jv_list s_res = eval_expr(node->args[0], input, env);
				if (s_res.len > 0)
					jv_list_append(&out, builtin_endswith(input, s_res.items[0]));
				jv_list_free(&s_res);
			}
		}
		else if (strcmp(fn, "ascii_downcase") == 0) {
			jv_list_append(&out, builtin_ascii_case(input, 0));
		}
		else if (strcmp(fn, "ascii_upcase") == 0) {
			jv_list_append(&out, builtin_ascii_case(input, 1));
		}
		else if (strcmp(fn, "tostring") == 0 || strcmp(fn, "text") == 0) {
			if (input && input->type == JV_STR)
				jv_list_append(&out, jv_copy(input));
			else
				jv_list_append(&out, jv_string(jv_to_string(input)));
		}
		else if (strcmp(fn, "tonumber") == 0) {
			if (input && input->type == JV_NUM)
				jv_list_append(&out, jv_copy(input));
			else if (input && input->type == JV_STR)
				jv_list_append(&out, jv_number(strtod(input->u.str.s, NULL)));
			else
				jv_list_append(&out, jv_null());
		}
		else if (strcmp(fn, "type") == 0) {
			const char *t = "null";
			if (input) {
				switch (input->type) {
				case JV_NULL: t = "null"; break;
				case JV_BOOL: t = "boolean"; break;
				case JV_NUM:  t = "number"; break;
				case JV_STR:  t = "string"; break;
				case JV_ARR:  t = "array"; break;
				case JV_OBJ:  t = "object"; break;
				}
			}
			jv_list_append(&out, jv_string(t));
		}
		else if (strcmp(fn, "not") == 0) {
			jv_list_append(&out, jv_bool(!jv_is_truthy(input)));
		}
		else if (strcmp(fn, "error") == 0) {
			char *msg = NULL;
			if (node->num_args > 0) {
				jv_list m = eval_expr(node->args[0], input, env);
				if (m.len > 0 && m.items[0]->type == JV_STR)
					msg = xstrdup(m.items[0]->u.str.s);
				jv_list_free(&m);
			}
			env->error = 1;
			free(env->error_msg);
			env->error_msg = msg ? msg : xstrdup("error");
		}
		else if (strcmp(fn, "to_entries") == 0) {
			jv *res = jv_array();
			if (input && input->type == JV_OBJ) {
				for (i = 0; i < input->u.obj.len; i++) {
					jv *e = jv_object();
					jv_object_set(e, "key", jv_string(input->u.obj.kv[i].k));
					jv_object_set(e, "value", jv_copy(input->u.obj.kv[i].v));
					jv_array_append(res, e);
				}
			} else if (input && input->type == JV_ARR) {
				for (i = 0; i < input->u.arr.len; i++) {
					jv *e = jv_object();
					jv_object_set(e, "key", jv_number(i));
					jv_object_set(e, "value", jv_copy(input->u.arr.a[i]));
					jv_array_append(res, e);
				}
			}
			jv_list_append(&out, res);
		}
		else if (strcmp(fn, "from_entries") == 0) {
			jv *res = jv_object();
			if (input && input->type == JV_ARR) {
				for (i = 0; i < input->u.arr.len; i++) {
					jv *e = input->u.arr.a[i];
					if (e && e->type == JV_OBJ) {
						jv *k_val = jv_object_get(e, "key");
						jv *v_val = jv_object_get(e, "value");
						if (!k_val) k_val = jv_object_get(e, "Key");
						if (!k_val) k_val = jv_object_get(e, "name");
						if (!v_val) v_val = jv_object_get(e, "Value");
						if (k_val) {
							char *kstr = (k_val->type == JV_STR) ? k_val->u.str.s : jv_to_string(k_val);
							jv_object_set(res, kstr, v_val ? jv_copy(v_val) : jv_null());
							if (k_val->type != JV_STR) free(kstr);
						}
					}
				}
			}
			jv_list_append(&out, res);
		}
		else if (strcmp(fn, "with_entries") == 0) {
			if (node->num_args > 0) {
				ast_node *to_e = ast_new(AST_CALL);
				ast_node *map_e = ast_new(AST_CALL);
				ast_node *from_e = ast_new(AST_CALL);
				ast_node *p1 = ast_new(AST_PIPE);
				ast_node *p2 = ast_new(AST_PIPE);
				to_e->sval = xstrdup("to_entries");
				from_e->sval = xstrdup("from_entries");
				map_e->sval = xstrdup("map");
				map_e->args = xmalloc(sizeof(ast_node*));
				map_e->args[0] = node->args[0];
				map_e->num_args = 1;
				p1->left = to_e;
				p1->right = map_e;
				p2->left = p1;
				p2->right = from_e;
				out = eval_expr(p2, input, env);
				map_e->args = NULL;
				map_e->num_args = 0;
				ast_free(p2);
			}
		}
		else if (strcmp(fn, "recurse") == 0) {
			ast_node *f_expr = (node->num_args > 0) ? node->args[0] : NULL;
			recurse_helper(&out, input, f_expr, env);
		}
		else if (strcmp(fn, "env") == 0) {
			jv_list_append(&out, get_env_object());
		}
		else if (strcmp(fn, "base64") == 0) {
			jv_list_append(&out, builtin_base64_encode(input));
		}
		else if (strcmp(fn, "base64d") == 0) {
			jv_list_append(&out, builtin_base64_decode(input));
		}
		else if (strcmp(fn, "uri") == 0) {
			jv_list_append(&out, builtin_uri_encode(input));
		}
		else if (strcmp(fn, "csv") == 0) {
			jv_list_append(&out, builtin_csv(input));
		}
		else if (strcmp(fn, "tsv") == 0) {
			jv_list_append(&out, builtin_tsv(input));
		}
		else if (strcmp(fn, "html") == 0) {
			jv_list_append(&out, builtin_html_escape(input));
		}
		else if (strcmp(fn, "sh") == 0) {
			jv_list_append(&out, builtin_sh_quote(input));
		}
		else if (strcmp(fn, "ascii") == 0) {
			jv_list_append(&out, builtin_ascii(input));
		}
		else if (strcmp(fn, "explode") == 0) {
			jv_list_append(&out, builtin_explode(input));
		}
		else if (strcmp(fn, "implode") == 0) {
			jv_list_append(&out, builtin_implode(input));
		}
		else if (strcmp(fn, "tojson") == 0 || strcmp(fn, "json") == 0) {
			jv_list_append(&out, jv_string(jv_to_string(input)));
		}
		else if (strcmp(fn, "fromjson") == 0) {
			if (input && input->type == JV_STR) {
				char *err = NULL;
				const char *p = input->u.str.s;
				jv *parsed = parse_json_value(&p, p + input->u.str.len, &err);
				free(err);
				jv_list_append(&out, parsed ? parsed : jv_null());
			}
		}
		else if (strcmp(fn, "debug") == 0) {
			char *dump = jv_to_string(input);
			fprintf(stderr, "DEBUG: %s\n", dump);
			free(dump);
			jv_list_append(&out, jv_copy(input));
		}
		else if (strcmp(fn, "first") == 0) {
			if (node->num_args > 0) {
				jv_list r = eval_expr(node->args[0], input, env);
				if (r.len > 0)
					jv_list_append(&out, jv_copy(r.items[0]));
				jv_list_free(&r);
			}
		}
		else if (strcmp(fn, "last") == 0) {
			if (node->num_args > 0) {
				jv_list r = eval_expr(node->args[0], input, env);
				if (r.len > 0)
					jv_list_append(&out, jv_copy(r.items[r.len - 1]));
				jv_list_free(&r);
			}
		}
		else if (strcmp(fn, "nth") == 0) {
			if (node->num_args >= 2) {
				jv_list nr = eval_expr(node->args[0], input, env);
				int n = (nr.len > 0 && nr.items[0]->type == JV_NUM) ? (int)nr.items[0]->u.nval : 0;
				jv_list r = eval_expr(node->args[1], input, env);
				if (n >= 0 && n < r.len)
					jv_list_append(&out, jv_copy(r.items[n]));
				jv_list_free(&nr);
				jv_list_free(&r);
			}
		}
		else if (strcmp(fn, "limit") == 0) {
			if (node->num_args >= 2) {
				jv_list nr = eval_expr(node->args[0], input, env);
				int n = (nr.len > 0 && nr.items[0]->type == JV_NUM) ? (int)nr.items[0]->u.nval : 0;
				jv_list r = eval_expr(node->args[1], input, env);
				int max_items = (n < r.len) ? n : r.len;
				for (i = 0; i < max_items; i++)
					jv_list_append(&out, jv_copy(r.items[i]));
				jv_list_free(&nr);
				jv_list_free(&r);
			}
		}
		else if (strcmp(fn, "while") == 0) {
			if (node->num_args >= 2) {
				jv *cur = jv_copy(input);
				while (1) {
					jv_list cr = eval_expr(node->args[0], cur, env);
					int is_ok = (cr.len > 0 && jv_is_truthy(cr.items[0]));
					jv_list_free(&cr);
					if (!is_ok) break;
					jv_list_append(&out, jv_copy(cur));
					{
						jv_list ur = eval_expr(node->args[1], cur, env);
						jv_free(cur);
						cur = (ur.len > 0) ? ur.items[0] : jv_null();
						for (j = 1; j < ur.len; j++) jv_free(ur.items[j]);
						free(ur.items);
					}
				}
				jv_free(cur);
			}
		}
		else if (strcmp(fn, "until") == 0) {
			if (node->num_args >= 2) {
				jv *cur = jv_copy(input);
				while (1) {
					jv_list cr = eval_expr(node->args[0], cur, env);
					int is_ok = (cr.len > 0 && jv_is_truthy(cr.items[0]));
					jv_list_free(&cr);
					if (is_ok) {
						jv_list_append(&out, cur);
						break;
					}
					{
						jv_list ur = eval_expr(node->args[1], cur, env);
						jv_free(cur);
						cur = (ur.len > 0) ? ur.items[0] : jv_null();
						for (j = 1; j < ur.len; j++) jv_free(ur.items[j]);
						free(ur.items);
					}
				}
			}
		}
		else if (strcmp(fn, "input") == 0 || strcmp(fn, "inputs") == 0) {
			/* stubs */
		}
		else {
			/* fallback to identity if unknown */
			jv_list_append(&out, jv_copy(input));
		}
		break;
	}
	}

	return out;
}

/* --- Output and CLI Processing --- */

static void output_value(jv *v)
{
	str_buf sb;

	G.had_output = 1;
	G.last_truthy = jv_is_truthy(v);

	str_buf_init(&sb);
	jv_dump_to_sb(&sb, v, G.compact_out, G.raw_out, G.indent_width, G.use_tabs,
	              G.ascii_out, G.sort_keys_opt, 0);
	if (sb.buf)
		fputs(sb.buf, stdout);
	str_buf_free(&sb);

	if (!G.join_out)
		putchar('\n');
	jv_free(v);
}

static void process_json_input(const char *buf, size_t sz, ast_node *ast, env_scope *env)
{
	const char *p = buf;
	const char *end = buf + sz;

	if (G.slurp_in) {
		jv *arr = jv_array();
		while (p < end) {
			char *err = NULL;
			jv *val = parse_json_value(&p, end, &err);
			if (!val) {
				free(err);
				break;
			}
			jv_array_append(arr, val);
		}
		{
			jv_list res = eval_expr(ast, arr, env);
			int i;
			for (i = 0; i < res.len; i++)
				output_value(res.items[i]);
			free(res.items);
			jv_free(arr);
		}
	} else {
		while (p < end) {
			char *err = NULL;
			jv *val = parse_json_value(&p, end, &err);
			if (!val) {
				free(err);
				break;
			}
			{
				jv_list res = eval_expr(ast, val, env);
				int i;
				for (i = 0; i < res.len; i++)
					output_value(res.items[i]);
				free(res.items);
				jv_free(val);
			}
		}
	}
}

static void process_raw_input(FILE *fp, ast_node *ast, env_scope *env)
{
	if (G.slurp_in) {
		char *buf;
		int fd = fileno(fp);
		buf = xmalloc_read(fd, NULL);
		if (buf) {
			jv *v = jv_string_take(buf, strlen(buf));
			jv_list res = eval_expr(ast, v, env);
			int i;
			for (i = 0; i < res.len; i++)
				output_value(res.items[i]);
			free(res.items);
			jv_free(v);
		}
	} else {
		char *line;
		while ((line = xmalloc_fgetline(fp)) != NULL) {
			jv *v = jv_string(line);
			jv_list res = eval_expr(ast, v, env);
			int i;
			for (i = 0; i < res.len; i++)
				output_value(res.items[i]);
			free(res.items);
			jv_free(v);
			free(line);
		}
	}
}

int jq_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int jq_main(int argc, char **argv)
{
	int argi;
	char *filter_str = NULL;
	char *filter_file = NULL;
	ast_node *ast;
	env_scope top_env;
	jv *args_obj = NULL;
	jv *positional_arr = NULL;
	int args_mode = 0; /* 1: --args, 2: --jsonargs */

	INIT_G();

	top_env.vars = NULL;
	top_env.parent = NULL;
	top_env.error = 0;
	top_env.error_msg = NULL;

	argi = 1;
	while (argi < argc) {
		char *arg = argv[argi];
		if (arg[0] != '-' || arg[1] == '\0')
			break;

		if (strcmp(arg, "--") == 0) {
			argi++;
			break;
		}

		if (strcmp(arg, "-c") == 0 || strcmp(arg, "--compact-output") == 0) {
			G.compact_out = 1;
		} else if (strcmp(arg, "-r") == 0 || strcmp(arg, "--raw-output") == 0) {
			G.raw_out = 1;
		} else if (strcmp(arg, "-n") == 0 || strcmp(arg, "--null-input") == 0) {
			G.null_in = 1;
		} else if (strcmp(arg, "-R") == 0 || strcmp(arg, "--raw-input") == 0) {
			G.raw_in = 1;
		} else if (strcmp(arg, "-s") == 0 || strcmp(arg, "--slurp") == 0) {
			G.slurp_in = 1;
		} else if (strcmp(arg, "-e") == 0 || strcmp(arg, "--exit-status") == 0) {
			G.exit_status_opt = 1;
		} else if (strcmp(arg, "-j") == 0 || strcmp(arg, "--join-output") == 0) {
			G.join_out = 1;
		} else if (strcmp(arg, "-a") == 0 || strcmp(arg, "--ascii-output") == 0) {
			G.ascii_out = 1;
		} else if (strcmp(arg, "-S") == 0 || strcmp(arg, "--sort-keys") == 0) {
			G.sort_keys_opt = 1;
		} else if (strcmp(arg, "--tab") == 0) {
			G.use_tabs = 1;
		} else if (strcmp(arg, "--indent") == 0 && argi + 1 < argc) {
			G.indent_width = atoi(argv[++argi]);
		} else if (strcmp(arg, "-f") == 0 || strcmp(arg, "--from-file") == 0) {
			if (++argi < argc)
				filter_file = argv[argi];
		} else if (strncmp(arg, "-f", 2) == 0) {
			filter_file = arg + 2;
		} else if (strcmp(arg, "-L") == 0) {
			argi++; /* stub */
		} else if (strncmp(arg, "-L", 2) == 0) {
			/* stub */
		} else if (strcmp(arg, "--arg") == 0 && argi + 2 < argc) {
			char *k = argv[++argi];
			char *v = argv[++argi];
			env_set(&top_env, k, jv_string(v));
		} else if (strcmp(arg, "--argjson") == 0 && argi + 2 < argc) {
			char *k = argv[++argi];
			char *jstr = argv[++argi];
			const char *p = jstr;
			char *err = NULL;
			jv *val = parse_json_value(&p, p + strlen(jstr), &err);
			free(err);
			env_set(&top_env, k, val ? val : jv_null());
		} else if (strcmp(arg, "--rawfile") == 0 && argi + 2 < argc) {
			char *k = argv[++argi];
			char *fn = argv[++argi];
			char *buf = xmalloc_open_read_close(fn, NULL);
			env_set(&top_env, k, jv_string_take(buf ? buf : xstrdup(""), buf ? strlen(buf) : 0));
		} else if (strcmp(arg, "--slurpfile") == 0 && argi + 2 < argc) {
			char *k = argv[++argi];
			char *fn = argv[++argi];
			char *buf = xmalloc_open_read_close(fn, NULL);
			jv *arr = jv_array();
			if (buf) {
				const char *p = buf;
				const char *end = buf + strlen(buf);
				while (p < end) {
					char *err = NULL;
					jv *val = parse_json_value(&p, end, &err);
					if (!val) { free(err); break; }
					jv_array_append(arr, val);
				}
				free(buf);
			}
			env_set(&top_env, k, arr);
		} else if (strcmp(arg, "--args") == 0) {
			args_mode = 1;
			argi++;
			break;
		} else if (strcmp(arg, "--jsonargs") == 0) {
			args_mode = 2;
			argi++;
			break;
		} else {
			/* Check clustered short options e.g. -crn */
			char *c = arg + 1;
			while (*c) {
				if (*c == 'c') G.compact_out = 1;
				else if (*c == 'r') G.raw_out = 1;
				else if (*c == 'n') G.null_in = 1;
				else if (*c == 'R') G.raw_in = 1;
				else if (*c == 's') G.slurp_in = 1;
				else if (*c == 'e') G.exit_status_opt = 1;
				else if (*c == 'j') G.join_out = 1;
				else if (*c == 'a') G.ascii_out = 1;
				else if (*c == 'S') G.sort_keys_opt = 1;
				else break;
				c++;
			}
		}
		argi++;
	}

	if (filter_file) {
		filter_str = xmalloc_open_read_close(filter_file, NULL);
		if (!filter_str)
			bb_error_msg_and_die("cannot read filter from %s", filter_file);
	} else if (argi < argc && !args_mode) {
		filter_str = xstrdup(argv[argi++]);
	} else if (args_mode && argi < argc) {
		filter_str = xstrdup(argv[argi++]);
	} else {
		filter_str = xstrdup(".");
	}

	if (args_mode) {
		positional_arr = jv_array();
		while (argi < argc) {
			if (args_mode == 1) {
				jv_array_append(positional_arr, jv_string(argv[argi]));
			} else {
				const char *p = argv[argi];
				char *err = NULL;
				jv *v = parse_json_value(&p, p + strlen(p), &err);
				free(err);
				jv_array_append(positional_arr, v ? v : jv_null());
			}
			argi++;
		}
		args_obj = jv_object();
		jv_object_set(args_obj, "positional", positional_arr);
		env_set(&top_env, "ARGS", args_obj);
	}

	ast = jq_compile(filter_str);
	free(filter_str);

	if (G.null_in) {
		jv *n = jv_null();
		jv_list res = eval_expr(ast, n, &top_env);
		int i;
		for (i = 0; i < res.len; i++)
			output_value(res.items[i]);
		free(res.items);
		jv_free(n);
	} else if (argi < argc && !args_mode) {
		while (argi < argc) {
			char *fname = argv[argi++];
			if (strcmp(fname, "-") == 0) {
				if (G.raw_in) {
					process_raw_input(stdin, ast, &top_env);
				} else {
					char *buf = xmalloc_read(STDIN_FILENO, NULL);
					if (buf) {
						process_json_input(buf, strlen(buf), ast, &top_env);
						free(buf);
					}
				}
			} else {
				if (G.raw_in) {
					FILE *fp = fopen_or_warn(fname, "r");
					if (fp) {
						process_raw_input(fp, ast, &top_env);
						fclose(fp);
					}
				} else {
					char *buf = xmalloc_open_read_close(fname, NULL);
					if (buf) {
						process_json_input(buf, strlen(buf), ast, &top_env);
						free(buf);
					}
				}
			}
		}
	} else {
		if (G.raw_in) {
			process_raw_input(stdin, ast, &top_env);
		} else {
			char *buf = xmalloc_read(STDIN_FILENO, NULL);
			if (buf) {
				process_json_input(buf, strlen(buf), ast, &top_env);
				free(buf);
			}
		}
	}

	ast_free(ast);
	env_free(&top_env);

	if (G.exit_status_opt) {
		if (!G.had_output)
			return 4;
		if (!G.last_truthy)
			return 1;
		return 0;
	}

	return 0;
}
