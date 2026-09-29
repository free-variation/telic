#include "telic.h"
#include <pthread.h>
#include <sys/resource.h>
#include <signal.h>
#include "tigr.h"

#if defined(__APPLE__)
#include <objc/objc.h>
#include <objc/message.h>
#include <objc/runtime.h>

#define ACTIVATION_POLICY_REGULAR 0
#define ACTIVATION_POLICY_PROHIBITED 2

static id shared_application(void) {
	Class application = objc_getClass("NSApplication");
	if (!application)
		return NULL;

	return ((id (*)(Class, SEL))objc_msgSend)(application, sel_registerName("sharedApplication"));
}

static void application_set_foreground(int foreground) {
	id application = shared_application();
	if (!application)
		return;

	((void (*)(id, SEL, long))objc_msgSend)(application, sel_registerName("setActivationPolicy:"),
			foreground ? ACTIVATION_POLICY_REGULAR : ACTIVATION_POLICY_PROHIBITED);
}

static void application_bring_forward(void *native_window) {
	if (!native_window)
		return;

	((void (*)(id, SEL))objc_msgSend)((id)native_window, sel_registerName("orderFrontRegardless"));
}

static int application_drain_events(void) {
	id application = shared_application();
	if (!application)
		return 0;

	id pool = ((id (*)(id, SEL))objc_msgSend)(
			((id (*)(Class, SEL))objc_msgSend)(objc_getClass("NSAutoreleasePool"), sel_registerName("alloc")),
			sel_registerName("init"));
	id mode = ((id (*)(Class, SEL, const char *))objc_msgSend)(objc_getClass("NSString"),
			sel_registerName("stringWithUTF8String:"), "kCFRunLoopDefaultMode");
	SEL next = sel_registerName("nextEventMatchingMask:untilDate:inMode:dequeue:");
	SEL send = sel_registerName("sendEvent:");

	int n_drained = 0;
	while (n_drained < 64) {
		id event = ((id (*)(id, SEL, unsigned long long, id, id, BOOL))objc_msgSend)(
				application, next, ~0ULL, NULL, mode, YES);
		if (!event)
			break;
		((void (*)(id, SEL, id))objc_msgSend)(application, send, event);
		n_drained++;
	}
	((void (*)(id, SEL))objc_msgSend)(pool, sel_registerName("drain"));
	return n_drained;
}

#else
static void application_bring_forward(void *native_window) { (void)native_window; }
static void application_set_foreground(int foreground) { (void)foreground; }
static int application_drain_events(void) { return 1; }
#endif

#define SCREEN_DEFAULT_WIDTH 640
#define SCREEN_DEFAULT_HEIGHT 480
#define SCREEN_MAX_EDGE 8192
#define SCREEN_MAX_ZOOM 16
#define SCREEN_PUMP_MICROSECONDS 16000
#define SCREEN_FRAME_WAIT_MICROSECONDS 100000
#define INTERPRETER_STACK_BYTES (8 * 1024 * 1024)
#define SCREEN_KEY_CAPACITY 256
#define SCREEN_TYPED_CAPACITY 256

static struct {
	pthread_mutex_t lock;
	Tigr *canvas;
	int width;
	int height;
	int zoom;
	TPixel ink;
	TPixel paper;
	int requested;
	int dirty;
	int geometry_changed;
	char *shader_source;
	int shader_length;
	int shader_pending;
	float effect[4];
	int effect_changed;
	long frames_presented;
	int interpreter_done;
	int interpreter_status;
	int hold;
	pthread_cond_t wake;
	pthread_cond_t shown;
	Tigr *presented;
	int frame_ready;
	long frames_queued;
	long frames_shown;
	unsigned char keys_held[SCREEN_KEY_CAPACITY];
	unsigned char keys_pressed[SCREEN_KEY_CAPACITY];
	unsigned char frame_keys_held[SCREEN_KEY_CAPACITY];
	unsigned char frame_keys_pressed[SCREEN_KEY_CAPACITY];
	char typed[SCREEN_TYPED_CAPACITY];
	int n_typed;
} screen = {
	PTHREAD_MUTEX_INITIALIZER, NULL, SCREEN_DEFAULT_WIDTH, SCREEN_DEFAULT_HEIGHT, 1,
	{255, 255, 255, 255}, {0, 0, 0, 255}, 0, 0, 0, NULL, 0, 0, {0, 0, 0, 1}, 0, 0, 0, 0, 0,
	PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER, NULL, 0, 0, 0,
	{0}, {0}, {0}, {0}, {0}, 0
};

static Tigr *canvas_for_drawing(Interpreter *interp) {
	if (screen.canvas)
		return screen.canvas;

	screen.canvas = tigrBitmap(screen.width, screen.height);
	if (!screen.canvas) {
		fail(interp, "out of memory");
		return NULL;
	}
	tigrClear(screen.canvas, screen.paper);
	return screen.canvas;
}

#define DRAW_WORD(c_name, word_name, n_operands, call) \
	void c_name(DISPATCH_ARGS) { \
		REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, n_operands); \
		double operand[n_operands]; \
		for (int i = 0; i < (n_operands); i++) { \
			Val operand_val = chain_sp[i - (n_operands)]; \
			REQUIRE_CHAIN_TAG(operand_val, T_FLOAT, word_name, "a coordinate"); \
			operand[i] = VAL_NUMBER(operand_val); \
		} \
		\
		pthread_mutex_lock(&screen.lock); \
		Tigr *canvas = canvas_for_drawing(interp); \
		if (canvas) { \
			TPixel ink = screen.ink; \
			call; \
			screen.requested = 1; \
			screen.dirty = 1; \
		} \
		pthread_mutex_unlock(&screen.lock); \
		if (!canvas) \
			return; \
		\
		DISPATCH_REGISTERS(interp, chain_ip, chain_sp - (n_operands)); \
	}

