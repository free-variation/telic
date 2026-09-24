#include "telic.h"
#include "nanoarrow/nanoarrow.h"
#include "nanoarrow/nanoarrow_ipc.h"

#define ARROW_UNIT_KEY "telic.unit"
#define ARROW_MICROSECONDS 1000000.0
#define ARROW_FILE_MAGIC "ARROW1"
#define ARROW_FILE_MAGIC_PADDED 8

static int unit_named(Interpreter *interp, const char *name) {
	int cfa = find(name);
	if (!cfa || (cfa_handler)vocab.dict[cfa] != dounit) {
		fail(interp, "unknown unit: %s", name);
		return -1;
	}

	return (int)vocab.dict[cfa + 1];
}

static const char *unit_name(int unit) {
	const char *name;
	const char *term_dimensions[MAX_UNIT_TERMS];
	int power_numerators[MAX_UNIT_TERMS];
	int power_denominators[MAX_UNIT_TERMS];
	int scale_numerator;
	int scale_denominator;

	if (!unit_description(unit, &name, term_dimensions, power_numerators, power_denominators,
			&scale_numerator, &scale_denominator))
		return NULL;

	return name;
}

typedef enum {
	COLUMN_NUMERIC,
	COLUMN_DATETIME,
	COLUMN_QUANTITY,
	COLUMN_TEXT
} ColumnKind;

typedef struct {
	ColumnKind kind;
	Object *elements;
	int unit;
	int n_rows;
} ColumnPlan;

static int plan_column(Interpreter *interp, Val column, const char *name, ColumnPlan *plan) {
	int unit;
	Val magnitude = quantity_unwrap(column, &unit);

	if (VAL_TAG(magnitude) == T_MATRIX) {
		Object *vector = OBJECT_AT(VAL_DATA(magnitude));
		int length = vector_length(interp, vector, "a column vector");
		if (length < 0)
			return 0;

		plan->elements = vector;
		plan->unit = unit;
		plan->n_rows = length;
		if (unit == 0)
			plan->kind = COLUMN_NUMERIC;
		else
			plan->kind = unit == unit_named(interp, "s") ? COLUMN_DATETIME : COLUMN_QUANTITY;
		return !interp->error_flag;
	}

	if (VAL_TAG(column) == T_ARRAY) {
		plan->kind = COLUMN_TEXT;
		plan->elements = OBJECT_AT(VAL_DATA(column));
		plan->unit = 0;
		plan->n_rows = plan->elements->len;
		return 1;
	}

	fail(interp, "column %s is neither a vector nor an array; got %s", name, tag_name(VAL_TAG(column)));
	return 0;
}

static int arrow_failed(Interpreter *interp, int code, struct ArrowError *error, const char *doing) {
	if (code == NANOARROW_OK)
		return 0;

	fail(interp, "%s: %s", doing, ArrowErrorMessage(error));
	return 1;
}

