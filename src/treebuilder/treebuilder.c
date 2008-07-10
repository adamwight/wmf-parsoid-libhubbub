/*
 * This file is part of Hubbub.
 * Licensed under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 * Copyright 2008 John-Mark Bell <jmb@netsurf-browser.org>
 */

#include <assert.h>
#include <string.h>

#include <stdio.h>

#include "treebuilder/modes.h"
#include "treebuilder/internal.h"
#include "treebuilder/treebuilder.h"
#include "utils/utils.h"
#include "utils/string.h"

static const struct {
	const char *name;
	element_type type;
} name_type_map[] = {
	{ "ADDRESS", ADDRESS },	{ "AREA", AREA },
	{ "BASE", BASE },	{ "BASEFONT", BASEFONT },
	{ "BGSOUND", BGSOUND },	{ "BLOCKQUOTE", BLOCKQUOTE },
	{ "BODY", BODY },	{ "BR", BR }, 
	{ "CENTER", CENTER },	{ "COL", COL },
	{ "COLGROUP", COLGROUP },	{ "DD", DD },
	{ "DIR", DIR },		{ "DIV", DIV },
	{ "DL", DL },		{ "DT", DT },
	{ "EMBED", EMBED },	{ "FIELDSET", FIELDSET },
	{ "FORM", FORM },	{ "FRAME", FRAME },
	{ "FRAMESET", FRAMESET },	{ "H1", H1 },
	{ "H2", H2 },		{ "H3", H3 },
	{ "H4", H4 },		{ "H5", H5 },
	{ "H6", H6 },		{ "HEAD", HEAD },
	{ "HR", HR },		{ "IFRAME", IFRAME },
	{ "IMAGE", IMAGE },	{ "IMG", IMG },
	{ "INPUT", INPUT },	{ "ISINDEX", ISINDEX },
	{ "LI", LI },		{ "LINK", LINK },
	{ "LISTING", LISTING },	{ "MENU", MENU },
	{ "META", META },	{ "NOEMBED", NOEMBED },
	{ "NOFRAMES", NOFRAMES },	{ "NOSCRIPT", NOSCRIPT },
	{ "OL", OL },		{ "OPTGROUP", OPTGROUP },
	{ "OPTION", OPTION },	{ "P", P },
	{ "PARAM", PARAM },	{ "PLAINTEXT", PLAINTEXT },
	{ "PRE", PRE },		{ "SCRIPT", SCRIPT },
	{ "SELECT", SELECT },	{ "SPACER", SPACER },
	{ "STYLE", STYLE }, 	{ "TBODY", TBODY },
	{ "TEXTAREA", TEXTAREA },	{ "TFOOT", TFOOT },
	{ "THEAD", THEAD },	{ "TITLE", TITLE },
	{ "TR", TR },		{ "UL", UL },
	{ "WBR", WBR },
	{ "APPLET", APPLET },	{ "BUTTON", BUTTON },
	{ "CAPTION", CAPTION },	{ "HTML", HTML },
	{ "MARQUEE", MARQUEE },	{ "OBJECT", OBJECT },
	{ "TABLE", TABLE },	{ "TD", TD },
	{ "TH", TH },
	{ "A", A },		{ "B", B },
	{ "BIG", BIG },		{ "EM", EM },
	{ "FONT", FONT },	{ "I", I },
	{ "NOBR", NOBR },	{ "S", S },
	{ "SMALL", SMALL },	{ "STRIKE", STRIKE },
	{ "STRONG", STRONG },	{ "TT", TT },
	{ "U", U },
};


static void hubbub_treebuilder_buffer_handler(const uint8_t *data,
		size_t len, void *pw);


/**
 * Create a hubbub treebuilder
 *
 * \param tokeniser  Underlying tokeniser instance
 * \param alloc      Memory (de)allocation function
 * \param pw         Pointer to client-specific private data
 * \return Pointer to treebuilder instance, or NULL on error.
 */
hubbub_treebuilder *hubbub_treebuilder_create(hubbub_tokeniser *tokeniser,
		hubbub_alloc alloc, void *pw)
{
	hubbub_treebuilder *tb;
	hubbub_tokeniser_optparams tokparams;

	if (tokeniser == NULL || alloc == NULL)
		return NULL;

	tb = alloc(NULL, sizeof(hubbub_treebuilder), pw);
	if (tb == NULL)
		return NULL;

	tb->tokeniser = tokeniser;

	tb->input_buffer = NULL;
	tb->input_buffer_len = 0;

	tb->tree_handler = NULL;

	memset(&tb->context, 0, sizeof(hubbub_treebuilder_context));
	tb->context.mode = INITIAL;

	tb->context.element_stack = alloc(NULL,
			ELEMENT_STACK_CHUNK * sizeof(element_context),
			pw);
	if (tb->context.element_stack == NULL) {
		alloc(tb, 0, pw);
		return NULL;
	}
	tb->context.stack_alloc = ELEMENT_STACK_CHUNK;
	/* We rely on HTML not being equal to zero to determine
	 * if the first item in the stack is in use. Assert this here. */
	assert(HTML != 0);
	tb->context.element_stack[0].type = 0;

	tb->context.collect.string.type = HUBBUB_STRING_OFF;

	tb->context.strip_leading_lr = false;

	tb->buffer_handler = NULL;
	tb->buffer_pw = NULL;

	tb->error_handler = NULL;
	tb->error_pw = NULL;

	tb->alloc = alloc;
	tb->alloc_pw = pw;

	tokparams.token_handler.handler = hubbub_treebuilder_token_handler;
	tokparams.token_handler.pw = tb;

	if (hubbub_tokeniser_setopt(tokeniser, HUBBUB_TOKENISER_TOKEN_HANDLER,
			&tokparams) != HUBBUB_OK) {
		alloc(tb->context.element_stack, 0, pw);
		alloc(tb, 0, pw);
		return NULL;
	}

	tokparams.buffer_handler.handler = hubbub_treebuilder_buffer_handler;
	tokparams.buffer_handler.pw = tb;

	if (hubbub_tokeniser_setopt(tokeniser, HUBBUB_TOKENISER_BUFFER_HANDLER,
			&tokparams) != HUBBUB_OK) {
		alloc(tb->context.element_stack, 0, pw);
		alloc(tb, 0, pw);
		return NULL;
	}

	return tb;	
}