DRAW_WORD(p_plot, "plot", 2,
		tigrPlot(canvas, (int)operand[0], (int)operand[1], ink))
DRAW_WORD(p_line, "line", 4,
		tigrLine(canvas, (int)operand[0], (int)operand[1], (int)operand[2], (int)operand[3], ink))
DRAW_WORD(p_rect, "rect", 4,
		tigrRect(canvas, (int)operand[0], (int)operand[1], (int)operand[2], (int)operand[3], ink))
DRAW_WORD(p_fill_rect, "fill-rect", 4,
		tigrFillRect(canvas, (int)operand[0] - 1, (int)operand[1] - 1, (int)operand[2] + 2, (int)operand[3] + 2, ink))
DRAW_WORD(p_circle, "circle", 3,
		tigrCircle(canvas, (int)operand[0], (int)operand[1], (int)operand[2], ink))
DRAW_WORD(p_fill_circle, "fill-circle", 3,
		tigrFillCircle(canvas, (int)operand[0], (int)operand[1], (int)operand[2], ink))

static TPixel pixel_from_rgb(unsigned int rgb) {
	TPixel pixel;
	pixel.r = (unsigned char)((rgb >> 16) & 0xFF);
	pixel.g = (unsigned char)((rgb >> 8) & 0xFF);
	pixel.b = (unsigned char)(rgb & 0xFF);
	pixel.a = 255;
	return pixel;
}

static unsigned int xterm256_rgb(int index) {
	static const int levels[6] = { 0, 95, 135, 175, 215, 255 };
	if (index < 16) {
		int level = index >= 8 ? 255 : 205;
		return (unsigned int)((((index & 1) ? level : 0) << 16) | (((index & 2) ? level : 0) << 8)
				| ((index & 4) ? level : 0));
	}
	if (index < 232) {
		int cube = index - 16;
		return (unsigned int)((levels[cube / 36] << 16) | (levels[(cube / 6) % 6] << 8) | levels[cube % 6]);
	}
	int grey = 8 + 10 * (index - 232);
	return (unsigned int)((grey << 16) | (grey << 8) | grey);
}

static int text_directive(const char *text, int remaining, TPixel start_ink, TPixel *pen) {
	int length = 1;
	while (length < remaining && text[length] >= 'a' && text[length] <= 'z')
		length++;
	if (length == 1 || length >= remaining || text[length] != '}')
		return 0;

	int name_length = length - 1;
	if (name_length == 5 && memcmp(text + 1, "plain", 5) == 0) {
		*pen = start_ink;
		return length + 1;
	}
	if ((name_length == 4 && memcmp(text + 1, "bold", 4) == 0)
			|| (name_length == 3 && memcmp(text + 1, "dim", 3) == 0))
		return length + 1;

	unsigned int rgb;
	if (!color_named(text + 1, name_length, &rgb))
		return 0;
	*pen = pixel_from_rgb(rgb);
	return length + 1;
}

static int text_escape(const char *text, int remaining, TPixel start_ink, TPixel *pen) {
	if (remaining < 3 || text[1] != '[')
		return 0;

	int parameters[16];
	int n_parameters = 0;
	int value = 0;
	int length = 2;
	for (; length < remaining; length++) {
		char c = text[length];
		if (c >= '0' && c <= '9') {
			value = value * 10 + (c - '0');
			continue;
		}
		if (n_parameters < 16)
			parameters[n_parameters++] = value;
		value = 0;
		if (c == 'm')
			break;
		if (c != ';')
			return 0;
	}
	if (length >= remaining)
		return 0;

	for (int i = 0; i < n_parameters; i++) {
		int code = parameters[i];
		if (code == 0 || code == 39)
			*pen = start_ink;
		else if (code >= 30 && code <= 37)
			*pen = pixel_from_rgb(xterm256_rgb(code - 30));
		else if (code >= 90 && code <= 97)
			*pen = pixel_from_rgb(xterm256_rgb(code - 90 + 8));
		else if (code == 38 && i + 2 < n_parameters && parameters[i + 1] == 5) {
			*pen = pixel_from_rgb(xterm256_rgb(parameters[i + 2] & 0xFF));
			i += 2;
		} else if (code == 38 && i + 4 < n_parameters && parameters[i + 1] == 2) {
			*pen = pixel_from_rgb((unsigned int)(((parameters[i + 2] & 0xFF) << 16)
					| ((parameters[i + 3] & 0xFF) << 8) | (parameters[i + 4] & 0xFF)));
			i += 4;
		}
	}
	return length + 1;
}

static TigrGlyph *glyph_for(TigrFont *font, int code) {
	int low = 0;
	int high = font->numGlyphs;
	while (low < high) {
		int middle = (low + high) / 2;
		if (code < font->glyphs[middle].code)
			high = middle;
		else
			low = middle + 1;
	}
	if (low == 0 || font->glyphs[low - 1].code != code)
		return &font->glyphs['?' - 32];
	return &font->glyphs[low - 1];
}

