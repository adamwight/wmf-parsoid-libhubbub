/*
 * Tree construction tester.
 * Leaks a fair bit of memory, because it doesn't make use of reference
 * counting.
 */

#define _GNU_SOURCE

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <hubbub/hubbub.h>
#include <hubbub/parser.h>
#include <hubbub/tree.h>

#include "utils/utils.h"

#include "testutils.h"

typedef struct attr_t attr_t;
typedef struct node_t node_t;
typedef struct buf_t buf_t;

struct attr_t {
	hubbub_ns ns;
	char *name;
	char *value;
};

struct node_t {
	enum { DOCTYPE, COMMENT, ELEMENT, CHARACTER } type;

	union {
		struct {
			char *name;
			char *public_id;
			char *system_id;
		} doctype;

		struct {
			hubbub_ns ns;
			char *name;
			attr_t *attrs;
			size_t n_attrs;
		} element;

		char *content;		/**< For comments, characters **/
	} data;

	node_t *next;
	node_t *prev;

	node_t *child;
	node_t *parent;
};

struct buf_t {
	char *buf;
	size_t len;
	size_t pos;
};


#define NUM_NAMESPACES 7
const char const *ns_names[NUM_NAMESPACES] =
		{ NULL, NULL /*html*/, "math", "svg", "xlink", "xml", "xmlns" };


node_t *Document;



static void node_print(buf_t *buf, node_t *node, unsigned depth);


static const uint8_t *pbuffer;

static void buffer_handler(const uint8_t *buffer, size_t len, void *pw);
static int create_comment(void *ctx, const hubbub_string *data, void **result);
static int create_doctype(void *ctx, const hubbub_string *qname,
		const hubbub_string *public_id, const hubbub_string *system_id,
		void **result);
static int create_element(void *ctx, const hubbub_tag *tag, void **result);
static int create_text(void *ctx, const hubbub_string *data, void **result);
static int ref_node(void *ctx, void *node);
static int unref_node(void *ctx, void *node);
static int append_child(void *ctx, void *parent, void *child, void **result);
static int insert_before(void *ctx, void *parent, void *child, void *ref_child,
		void **result);
static int remove_child(void *ctx, void *parent, void *child, void **result);
static int clone_node(void *ctx, void *node, bool deep, void **result);
static int reparent_children(void *ctx, void *node, void *new_parent);
static int get_parent(void *ctx, void *node, bool element_only, void **result);
static int has_children(void *ctx, void *node, bool *result);
static int form_associate(void *ctx, void *form, void *node);
static int add_attributes(void *ctx, void *node,
		const hubbub_attribute *attributes, uint32_t n_attributes);
static int set_quirks_mode(void *ctx, hubbub_quirks_mode mode);

static hubbub_tree_handler tree_handler = {
	create_comment,
	create_doctype,
	create_element,
	create_text,
	ref_node,
	unref_node,
	append_child,
	insert_before,
	remove_child,
	clone_node,
	reparent_children,
	get_parent,
	has_children,
	form_associate,
	add_attributes,
	set_quirks_mode,
	NULL
};

static void *myrealloc(void *ptr, size_t len, void *pw)
{
	UNUSED(pw);

	return realloc(ptr, len);
}

static const uint8_t *ptr_from_hubbub_string(const hubbub_string *string)
{
	const uint8_t *data;

	switch (string->type) {
	case HUBBUB_STRING_OFF:
		data = pbuffer + string->data.off;
		break;
	case HUBBUB_STRING_PTR:
		data = string->data.ptr;
		break;
	}

	return data;
}



/*
 * Create, initialise, and return, a parser instance.
 */
static hubbub_parser *setup_parser(void)
{
	hubbub_parser *parser;
	hubbub_parser_optparams params;

	parser = hubbub_parser_create("UTF-8", "UTF-8", myrealloc, NULL);
	assert(parser != NULL);

	params.buffer_handler.handler = buffer_handler;
	params.buffer_handler.pw = NULL;
	assert(hubbub_parser_setopt(parser, HUBBUB_PARSER_BUFFER_HANDLER,
			&params) == HUBBUB_OK);

	params.tree_handler = &tree_handler;
	assert(hubbub_parser_setopt(parser, HUBBUB_PARSER_TREE_HANDLER,
			&params) == HUBBUB_OK);

	params.document_node = (void *)1;
	assert(hubbub_parser_setopt(parser, HUBBUB_PARSER_DOCUMENT_NODE,
			&params) == HUBBUB_OK);

	return parser;
}


