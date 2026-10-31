#include "telic.h"
#include <yaml.h>

typedef enum {
	SCALAR_KIND_NULL,
	SCALAR_KIND_BOOLEAN,
	SCALAR_KIND_INTEGER,
	SCALAR_KIND_FLOAT,
	SCALAR_KIND_STRING
} ScalarKind;

typedef struct {
	yaml_parser_t parser;
	yaml_event_t event;
	int has_event;
	int anchor_handle;
	char **anchor_names;
	int n_anchors;
	int anchor_capacity;
} YamlLoadContext;

typedef struct {
	yaml_emitter_t emitter;
	char *buffer;
	int length;
	int capacity;
	int failed;
} YamlEmitContext;

static int text_is_one_of(const char *text, const char *first, const char *second, const char *third) {
	return strcmp(text, first) == 0 || strcmp(text, second) == 0 || strcmp(text, third) == 0;
}

static int digits_in_base(const char *text, int base) {
	if (!*text)
		return 0;
	for (const char *scan = text; *scan; scan++) {
		int digit = isdigit((unsigned char)*scan) ? *scan - '0'
			: isxdigit((unsigned char)*scan) ? tolower((unsigned char)*scan) - 'a' + 10
			: base;
		if (digit >= base)
			return 0;
	}
	return 1;
}

static int float_text(const char *text) {
	const char *scan = text;
	if (*scan == '-' || *scan == '+')
		scan++;

	int n_whole = 0;
	while (isdigit((unsigned char)*scan)) {
		scan++;
		n_whole++;
	}
	int n_fraction = 0;
	if (*scan == '.') {
		scan++;
		while (isdigit((unsigned char)*scan)) {
			scan++;
			n_fraction++;
		}
	}
	if (n_whole == 0 && n_fraction == 0)
		return 0;

	if (*scan == 'e' || *scan == 'E') {
		scan++;
		if (*scan == '-' || *scan == '+')
			scan++;
		if (!isdigit((unsigned char)*scan))
			return 0;
		while (isdigit((unsigned char)*scan))
			scan++;
	}
	return *scan == 0;
}

static ScalarKind scalar_kind(const char *text) {
	if (!*text || text_is_one_of(text, "~", "null", "Null") || strcmp(text, "NULL") == 0)
		return SCALAR_KIND_NULL;
	if (text_is_one_of(text, "true", "True", "TRUE") || text_is_one_of(text, "false", "False", "FALSE"))
		return SCALAR_KIND_BOOLEAN;

	const char *unsigned_text = (*text == '-' || *text == '+') ? text + 1 : text;
	if (digits_in_base(unsigned_text, 10))
		return SCALAR_KIND_INTEGER;
	if ((strncmp(text, "0o", 2) == 0 && digits_in_base(text + 2, 8)) || (strncmp(text, "0x", 2) == 0 && digits_in_base(text + 2, 16)))
		return SCALAR_KIND_INTEGER;

	if (float_text(text))
		return SCALAR_KIND_FLOAT;
	if (text_is_one_of(unsigned_text, ".inf", ".Inf", ".INF") || text_is_one_of(text, ".nan", ".NaN", ".NAN"))
		return SCALAR_KIND_FLOAT;

	return SCALAR_KIND_STRING;
}

static double digits_value(const char *digits, int base) {
	double value = 0;
	for (const char *scan = digits; *scan; scan++) {
		int digit = isdigit((unsigned char)*scan) ? *scan - '0' : tolower((unsigned char)*scan) - 'a' + 10;
		value = value * base + digit;
	}
	return value;
}

static void scalar_number(Interpreter *interp, const char *text, Val *destination) {
	if (strncmp(text, "0o", 2) == 0) {
		*destination = make_float(digits_value(text + 2, 8));
		return;
	}
	if (strncmp(text, "0x", 2) == 0) {
		*destination = make_float(digits_value(text + 2, 16));
		return;
	}

	const char *unsigned_text = (*text == '-' || *text == '+') ? text + 1 : text;
	if (text_is_one_of(text, ".nan", ".NaN", ".NAN")) {
		*destination = make_tagged(T_NONE, 0);
		return;
	}
	if (text_is_one_of(unsigned_text, ".inf", ".Inf", ".INF")) {
		*destination = make_float(*text == '-' ? -INFINITY : INFINITY);
		return;
	}

	int n_digits = (int)strlen(unsigned_text);
	if (n_digits >= 16 && digits_in_base(unsigned_text, 10)
			&& exact_claim_integer_digits(interp, unsigned_text, n_digits, *text == '-', destination))
		return;

	*destination = make_float(strtod(text, NULL));
}