static void draw_text(Tigr *canvas, int x, int y, const char *text, int n_bytes, TPixel start_ink) {
	int line_height = tigrTextHeight(tfont, "");
	TPixel pen = start_ink;
	int pen_x = x;
	int pen_y = y;
	const char *cursor = text;
	const char *end = text + n_bytes;

	while (cursor < end) {
		int remaining = (int)(end - cursor);
		int consumed = 0;
		if (*cursor == '{')
			consumed = text_directive(cursor, remaining, start_ink, &pen);
		else if (*cursor == '\x1b')
			consumed = text_escape(cursor, remaining, start_ink, &pen);
		if (consumed) {
			cursor += consumed;
			continue;
		}

		if (*cursor == '\n') {
			pen_x = x;
			pen_y += line_height;
			cursor++;
			continue;
		}
		if (*cursor == '\r') {
			cursor++;
			continue;
		}

		int code;
		const char *next = tigrDecodeUTF8(cursor, &code);
		if (next <= cursor || next > end)
			next = cursor + 1;
		TigrGlyph *glyph = glyph_for(tfont, code);
		tigrBlitTint(canvas, tfont->bitmap, pen_x, pen_y, glyph->x, glyph->y, glyph->w, glyph->h, pen);
		pen_x += glyph->w;
		cursor = next;
	}
}

void p_print_at(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 3);
	Val x_val = chain_sp[-3];
	REQUIRE_CHAIN_TAG(x_val, T_FLOAT, "print-at", "a coordinate");
	Val y_val = chain_sp[-2];
	REQUIRE_CHAIN_TAG(y_val, T_FLOAT, "print-at", "a coordinate");
	Val text_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(text_val, T_STRING, "print-at", "a string");
	Object *text = OBJECT_AT(VAL_DATA(text_val));

	pthread_mutex_lock(&screen.lock);
	Tigr *canvas = canvas_for_drawing(interp);
	if (canvas) {
		draw_text(canvas, (int)VAL_NUMBER(x_val), (int)VAL_NUMBER(y_val), text->bytes, text->len, screen.ink);
		screen.requested = 1;
		screen.dirty = 1;
	}
	pthread_mutex_unlock(&screen.lock);
	if (!canvas)
		return;

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 3);
}

void p_cls(DISPATCH_ARGS) {
	pthread_mutex_lock(&screen.lock);
	Tigr *canvas = canvas_for_drawing(interp);
	if (canvas) {
		tigrClear(canvas, screen.paper);
		screen.requested = 1;
		screen.dirty = 1;
	}
	pthread_mutex_unlock(&screen.lock);
	if (!canvas)
		return;

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}


#define COLOR_WORD(c_name, word_name, field) \
	void c_name(DISPATCH_ARGS) { \
		REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1); \
		Val packed_val = chain_sp[-1]; \
		REQUIRE_CHAIN_TAG(packed_val, T_FLOAT, word_name, "a packed color"); \
		\
		pthread_mutex_lock(&screen.lock); \
		screen.field = pixel_from_rgb((unsigned int)VAL_NUMBER(packed_val)); \
		pthread_mutex_unlock(&screen.lock); \
		\
		DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1); \
	}

COLOR_WORD(p_ink, "(ink)", ink)
COLOR_WORD(p_paper, "(paper)", paper)

void p_screen_size(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val width_val = chain_sp[-2];
	REQUIRE_CHAIN_TAG(width_val, T_FLOAT, "screen-size", "a width");
	Val height_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(height_val, T_FLOAT, "screen-size", "a height");
	double width = VAL_NUMBER(width_val);
	double height = VAL_NUMBER(height_val);

	if (width < 1 || height < 1 || width > SCREEN_MAX_EDGE || height > SCREEN_MAX_EDGE) {
		fail(interp, "screen edges must be in [1, %d]; got %gx%g", SCREEN_MAX_EDGE, width, height);
		return;
	}

	pthread_mutex_lock(&screen.lock);
	Tigr *resized_canvas = NULL;
	if (screen.canvas) {
		resized_canvas = tigrBitmap((int)width, (int)height);
		if (resized_canvas)
			tigrClear(resized_canvas, screen.paper);
	}
	int allocated = !screen.canvas || resized_canvas;
	if (allocated) {
		if (screen.canvas) {
			tigrFree(screen.canvas);
			screen.canvas = resized_canvas;
			screen.dirty = 1;
		}
		screen.width = (int)width;
		screen.height = (int)height;
		screen.geometry_changed = 1;
	}
	pthread_mutex_unlock(&screen.lock);

	if (!allocated) {
		fail(interp, "out of memory");
		return;
	}

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 2);
}

void p_screen_zoom(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val zoom_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(zoom_val, T_FLOAT, "screen-zoom", "a zoom factor");
	double zoom = VAL_NUMBER(zoom_val);

	if (zoom < 1 || zoom > SCREEN_MAX_ZOOM || zoom != (double)(int)zoom) {
		fail(interp, "zoom must be an integer in [1, %d]; got %g", SCREEN_MAX_ZOOM, zoom);
		return;
	}

	pthread_mutex_lock(&screen.lock);
	screen.zoom = (int)zoom;
	screen.geometry_changed = 1;
	pthread_mutex_unlock(&screen.lock);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_screen_shader(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val source_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(source_val, T_STRING, "screen-shader", "a GLSL string");
	Object *source = OBJECT_AT(VAL_DATA(source_val));

	char *copy = malloc((size_t)source->len + 1);
	if (!copy) {
		fail(interp, "out of memory");
		return;
	}
	memcpy(copy, source->bytes, (size_t)source->len);
	copy[source->len] = 0;

	pthread_mutex_lock(&screen.lock);
	free(screen.shader_source);
	screen.shader_source = copy;
	screen.shader_length = source->len;
	screen.shader_pending = 1;
	screen.requested = 1;
	pthread_mutex_unlock(&screen.lock);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_screen_effect(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 4);
	float effect[4];
	for (int i = 0; i < 4; i++) {
		Val effect_val = chain_sp[i - 4];
		REQUIRE_CHAIN_TAG(effect_val, T_FLOAT, "screen-effect", "a number");
		effect[i] = (float)VAL_NUMBER(effect_val);
	}

	pthread_mutex_lock(&screen.lock);
	if (memcmp(screen.effect, effect, sizeof effect) != 0) {
		memcpy(screen.effect, effect, sizeof effect);
		screen.effect_changed = 1;
	}
	pthread_mutex_unlock(&screen.lock);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 4);
}