void buffer_handler(const uint8_t *buffer, size_t len, void *pw)
{
	UNUSED(len);
	UNUSED(pw);

	pbuffer = buffer;
}


/*** Buffer handling bits ***/
static void buf_clear(buf_t *buf)
{
	buf->buf[0] = '\0';
	buf->pos = 0;
}

static void buf_add(buf_t *buf, const char *str)
{
	size_t len = strlen(str) + 1;

	if (!buf) {
		printf("%s", str);
		return;
	}

	if (buf->buf == NULL) {
		buf->len = ((len + 1024) / 1024) * 1024;
		buf->buf = calloc(1, buf->len);
	}

	while (buf->pos + len > buf->len) {
		buf->len *= 2;
		buf->buf = realloc(buf->buf, buf->len);
	}

	strcat(buf->buf, str);
	buf->pos += len;
}



/* States for reading in data from the tree construction file */
enum reading_state {
	EXPECT_DATA,
	READING_DATA,
	READING_ERRORS,
	READING_TREE,
};

int main(int argc, char **argv)
{
	FILE *fp;
	char line[1024];

	bool passed = true;

	hubbub_parser *parser;
	enum reading_state state = EXPECT_DATA;

	buf_t expected = { NULL, 0, 0 };
	buf_t got = { NULL, 0, 0 };


	if (argc != 3) {
		printf("Usage: %s <aliases_file> <filename>\n", argv[0]);
		return 1;
	}

	/* Initialise library */
	assert(hubbub_initialise(argv[1], myrealloc, NULL) == HUBBUB_OK);

	fp = fopen(argv[2], "rb");
	if (fp == NULL) {
		printf("Failed opening %s\n", argv[2]);
		return 1;
	}

	/* We rely on lines not being anywhere near 1024 characters... */
	while (passed && fgets(line, sizeof line, fp) == line) {
		switch (state)
		{
 		case EXPECT_DATA:
			if (strcmp(line, "#data\n") == 0) {
				parser = setup_parser();
				state = READING_DATA;
			}
			break;

		case READING_DATA:
			if (strcmp(line, "#errors\n") == 0) {
				assert(hubbub_parser_completed(parser) == HUBBUB_OK);
				state = READING_ERRORS;
			} else {
				size_t len = strlen(line);

				printf(": %s", line);
				assert(hubbub_parser_parse_chunk(parser, (uint8_t *)line,
						len - 1) == HUBBUB_OK);
			}
			break;

		case READING_ERRORS:
			if (strcmp(line, "#document-fragment\n") == 0) {
				hubbub_parser_destroy(parser);
				state = EXPECT_DATA;
			}

			if (strcmp(line, "#document\n") == 0)
				state = READING_TREE;
			else {
			}
			break;

		case READING_TREE:
			if (line[0] == '|' && line[1] == ' ') {
				buf_add(&expected, line);
			} else {
				node_print(&got, Document, 0);

				passed = !strcmp(got.buf, expected.buf);
				if (!passed) {
					printf("expected:\n");
					printf("%s", expected.buf);
					printf("got:\n");
					printf("%s", got.buf);
				}

				buf_clear(&got);
				buf_clear(&expected);

				hubbub_parser_destroy(parser);
				Document = NULL;

				state = EXPECT_DATA;
			}
			break;
		}
	}

	printf("%s\n", passed ? "PASS" : "FAIL");

	assert(hubbub_finalise(myrealloc, NULL) == HUBBUB_OK);

	return 0;
}


/*** Tree construction functions ***/

int create_comment(void *ctx, const hubbub_string *data, void **result)
{
	node_t *node = calloc(1, sizeof *node);

	node->type = COMMENT;
	node->data.content = strndup((char *)ptr_from_hubbub_string(data),
			data->len);

	*result = node;

	return 0;
}