static void scalar_value(Interpreter *interp, ScalarKind kind, const char *text, int length, Val *destination) {
	switch (kind) {
		case SCALAR_KIND_NULL:
			*destination = make_tagged(T_NONE, 0);
			return;
		case SCALAR_KIND_BOOLEAN:
			*destination = make_symbol(tolower((unsigned char)*text) == 't' ? vocab.true_symbol : vocab.false_symbol);
			return;
		case SCALAR_KIND_INTEGER:
		case SCALAR_KIND_FLOAT:
			scalar_number(interp, text, destination);
			return;
		case SCALAR_KIND_STRING: {
			int handle = object_new_string(interp, text, length);
			if (interp->error_flag)
				return;
			*destination = make_string(handle);
			return;
		}
	}
}

static ScalarKind tagged_kind(Interpreter *interp, const char *tag, const char *text) {
	ScalarKind kind = scalar_kind(text);

	if (strcmp(tag, "!") == 0 || strcmp(tag, YAML_STR_TAG) == 0)
		return SCALAR_KIND_STRING;
	if (strcmp(tag, YAML_NULL_TAG) == 0 && kind == SCALAR_KIND_NULL)
		return kind;
	if (strcmp(tag, YAML_BOOL_TAG) == 0 && kind == SCALAR_KIND_BOOLEAN)
		return kind;
	if (strcmp(tag, YAML_INT_TAG) == 0 && kind == SCALAR_KIND_INTEGER)
		return kind;
	if (strcmp(tag, YAML_FLOAT_TAG) == 0 && (kind == SCALAR_KIND_FLOAT || kind == SCALAR_KIND_INTEGER))
		return SCALAR_KIND_FLOAT;

	if (strcmp(tag, YAML_NULL_TAG) == 0 || strcmp(tag, YAML_BOOL_TAG) == 0 || strcmp(tag, YAML_INT_TAG) == 0 || strcmp(tag, YAML_FLOAT_TAG) == 0)
		fail(interp, "expected a value !!%s takes; got '%s'", tag + strlen("tag:yaml.org,2002:"), text);
	else
		fail(interp, "unsupported tag %s", tag);
	return SCALAR_KIND_STRING;
}

static void load_failure(Interpreter *interp, yaml_parser_t *parser) {
	if (parser->error == YAML_MEMORY_ERROR) {
		fail(interp, "out of memory");
		return;
	}
	if (parser->error == YAML_READER_ERROR) {
		fail(interp, "%s at byte %d", parser->problem, (int)parser->problem_offset);
		return;
	}

	int line = (int)parser->problem_mark.line + 1;
	int column = (int)parser->problem_mark.column + 1;
	if (parser->context)
		fail(interp, "%s, %s at line %d, column %d", parser->context, parser->problem, line, column);
	else
		fail(interp, "%s at line %d, column %d", parser->problem, line, column);
}

static int load_advance(Interpreter *interp, YamlLoadContext *context) {
	if (context->has_event) {
		yaml_event_delete(&context->event);
		context->has_event = 0;
	}

	if (!yaml_parser_parse(&context->parser, &context->event)) {
		load_failure(interp, &context->parser);
		return 0;
	}
	context->has_event = 1;
	return 1;
}

static void load_forget_anchors(YamlLoadContext *context) {
	for (int i = 0; i < context->n_anchors; i++)
		free(context->anchor_names[i]);
	context->n_anchors = 0;
	OBJECT_AT(context->anchor_handle)->len = 0;
}

static void load_remember_anchor(Interpreter *interp, YamlLoadContext *context, const yaml_char_t *anchor, Val value) {
	if (!anchor)
		return;

	GROW_IF_FULL_SYS(context->n_anchors, context->anchor_capacity, context->anchor_names);
	if (!context->anchor_names) {
		fail(interp, "out of memory");
		return;
	}
	char *name = strdup((const char *)anchor);
	if (!name) {
		fail(interp, "out of memory");
		return;
	}

	Object *anchors = OBJECT_AT(context->anchor_handle);
	ITEMS_GROW_IF_FULL(anchors);
	anchors->items[anchors->len++] = value;
	context->anchor_names[context->n_anchors++] = name;
}