static int build_schema(Interpreter *interp, Object *dataset, const ColumnPlan *plans,
		struct ArrowSchema *schema, struct ArrowError *error) {
	int n_columns = dataset->len;
	if (arrow_failed(interp, ArrowSchemaInitFromType(schema, NANOARROW_TYPE_STRUCT), error, "arrow schema"))
		return 0;
	if (arrow_failed(interp, ArrowSchemaAllocateChildren(schema, n_columns), error, "arrow schema")) {
		schema->release(schema);
		return 0;
	}

	for (int j = 0; j < n_columns; j++) {
		struct ArrowSchema *field = schema->children[j];
		int code = NANOARROW_OK;

		switch (plans[j].kind) {
			case COLUMN_DATETIME:
				ArrowSchemaInit(field);
				code = ArrowSchemaSetTypeDateTime(field, NANOARROW_TYPE_TIMESTAMP,
						NANOARROW_TIME_UNIT_MICRO, NULL);
				break;
			case COLUMN_TEXT:
				code = ArrowSchemaInitFromType(field, NANOARROW_TYPE_STRING);
				break;
			default:
				code = ArrowSchemaInitFromType(field, NANOARROW_TYPE_DOUBLE);
				break;
		}
		if (arrow_failed(interp, code, error, "arrow schema")) {
			schema->release(schema);
			return 0;
		}

		if (arrow_failed(interp, ArrowSchemaSetName(field, &vocab.symbol_pool[dataset->frame.keys[j]]),
				error, "arrow schema")) {
			schema->release(schema);
			return 0;
		}

		if (plans[j].kind != COLUMN_QUANTITY)
			continue;

		const char *name = unit_name(plans[j].unit);
		if (!name) {
			fail(interp, "column %s carries a unit with no name",
					&vocab.symbol_pool[dataset->frame.keys[j]]);
			schema->release(schema);
			return 0;
		}

		struct ArrowBuffer metadata;
		ArrowBufferInit(&metadata);
		code = ArrowMetadataBuilderInit(&metadata, NULL);
		if (code == NANOARROW_OK)
			code = ArrowMetadataBuilderAppend(&metadata, ArrowCharView(ARROW_UNIT_KEY), ArrowCharView(name));
		if (code == NANOARROW_OK)
			code = ArrowSchemaSetMetadata(field, (const char *)metadata.data);
		ArrowBufferReset(&metadata);
		if (arrow_failed(interp, code, error, "arrow schema")) {
			schema->release(schema);
			return 0;
		}
	}

	return 1;
}

static int append_cell(Interpreter *interp, struct ArrowArray *field, const ColumnPlan *plan,
		int row, const char *name, struct ArrowError *error) {
	if (plan->kind == COLUMN_TEXT) {
		Val cell_value = plan->elements->items[row];
		if (VAL_TAG(cell_value) == T_NONE)
			return !arrow_failed(interp, ArrowArrayAppendNull(field, 1), error, "arrow value");
		if (VAL_TAG(cell_value) == T_SYMBOL)
			return !arrow_failed(interp, ArrowArrayAppendString(field,
					ArrowCharView(&vocab.symbol_pool[VAL_DATA(cell_value)])), error, "arrow value");
		if (VAL_TAG(cell_value) == T_STRING) {
			Object *text = OBJECT_AT(VAL_DATA(cell_value));
			struct ArrowStringView view = {.data = text->bytes, .size_bytes = text->len};
			return !arrow_failed(interp, ArrowArrayAppendString(field, view), error, "arrow value");
		}

		fail(interp, "column %s holds %s, which has no arrow type", name, tag_name(VAL_TAG(cell_value)));
		return 0;
	}

	double element = plan->elements->matrix.elements[row];
	if (plan->kind == COLUMN_DATETIME)
		return !arrow_failed(interp, ArrowArrayAppendInt(field, (int64_t)(element * ARROW_MICROSECONDS)),
				error, "arrow value");

	return !arrow_failed(interp, ArrowArrayAppendDouble(field, element), error, "arrow value");
}