static struct timespec deadline_after(long microseconds) {
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	long nanoseconds = now.tv_nsec + microseconds * 1000;
	struct timespec deadline = {
		.tv_sec = now.tv_sec + nanoseconds / 1000000000,
		.tv_nsec = nanoseconds % 1000000000,
	};
	return deadline;
}

static int copy_canvas_to_presented(void) {
	Tigr *canvas = screen.canvas;
	Tigr *presented = screen.presented;
	if (!presented || presented->w != canvas->w || presented->h != canvas->h) {
		if (presented)
			tigrFree(presented);
		presented = tigrBitmap(canvas->w, canvas->h);
		screen.presented = presented;
		if (!presented)
			return 0;
	}
	memcpy(presented->pix, canvas->pix, (size_t)canvas->w * (size_t)canvas->h * sizeof(TPixel));
	return 1;
}

static void await_presentation(Interpreter *interp, long serial) {
	struct timespec deadline = deadline_after(SCREEN_FRAME_WAIT_MICROSECONDS);
	while (screen.frames_shown < serial && !(interp->gc_pending & INTERRUPT_PENDING))
		if (pthread_cond_timedwait(&screen.shown, &screen.lock, &deadline) != 0)
			break;
}

void p_screen_frame(DISPATCH_ARGS) {
	POP_CALLABLE(body, "screen-frame");
	push_curried_bindings(interp, body_val);
	if (interp->error_flag)
		return;

	pthread_mutex_lock(&screen.lock);
	screen.hold++;
	if (screen.hold == 1) {
		memcpy(screen.frame_keys_held, screen.keys_held, sizeof screen.keys_held);
		memcpy(screen.frame_keys_pressed, screen.keys_pressed, sizeof screen.keys_pressed);
		memset(screen.keys_pressed, 0, sizeof screen.keys_pressed);
	}
	pthread_mutex_unlock(&screen.lock);

	execute_xt(interp, body);

	pthread_mutex_lock(&screen.lock);
	screen.hold--;
	int queues = screen.hold == 0 && screen.dirty && screen.canvas && !interp->error_flag;
	if (queues && !copy_canvas_to_presented()) {
		pthread_mutex_unlock(&screen.lock);
		fail(interp, "out of memory");
		return;
	}
	if (queues) {
		long serial = ++screen.frames_queued;
		screen.frame_ready = 1;
		screen.dirty = 0;
		pthread_cond_signal(&screen.wake);
		await_presentation(interp, serial);
	}
	pthread_mutex_unlock(&screen.lock);

	DISPATCH(interp);
}

void p_screen_frames(DISPATCH_ARGS) {
	REQUIRE_STACK_ROOM(interp, chain_ip, chain_sp, 1);

	pthread_mutex_lock(&screen.lock);
	long frames = screen.frames_presented;
	pthread_mutex_unlock(&screen.lock);

	chain_sp[0] = make_float((double)frames);
	DISPATCH_REGISTERS(interp, chain_ip, chain_sp + 1);
}

#define BITMAP_FLIP_HORIZONTAL 1
#define BITMAP_FLIP_VERTICAL 2
#define BITMAP_MAX_SCALE 64
#define BITMAP_SOLID_TRANSPARENCY 128
#define BITMAP_OPAQUE_LIMIT 16777216.0

typedef struct {
	Object *bitmap;
	int x;
	int y;
	int scale;
	int flip;
	double opacity;
} BitmapPlacement;

typedef void (*PixelSink)(void *target, int x, int y, unsigned int rgb, double coverage);

static int bitmap_color(double element, unsigned int *rgb, int *transparency) {
	if (isnan(element) || element < 0)
		return 0;
	unsigned long packed = element >= 4294967296.0 ? 0xFFFFFFFFul : (unsigned long)element;
	*rgb = (unsigned int)(packed & 0xFFFFFF);
	*transparency = (int)((packed >> 24) & 0xFF);
	return *transparency < 255;
}

static int placement_from_operands(Interpreter *interp, Val bitmap_val, const Val *operands, const char *word_name,
		BitmapPlacement *placement) {
	if (VAL_TAG(bitmap_val) != T_MATRIX) {
		fail(interp, "expected a bitmap (a matrix); got %s", tag_name(VAL_TAG(bitmap_val)));
		return 0;
	}
	for (int i = 0; i < 5; i++)
		if (VAL_TAG(operands[i]) != T_FLOAT) {
			fail(interp, "expected a number for %s; got %s", word_name, tag_name(VAL_TAG(operands[i])));
			return 0;
		}
	double scale = VAL_NUMBER(operands[2]);
	double flip = VAL_NUMBER(operands[3]);
	double opacity = VAL_NUMBER(operands[4]);
	if (!(scale >= 1 && scale <= BITMAP_MAX_SCALE) || scale != floor(scale)) {
		fail(interp, "expected a scale that is an integer in [1, %d]; got %g", BITMAP_MAX_SCALE, scale);
		return 0;
	}
	if (!(flip >= 0 && flip <= 3) || flip != floor(flip)) {
		fail(interp, "expected a flip code in [0, 3]; got %g", flip);
		return 0;
	}
	if (!(opacity >= 0 && opacity <= 1)) {
		fail(interp, "expected an opacity in [0, 1]; got %g", opacity);
		return 0;
	}
	placement->bitmap = OBJECT_AT(VAL_DATA(bitmap_val));
	placement->x = (int)floor(VAL_NUMBER(operands[0]));
	placement->y = (int)floor(VAL_NUMBER(operands[1]));
	placement->scale = (int)scale;
	placement->flip = (int)flip;
	placement->opacity = opacity;
	return 1;
}