static int load_find_anchor(Interpreter *interp, YamlLoadContext *context, const yaml_char_t *anchor) {
	for (int i = context->n_anchors - 1; i >= 0; i--)
		if (strcmp(context->anchor_names[i], (const char *)anchor) == 0)
			return i;
	fail(interp, "unknown alias *%s", (const char *)anchor);
	return -1;
}

static void load_scalar(Interpreter *interp, YamlLoadContext *context, Val *destination) {
	yaml_event_t *event = &context->event;
	const char *text = (const char *)event->data.scalar.value;
	int length = (int)event->data.scalar.length;
	const char *tag = (const char *)event->data.scalar.tag;

	ScalarKind kind;
	if (tag)
		kind = tagged_kind(interp, tag, text);
	else
		kind = event->data.scalar.style == YAML_PLAIN_SCALAR_STYLE ? scalar_kind(text) : SCALAR_KIND_STRING;
	if (interp->error_flag)
		return;

	scalar_value(interp, kind, text, length, destination);
	if (interp->error_flag)
		return;

	load_remember_anchor(interp, context, event->data.scalar.anchor, *destination);
}

static int load_collection_tag(Interpreter *interp, const yaml_char_t *tag, const char *expected) {
	if (!tag || strcmp((const char *)tag, expected) == 0)
		return 1;
	fail(interp, "unsupported tag %s", (const char *)tag);
	return 0;
}

static void load_node(Interpreter *interp, YamlLoadContext *context, Val *destination, int depth);

static void load_sequence(Interpreter *interp, YamlLoadContext *context, Val *destination, int depth) {
	yaml_event_t *event = &context->event;
	if (!load_collection_tag(interp, event->data.sequence_start.tag, YAML_SEQ_TAG))
		return;

	int handle = object_new_array(interp, 0);
	if (interp->error_flag)
		return;
	*destination = make_array(handle);
	load_remember_anchor(interp, context, event->data.sequence_start.anchor, *destination);
	if (interp->error_flag)
		return;

	for (;;) {
		if (!load_advance(interp, context))
			return;
		if (context->event.type == YAML_SEQUENCE_END_EVENT)
			return;

		Object *array = OBJECT_AT(handle);
		ITEMS_GROW_IF_FULL(array);
		array->items[array->len] = make_tagged(T_NONE, 0);
		array->len++;

		load_node(interp, context, &OBJECT_AT(handle)->items[array->len - 1], depth + 1);
		if (interp->error_flag)
			return;
	}
}

static cell load_key(Interpreter *interp, YamlLoadContext *context) {
	yaml_event_t *event = &context->event;

	if (event->type == YAML_SCALAR_EVENT) {
		const char *text = (const char *)event->data.scalar.value;
		if (event->data.scalar.anchor) {
			int handle = object_new_string(interp, text, (int)event->data.scalar.length);
			if (interp->error_flag)
				return 0;
			load_remember_anchor(interp, context, event->data.scalar.anchor, make_string(handle));
			if (interp->error_flag)
				return 0;
		}
		return intern_symbol(interp, text);
	}

	if (event->type == YAML_ALIAS_EVENT) {
		int at = load_find_anchor(interp, context, event->data.alias.anchor);
		if (at < 0)
			return 0;
		Val aliased = OBJECT_AT(context->anchor_handle)->items[at];
		if (VAL_TAG(aliased) != T_STRING) {
			fail(interp, "expected a string alias as a mapping key; got %s", tag_name(VAL_TAG(aliased)));
			return 0;
		}
		return intern_symbol(interp, OBJECT_AT(VAL_DATA(aliased))->bytes);
	}

	fail(interp, "expected a scalar mapping key; got a %s", event->type == YAML_SEQUENCE_START_EVENT ? "sequence" : "mapping");
	return 0;
}

static void load_mapping(Interpreter *interp, YamlLoadContext *context, Val *destination, int depth) {
	yaml_event_t *event = &context->event;
	if (!load_collection_tag(interp, event->data.mapping_start.tag, YAML_MAP_TAG))
		return;

	int handle = object_new_frame(interp);
	if (interp->error_flag)
		return;
	*destination = make_frame(handle);
	load_remember_anchor(interp, context, event->data.mapping_start.anchor, *destination);
	if (interp->error_flag)
		return;

	for (;;) {
		if (!load_advance(interp, context))
			return;
		if (context->event.type == YAML_MAPPING_END_EVENT)
			break;

		cell key = load_key(interp, context);
		if (interp->error_flag)
			return;
		if (!load_advance(interp, context))
			return;

		Object *frame = OBJECT_AT(handle);
		frame_reserve(frame, frame->len + 1);
		frame->frame.keys[frame->len] = key;
		frame->frame.values[frame->len] = make_tagged(T_NONE, 0);
		frame->len++;

		load_node(interp, context, &OBJECT_AT(handle)->frame.values[frame->len - 1], depth + 1);
		if (interp->error_flag)
			return;
	}

	frame_sort_dedup(interp, OBJECT_AT(handle));
}