/**
 * Destroy a hubbub treebuilder
 *
 * \param treebuilder  The treebuilder instance to destroy
 */
void hubbub_treebuilder_destroy(hubbub_treebuilder *treebuilder)
{
	formatting_list_entry *entry, *next;
	hubbub_tokeniser_optparams tokparams;

	if (treebuilder == NULL)
		return;

	tokparams.buffer_handler.handler = treebuilder->buffer_handler;
	tokparams.buffer_handler.pw = treebuilder->buffer_pw;

	hubbub_tokeniser_setopt(treebuilder->tokeniser,
			HUBBUB_TOKENISER_BUFFER_HANDLER, &tokparams);

	tokparams.token_handler.handler = NULL;
	tokparams.token_handler.pw = NULL;

	hubbub_tokeniser_setopt(treebuilder->tokeniser,
			HUBBUB_TOKENISER_TOKEN_HANDLER, &tokparams);

	/* Clean up context */
	if (treebuilder->tree_handler != NULL) {
		if (treebuilder->context.head_element != NULL) {
			treebuilder->tree_handler->unref_node(
					treebuilder->tree_handler->ctx,
					treebuilder->context.head_element);
		}

		if (treebuilder->context.form_element != NULL) {
			treebuilder->tree_handler->unref_node(
					treebuilder->tree_handler->ctx,
					treebuilder->context.form_element);
		}

		if (treebuilder->context.document != NULL) {
			treebuilder->tree_handler->unref_node(
					treebuilder->tree_handler->ctx,
					treebuilder->context.document);
		}

		for (uint32_t n = treebuilder->context.current_node; 
				n > 0; n--) {
			treebuilder->tree_handler->unref_node(
				treebuilder->tree_handler->ctx,
				treebuilder->context.element_stack[n].node);
		}
		if (treebuilder->context.element_stack[0].type == HTML) {
			treebuilder->tree_handler->unref_node(
				treebuilder->tree_handler->ctx,
				treebuilder->context.element_stack[0].node);
		}
	}
	treebuilder->alloc(treebuilder->context.element_stack, 0, 
			treebuilder->alloc_pw);
	treebuilder->context.element_stack = NULL;

	for (entry = treebuilder->context.formatting_list; entry != NULL;
			entry = next) {
		next = entry->next;

		if (treebuilder->tree_handler != NULL) {
			treebuilder->tree_handler->unref_node(
					treebuilder->tree_handler->ctx,
					entry->details.node);
		}

		treebuilder->alloc(entry, 0, treebuilder->alloc_pw);
	}

	treebuilder->alloc(treebuilder, 0, treebuilder->alloc_pw);
}

/**
 * Configure a hubbub treebuilder
 *
 * \param treebuilder  The treebuilder instance to configure
 * \param type         The option type to configure
 * \param params       Pointer to option-specific parameters
 * \return HUBBUB_OK on success, appropriate error otherwise.
 */
hubbub_error hubbub_treebuilder_setopt(hubbub_treebuilder *treebuilder,
		hubbub_treebuilder_opttype type,
		hubbub_treebuilder_optparams *params)
{
	if (treebuilder == NULL || params == NULL)
		return HUBBUB_BADPARM;

	switch (type) {
	case HUBBUB_TREEBUILDER_BUFFER_HANDLER:
		treebuilder->buffer_handler = params->buffer_handler.handler;
		treebuilder->buffer_pw = params->buffer_handler.pw;
		treebuilder->buffer_handler(treebuilder->input_buffer,
				treebuilder->input_buffer_len,
				treebuilder->buffer_pw);
		break;
	case HUBBUB_TREEBUILDER_ERROR_HANDLER:
		treebuilder->error_handler = params->error_handler.handler;
		treebuilder->error_pw = params->error_handler.pw;
		break;
	case HUBBUB_TREEBUILDER_TREE_HANDLER:
		treebuilder->tree_handler = params->tree_handler;
		break;
	case HUBBUB_TREEBUILDER_DOCUMENT_NODE:
		treebuilder->context.document = params->document_node;
		break;
	}

	return HUBBUB_OK;
}

/**
 * Handle tokeniser buffer moving
 *
 * \param data  New location of buffer
 * \param len   Length of buffer in bytes
 * \param pw    Pointer to treebuilder instance
 */
