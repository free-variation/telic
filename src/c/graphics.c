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
} screen = {
	PTHREAD_MUTEX_INITIALIZER, NULL, SCREEN_DEFAULT_WIDTH, SCREEN_DEFAULT_HEIGHT, 1,
	{255, 255, 255, 255}, {0, 0, 0, 255}, 0, 0, 0, NULL, 0, 0, {0, 0, 0, 1}, 0, 0, 0, 0, 0,
	PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER, NULL, 0, 0, 0
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
		tigrFillRect(canvas, (int)operand[0], (int)operand[1], (int)operand[2], (int)operand[3], ink))
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

static Tigr *window = NULL;
static pthread_t interpreter_thread;
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
	if (!busy && !resized && !window_needs_front && !application_drain_events())
		return 0;

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