static void load_node(Interpreter *interp, YamlLoadContext *context, Val *destination, int depth) {
	if (depth > JSON_MAX_DEPTH) {
		fail(interp, "nesting too deep");
		return;
	}

	yaml_event_t *event = &context->event;
	switch (event->type) {
		case YAML_SCALAR_EVENT:
			load_scalar(interp, context, destination);
			return;
		case YAML_ALIAS_EVENT: {
			int at = load_find_anchor(interp, context, event->data.alias.anchor);
			if (at < 0)
				return;
			*destination = OBJECT_AT(context->anchor_handle)->items[at];
			return;
		}
		case YAML_SEQUENCE_START_EVENT:
			load_sequence(interp, context, destination, depth);
			return;
		case YAML_MAPPING_START_EVENT:
			load_mapping(interp, context, destination, depth);
			return;
		default:
			fail(interp, "expected a node; got event %d", (int)event->type);
			return;
	}
}

static void load_document(Interpreter *interp, YamlLoadContext *context, Val *destination) {
	load_forget_anchors(context);

	if (!load_advance(interp, context))
		return;
	load_node(interp, context, destination, 0);
	if (interp->error_flag)
		return;

	load_advance(interp, context);
}

static int load_open(Interpreter *interp, YamlLoadContext *context, Object *source) {
	*context = (YamlLoadContext){
		.has_event = 0,
		.anchor_handle = 0,
		.anchor_names = NULL,
		.n_anchors = 0,
		.anchor_capacity = 0,
	};

	int anchor_handle = object_new_array(interp, 0);
	if (interp->error_flag)
		return 0;
	gc_root_push(interp, make_array(anchor_handle));
	if (interp->error_flag)
		return 0;
	context->anchor_handle = anchor_handle;

	if (!yaml_parser_initialize(&context->parser)) {
		gc_root_pop(interp);
		fail(interp, "out of memory");
		return 0;
	}
	yaml_parser_set_input_string(&context->parser, (const unsigned char *)source->bytes, (size_t)source->len);

	if (!load_advance(interp, context)) {
		yaml_parser_delete(&context->parser);
		gc_root_pop(interp);
		return 0;
	}
	return 1;
}

static void load_close(Interpreter *interp, YamlLoadContext *context) {
	if (context->has_event)
		yaml_event_delete(&context->event);
	yaml_parser_delete(&context->parser);
	for (int i = 0; i < context->n_anchors; i++)
		free(context->anchor_names[i]);
	free(context->anchor_names);
	gc_root_pop(interp);
}

static void load_single_document(Interpreter *interp, YamlLoadContext *context, Val *destination) {
	if (!load_advance(interp, context))
		return;
	if (context->event.type == YAML_STREAM_END_EVENT) {
		*destination = make_tagged(T_NONE, 0);
		return;
	}

	load_document(interp, context, destination);
	if (interp->error_flag)
		return;

	if (!load_advance(interp, context))
		return;
	if (context->event.type != YAML_STREAM_END_EVENT)
		fail(interp, "expected one document; got several (yaml-documents reads them all)");
}

static void load_every_document(Interpreter *interp, YamlLoadContext *context, int documents_handle) {
	for (;;) {
		if (!load_advance(interp, context))
			return;
		if (context->event.type == YAML_STREAM_END_EVENT)
			return;

		Object *documents = OBJECT_AT(documents_handle);
		ITEMS_GROW_IF_FULL(documents);
		documents->items[documents->len] = make_tagged(T_NONE, 0);
		documents->len++;

		load_document(interp, context, &OBJECT_AT(documents_handle)->items[documents->len - 1]);
		if (interp->error_flag)
			return;
	}
}