static int write_dataset(Interpreter *interp, Object *dataset, FILE *out) {
	struct ArrowError error;
	int n_columns = dataset->len;

	ColumnPlan *plans;
	MALLOC_OR_FAIL_RETURNING(interp, plans, sizeof(ColumnPlan) * (size_t)(n_columns ? n_columns : 1), 0);

	int n_rows = 0;
	for (int j = 0; j < n_columns; j++) {
		const char *name = &vocab.symbol_pool[dataset->frame.keys[j]];
		if (!plan_column(interp, dataset->frame.values[j], name, &plans[j])) {
			free(plans);
			return 0;
		}
		if (j == 0)
			n_rows = plans[j].n_rows;
		else if (plans[j].n_rows != n_rows) {
			fail(interp, "column %s has %d rows; the first column has %d", name, plans[j].n_rows, n_rows);
			free(plans);
			return 0;
		}
	}

	struct ArrowSchema schema;
	if (!build_schema(interp, dataset, plans, &schema, &error)) {
		free(plans);
		return 0;
	}

	struct ArrowArray array;
	if (arrow_failed(interp, ArrowArrayInitFromSchema(&array, &schema, &error), &error, "arrow array")
			|| arrow_failed(interp, ArrowArrayStartAppending(&array), &error, "arrow array")) {
		schema.release(&schema);
		free(plans);
		return 0;
	}

	for (int row = 0; row < n_rows; row++) {
		for (int j = 0; j < n_columns; j++)
			if (!append_cell(interp, array.children[j], &plans[j], row,
					&vocab.symbol_pool[dataset->frame.keys[j]], &error)) {
				array.release(&array);
				schema.release(&schema);
				free(plans);
				return 0;
			}
		if (arrow_failed(interp, ArrowArrayFinishElement(&array), &error, "arrow array")) {
			array.release(&array);
			schema.release(&schema);
			free(plans);
			return 0;
		}
	}
	free(plans);

	struct ArrowArrayView view;
	if (arrow_failed(interp, ArrowArrayFinishBuildingDefault(&array, &error), &error, "arrow array")
			|| arrow_failed(interp, ArrowArrayViewInitFromSchema(&view, &schema, &error), &error, "arrow array")
			|| arrow_failed(interp, ArrowArrayViewSetArray(&view, &array, &error), &error, "arrow array")) {
		array.release(&array);
		schema.release(&schema);
		return 0;
	}

	struct ArrowIpcOutputStream stream;
	if (arrow_failed(interp, ArrowIpcOutputStreamInitFile(&stream, out, 1), &error, "arrow file")) {
		ArrowArrayViewReset(&view);
		array.release(&array);
		schema.release(&schema);
		return 0;
	}

	struct ArrowIpcWriter writer;
	int code = ArrowIpcWriterInit(&writer, &stream);
	if (code == NANOARROW_OK)
		code = ArrowIpcWriterStartFile(&writer, &error);
	if (code == NANOARROW_OK)
		code = ArrowIpcWriterWriteSchema(&writer, &schema, &error);
	if (code == NANOARROW_OK)
		code = ArrowIpcWriterWriteArrayView(&writer, &view, &error);
	if (code == NANOARROW_OK)
		code = ArrowIpcWriterWriteArrayView(&writer, NULL, &error);
	if (code == NANOARROW_OK)
		code = ArrowIpcWriterFinalizeFile(&writer, &error);
	ArrowIpcWriterReset(&writer);

	ArrowArrayViewReset(&view);
	array.release(&array);
	schema.release(&schema);

	return !arrow_failed(interp, code, &error, "arrow file");
}

void p_write_arrow(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val path_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(path_val, T_STRING, "write-arrow", "a string");
	Val dataset_val = chain_sp[-2];
	REQUIRE_CHAIN_TAG(dataset_val, T_FRAME, "write-arrow", "a dataset");

	Object *path = OBJECT_AT(VAL_DATA(path_val));
	FILE *out = fopen(path->bytes, "wb");
	if (!out) {
		fail(interp, "cannot open %s", path->bytes);
		return;
	}

	if (!write_dataset(interp, OBJECT_AT(VAL_DATA(dataset_val)), out))
		return;

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 2);
}

typedef struct {
	struct ArrowArray array;
	struct ArrowArrayView view;
	int has_view;
} BatchView;