static double placement_element(const BitmapPlacement *placement, int screen_x, int screen_y) {
	Object *bitmap = placement->bitmap;
	int n_rows = bitmap->matrix.rows;
	int n_columns = bitmap->matrix.columns;
	int column = (screen_x - placement->x) / placement->scale;
	int row = (screen_y - placement->y) / placement->scale;
	if (placement->flip & BITMAP_FLIP_HORIZONTAL)
		column = n_columns - 1 - column;
	if (placement->flip & BITMAP_FLIP_VERTICAL)
		row = n_rows - 1 - row;
	return MAT(bitmap, row, column);
}

static void place_bitmap(const BitmapPlacement *placement, int target_width, int target_height, PixelSink sink, void *target) {
	Object *bitmap = placement->bitmap;
	int left = MAX(placement->x, 0);
	int top = MAX(placement->y, 0);
	int right = MIN(placement->x + bitmap->matrix.columns * placement->scale, target_width);
	int bottom = MIN(placement->y + bitmap->matrix.rows * placement->scale, target_height);

	for (int screen_y = top; screen_y < bottom; screen_y++)
		for (int screen_x = left; screen_x < right; screen_x++) {
			unsigned int rgb;
			int transparency;
			if (!bitmap_color(placement_element(placement, screen_x, screen_y), &rgb, &transparency))
				continue;
			double coverage = (255 - transparency) / 255.0 * placement->opacity;
			if (coverage > 0)
				sink(target, screen_x, screen_y, rgb, coverage);
		}
}

static unsigned char blended_channel(unsigned int over, unsigned int under, double coverage) {
	return (unsigned char)lround(over * coverage + under * (1.0 - coverage));
}

static void canvas_sink(void *target, int x, int y, unsigned int rgb, double coverage) {
	Tigr *canvas = target;
	TPixel *pixel = &canvas->pix[y * canvas->w + x];
	pixel->r = blended_channel((rgb >> 16) & 0xFF, pixel->r, coverage);
	pixel->g = blended_channel((rgb >> 8) & 0xFF, pixel->g, coverage);
	pixel->b = blended_channel(rgb & 0xFF, pixel->b, coverage);
}

static void bitmap_sink(void *target, int x, int y, unsigned int rgb, double coverage) {
	Object *bitmap = target;
	double *element = &MAT(bitmap, y, x);
	unsigned int under_rgb = 0;
	int under_transparency = 255;
	bitmap_color(*element, &under_rgb, &under_transparency);
	double under_coverage = (255 - under_transparency) / 255.0;
	double combined = coverage + under_coverage * (1.0 - coverage);
	if (combined <= 0) {
		*element = NAN;
		return;
	}

	unsigned int channels = 0;
	for (int shift = 16; shift >= 0; shift -= 8) {
		double over = (rgb >> shift) & 0xFF;
		double under = (under_rgb >> shift) & 0xFF;
		double mixed = (over * coverage + under * under_coverage * (1.0 - coverage)) / combined;
		channels |= (unsigned int)lround(mixed) << shift;
	}
	long transparency = lround((1.0 - combined) * 255.0);
	*element = (double)channels + (double)transparency * BITMAP_OPAQUE_LIMIT;
}

void p_draw_bitmap_ext(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 6);
	BitmapPlacement placement;
	if (!placement_from_operands(interp, chain_sp[-6], chain_sp - 5, "(draw-bitmap)", &placement))
		return;

	pthread_mutex_lock(&screen.lock);
	Tigr *canvas = canvas_for_drawing(interp);
	if (canvas) {
		place_bitmap(&placement, canvas->w, canvas->h, canvas_sink, canvas);
		screen.requested = 1;
		screen.dirty = 1;
	}
	pthread_mutex_unlock(&screen.lock);
	if (!canvas)
		return;

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 6);
}

void p_blit_ext(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 7);
	Val target_val = chain_sp[-6];
	REQUIRE_CHAIN_TAG(target_val, T_MATRIX, "(blit)", "a target bitmap (a matrix)");
	BitmapPlacement placement;
	if (!placement_from_operands(interp, chain_sp[-7], chain_sp - 5, "(blit)", &placement))
		return;
	Object *target = OBJECT_AT(VAL_DATA(target_val));

	place_bitmap(&placement, target->matrix.columns, target->matrix.rows, bitmap_sink, target);

	chain_sp[-7] = target_val;
	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 6);
}

void p_capture_bitmap(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 4);
	double region[4];
	for (int i = 0; i < 4; i++) {
		Val region_val = chain_sp[i - 4];
		REQUIRE_CHAIN_TAG(region_val, T_FLOAT, "capture-bitmap", "a coordinate");
		region[i] = VAL_NUMBER(region_val);
	}
	int left = (int)floor(region[0]);
	int top = (int)floor(region[1]);
	double width = region[2];
	double height = region[3];
	if (!(width >= 1 && height >= 1 && width <= SCREEN_MAX_EDGE && height <= SCREEN_MAX_EDGE)) {
		fail(interp, "expected a region with edges in [1, %d]; got %gx%g", SCREEN_MAX_EDGE, width, height);
		return;
	}

	int n_columns = (int)width;
	int n_rows = (int)height;
	NEW_MATRIX(bitmap_handle, bitmap, n_rows, n_columns);

	pthread_mutex_lock(&screen.lock);
	Tigr *canvas = canvas_for_drawing(interp);
	if (canvas)
		for (int row = 0; row < n_rows; row++)
			for (int column = 0; column < n_columns; column++) {
				int x = left + column;
				int y = top + row;
				if (x < 0 || y < 0 || x >= canvas->w || y >= canvas->h) {
					MAT(bitmap, row, column) = NAN;
					continue;
				}
				TPixel pixel = canvas->pix[y * canvas->w + x];
				MAT(bitmap, row, column) = (double)(((unsigned int)pixel.r << 16) | ((unsigned int)pixel.g << 8) | pixel.b);
			}
	pthread_mutex_unlock(&screen.lock);
	if (!canvas)
		return;

	chain_sp[-4] = make_matrix(bitmap_handle);
	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 3);
}