void p_yaml_to_frame(DISPATCH_ARGS) {
	PEEK_STRING_AT(source, 0, "yaml>frame");

	YamlLoadContext context;
	if (!load_open(interp, &context, source))
		return;

	gc_root_push(interp, make_tagged(T_NONE, 0));
	if (interp->error_flag) {
		load_close(interp, &context);
		return;
	}

	int slot = interp->n_gc_roots - 1;
	load_single_document(interp, &context, &interp->gc_roots[slot]);
	Val parsed = interp->gc_roots[slot];
	gc_root_pop(interp);
	load_close(interp, &context);
	if (interp->error_flag)
		return;

	chain_sp[-1] = parsed;

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_yaml_documents(DISPATCH_ARGS) {
	PEEK_STRING_AT(source, 0, "yaml-documents");

	int documents_handle = object_new_array(interp, 0);
	if (interp->error_flag)
		return;
	gc_root_push(interp, make_array(documents_handle));
	if (interp->error_flag)
		return;

	YamlLoadContext context;
	if (!load_open(interp, &context, source)) {
		gc_root_pop(interp);
		return;
	}

	load_every_document(interp, &context, documents_handle);
	load_close(interp, &context);
	gc_root_pop(interp);
	if (interp->error_flag)
		return;

	chain_sp[-1] = make_array(documents_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

static int emit_output(void *data, unsigned char *bytes, size_t size) {
	YamlEmitContext *context = data;
	int n = (int)size;

	if (context->length + n > context->capacity) {
		int capacity = context->capacity < 64 ? 64 : context->capacity;
		while (capacity < context->length + n) capacity *= 2;
		char *grown_buffer = realloc(context->buffer, (size_t)capacity);
		if (!grown_buffer) {
			context->failed = 1;
			return 0;
		}
		context->buffer = grown_buffer;
		context->capacity = capacity;
	}

	memcpy(context->buffer + context->length, bytes, size);
	context->length += n;
	return 1;
}

static int emit_event(Interpreter *interp, YamlEmitContext *context, yaml_event_t *event) {
	if (yaml_emitter_emit(&context->emitter, event))
		return 1;

	if (context->failed || context->emitter.error == YAML_MEMORY_ERROR)
		fail(interp, "out of memory");
	else
		fail(interp, "%s", context->emitter.problem);
	return 0;
}

static int emit_scalar(Interpreter *interp, YamlEmitContext *context, const char *text, int length, yaml_scalar_style_t style) {
	yaml_event_t event;
	if (!yaml_scalar_event_initialize(&event, NULL, NULL, (yaml_char_t *)text, length, 1, 1, style)) {
		fail(interp, "out of memory");
		return 0;
	}
	return emit_event(interp, context, &event);
}

static int older_boolean_text(const char *text) {
	static const char *words[] = { "y", "Y", "yes", "Yes", "YES", "n", "N", "no", "No", "NO", "on", "On", "ON", "off", "Off", "OFF" };
	for (int i = 0; i < (int)(sizeof words / sizeof words[0]); i++)
		if (strcmp(text, words[i]) == 0)
			return 1;
	return 0;
}

static int emit_text(Interpreter *interp, YamlEmitContext *context, const char *text, int length) {
	if (memchr(text, '\n', (size_t)length))
		return emit_scalar(interp, context, text, length, YAML_LITERAL_SCALAR_STYLE);

	int quoted = (int)strlen(text) == length && (scalar_kind(text) != SCALAR_KIND_STRING || older_boolean_text(text));
	return emit_scalar(interp, context, text, length, quoted ? YAML_SINGLE_QUOTED_SCALAR_STYLE : YAML_ANY_SCALAR_STYLE);
}

static int emit_number(Interpreter *interp, YamlEmitContext *context, double number) {
	if (isinf(number))
		return emit_scalar(interp, context, number < 0 ? "-.inf" : ".inf", number < 0 ? 5 : 4, YAML_PLAIN_SCALAR_STYLE);

	char text[NUMBER_TEXT_CAPACITY];
	int n = shortest_number_text(number, text);
	return emit_scalar(interp, context, text, n, YAML_PLAIN_SCALAR_STYLE);
}

static int emit_exact(Interpreter *interp, YamlEmitContext *context, Val value) {
	if (!exact_is_integer(value)) {
		fail(interp, "cannot serialize a non-integer exact as YAML");
		return 0;
	}

	char *rendered = NULL;
	size_t rendered_size = 0;
	FILE *stream = open_memstream(&rendered, &rendered_size);
	if (!stream) {
		fail(interp, "out of memory");
		return 0;
	}
	exact_print(stream, value);
	fclose(stream);

	int emitted = emit_scalar(interp, context, rendered, (int)rendered_size, YAML_PLAIN_SCALAR_STYLE);
	free(rendered);
	return emitted;
}

static int emit_value(Interpreter *interp, YamlEmitContext *context, Val value, int depth) {
	if (depth > JSON_MAX_DEPTH) {
		fail(interp, "nesting too deep");
		return 0;
	}

	yaml_event_t event;
	switch (VAL_TAG(value)) {
		case T_NONE:
			return emit_scalar(interp, context, "null", 4, YAML_PLAIN_SCALAR_STYLE);
		case T_FLOAT:
			return emit_number(interp, context, VAL_NUMBER(value));
		case T_EXACT:
			return emit_exact(interp, context, value);
		case T_SYMBOL:
			if ((int)VAL_DATA(value) == vocab.true_symbol)
				return emit_scalar(interp, context, "true", 4, YAML_PLAIN_SCALAR_STYLE);
			if ((int)VAL_DATA(value) == vocab.false_symbol)
				return emit_scalar(interp, context, "false", 5, YAML_PLAIN_SCALAR_STYLE);
			fail(interp, "cannot serialize a non-boolean symbol");
			return 0;
		case T_STRING: {
			Object *string = OBJECT_AT(VAL_DATA(value));
			return emit_text(interp, context, string->bytes, string->len);
		}
		case T_ARRAY: {
			yaml_sequence_start_event_initialize(&event, NULL, NULL, 1, YAML_BLOCK_SEQUENCE_STYLE);
			if (!emit_event(interp, context, &event))
				return 0;
			Object *array = OBJECT_AT(VAL_DATA(value));
			for (int i = 0; i < array->len; i++)
				if (!emit_value(interp, context, array->items[i], depth + 1))
					return 0;
			yaml_sequence_end_event_initialize(&event);
			return emit_event(interp, context, &event);
		}
		case T_FRAME: {
			yaml_mapping_start_event_initialize(&event, NULL, NULL, 1, YAML_BLOCK_MAPPING_STYLE);
			if (!emit_event(interp, context, &event))
				return 0;
			Object *frame = OBJECT_AT(VAL_DATA(value));
			for (int i = 0; i < frame->len; i++) {
				const char *key = &vocab.symbol_pool[frame->frame.keys[i]];
				if (!emit_text(interp, context, key, (int)strlen(key)))
					return 0;
				if (!emit_value(interp, context, frame->frame.values[i], depth + 1))
					return 0;
			}
			yaml_mapping_end_event_initialize(&event);
			return emit_event(interp, context, &event);
		}
		default:
			fail(interp, "cannot serialize %s", tag_name(VAL_TAG(value)));
			return 0;
	}
}

static int emit_document(Interpreter *interp, YamlEmitContext *context, Val value) {
	yaml_event_t event;

	yaml_stream_start_event_initialize(&event, YAML_UTF8_ENCODING);
	if (!emit_event(interp, context, &event))
		return 0;
	yaml_document_start_event_initialize(&event, NULL, NULL, NULL, 1);
	if (!emit_event(interp, context, &event))
		return 0;

	if (!emit_value(interp, context, value, 0))
		return 0;

	yaml_document_end_event_initialize(&event, 1);
	if (!emit_event(interp, context, &event))
		return 0;
	yaml_stream_end_event_initialize(&event);
	return emit_event(interp, context, &event);
}

void p_frame_to_yaml(DISPATCH_ARGS) {
	PEEK_AT(value, 0, "frame>yaml");

	YamlEmitContext context = {
		.buffer = NULL,
		.length = 0,
		.capacity = 0,
		.failed = 0,
	};
	if (!yaml_emitter_initialize(&context.emitter)) {
		fail(interp, "out of memory");
		return;
	}
	yaml_emitter_set_output(&context.emitter, emit_output, &context);
	yaml_emitter_set_unicode(&context.emitter, 1);
	yaml_emitter_set_width(&context.emitter, -1);

	int emitted = emit_document(interp, &context, value);
	yaml_emitter_delete(&context.emitter);
	if (!emitted) {
		free(context.buffer);
		return;
	}

	int handle = object_new_string(interp, context.buffer ? context.buffer : "", context.length);
	free(context.buffer);
	if (interp->error_flag)
		return;

	chain_sp[-1] = make_string(handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}