static ColumnKind read_kind(Interpreter *interp, struct ArrowSchema *field, int *unit) {
	struct ArrowError error;
	struct ArrowSchemaView schema_view;
	*unit = 0;

	if (arrow_failed(interp, ArrowSchemaViewInit(&schema_view, field, &error), &error, "arrow schema"))
		return COLUMN_TEXT;

	switch (schema_view.type) {
		case NANOARROW_TYPE_STRING:
		case NANOARROW_TYPE_LARGE_STRING:
			return COLUMN_TEXT;
		case NANOARROW_TYPE_TIMESTAMP:
			*unit = unit_named(interp, "s");
			return COLUMN_DATETIME;
		case NANOARROW_TYPE_BOOL:
		case NANOARROW_TYPE_INT8:
		case NANOARROW_TYPE_UINT8:
		case NANOARROW_TYPE_INT16:
		case NANOARROW_TYPE_UINT16:
		case NANOARROW_TYPE_INT32:
		case NANOARROW_TYPE_UINT32:
		case NANOARROW_TYPE_INT64:
		case NANOARROW_TYPE_UINT64:
		case NANOARROW_TYPE_HALF_FLOAT:
		case NANOARROW_TYPE_FLOAT:
		case NANOARROW_TYPE_DOUBLE: {
			struct ArrowStringView declared = ArrowCharView("");
			ArrowMetadataGetValue(field->metadata, ArrowCharView(ARROW_UNIT_KEY), &declared);
			if (declared.size_bytes == 0)
				return COLUMN_NUMERIC;

			char name[NAME_MAX_LENGTH];
			int length = (int)declared.size_bytes;
			if (length > NAME_MAX_LENGTH - 1)
				length = NAME_MAX_LENGTH - 1;
			memcpy(name, declared.data, (size_t)length);
			name[length] = 0;
			*unit = unit_named(interp, name);
			return COLUMN_QUANTITY;
		}
		default:
			fail(interp, "column %s has arrow type %s, which telic has no column for",
					field->name ? field->name : "", ArrowTypeString(schema_view.type));
			return COLUMN_TEXT;
	}
}

static int fill_text_column(Interpreter *interp, int column_handle, BatchView *batches,
		int n_batches, int child) {
	for (int b = 0; b < n_batches; b++) {
		struct ArrowArrayView *field = batches[b].view.children[child];
		for (int64_t row = 0; row < field->length; row++) {
			Val value;
			if (ArrowArrayViewIsNull(field, row)) {
				value = make_tagged(T_NONE, 0);
			} else {
				struct ArrowStringView text = ArrowArrayViewGetStringUnsafe(field, row);
				int handle = object_new_string(interp, text.data, (int)text.size_bytes);
				if (interp->error_flag)
					return 0;
				value = make_string(handle);
			}

			Object *column = OBJECT_AT(column_handle);
			ITEMS_GROW_IF_FULL(column);
			if (interp->error_flag)
				return 0;
			column = OBJECT_AT(column_handle);
			column->items[column->len++] = value;
		}
	}

	return 1;
}

static void fill_numeric_column(Object *column, BatchView *batches, int n_batches, int child,
		ColumnKind kind) {
	int at = 0;
	for (int b = 0; b < n_batches; b++) {
		struct ArrowArrayView *field = batches[b].view.children[child];
		for (int64_t row = 0; row < field->length; row++) {
			double element;
			if (ArrowArrayViewIsNull(field, row))
				element = NAN;
			else if (kind == COLUMN_DATETIME)
				element = (double)ArrowArrayViewGetIntUnsafe(field, row) / ARROW_MICROSECONDS;
			else
				element = ArrowArrayViewGetDoubleUnsafe(field, row);
			column->matrix.elements[at++] = element;
		}
	}
}