static int placement_solid_at(const BitmapPlacement *placement, int screen_x, int screen_y) {
	unsigned int rgb;
	int transparency;
	return bitmap_color(placement_element(placement, screen_x, screen_y), &rgb, &transparency)
		&& transparency < BITMAP_SOLID_TRANSPARENCY;
}

void p_pixels_collide(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 12);
	BitmapPlacement first;
	BitmapPlacement second;
	if (!placement_from_operands(interp, chain_sp[-12], chain_sp - 11, "(pixels-collide)", &first)
			|| !placement_from_operands(interp, chain_sp[-6], chain_sp - 5, "(pixels-collide)", &second))
		return;

	int left = MAX(first.x, second.x);
	int top = MAX(first.y, second.y);
	int right = MIN(first.x + first.bitmap->matrix.columns * first.scale, second.x + second.bitmap->matrix.columns * second.scale);
	int bottom = MIN(first.y + first.bitmap->matrix.rows * first.scale, second.y + second.bitmap->matrix.rows * second.scale);
	int collide = 0;
	for (int screen_y = top; screen_y < bottom && !collide; screen_y++)
		for (int screen_x = left; screen_x < right && !collide; screen_x++)
			collide = placement_solid_at(&first, screen_x, screen_y) && placement_solid_at(&second, screen_x, screen_y);

	chain_sp[-12] = make_float(collide ? 1.0 : 0.0);
	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 11);
}