int create_doctype(void *ctx, const hubbub_string *qname,
		const hubbub_string *public_id, const hubbub_string *system_id,
		void **result)
{
	node_t *node = calloc(1, sizeof *node);

	node->type = DOCTYPE;
	node->data.doctype.name = strndup((char *)ptr_from_hubbub_string(qname),
			qname->len);
	node->data.doctype.public_id =
			strndup((char *)ptr_from_hubbub_string(public_id),
			public_id->len);
	node->data.doctype.system_id = strndup(
			(char *)ptr_from_hubbub_string(system_id),
			system_id->len);

	*result = node;

	return 0;
}

int create_element(void *ctx, const hubbub_tag *tag, void **result)
{
	node_t *node = calloc(1, sizeof *node);

	assert(tag->ns < NUM_NAMESPACES);

	node->type = ELEMENT;
	node->data.element.ns = tag->ns;
	node->data.element.name = strndup(
			(char *)ptr_from_hubbub_string(&tag->name),
			tag->name.len);
	node->data.element.n_attrs = tag->n_attributes;

	node->data.element.attrs = calloc(node->data.element.n_attrs,
			sizeof *node->data.element.attrs);

	for (size_t i = 0; i < tag->n_attributes; i++) {
		attr_t *attr = &node->data.element.attrs[i];

		assert(tag->attributes[i].ns < NUM_NAMESPACES);

		attr->ns = tag->attributes[i].ns;

		attr->name = strndup((char *)ptr_from_hubbub_string(
						&tag->attributes[i].name),
				tag->attributes[i].name.len);

		attr->value = strndup((char *)ptr_from_hubbub_string(
						&tag->attributes[i].value),
				tag->attributes[i].value.len);
	}

	*result = node;

	return 0;
}

int create_text(void *ctx, const hubbub_string *data, void **result)
{
	node_t *node = calloc(1, sizeof *node);

	node->type = CHARACTER;
	node->data.content = strndup((char *)ptr_from_hubbub_string(data),
			data->len);

	*result = node;

	return 0;
}

int ref_node(void *ctx, void *node)
{
	return 0;
}

int unref_node(void *ctx, void *node)
{
	return 0;
}

int append_child(void *ctx, void *parent, void *child, void **result)
{
	node_t *tparent = parent;
	node_t *tchild = child;

	node_t *insert = NULL;

	tchild->parent = tparent;
	tchild->next = tchild->prev = NULL;

#ifndef NDEBUG
	printf("appending (%x):\n", (unsigned)tchild);
	node_print(NULL, tchild, 0);
	printf("to:\n");
	if (parent != (void *)1)
		node_print(NULL, tparent, 0);
#endif

	if (parent == (void *)1) {
		if (Document) {
			insert = Document;
		} else {
			Document = tchild;
		}
	} else {
		if (tparent->child == NULL) {
			tparent->child = tchild;
		} else {
			insert = tparent->child;
		}
	}

	if (insert) {
		while (insert->next != NULL) {
			insert = insert->next;
		}

		insert->next = tchild;
		tchild->prev = insert;
	}

	*result = child;

	return 0;
}

/* insert 'child' before 'ref_child', under 'parent' */
int insert_before(void *ctx, void *parent, void *child, void *ref_child,
		void **result)
{
	node_t *tparent = parent;
	node_t *tchild = child;
	node_t *tref = ref_child;

	tchild->parent = parent;

	tchild->prev = tref->prev;
	tchild->next = tref;
	tref->prev = tchild;

	if (tchild->prev)
		tchild->prev->next = tchild;
	else
		tparent->child = tchild;

	*result = child;

	return 0;
}

int remove_child(void *ctx, void *parent, void *child, void **result)
{
	node_t *tparent = parent;
	node_t *tchild = child;

	assert(tparent->child);
	assert(tchild->parent == tparent);

	if (tchild->parent->child == tchild) {
		tchild->parent->child = tchild->next;
	}

	if (tchild->prev)
		tchild->prev->next = tchild->next;

	if (tchild->next)
		tchild->next->prev = tchild->prev;

	/* now reset all the child's pointers */
	tchild->next = tchild->prev = tchild->parent = NULL;

	*result = child;

	return 0;
}