void hubbub_treebuilder_buffer_handler(const uint8_t *data,
		size_t len, void *pw)
{
	hubbub_treebuilder *treebuilder = (hubbub_treebuilder *) pw;

	treebuilder->input_buffer = data;
	treebuilder->input_buffer_len = len;

	/* Inform client buffer handler, too (if there is one) */
	if (treebuilder->buffer_handler != NULL) {
		treebuilder->buffer_handler(treebuilder->input_buffer,
				treebuilder->input_buffer_len,
				treebuilder->buffer_pw);
	}
}

/**
 * Handle tokeniser emitting a token
 *
 * \param token  The emitted token
 * \param pw     Pointer to treebuilder instance
 */
void hubbub_treebuilder_token_handler(const hubbub_token *token, 
		void *pw)
{
	hubbub_treebuilder *treebuilder = (hubbub_treebuilder *) pw;
	bool reprocess = true;

	/* Do nothing if we have no document node or there's no tree handler */
	if (treebuilder->context.document == NULL ||
			treebuilder->tree_handler == NULL)
		return;

	assert((signed) treebuilder->context.current_node >= 0);

#ifdef NDEBUG
# define hack(x) \
		case x:
#else
# define hack(x) \
		case x: \
			printf( #x "\n");
#endif

	while (reprocess) {
		switch (treebuilder->context.mode) {
		hack(INITIAL)
			reprocess = handle_initial(treebuilder, token);
			break;
		hack(BEFORE_HTML)
			reprocess = handle_before_html(treebuilder, token);
			break;
		hack(BEFORE_HEAD)
			reprocess = handle_before_head(treebuilder, token);
			break;
		hack(IN_HEAD)
			reprocess = handle_in_head(treebuilder, token);
			break;
		hack(IN_HEAD_NOSCRIPT)
			reprocess = handle_in_head_noscript(treebuilder, token);
			break;
		hack(AFTER_HEAD)
			reprocess = handle_after_head(treebuilder, token);
			break;
		hack(IN_BODY)
			reprocess = handle_in_body(treebuilder, token);
			break;
		hack(IN_TABLE)
			reprocess = handle_in_table(treebuilder, token);
			break;
		hack(IN_CAPTION)
			reprocess = handle_in_caption(treebuilder, token);
			break;
		hack(IN_COLUMN_GROUP)
			reprocess = handle_in_column_group(treebuilder, token);
			break;
		hack(IN_TABLE_BODY)
			reprocess = handle_in_table_body(treebuilder, token);
			break;
		hack(IN_ROW)
			reprocess = handle_in_row(treebuilder, token);
			break;
		hack(IN_CELL)
			reprocess = handle_in_cell(treebuilder, token);
			break;
		hack(IN_SELECT)
			reprocess = handle_in_select(treebuilder, token);
			break;
		hack(IN_SELECT_IN_TABLE)
			reprocess = handle_in_select_in_table(treebuilder, token);
			break;
		hack(IN_FOREIGN_CONTENT)
			reprocess = handle_in_foreign_content(treebuilder, token);
			break;
		hack(AFTER_BODY)
			reprocess = handle_after_body(treebuilder, token);
			break;
		hack(IN_FRAMESET)
			reprocess = handle_in_frameset(treebuilder, token);
			break;
		hack(AFTER_FRAMESET)
			reprocess = handle_after_frameset(treebuilder, token);
			break;
		hack(AFTER_AFTER_BODY)
			reprocess = handle_after_after_body(treebuilder, token);
			break;
		hack(AFTER_AFTER_FRAMESET)
			reprocess = handle_after_after_frameset(treebuilder, token);
			break;
		hack(GENERIC_RCDATA)
			reprocess = handle_generic_rcdata(treebuilder, token);
			break;
		hack(SCRIPT_COLLECT_CHARACTERS)
			reprocess = handle_script_collect_characters(
					treebuilder, token);
			break;
		}
	}
}


/**
 * Process a character token in cases where we expect only whitespace
 *
 * \param treebuilder               The treebuilder instance
 * \param token                     The character token
 * \param insert_into_current_node  Whether to insert whitespace into 
 *                                  current node
 * \return True if the token needs reprocessing 
 *              (token data updated to skip any leading whitespace), 
 *         false if it contained only whitespace
 */
bool process_characters_expect_whitespace(hubbub_treebuilder *treebuilder,
		const hubbub_token *token, bool insert_into_current_node)
{
	const uint8_t *data = treebuilder->input_buffer +
			token->data.character.data.off;
	size_t len = token->data.character.len;
	size_t c;

	/** \todo UTF-16 */

	for (c = 0; c < len; c++) {
		if (data[c] != 0x09 && data[c] != 0x0A &&
				data[c] != 0x0C && data[c] != 0x20)
			break;
	}
	/* Non-whitespace characters in token, so reprocess */
	if (c != len) {
		if (c > 0 && insert_into_current_node) {
			hubbub_string temp;

			temp.type = HUBBUB_STRING_OFF;
			temp.data.off = token->data.character.data.off;
			temp.len = len - c;

			append_text(treebuilder, &temp);
		}

		/* Update token data to strip leading whitespace */
		((hubbub_token *) token)->data.character.data.off += c;
		((hubbub_token *) token)->data.character.len -= c;

		return true;
	}

	return false;
}

/**
 * Process a comment token, appending it to the given parent
 *
 * \param treebuilder  The treebuilder instance
 * \param token        The comment token
 * \param parent       The node to append to
 */
void process_comment_append(hubbub_treebuilder *treebuilder,
		const hubbub_token *token, void *parent)
{
	int success;
	void *comment, *appended;

	success = treebuilder->tree_handler->create_comment(
			treebuilder->tree_handler->ctx,
			&token->data.comment, &comment);
	if (success != 0) {
		/** \todo errors */
	}

	/* Append to Document node */
	success = treebuilder->tree_handler->append_child(
			treebuilder->tree_handler->ctx,
			parent, comment, &appended);
	if (success != 0) {
		/** \todo errors */
		treebuilder->tree_handler->unref_node(
				treebuilder->tree_handler->ctx,
				comment);
	}

	treebuilder->tree_handler->unref_node(
			treebuilder->tree_handler->ctx, appended);
	treebuilder->tree_handler->unref_node(
			treebuilder->tree_handler->ctx, comment);
}

/**
 * Trigger parsing of generic (R)CDATA
 *
 * \param treebuilder  The treebuilder instance
 * \param token        The current token
 * \param rcdata       True for RCDATA, false for CDATA
 */
void parse_generic_rcdata(hubbub_treebuilder *treebuilder,
		const hubbub_token *token, bool rcdata)
{
	int success;
	void *node, *appended;
	element_type type;
	hubbub_tokeniser_optparams params;

	type = element_type_from_name(treebuilder, &token->data.tag.name);

	success = treebuilder->tree_handler->create_element(
			treebuilder->tree_handler->ctx,
			&token->data.tag, &node);
	if (success != 0) {
		/** \todo errors */
	}

	/* It's a bit nasty having this code deal with textarea->form
	 * association, but it avoids having to duplicate the entire rest
	 * of this function for textarea processing */
	if (type == TEXTAREA && treebuilder->context.form_element != NULL) {
		/** \todo associate textarea with form */
	}

	success = treebuilder->tree_handler->append_child(
			treebuilder->tree_handler->ctx,
			treebuilder->context.element_stack[
			treebuilder->context.current_node].node,
			node, &appended);
	if (success != 0) {
		/** \todo errors */
		treebuilder->tree_handler->unref_node(
				treebuilder->tree_handler->ctx,
				node);
	}

	params.content_model.model = rcdata ? HUBBUB_CONTENT_MODEL_RCDATA 
					    : HUBBUB_CONTENT_MODEL_CDATA;
	hubbub_tokeniser_setopt(treebuilder->tokeniser,
				HUBBUB_TOKENISER_CONTENT_MODEL, &params);

	treebuilder->context.collect.mode = treebuilder->context.mode;
	treebuilder->context.collect.type = type;
	treebuilder->context.collect.node = node;
	treebuilder->context.collect.string.data.off = 0;
	treebuilder->context.collect.string.len = 0;

	treebuilder->tree_handler->unref_node(
			treebuilder->tree_handler->ctx,
			appended);

	treebuilder->context.mode = GENERIC_RCDATA;
}

/**
 * Determine if an element is in (table) scope
 *
 * \param treebuilder  Treebuilder to look in
 * \param type         Element type to find
 * \param in_table     Whether we're looking in table scope
 * \return Element stack index, or 0 if not in scope
 */
uint32_t element_in_scope(hubbub_treebuilder *treebuilder,
		element_type type, bool in_table)
{
	uint32_t node;

	if (treebuilder->context.element_stack == NULL)
		return false;

	assert((signed) treebuilder->context.current_node >= 0);

	for (node = treebuilder->context.current_node; node > 0; node--) {
		element_type node_type =
				treebuilder->context.element_stack[node].type;

		if (node_type == type)
			return node;

		if (node_type == TABLE)
			break;

		/* The list of element types given in the spec here are the
		 * scoping elements excluding TABLE and HTML. TABLE is handled
		 * in the previous conditional and HTML should only occur
		 * as the first node in the stack, which is never processed
		 * in this loop. */
		if (!in_table && is_scoping_element(node_type))
			break;
	}

	return 0;
}

/**
 * Reconstruct the list of active formatting elements
 *
 * \param treebuilder  Treebuilder instance containing list
 */
void reconstruct_active_formatting_list(hubbub_treebuilder *treebuilder)
{
	formatting_list_entry *entry;

	if (treebuilder->context.formatting_list == NULL)
		return;

	entry = treebuilder->context.formatting_list_end;

	/* Assumption: HTML and TABLE elements are not inserted into the list */
	if (is_scoping_element(entry->details.type) || entry->stack_index != 0)
		return;

	while (entry->prev != NULL) {
		entry = entry->prev;

		if (is_scoping_element(entry->details.type) ||
				entry->stack_index != 0) {
			entry = entry->next;
			break;
		}
	}

	while (entry != NULL) {
		int success;
		void *clone, *appended;
		element_type prev_type;
		void *prev_node;
		uint32_t prev_stack_index;

		success = treebuilder->tree_handler->clone_node(
				treebuilder->tree_handler->ctx,
				entry->details.node,
				false,
				&clone);
		if (success != 0) {
			/** \todo handle errors */
			return;
		}

		success = treebuilder->tree_handler->append_child(
				treebuilder->tree_handler->ctx,
				treebuilder->context.element_stack[
					treebuilder->context.current_node].node,
				clone,
				&appended);
		if (success != 0) {
			/** \todo handle errors */
			treebuilder->tree_handler->unref_node(
					treebuilder->tree_handler->ctx,
					clone);
			return;
		}

		if (!element_stack_push(treebuilder,
				entry->details.ns, entry->details.type,
				appended)) {
			/** \todo handle memory exhaustion */
			treebuilder->tree_handler->unref_node(
					treebuilder->tree_handler->ctx,
					appended);
			treebuilder->tree_handler->unref_node(
					treebuilder->tree_handler->ctx,
					clone);
		}

		if (!formatting_list_replace(treebuilder, entry,
				entry->details.type, clone,
				treebuilder->context.current_node,
				&prev_type, &prev_node,
				&prev_stack_index)) {
			/** \todo handle errors */
			treebuilder->tree_handler->unref_node(
					treebuilder->tree_handler->ctx,
					clone);
		}

		treebuilder->tree_handler->unref_node(
				treebuilder->tree_handler->ctx,
				prev_node);

		entry = entry->next;
	}
}

/**
 * Clear the list of active formatting elements up to the last marker
 *
 * \param treebuilder  The treebuilder instance containing the list
 */
void clear_active_formatting_list_to_marker(hubbub_treebuilder *treebuilder)
{
	formatting_list_entry *entry;
	bool done = false;

	while ((entry = treebuilder->context.formatting_list_end) != NULL) {
		element_type type;
		void *node;
		uint32_t stack_index;

		if (is_scoping_element(entry->details.type))
			done = true;

		if (!formatting_list_remove(treebuilder, entry, 
				&type, &node, &stack_index)) {
			/** \todo handle errors */
		}

		treebuilder->tree_handler->unref_node(
				treebuilder->tree_handler->ctx,
				node);

		if (done)
			break;
	}
}

/**
 * Create element and insert it into the DOM, pushing it on the stack
 *
 * \param treebuilder  The treebuilder instance
 * \param tag          The element to insert
 */
void insert_element(hubbub_treebuilder *treebuilder, const hubbub_tag *tag)
{
	int success;
	void *node, *appended;

	/** \todo handle treebuilder->context.in_table_foster */

	success = treebuilder->tree_handler->create_element(
			treebuilder->tree_handler->ctx, tag, &node);
	if (success != 0) {
		/** \todo errors */
	}

	success = treebuilder->tree_handler->append_child(
			treebuilder->tree_handler->ctx,
			treebuilder->context.element_stack[
				treebuilder->context.current_node].node,
			node, &appended);
	if (success != 0) {
		/** \todo errors */
	}

	treebuilder->tree_handler->unref_node(treebuilder->tree_handler->ctx,
			appended);

	if (!element_stack_push(treebuilder,
			tag->ns,
			element_type_from_name(treebuilder, &tag->name),
			node)) {
		/** \todo errors */
	}
}

/**
 * Create element and insert it into the DOM, do not push it onto the stack
 *
 * \param treebuilder  The treebuilder instance
 * \param tag          The element to insert
 */
void insert_element_no_push(hubbub_treebuilder *treebuilder, 
		const hubbub_tag *tag)
{
	int success;
	void *node, *appended;

	/** \todo handle treebuilder->context.in_table_foster */

	success = treebuilder->tree_handler->create_element(
			treebuilder->tree_handler->ctx, tag, &node);
	if (success != 0) {
		/** \todo errors */
	}

	success = treebuilder->tree_handler->append_child(
			treebuilder->tree_handler->ctx,
			treebuilder->context.element_stack[
				treebuilder->context.current_node].node,
			node, &appended);
	if (success != 0) {
		/** \todo errors */
	}

	treebuilder->tree_handler->unref_node(treebuilder->tree_handler->ctx,
			appended);
	treebuilder->tree_handler->unref_node(treebuilder->tree_handler->ctx,
			node);
}

/**
 * Close implied end tags
 *
 * \param treebuilder  The treebuilder instance
 * \param except       Tag type to exclude from processing [DD,DT,LI,OPTION,
 *                     OPTGROUP,P,RP,RT], UNKNOWN to exclude nothing
 */
void close_implied_end_tags(hubbub_treebuilder *treebuilder,
		element_type except)
{
	element_type type;

	type = treebuilder->context.element_stack[
			treebuilder->context.current_node].type;

	while (type == DD || type == DT || type == LI || type == OPTION ||
			type == OPTGROUP || type == P || type == RP ||
			type == RT) {
		hubbub_ns ns;
		element_type otype;
		void *node;

		if (except != UNKNOWN && type == except)
			break;

		if (!element_stack_pop(treebuilder, &ns, &otype, &node)) {
			/** \todo errors */
		}

		treebuilder->tree_handler->unref_node(
				treebuilder->tree_handler->ctx,
				node);

		type = treebuilder->context.element_stack[
				treebuilder->context.current_node].type;
	}
}

/**
 * Reset the insertion mode
 *
 * \param treebuilder  The treebuilder to reset
 */
void reset_insertion_mode(hubbub_treebuilder *treebuilder)
{
	uint32_t node;
	element_context *stack = treebuilder->context.element_stack;

	/** \todo fragment parsing algorithm */

	for (node = treebuilder->context.current_node; node > 0; node--) {
		switch (stack[node].type) {
		case SELECT:
			/* fragment case */
			break;
		case TD:
		case TH:
			treebuilder->context.mode = IN_CELL;
			return;
		case TR:
			treebuilder->context.mode = IN_ROW;
			return;
		case TBODY:
		case TFOOT:
		case THEAD:
			treebuilder->context.mode = IN_TABLE_BODY;
			return;
		case CAPTION:
			treebuilder->context.mode = IN_CAPTION;
			return;
		case COLGROUP:
			/* fragment case */
			break;
		case TABLE:
			treebuilder->context.mode = IN_TABLE;
			return;
		case HEAD:
			/* fragment case */
			break;
		case BODY:
			treebuilder->context.mode = IN_BODY;
			return;
		case FRAMESET:
			/* fragment case */
			break;
		case HTML:
			/* fragment case */
			break;
		default:
			break;
		}
	}
}

/**
 * Append text to the current node, inserting into the last child of the 
 * current node, iff it's a Text node.
 *
 * \param treebuilder  The treebuilder instance
 * \param string       The string to append
 */
void append_text(hubbub_treebuilder *treebuilder,
		const hubbub_string *string)
{
	int success;
	void *text, *appended;

	/** \todo Append to pre-existing text child, iff
	 * one exists and it's the last in the child list */

	success = treebuilder->tree_handler->create_text(
			treebuilder->tree_handler->ctx, string, &text);
	if (success != 0) {
		/** \todo errors */
	}

	success = treebuilder->tree_handler->append_child(
			treebuilder->tree_handler->ctx,
			treebuilder->context.element_stack[
				treebuilder->context.current_node].node,
					text, &appended);
	if (success != 0) {
		/** \todo errors */
		treebuilder->tree_handler->unref_node(
				treebuilder->tree_handler->ctx,
				text);
	}

	treebuilder->tree_handler->unref_node(
			treebuilder->tree_handler->ctx, appended);
	treebuilder->tree_handler->unref_node(
			treebuilder->tree_handler->ctx, text);
}

/**
 * Convert an element name into an element type
 *
 * \param treebuilder  The treebuilder instance
 * \param tag_name     The tag name to consider
 * \return The corresponding element type
 */
element_type element_type_from_name(hubbub_treebuilder *treebuilder,
		const hubbub_string *tag_name)
{
	const uint8_t *name = NULL;
	size_t len = tag_name->len;

	switch (tag_name->type) {
	case HUBBUB_STRING_OFF:
		name = treebuilder->input_buffer + tag_name->data.off;
		break;
	case HUBBUB_STRING_PTR:
		name = tag_name->data.ptr;
		break;
	}


	/** \todo UTF-16 support */
	/** \todo optimise this */

	for (uint32_t i = 0; 
			i < sizeof(name_type_map) / sizeof(name_type_map[0]);
			i++) {
		if (strlen(name_type_map[i].name) != len)
			continue;

		if (strncasecmp(name_type_map[i].name, 
				(const char *) name, len) == 0)
			return name_type_map[i].type;
	}

	return UNKNOWN;
}

/**
 * Determine if a node is a special element
 *
 * \param type  Node type to consider
 * \return True iff node is a special element
 */
bool is_special_element(element_type type)
{
	return (type <= WBR);
}

/**
 * Determine if a node is a scoping element
 *
 * \param type  Node type to consider
 * \return True iff node is a scoping element
 */
bool is_scoping_element(element_type type)
{
	return (type >= APPLET && type <= TH);
}

/**
 * Determine if a node is a formatting element
 *
 * \param type  Node type to consider
 * \return True iff node is a formatting element
 */
bool is_formatting_element(element_type type)
{
	return (type >= A && type <= U);
}

/**
 * Determine if a node is a phrasing element
 *
 * \param type  Node type to consider
 * \return True iff node is a phrasing element
 */
bool is_phrasing_element(element_type type)
{
	return (type > U);
}

/**
 * Push an element onto the stack of open elements
 *
 * \param treebuilder  The treebuilder instance containing the stack
 * \param ns           The namespace of element being pushed
 * \param type         The type of element being pushed
 * \param node         The node to push
 * \return True on success, false on memory exhaustion
 */
bool element_stack_push(hubbub_treebuilder *treebuilder,
		hubbub_ns ns, element_type type, void *node)
{
	uint32_t slot = treebuilder->context.current_node + 1;

	if (slot >= treebuilder->context.stack_alloc) {
		element_context *temp = treebuilder->alloc(
				treebuilder->context.element_stack, 
				(treebuilder->context.stack_alloc + 
					ELEMENT_STACK_CHUNK) * 
					sizeof(element_context), 
				treebuilder->alloc_pw);

		if (temp == NULL)
			return false;

		treebuilder->context.element_stack = temp;
		treebuilder->context.stack_alloc += ELEMENT_STACK_CHUNK;
	}

	treebuilder->context.element_stack[slot].ns = ns;
	treebuilder->context.element_stack[slot].type = type;
	treebuilder->context.element_stack[slot].node = node;

	treebuilder->context.current_node = slot;

	/* Update current table index */
	if (type == TABLE)
		treebuilder->context.current_table = slot;

	return true;
}

/**
 * Pop an element off the stack of open elements
 *
 * \param treebuilder  The treebuilder instance containing the stack
 * \param type         Pointer to location to receive element type
 * \param node         Pointer to location to receive node
 * \return True on success, false on memory exhaustion.
 */
bool element_stack_pop(hubbub_treebuilder *treebuilder,
		hubbub_ns *ns, element_type *type, void **node)
{
	element_context *stack = treebuilder->context.element_stack;
	uint32_t slot = treebuilder->context.current_node;
	formatting_list_entry *entry;

	/* We're popping a table, find previous */
	if (stack[slot].type == TABLE) {
		uint32_t t;
		for (t = slot - 1; t > 0; t--) {
			if (stack[t].type == TABLE)
				break;
		}

		treebuilder->context.current_table = t;
	}

	if (is_formatting_element(stack[slot].type) || 
			(is_scoping_element(stack[slot].type) && 
			stack[slot].type != HTML && 
			stack[slot].type != TABLE)) {
		/* Find occurrences of the node we're about to pop in the list 
		 * of active formatting elements. We need to invalidate their 
		 * stack index information. */
		for (entry = treebuilder->context.formatting_list_end; 
				entry != NULL; entry = entry->prev) {
			/** \todo Can we optimise this? 
			 * (i.e. by not traversing the entire list) */
			if (entry->stack_index == slot)
				entry->stack_index = 0;
		}
	}

	*ns = stack[slot].ns;
	*type = stack[slot].type;
	*node = stack[slot].node;

	/** \todo reduce allocated stack size once there's enough free */

	treebuilder->context.current_node = slot - 1;
	assert((signed) treebuilder->context.current_node >= 0);

	return true;
}

/**
 * Pop elements until an element of type "element" has been popped.
 *
 * \return True on success, false on memory exhaustion.
 */
bool element_stack_pop_until(hubbub_treebuilder *treebuilder,
		element_type type)
{
	element_type otype = UNKNOWN;
	void *node;
	hubbub_ns ns;

	while (otype != type) {
		if (!element_stack_pop(treebuilder, &ns, &otype, &node)) {
			/** \todo error -- never happens */
			return false;
		}

		treebuilder->tree_handler->unref_node(
				treebuilder->tree_handler->ctx, node);

		assert((signed) treebuilder->context.current_node >= 0);
	}

	return true;
}

/**
 * Peek at the top element of the element stack.
 *
 * \param treebuilder  Treebuilder instance
 * \return Element type on the top of the stack
 */
element_type current_node(hubbub_treebuilder *treebuilder)
{
	return treebuilder->context.element_stack
			[treebuilder->context.current_node].type;
}

/**
 * Peek at the top element of the element stack.
 *
 * \param treebuilder  Treebuilder instance
 * \return Element type on the top of the stack
 */
hubbub_ns current_node_ns(hubbub_treebuilder *treebuilder)
{
	return treebuilder->context.element_stack
			[treebuilder->context.current_node].ns;
}

/**
 * Peek at the element below the top of the element stack.
 *
 * \param treebuilder  Treebuilder instance
 * \return Element type of the element one below the top of the stack
 */
element_type prev_node(hubbub_treebuilder *treebuilder)
{
	if (treebuilder->context.current_node == 0)
		return UNKNOWN;

	return treebuilder->context.element_stack
			[treebuilder->context.current_node - 1].type;
}



/**
 * Append an element to the end of the list of active formatting elements
 *
 * \param treebuilder  Treebuilder instance containing list
 * \param type         Type of node being inserted
 * \param node         Node being inserted
 * \param stack_index  Index into stack of open elements
 * \return True on success, false on memory exhaustion
 */
bool formatting_list_append(hubbub_treebuilder *treebuilder,
		element_type type, void *node, uint32_t stack_index)
{
	formatting_list_entry *entry;

	entry = treebuilder->alloc(NULL, sizeof(formatting_list_entry),
			treebuilder->alloc_pw);
	if (entry == NULL)
		return false;

	entry->details.type = type;
	entry->details.node = node;
	entry->stack_index = stack_index;

	entry->prev = treebuilder->context.formatting_list_end;
	entry->next = NULL;

	if (entry->prev != NULL)
		entry->prev->next = entry;
	else
		treebuilder->context.formatting_list = entry;

	treebuilder->context.formatting_list_end = entry;

	return true;
}

/**
 * Insert an element into the list of active formatting elements
 *
 * \param treebuilder  Treebuilder instance containing list
 * \param prev         Previous entry
 * \param next         Next entry
 * \param type         Type of node being inserted
 * \param node         Node being inserted
 * \param stack_index  Index into stack of open elements
 * \return True on success, false on memory exhaustion
 */
bool formatting_list_insert(hubbub_treebuilder *treebuilder,
		formatting_list_entry *prev, formatting_list_entry *next,
		element_type type, void *node, uint32_t stack_index)
{
	formatting_list_entry *entry;

	if (prev != NULL) {
		assert(prev->next == next);
	}

	if (next != NULL) {
		assert(next->prev == prev);
	}

	entry = treebuilder->alloc(NULL, sizeof(formatting_list_entry),
			treebuilder->alloc_pw);
	if (entry == NULL)
		return false;

	entry->details.type = type;
	entry->details.node = node;
	entry->stack_index = stack_index;

	entry->prev = prev;
	entry->next = next;

	if (entry->prev != NULL)
		entry->prev->next = entry;
	else
		treebuilder->context.formatting_list = entry;

	if (entry->next != NULL)
		entry->next->prev = entry;
	else
		treebuilder->context.formatting_list_end = entry;

	return true;
}


/**
 * Remove an element from the list of active formatting elements
 *
 * \param treebuilder  Treebuilder instance containing list
 * \param entry        The item to remove
 * \param type         Pointer to location to receive type of node
 * \param node         Pointer to location to receive node
 * \param stack_index  Pointer to location to receive stack index
 * \return True on success, false on memory exhaustion
 */
bool formatting_list_remove(hubbub_treebuilder *treebuilder,
		formatting_list_entry *entry,
		element_type *type, void **node, uint32_t *stack_index)
{
	*type = entry->details.type;
	*node = entry->details.node;
	*stack_index = entry->stack_index;

	if (entry->prev == NULL)
		treebuilder->context.formatting_list = entry->next;
	else
		entry->prev->next = entry->next;

	if (entry->next == NULL)
		treebuilder->context.formatting_list_end = entry->prev;
	else
		entry->next->prev = entry->prev;

	treebuilder->alloc(entry, 0, treebuilder->alloc_pw);

	return true;
}

/**
 * Remove an element from the list of active formatting elements
 *
 * \param treebuilder   Treebuilder instance containing list
 * \param entry         The item to replace
 * \param type          Replacement node type
 * \param node          Replacement node
 * \param stack_index   Replacement stack index
 * \param otype         Pointer to location to receive old type
 * \param onode         Pointer to location to receive old node
 * \param ostack_index  Pointer to location to receive old stack index
 * \return True on success, false on memory exhaustion
 */
bool formatting_list_replace(hubbub_treebuilder *treebuilder,
		formatting_list_entry *entry,
		element_type type, void *node, uint32_t stack_index,
		element_type *otype, void **onode, uint32_t *ostack_index)
{
	UNUSED(treebuilder);

	*otype = entry->details.type;
	*onode = entry->details.node;
	*ostack_index = entry->stack_index;

	entry->details.type = type;
	entry->details.node = node;
	entry->stack_index = stack_index;

	return true;
}

/**
 * Adjust foreign attributes.
 *
 * \param treebuilder	Treebuilder instance
 * \param tag		Tag to adjust the attributes of
 */
void adjust_foreign_attributes(hubbub_treebuilder *treebuilder,
		hubbub_tag *tag)
{
	for (size_t i = 0; i < tag->n_attributes; i++) {
		hubbub_attribute *attr = &tag->attributes[i];
		const uint8_t *name = treebuilder->input_buffer +
				attr->name.data.off;

#define S(s)	(uint8_t *) s, SLEN(s)

		/* 10 == strlen("xlink:href") */
		if (attr->name.len >= 10 &&
				strncmp((char *) name, "xlink:", 
						SLEN("xlink:")) == 0) {
			size_t len = attr->name.len - 6;
			name += 6;

			if (hubbub_string_match(name, len, S("actutate")) ||
					hubbub_string_match(name, len,
							S("arcrole")) ||
					hubbub_string_match(name, len,
							S("href")) ||
					hubbub_string_match(name, len,
							S("role")) ||
					hubbub_string_match(name, len,
							S("show")) ||
					hubbub_string_match(name, len,
							S("title")) ||
					hubbub_string_match(name, len,
							S("type"))) {
				attr->ns = HUBBUB_NS_XLINK;
				attr->name.data.off += 6;
				attr->name.len -= 6;
			}
		/* 8 == strlen("xml:base") */
		} else if (attr->name.len >= 8 &&
				strncmp((char *) name, "xml:", SLEN("xml:")) == 0) {
			size_t len = attr->name.len - 4;
			name += 4;

			if (hubbub_string_match(name, len, S("base")) ||
					hubbub_string_match(name, len,
							S("lang")) ||
					hubbub_string_match(name, len,
							S("space"))) {
				attr->ns = HUBBUB_NS_XML;
				attr->name.data.off += 4;
				attr->name.len -= 4;
			}
		} else if (hubbub_string_match(name, attr->name.len,
						S("xmlns")) ||
				hubbub_string_match(name, attr->name.len,
						S("xmlns:xlink"))) {
			attr->ns = HUBBUB_NS_XMLNS;
			attr->name.data.off += 6;
			attr->name.len -= 6;
		}

#undef S
	}
}


#ifndef NDEBUG

/**
 * Dump an element stack to the given file pointer
 *
 * \param treebuilder  The treebuilder instance
 * \param fp           The file to dump to
 */
void element_stack_dump(hubbub_treebuilder *treebuilder, FILE *fp)
{
	element_context *stack = treebuilder->context.element_stack;
	uint32_t i;

	for (i = 0; i <= treebuilder->context.current_node; i++) {
		fprintf(fp, "%u: %s %p\n", 
				i,
				element_type_to_name(stack[i].type),
				stack[i].node);
	}
}

/**
 * Dump a formatting list to the given file pointer
 *
 * \param treebuilder  The treebuilder instance
 * \param fp           The file to dump to
 */
void formatting_list_dump(hubbub_treebuilder *treebuilder, FILE *fp)
{
	formatting_list_entry *entry;

	for (entry = treebuilder->context.formatting_list; entry != NULL; 
			entry = entry->next) {
		fprintf(fp, "%s %p %u\n", 
				element_type_to_name(entry->details.type),
				entry->details.node, entry->stack_index);
	}
}

/**
 * Convert an element type to a name
 *
 * \param type  The element type
 * \return Pointer to name
 */
const char *element_type_to_name(element_type type)
{
	for (size_t i = 0; 
			i < sizeof(name_type_map) / sizeof(name_type_map[0]);
			i++) {
		if (name_type_map[i].type == type)
			return name_type_map[i].name;
	}

	return "UNKNOWN";
}
#endif