static int read_dataset(Interpreter *interp, FILE *in, int *dataset_handle_out) {
	struct ArrowError error;
	char magic[ARROW_FILE_MAGIC_PADDED];

	if (fread(magic, 1, sizeof magic, in) != sizeof magic
			|| memcmp(magic, ARROW_FILE_MAGIC, sizeof ARROW_FILE_MAGIC - 1) != 0) {
		fail(interp, "not an arrow file (no ARROW1 magic)");
		fclose(in);
		return 0;
	}

	struct ArrowIpcInputStream input;
	if (arrow_failed(interp, ArrowIpcInputStreamInitFile(&input, in, 1), &error, "arrow file")) {
		fclose(in);
		return 0;
	}

	struct ArrowArrayStream stream;
	if (arrow_failed(interp, ArrowIpcArrayStreamReaderInit(&stream, &input, NULL), &error, "arrow file"))
		return 0;

	struct ArrowSchema schema;
	if (stream.get_schema(&stream, &schema) != 0) {
		fail(interp, "arrow schema: %s", stream.get_last_error(&stream));
		stream.release(&stream);
		return 0;
	}

	BatchView *batches = NULL;
	int n_batches = 0;
	int capacity = 0;
	int n_rows = 0;
	while (1) {
		struct ArrowArray array;
		if (stream.get_next(&stream, &array) != 0) {
			fail(interp, "arrow batch: %s", stream.get_last_error(&stream));
			break;
		}
		if (array.release == NULL)
			break;

		if (n_batches == capacity) {
			capacity = capacity ? capacity * 2 : 4;
			BatchView *grown = realloc(batches, sizeof(BatchView) * (size_t)capacity);
			if (!grown) {
				array.release(&array);
				fail(interp, "out of memory");
				break;
			}
			batches = grown;
		}

		batches[n_batches].array = array;
		batches[n_batches].has_view = 0;
		int code = ArrowArrayViewInitFromSchema(&batches[n_batches].view, &schema, &error);
		if (code == NANOARROW_OK) {
			batches[n_batches].has_view = 1;
			code = ArrowArrayViewSetArray(&batches[n_batches].view, &batches[n_batches].array, &error);
		}
		n_batches++;
		if (arrow_failed(interp, code, &error, "arrow batch"))
			break;

		n_rows += (int)batches[n_batches - 1].view.length;
	}

	int n_columns = (int)schema.n_children;
	int dataset_handle = 0;
	if (!interp->error_flag) {
		dataset_handle = object_new_frame(interp);
		if (!interp->error_flag)
			gc_root_push(interp, make_frame(dataset_handle));
	}

	for (int j = 0; j < n_columns && !interp->error_flag; j++) {
		struct ArrowSchema *field = schema.children[j];
		int unit;
		ColumnKind kind = read_kind(interp, field, &unit);
		if (interp->error_flag)
			break;

		cell key = intern_symbol(interp, field->name ? field->name : "");
		if (interp->error_flag)
			break;

		if (kind == COLUMN_TEXT) {
			int column_handle = object_new_array(interp, 0);
			if (interp->error_flag)
				break;
			frame_put(OBJECT_AT(dataset_handle), key, make_array(column_handle));
			fill_text_column(interp, column_handle, batches, n_batches, j);
			continue;
		}

		int column_handle = object_new_matrix(interp, n_rows, 1);
		if (interp->error_flag)
			break;
		fill_numeric_column(OBJECT_AT(column_handle), batches, n_batches, j, kind);

		Val column = make_matrix(column_handle);
		if (kind != COLUMN_NUMERIC) {
			column = quantity_of(interp, column, unit);
			if (interp->error_flag)
				break;
		}
		frame_put(OBJECT_AT(dataset_handle), key, column);
	}

	if (dataset_handle)
		gc_root_pop(interp);

	for (int b = 0; b < n_batches; b++) {
		if (batches[b].has_view)
			ArrowArrayViewReset(&batches[b].view);
		batches[b].array.release(&batches[b].array);
	}
	free(batches);
	schema.release(&schema);
	stream.release(&stream);

	if (interp->error_flag)
		return 0;

	*dataset_handle_out = dataset_handle;
	return 1;
}

void p_read_arrow(DISPATCH_ARGS) {
	SYNC_REGISTERS(interp, chain_ip, chain_sp);
	PEEK_STRING_AT(path, 0, "read-arrow");

	FILE *in = fopen(path->bytes, "rb");
	if (!in) {
		fail(interp, "cannot open %s", path->bytes);
		return;
	}

	int dataset_handle;
	if (!read_dataset(interp, in, &dataset_handle))
		return;

	chain_sp[-1] = make_frame(dataset_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}