int clone_node(void *ctx, void *node, bool deep, void **result)
{
	node_t *old_node = node;
	node_t *new_node = calloc(1, sizeof *new_node);

	*new_node = *old_node;
	*result = new_node;

	new_node->child = new_node->parent =
			new_node->next = new_node->prev =
			NULL;

	if (deep == false)
		return 0;

	if (old_node->next) {
		void *n;

		clone_node(ctx, old_node->next, true, &n);

		new_node->next = n;
		new_node->next->prev = new_node;
	}

	if (old_node->child) {
		void *n;

		clone_node(ctx, old_node->child, true, &n);

		new_node->child = n;
		new_node->child->parent = new_node;
	}

	return 0;
}

/* Take all of the child nodes of "node" and append them to "new_parent" */
int reparent_children(void *ctx, void *node, void *new_parent)
{
	node_t *parent = new_parent;
	node_t *old_parent = node;

	node_t *insert;
	node_t *kids;

	kids = old_parent->child;
	if (!kids) return 0;

	old_parent->child = NULL;

	insert = parent->child;
	if (!insert) {
		parent->child = kids;
	} else {
		while (insert->next != NULL) {
			insert = insert->next;
		}

		insert->next = kids;
		kids->prev = insert;
	}

	while (kids) {
		kids->parent = parent;
		kids = kids->next;
	}

	return 0;
}

int get_parent(void *ctx, void *node, bool element_only, void **result)
{
	*result = ((node_t *)node)->parent;

	return 0;
}

int has_children(void *ctx, void *node, bool *result)
{
	*result = ((node_t *)node)->child ? true : false;

	return 0;
}

int form_associate(void *ctx, void *form, void *node)
{
	return 0;
}

int add_attributes(void *ctx, void *node,
		const hubbub_attribute *attributes, uint32_t n_attributes)
{
	/* not yet implemented */

	return 0;
}

int set_quirks_mode(void *ctx, hubbub_quirks_mode mode)
{
	return 0;
}



/*** Serialising bits ***/

static int compare_attrs(const void *a, const void *b) {
	const attr_t *first = a;
	const attr_t *second = b;

	return strcmp(first->name, second->name);
}




static void indent(buf_t *buf, unsigned depth)
{
	buf_add(buf, "| ");
	for (unsigned i = 0; i < depth; i++) {
		buf_add(buf, "  ");
	}
}

static void print_ns(buf_t *buf, hubbub_ns ns)
{
	if (ns_names[ns] != NULL) {
		buf_add(buf, ns_names[ns]);
		buf_add(buf, " ");
	}
}

static void node_print(buf_t *buf, node_t *node, unsigned depth)
{
	if (!node) return;

	indent(buf, depth);

	switch (node->type)
	{
	case DOCTYPE:
		buf_add(buf, "<!DOCTYPE \n");
		break;
	case ELEMENT:
		buf_add(buf, "<");
		print_ns(buf, node->data.element.ns);
		buf_add(buf, node->data.element.name);
		buf_add(buf, ">\n");

		qsort(node->data.element.attrs, node->data.element.n_attrs,
				sizeof *node->data.element.attrs,
				compare_attrs);

		for (size_t i = 0; i < node->data.element.n_attrs; i++) {
			indent(buf, depth + 1);
			print_ns(buf, node->data.element.attrs[i].ns);
			buf_add(buf, node->data.element.attrs[i].name);
			buf_add(buf, "=");
			buf_add(buf, "\"");
			buf_add(buf, node->data.element.attrs[i].value);
			buf_add(buf, "\"\n");
		}

		break;
	case CHARACTER:
		buf_add(buf, "\"");
		buf_add(buf, node->data.content);
		buf_add(buf, "\"\n");
		break;
	case COMMENT:
		buf_add(buf, "<!-- ");
		buf_add(buf, node->data.content);
		buf_add(buf, " -->\n");
		break;
	}

	if (node->child) {
		node_print(buf, node->child, depth + 1);
	}

	if (node->next) {
		node_print(buf, node->next, depth);
	}
}