void p_rotate_bitmap_angle(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val source_val = chain_sp[-2];
	REQUIRE_CHAIN_TAG(source_val, T_MATRIX, "rotate-bitmap-angle", "a bitmap (a matrix)");
	Val angle_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(angle_val, T_FLOAT, "rotate-bitmap-angle", "an angle in radians");
	Object *source = OBJECT_AT(VAL_DATA(source_val));
	double angle = VAL_NUMBER(angle_val);
	double cosine = cos(angle);
	double sine = sin(angle);
	int n_source_rows = source->matrix.rows;
	int n_source_columns = source->matrix.columns;
	int n_columns = (int)ceil(fabs(n_source_columns * cosine) + fabs(n_source_rows * sine) - 1e-9);
	int n_rows = (int)ceil(fabs(n_source_columns * sine) + fabs(n_source_rows * cosine) - 1e-9);
	n_columns = MAX(n_columns, 1);
	n_rows = MAX(n_rows, 1);

	NEW_MATRIX(rotated_handle, rotated, n_rows, n_columns);
	for (int row = 0; row < n_rows; row++)
		for (int column = 0; column < n_columns; column++) {
			double dx = column + 0.5 - n_columns / 2.0;
			double dy = row + 0.5 - n_rows / 2.0;
			double source_x = cosine * dx + sine * dy + n_source_columns / 2.0;
			double source_y = -sine * dx + cosine * dy + n_source_rows / 2.0;
			int source_column = (int)floor(source_x);
			int source_row = (int)floor(source_y);
			int inside = source_column >= 0 && source_row >= 0 && source_column < n_source_columns && source_row < n_source_rows;
			MAT(rotated, row, column) = inside ? MAT(source, source_row, source_column) : NAN;
		}

	chain_sp[-2] = make_matrix(rotated_handle);
	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

static const struct {
	const char *name;
	int key;
} keyboard_names[] = {
	{"space", TK_SPACE}, {"return", TK_RETURN}, {"escape", TK_ESCAPE}, {"tab", TK_TAB},
	{"backspace", TK_BACKSPACE}, {"delete", TK_DELETE}, {"insert", TK_INSERT},
	{"left", TK_LEFT}, {"right", TK_RIGHT}, {"up", TK_UP}, {"down", TK_DOWN},
	{"home", TK_HOME}, {"end", TK_END}, {"page-up", TK_PAGEUP}, {"page-down", TK_PAGEDN},
	{"shift", TK_SHIFT}, {"control", TK_CONTROL}, {"alt", TK_ALT},
	{"left-shift", TK_LSHIFT}, {"right-shift", TK_RSHIFT}, {"left-control", TK_LCONTROL},
	{"right-control", TK_RCONTROL}, {"left-alt", TK_LALT}, {"right-alt", TK_RALT},
	{"semicolon", TK_SEMICOLON}, {"equals", TK_EQUALS}, {"comma", TK_COMMA}, {"minus", TK_MINUS},
	{"period", TK_DOT}, {"slash", TK_SLASH}, {"backquote", TK_BACKTICK}, {"left-bracket", TK_LSQUARE},
	{"backslash", TK_BACKSLASH}, {"right-bracket", TK_RSQUARE}, {"quote", TK_TICK},
	{"f1", TK_F1}, {"f2", TK_F2}, {"f3", TK_F3}, {"f4", TK_F4}, {"f5", TK_F5}, {"f6", TK_F6},
	{"f7", TK_F7}, {"f8", TK_F8}, {"f9", TK_F9}, {"f10", TK_F10}, {"f11", TK_F11}, {"f12", TK_F12},
	{NULL, 0}
};

static int keyboard_key(Interpreter *interp, Val key_val) {
	if (VAL_TAG(key_val) != T_SYMBOL) {
		fail(interp, "expected a key name (a symbol such as :a or :space); got %s", tag_name(VAL_TAG(key_val)));
		return -1;
	}
	const char *name = &vocab.symbol_pool[VAL_DATA(key_val)];
	if (name[0] && !name[1] && name[0] >= 'a' && name[0] <= 'z')
		return 'A' + (name[0] - 'a');
	if (name[0] && !name[1] && name[0] >= '0' && name[0] <= '9')
		return name[0];
	for (int i = 0; keyboard_names[i].name; i++)
		if (strcmp(keyboard_names[i].name, name) == 0)
			return keyboard_names[i].key;
	fail(interp, "unknown key :%s", name);
	return -1;
}

static int keyboard_state(Interpreter *interp, Val key_val, const unsigned char *keys) {
	int key = keyboard_key(interp, key_val);
	if (key < 0)
		return -1;

	pthread_mutex_lock(&screen.lock);
	int down = keys[key];
	pthread_mutex_unlock(&screen.lock);
	return down;
}

void p_key_down(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	int down = keyboard_state(interp, chain_sp[-1], screen.frame_keys_held);
	if (down < 0)
		return;

	chain_sp[-1] = make_float(down ? 1.0 : 0.0);
	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_key_pressed(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	int pressed = keyboard_state(interp, chain_sp[-1], screen.frame_keys_pressed);
	if (pressed < 0)
		return;

	chain_sp[-1] = make_float(pressed ? 1.0 : 0.0);
	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_typed_text(DISPATCH_ARGS) {
	REQUIRE_STACK_ROOM(interp, chain_ip, chain_sp, 1);
	char typed[SCREEN_TYPED_CAPACITY];

	pthread_mutex_lock(&screen.lock);
	int n_typed = screen.n_typed;
	memcpy(typed, screen.typed, (size_t)n_typed);
	screen.n_typed = 0;
	pthread_mutex_unlock(&screen.lock);

	int text_handle = object_new_string(interp, typed, n_typed);
	if (interp->error_flag)
		return;

	chain_sp[0] = make_string(text_handle);
	DISPATCH_REGISTERS(interp, chain_ip, chain_sp + 1);
}

static void keyboard_collect(Tigr *source) {
	for (int key = 0; key < SCREEN_KEY_CAPACITY; key++) {
		screen.keys_held[key] = (unsigned char)(tigrKeyHeld(source, key) != 0);
		if (tigrKeyDown(source, key))
			screen.keys_pressed[key] = 1;
	}
	int code = tigrReadChar(source);
	if (code <= 0 || screen.n_typed + 4 > SCREEN_TYPED_CAPACITY)
		return;
	char *out = &screen.typed[screen.n_typed];
	if (code < 0x80) {
		out[0] = (char)code;
		screen.n_typed += 1;
	} else if (code < 0x800) {
		out[0] = (char)(0xC0 | (code >> 6));
		out[1] = (char)(0x80 | (code & 0x3F));
		screen.n_typed += 2;
	} else if (code < 0x10000) {
		out[0] = (char)(0xE0 | (code >> 12));
		out[1] = (char)(0x80 | ((code >> 6) & 0x3F));
		out[2] = (char)(0x80 | (code & 0x3F));
		screen.n_typed += 3;
	} else {
		out[0] = (char)(0xF0 | (code >> 18));
		out[1] = (char)(0x80 | ((code >> 12) & 0x3F));
		out[2] = (char)(0x80 | ((code >> 6) & 0x3F));
		out[3] = (char)(0x80 | (code & 0x3F));
		screen.n_typed += 4;
	}
}

static Tigr *window = NULL;
static pthread_t interpreter_thread;
static int interpreter_thread_started = 0;
static int window_needs_front = 0;
static int window_blitted_width = 0;
static int window_blitted_height = 0;

static void blit_zoomed(Tigr *target, const Tigr *canvas) {
	int zoom = MAX(1, MIN(target->w / canvas->w, target->h / canvas->h));
	int n_rows = MIN(target->h, canvas->h * zoom);
	int n_columns = MIN(target->w, canvas->w * zoom);

	for (int row = 0; row < n_rows; row++) {
		TPixel *target_row = &target->pix[row * target->w];
		const TPixel *canvas_row = &canvas->pix[(row / zoom) * canvas->w];
		for (int column = 0; column < n_columns; column++)
			target_row[column] = canvas_row[column / zoom];
	}
}

static int screen_step(void) {
	pthread_mutex_lock(&screen.lock);
	int busy = screen.frame_ready || (screen.dirty && screen.hold == 0)
		|| screen.effect_changed || screen.shader_pending;
	screen.effect_changed = 0;
	int requested = screen.requested;
	int width = screen.width;
	int height = screen.height;
	int zoom = screen.zoom;
	int geometry_changed = screen.geometry_changed;
	screen.geometry_changed = 0;
	pthread_mutex_unlock(&screen.lock);

	if (geometry_changed && window) {
		tigrFree(window);
		window = NULL;
		window_blitted_width = 0;
		window_blitted_height = 0;
	}

	if (!requested && !window) {
		application_drain_events();
		return 0;
	}

	if (requested && !window) {
		application_set_foreground(1);
		window = tigrWindow(width * zoom, height * zoom, "telic", TIGR_AUTO);
		if (!window) {
			application_set_foreground(0);
			pthread_mutex_lock(&screen.lock);
			screen.requested = 0;
			pthread_mutex_unlock(&screen.lock);
			return 0;
		}
		window_needs_front = 1;
		pthread_mutex_lock(&screen.lock);
		if (screen.shader_source)
			screen.shader_pending = 1;
		pthread_mutex_unlock(&screen.lock);
	}

	if (!window)
		return 0;

	int resized = window->w != window_blitted_width || window->h != window_blitted_height;

	pthread_mutex_lock(&screen.lock);
	Tigr *source = NULL;
	long shown_serial = 0;
	if (screen.frame_ready) {
		source = screen.presented;
		shown_serial = screen.frames_queued;
		screen.frame_ready = 0;
	} else if ((screen.dirty || resized) && screen.hold == 0) {
		source = screen.canvas;
		screen.dirty = 0;
	} else if (resized) {
		source = screen.presented ? screen.presented : screen.canvas;
	}
	if (source) {
		if (resized)
			tigrClear(window, screen.paper);
		blit_zoomed(window, source);
		window_blitted_width = window->w;
		window_blitted_height = window->h;
	}
	if (shown_serial) {
		screen.frames_shown = shown_serial;
		pthread_cond_broadcast(&screen.shown);
	}
	pthread_mutex_unlock(&screen.lock);

	pthread_mutex_lock(&screen.lock);
	if (screen.shader_pending && screen.shader_source) {
		tigrSetPostShader(window, screen.shader_source, screen.shader_length);
		screen.shader_pending = 0;
	}
	tigrSetPostFX(window, screen.effect[0], screen.effect[1], screen.effect[2], screen.effect[3]);
	pthread_mutex_unlock(&screen.lock);

	tigrUpdate(window);

	pthread_mutex_lock(&screen.lock);
	screen.frames_presented++;
	keyboard_collect(window);
	pthread_mutex_unlock(&screen.lock);

	if (window_needs_front) {
		application_bring_forward(window->handle);
		window_needs_front = 0;
	}

	if (tigrClosed(window)) {
		if (platform_interrupt_handled())
			pthread_kill(interpreter_thread, SIGUSR1);
		else
			kill(getpid(), SIGINT);
		tigrFree(window);
		window = NULL;
		window_blitted_width = 0;
		window_blitted_height = 0;
		application_drain_events();
		application_set_foreground(0);
		pthread_mutex_lock(&screen.lock);
		screen.requested = 0;
		pthread_mutex_unlock(&screen.lock);
	}
	return busy;
}

typedef struct {
	int argc;
	char **argv;
	MainBody body;
} InterpreterArguments;

static void interpreter_signals(sigset_t *set) {
	sigemptyset(set);
	sigaddset(set, SIGALRM);
	sigaddset(set, SIGINT);
	sigaddset(set, SIGWINCH);
	sigaddset(set, SIGUSR1);
}

static void *interpreter_entry(void *raw) {
	InterpreterArguments *arguments = raw;

	sigset_t handled;
	interpreter_signals(&handled);
	pthread_sigmask(SIG_UNBLOCK, &handled, NULL);

	int status = arguments->body(arguments->argc, arguments->argv);

	pthread_mutex_lock(&screen.lock);
	screen.interpreter_status = status;
	screen.interpreter_done = 1;
	pthread_mutex_unlock(&screen.lock);
	return NULL;
}

static size_t interpreter_stack_bytes(void) {
	struct rlimit limit;
	if (getrlimit(RLIMIT_STACK, &limit) != 0 || limit.rlim_cur == RLIM_INFINITY
			|| limit.rlim_cur < INTERPRETER_STACK_BYTES)
		return INTERPRETER_STACK_BYTES;

	return (size_t)limit.rlim_cur;
}

void platform_exit(int status) {
	if (!interpreter_thread_started || !pthread_equal(pthread_self(), interpreter_thread))
		exit(status);

	fflush(stdout);
	pthread_mutex_lock(&screen.lock);
	screen.interpreter_status = status;
	screen.interpreter_done = 1;
	pthread_cond_signal(&screen.wake);
	pthread_mutex_unlock(&screen.lock);
	pthread_exit(NULL);
}

int platform_run_main(int argc, char **argv, MainBody body) {
	InterpreterArguments arguments = {.argc = argc, .argv = argv, .body = body};

	sigset_t handled;
	interpreter_signals(&handled);
	pthread_sigmask(SIG_BLOCK, &handled, NULL);

	pthread_attr_t attributes;
	pthread_attr_init(&attributes);
	pthread_attr_setstacksize(&attributes, interpreter_stack_bytes());

	int created = pthread_create(&interpreter_thread, &attributes, interpreter_entry, &arguments);
	pthread_attr_destroy(&attributes);
	if (created != 0) {
		pthread_sigmask(SIG_UNBLOCK, &handled, NULL);
		return body(argc, argv);
	}
	interpreter_thread_started = 1;

	while (1) {
		pthread_mutex_lock(&screen.lock);
		int done = screen.interpreter_done;
		pthread_mutex_unlock(&screen.lock);
		if (done)
			break;

		if (screen_step())
			continue;

		pthread_mutex_lock(&screen.lock);
		if (!screen.frame_ready) {
			struct timespec deadline = deadline_after(SCREEN_PUMP_MICROSECONDS);
			pthread_cond_timedwait(&screen.wake, &screen.lock, &deadline);
		}
		pthread_mutex_unlock(&screen.lock);
	}

	pthread_join(interpreter_thread, NULL);

	if (window) {
		tigrFree(window);
		window = NULL;
	}
	if (screen.canvas) {
		tigrFree(screen.canvas);
		screen.canvas = NULL;
	}
	if (screen.presented) {
		tigrFree(screen.presented);
		screen.presented = NULL;
	}

	return screen.interpreter_status;
}
