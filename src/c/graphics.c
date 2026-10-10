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

typedef struct {
	unsigned char held[GAMEPAD_BUTTON_COUNT];
	unsigned char pressed[GAMEPAD_BUTTON_COUNT];
	float axes[GAMEPAD_AXIS_COUNT];
} GamepadState;

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
	int gamepads_wanted;
	int n_gamepads;
	int frame_n_gamepads;
	GamepadState gamepads[GAMEPAD_CAPACITY];
	GamepadState frame_gamepads[GAMEPAD_CAPACITY];
	int text_scale;
} screen = {
	PTHREAD_MUTEX_INITIALIZER, NULL, SCREEN_DEFAULT_WIDTH, SCREEN_DEFAULT_HEIGHT, 1,
	{255, 255, 255, 255}, {0, 0, 0, 255}, 0, 0, 0, NULL, 0, 0, {0, 0, 0, 1}, 0, 0, 0, 0, 0,
	PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER, NULL, 0, 0, 0,
	{0}, {0}, {0}, {0}, {0}, 0,
	0, 0, 0, {{{0}, {0}, {0}}}, {{{0}, {0}, {0}}},
	1
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

typedef struct {
	double a;
	double b;
	double c;
	double bias;
} DepthPlane;

static struct {
	float *nearness;
	unsigned int *stamps;
	int width;
	int height;
	unsigned int stamp;
} depth = { NULL, NULL, 0, 0, 0 };

static int depth_ready(Tigr *canvas) {
	if (depth.width == canvas->w && depth.height == canvas->h && depth.nearness)
		return 1;
	size_t n_pixels = (size_t)canvas->w * (size_t)canvas->h;
	float *nearness = realloc(depth.nearness, n_pixels * sizeof(float));
	if (!nearness)
		return 0;
	depth.nearness = nearness;
	unsigned int *stamps = realloc(depth.stamps, n_pixels * sizeof(unsigned int));
	if (!stamps)
		return 0;
	depth.stamps = stamps;
	memset(depth.stamps, 0, n_pixels * sizeof(unsigned int));
	depth.width = canvas->w;
	depth.height = canvas->h;
	depth.stamp = 0;
	return 1;
}

static int depth_passes(const DepthPlane *plane, size_t pixel, int column, int row, int writes) {
	if (!plane)
		return 1;
	float nearness = (float)(plane->a * (column + 0.5) + plane->b * (row + 0.5) + plane->c + plane->bias);
	if (depth.stamps[pixel] == depth.stamp && nearness < depth.nearness[pixel])
		return 0;
	if (writes) {
		depth.stamps[pixel] = depth.stamp;
		depth.nearness[pixel] = nearness;
	}
	return 1;
}

static void opaque_line(Tigr *canvas, int x0, int y0, int x1, int y1, TPixel ink, const DepthPlane *plane) {
	int dx = abs(x1 - x0);
	int dy = -abs(y1 - y0);
	int step_x = x0 < x1 ? 1 : -1;
	int step_y = y0 < y1 ? 1 : -1;
	int error = dx + dy;
	for (;;) {
		if (x0 >= 0 && x0 < canvas->w && y0 >= 0 && y0 < canvas->h) {
			size_t pixel = (size_t)y0 * (size_t)canvas->w + (size_t)x0;
			if (depth_passes(plane, pixel, x0, y0, 0))
				canvas->pix[pixel] = ink;
		}
		if (x0 == x1 && y0 == y1)
			break;
		int doubled = 2 * error;
		if (doubled >= dy) {
			error += dy;
			x0 += step_x;
		}
		if (doubled <= dx) {
			error += dx;
			y0 += step_y;
		}
	}
}

static void line_with_ends(Tigr *canvas, int x0, int y0, int x1, int y1, TPixel ink) {
	tigrLine(canvas, x0, y0, x1, y1, ink);
	tigrPlot(canvas, x1, y1, ink);
}

DRAW_WORD(p_plot, "plot", 2,
		tigrPlot(canvas, (int)operand[0], (int)operand[1], ink))
DRAW_WORD(p_line, "line", 4,
		line_with_ends(canvas, (int)operand[0], (int)operand[1], (int)operand[2], (int)operand[3], ink))
DRAW_WORD(p_rect, "rect", 4,
		tigrRect(canvas, (int)operand[0], (int)operand[1], (int)operand[2], (int)operand[3], ink))
DRAW_WORD(p_fill_rect, "fill-rect", 4,
		tigrFillRect(canvas, (int)operand[0] - 1, (int)operand[1] - 1, (int)operand[2] + 2, (int)operand[3] + 2, ink))
static void fill_triangle(Tigr *canvas, double x0, double y0, double x1, double y1, double x2, double y2, TPixel ink) {
	double xs[3] = { x0, x1, x2 };
	double ys[3] = { y0, y1, y2 };
	double top = fmin(y0, fmin(y1, y2));
	double bottom = fmax(y0, fmax(y1, y2));
	int first_row = (int)ceil(top - 0.5);
	int last_row = (int)ceil(bottom - 0.5) - 1;
	if (first_row < 0)
		first_row = 0;
	if (last_row > canvas->h - 1)
		last_row = canvas->h - 1;

	for (int row = first_row; row <= last_row; row++) {
		double centre = row + 0.5;
		double left = INFINITY;
		double right = -INFINITY;
		for (int edge = 0; edge < 3; edge++) {
			double ay = ys[edge];
			double by = ys[(edge + 1) % 3];
			if ((ay <= centre) == (by <= centre))
				continue;
			double ax = xs[edge];
			double bx = xs[(edge + 1) % 3];
			double crossing = ax + (centre - ay) * (bx - ax) / (by - ay);
			left = fmin(left, crossing);
			right = fmax(right, crossing);
		}
		if (left > right)
			continue;
		int first_column = (int)ceil(left - 0.5);
		int last_column = (int)ceil(right - 0.5) - 1;
		if (first_column < 0)
			first_column = 0;
		if (last_column > canvas->w - 1)
			last_column = canvas->w - 1;
		if (last_column < first_column)
			continue;
		tigrFillRect(canvas, first_column - 1, row - 1, last_column - first_column + 3, 3, ink);
	}
}

DRAW_WORD(p_fill_triangle, "fill-triangle", 6,
		fill_triangle(canvas, operand[0], operand[1], operand[2], operand[3], operand[4], operand[5], ink))
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

static int clip_to_canvas(const Tigr *canvas, double *x0, double *y0, double *x1, double *y1) {
	double dx = *x1 - *x0;
	double dy = *y1 - *y0;
	double steps[4] = { -dx, dx, -dy, dy };
	double rooms[4] = { *x0, canvas->w - *x0, *y0, canvas->h - *y0 };
	double entering = 0;
	double leaving = 1;
	for (int side = 0; side < 4; side++) {
		if (steps[side] == 0) {
			if (rooms[side] < 0)
				return 0;
			continue;
		}
		double ratio = rooms[side] / steps[side];
		if (steps[side] < 0)
			entering = fmax(entering, ratio);
		else
			leaving = fmin(leaving, ratio);
	}
	if (entering > leaving)
		return 0;

	double start_x = *x0;
	double start_y = *y0;
	*x0 = start_x + dx * entering;
	*y0 = start_y + dy * entering;
	*x1 = start_x + dx * leaving;
	*y1 = start_y + dy * leaving;
	return 1;
}

static int polygons_valid(Interpreter *interp, int n_points, Object *faces) {
	for (int f = 0; f < faces->len; f++) {
		Val face_val = faces->items[f];
		if (VAL_TAG(face_val) != T_ARRAY) {
			fail(interp, "expected an array of point indices; got %s", tag_name(VAL_TAG(face_val)));
			return 0;
		}
		Object *face = OBJECT_AT(VAL_DATA(face_val));
		for (int k = 0; k < face->len; k++) {
			Val index_val = face->items[k];
			if (VAL_TAG(index_val) != T_FLOAT) {
				fail(interp, "expected a point index; got %s", tag_name(VAL_TAG(index_val)));
				return 0;
			}
			int index = (int)VAL_NUMBER(index_val);
			if (index < 0 || index >= n_points) {
				fail(interp, "point index %d out of bounds (%d points)", index, n_points);
				return 0;
			}
		}
	}
	return 1;
}

static void fill_polygon_corners(Tigr *canvas, const double *xs, const double *ys, const int *corners, int n_corners, TPixel ink,
		const DepthPlane *plane) {
	double crossings[n_corners];
	double top = INFINITY;
	double bottom = -INFINITY;
	for (int k = 0; k < n_corners; k++) {
		top = fmin(top, ys[corners[k]]);
		bottom = fmax(bottom, ys[corners[k]]);
	}
	int first_row = (int)ceil(top - 0.5);
	int last_row = (int)ceil(bottom - 0.5) - 1;
	if (first_row < 0)
		first_row = 0;
	if (last_row > canvas->h - 1)
		last_row = canvas->h - 1;

	for (int row = first_row; row <= last_row; row++) {
		double centre = row + 0.5;
		int n_crossings = 0;
		for (int k = 0; k < n_corners; k++) {
			int a = corners[k];
			int b = corners[(k + 1) % n_corners];
			if ((ys[a] <= centre) == (ys[b] <= centre))
				continue;
			crossings[n_crossings++] = xs[a] + (centre - ys[a]) * (xs[b] - xs[a]) / (ys[b] - ys[a]);
		}
		for (int k = 1; k < n_crossings; k++) {
			double crossing = crossings[k];
			int slot = k;
			while (slot > 0 && crossings[slot - 1] > crossing) {
				crossings[slot] = crossings[slot - 1];
				slot--;
			}
			crossings[slot] = crossing;
		}
		size_t row_start = (size_t)row * (size_t)canvas->w;
		for (int k = 0; k + 1 < n_crossings; k += 2) {
			int first_column = (int)ceil(crossings[k] - 0.5);
			int last_column = (int)ceil(crossings[k + 1] - 0.5) - 1;
			if (first_column < 0)
				first_column = 0;
			if (last_column > canvas->w - 1)
				last_column = canvas->w - 1;
			if (!plane) {
				for (int column = first_column; column <= last_column; column++)
					canvas->pix[row_start + (size_t)column] = ink;
				continue;
			}
			double nearness = plane->a * (first_column + 0.5) + plane->b * centre + plane->c + plane->bias;
			for (int column = first_column; column <= last_column; column++, nearness += plane->a) {
				size_t pixel = row_start + (size_t)column;
				if (depth.stamps[pixel] == depth.stamp && (float)nearness < depth.nearness[pixel])
					continue;
				depth.stamps[pixel] = depth.stamp;
				depth.nearness[pixel] = (float)nearness;
				canvas->pix[pixel] = ink;
			}
		}
	}
}

static void fill_polygon(Tigr *canvas, const double *xs, const double *ys, Object *face, TPixel ink) {
	int n_corners = face->len;
	int corners[n_corners];
	for (int k = 0; k < n_corners; k++)
		corners[k] = (int)VAL_NUMBER(face->items[k]);
	fill_polygon_corners(canvas, xs, ys, corners, n_corners, ink, NULL);
}

static void draw_polygons(Tigr *canvas, const double *xs, const double *ys, Object *faces,
		const double *fills, const double *outlines) {
	for (int f = 0; f < faces->len; f++) {
		Object *face = OBJECT_AT(VAL_DATA(faces->items[f]));
		int n_corners = face->len;
		int finite = n_corners > 0;
		for (int k = 0; k < n_corners && finite; k++) {
			int corner = (int)VAL_NUMBER(face->items[k]);
			finite = isfinite(xs[corner]) && isfinite(ys[corner]);
		}
		if (!finite)
			continue;
		fill_polygon(canvas, xs, ys, face, pixel_from_rgb((unsigned int)fills[f]));
		if (outlines[f] < 0)
			continue;
		TPixel outline = pixel_from_rgb((unsigned int)outlines[f]);
		for (int k = 0; k < n_corners; k++) {
			int a = (int)VAL_NUMBER(face->items[k]);
			int b = (int)VAL_NUMBER(face->items[(k + 1) % n_corners]);
			double x0 = xs[a];
			double y0 = ys[a];
			double x1 = xs[b];
			double y1 = ys[b];
			if (clip_to_canvas(canvas, &x0, &y0, &x1, &y1))
				line_with_ends(canvas, (int)x0, (int)y0, (int)x1, (int)y1, outline);
		}
	}
}

void p_fill_polygons(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 5);
	Val xs_val = chain_sp[-5];
	REQUIRE_CHAIN_TAG(xs_val, T_MATRIX, "fill-polygons", "a matrix of x coordinates");
	Val ys_val = chain_sp[-4];
	REQUIRE_CHAIN_TAG(ys_val, T_MATRIX, "fill-polygons", "a matrix of y coordinates");
	Val faces_val = chain_sp[-3];
	REQUIRE_CHAIN_TAG(faces_val, T_ARRAY, "fill-polygons", "an array of faces");
	Val fills_val = chain_sp[-2];
	REQUIRE_CHAIN_TAG(fills_val, T_MATRIX, "fill-polygons", "a matrix of fill colours");
	Val outlines_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(outlines_val, T_MATRIX, "fill-polygons", "a matrix of outline colours");
	Object *xs = OBJECT_AT(VAL_DATA(xs_val));
	Object *ys = OBJECT_AT(VAL_DATA(ys_val));
	Object *faces = OBJECT_AT(VAL_DATA(faces_val));
	Object *fills = OBJECT_AT(VAL_DATA(fills_val));
	Object *outlines = OBJECT_AT(VAL_DATA(outlines_val));
	int n_points = xs->matrix.rows * xs->matrix.columns;
	int n_ys = ys->matrix.rows * ys->matrix.columns;
	int n_fills = fills->matrix.rows * fills->matrix.columns;
	int n_outlines = outlines->matrix.rows * outlines->matrix.columns;

	if (n_ys != n_points) {
		fail(interp, "expected as many y coordinates as x coordinates; got %d and %d", n_ys, n_points);
		return;
	}
	if (n_fills != faces->len || n_outlines != faces->len) {
		fail(interp, "expected a fill and an outline colour for each of %d faces; got %d and %d", faces->len, n_fills, n_outlines);
		return;
	}
	if (!polygons_valid(interp, n_points, faces))
		return;

	pthread_mutex_lock(&screen.lock);
	Tigr *canvas = canvas_for_drawing(interp);
	if (canvas) {
		draw_polygons(canvas, xs->matrix.elements, ys->matrix.elements, faces, fills->matrix.elements, outlines->matrix.elements);
		screen.requested = 1;
		screen.dirty = 1;
	}
	pthread_mutex_unlock(&screen.lock);
	if (!canvas)
		return;

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 5);
}

#define SOLID_OUTLINE_SPAN 2
#define SOLID_OUTLINE_LEAD 0.002
#define SOLID_UNLIT 0x1000000

typedef struct {
	double depth;
	int face;
} SolidFace;

typedef struct {
	double focal;
	double centre_x;
	double centre_y;
	double near;
} SolidProjection;

typedef struct {
	double eye[3];
	double sun[3];
	double shadow;
	double lighting;
	double kept;
	unsigned int sky;
	double outline_share;
	int grey;
	int depth_tested;
} SolidShading;

static int compare_solid_faces(const void *a, const void *b) {
	const SolidFace *left = a;
	const SolidFace *right = b;
	if (left->depth != right->depth)
		return left->depth > right->depth ? -1 : 1;
	return left->face - right->face;
}

static unsigned int shaded_rgb(unsigned int rgb, double light, const SolidShading *shading) {
	unsigned int channels[3];
	for (int k = 0; k < 3; k++) {
		int shift = 16 - 8 * k;
		double own = trunc((double)((rgb >> shift) & 0xFF) * light);
		double sky = (double)((shading->sky >> shift) & 0xFF);
		channels[k] = (unsigned int)(trunc(own * shading->kept) + trunc(sky * (1 - shading->kept)));
	}
	if (shading->grey) {
		unsigned int level = (unsigned int)trunc(channels[0] * 0.3 + channels[1] * 0.59 + channels[2] * 0.11);
		return level * 65536 + level * 256 + level;
	}
	return channels[0] * 65536 + channels[1] * 256 + channels[2];
}

typedef struct {
	int n_faces;
	int *starts;
	int *corners;
} SolidFaces;

static int solid_faces(Interpreter *interp, Val faces_val, int n_points, SolidFaces *faces) {
	int n_faces = 0;
	int capacity = 0;
	Object *source = OBJECT_AT(VAL_DATA(faces_val));
	if (VAL_TAG(faces_val) == T_ARRAY) {
		if (!polygons_valid(interp, n_points, source))
			return 0;
		n_faces = source->len;
		for (int f = 0; f < n_faces; f++)
			capacity += OBJECT_AT(VAL_DATA(source->items[f]))->len;
	} else if (VAL_TAG(faces_val) == T_MATRIX) {
		n_faces = source->matrix.rows;
		capacity = source->matrix.rows * source->matrix.columns;
	} else {
		fail(interp, "expected an array of faces or a matrix of corner indices; got %s", tag_name(VAL_TAG(faces_val)));
		return 0;
	}

	faces->n_faces = n_faces;
	faces->starts = malloc((size_t)(n_faces + 1) * sizeof(int));
	faces->corners = malloc((size_t)(capacity + 1) * sizeof(int));
	if (!faces->starts || !faces->corners) {
		free(faces->starts);
		free(faces->corners);
		fail(interp, "out of memory");
		return 0;
	}
	int n_corners = 0;
	for (int f = 0; f < n_faces; f++) {
		faces->starts[f] = n_corners;
		if (VAL_TAG(faces_val) == T_ARRAY) {
			Object *face = OBJECT_AT(VAL_DATA(source->items[f]));
			for (int k = 0; k < face->len; k++)
				faces->corners[n_corners++] = (int)VAL_NUMBER(face->items[k]);
			continue;
		}
		for (int k = 0; k < source->matrix.columns; k++) {
			double index = MAT(source, f, k);
			if (index < 0)
				break;
			if (index >= n_points) {
				free(faces->starts);
				free(faces->corners);
				fail(interp, "point index %d out of bounds (%d points)", (int)index, n_points);
				return 0;
			}
			faces->corners[n_corners++] = (int)index;
		}
	}
	faces->starts[n_faces] = n_corners;
	return 1;
}

static void solid_line(Tigr *canvas, const double *xs, const double *ys, const double *aheads, int a, int b, TPixel ink, int tested) {
	double x0 = xs[a];
	double y0 = ys[a];
	double x1 = xs[b];
	double y1 = ys[b];
	if (!clip_to_canvas(canvas, &x0, &y0, &x1, &y1))
		return;
	if (!tested) {
		opaque_line(canvas, (int)x0, (int)y0, (int)x1, (int)y1, ink, NULL);
		return;
	}
	double span_x = xs[b] - xs[a];
	double span_y = ys[b] - ys[a];
	int along_x = fabs(span_x) >= fabs(span_y);
	double share0 = along_x ? (span_x == 0 ? 0 : (x0 - xs[a]) / span_x) : (y0 - ys[a]) / span_y;
	double share1 = along_x ? (span_x == 0 ? 1 : (x1 - xs[a]) / span_x) : (y1 - ys[a]) / span_y;
	double near_a = 1 / aheads[a];
	double near_b = 1 / aheads[b];
	double nearness0 = near_a + (near_b - near_a) * share0;
	double nearness1 = near_a + (near_b - near_a) * share1;
	int column = (int)x0;
	int row = (int)y0;
	int end_column = (int)x1;
	int end_row = (int)y1;
	int dx = abs(end_column - column);
	int dy = -abs(end_row - row);
	int n_steps = dx > -dy ? dx : -dy;
	int step_x = column < end_column ? 1 : -1;
	int step_y = row < end_row ? 1 : -1;
	int error = dx + dy;
	for (int step = 0;; step++) {
		if (column >= 0 && column < canvas->w && row >= 0 && row < canvas->h) {
			size_t pixel = (size_t)row * (size_t)canvas->w + (size_t)column;
			double nearness = n_steps == 0 ? nearness0 : nearness0 + (nearness1 - nearness0) * step / n_steps;
			nearness *= 1 + SOLID_OUTLINE_LEAD;
			if (depth.stamps[pixel] != depth.stamp || (float)nearness >= depth.nearness[pixel])
				canvas->pix[pixel] = ink;
		}
		if (column == end_column && row == end_row)
			break;
		int doubled = 2 * error;
		if (doubled >= dy) {
			error += dy;
			column += step_x;
		}
		if (doubled <= dx) {
			error += dx;
			row += step_y;
		}
	}
}

static int solid_face_plane(const double *xs, const double *ys, const double *aheads, const int *corners, int n_corners,
		DepthPlane *plane) {
	int first = corners[0];
	double best = 0;
	int second = -1;
	int third = -1;
	for (int k = 1; k + 1 < n_corners; k++) {
		int b = corners[k];
		int c = corners[k + 1];
		double area = (xs[b] - xs[first]) * (ys[c] - ys[first]) - (xs[c] - xs[first]) * (ys[b] - ys[first]);
		if (fabs(area) > fabs(best)) {
			best = area;
			second = b;
			third = c;
		}
	}
	if (fabs(best) < 1e-9)
		return 0;
	double w0 = 1 / aheads[first];
	double w1 = 1 / aheads[second];
	double w2 = 1 / aheads[third];
	double x1 = xs[second] - xs[first];
	double y1 = ys[second] - ys[first];
	double x2 = xs[third] - xs[first];
	double y2 = ys[third] - ys[first];
	plane->a = ((w1 - w0) * y2 - (w2 - w0) * y1) / best;
	plane->b = (x1 * (w2 - w0) - x2 * (w1 - w0)) / best;
	plane->c = w0 - plane->a * xs[first] - plane->b * ys[first];
	plane->bias = 0;
	return 1;
}

static int solid_face_convex(const double *xs, const double *ys, const int *corners, int n_corners) {
	int sign = 0;
	for (int k = 0; k < n_corners; k++) {
		int a = corners[k];
		int b = corners[(k + 1) % n_corners];
		int c = corners[(k + 2) % n_corners];
		double turn = (xs[b] - xs[a]) * (ys[c] - ys[b]) - (ys[b] - ys[a]) * (xs[c] - xs[b]);
		if (fabs(turn) < 1e-9)
			continue;
		int turn_sign = turn > 0 ? 1 : -1;
		if (sign && turn_sign != sign)
			return 0;
		sign = turn_sign;
	}
	return 1;
}

static void solid_fill_fan(Tigr *canvas, const double *xs, const double *ys, const double *aheads, const int *corners, int n_corners,
		TPixel ink) {
	for (int k = 1; k + 1 < n_corners; k++) {
		int triangle[3] = { corners[0], corners[k], corners[k + 1] };
		DepthPlane plane;
		if (solid_face_plane(xs, ys, aheads, triangle, 3, &plane))
			fill_polygon_corners(canvas, xs, ys, triangle, 3, ink, &plane);
	}
}

static void draw_solid(Tigr *canvas, Object *points, const SolidFaces *faces, Object *centres, Object *normals, const double *colours,
		const double *leads, Object *camera, const SolidProjection *projection, const SolidShading *shading,
		double *xs, double *ys, double *aheads, SolidFace *order) {
	int n_points = points->matrix.rows;
	int point_columns = points->matrix.columns;
	for (int p = 0; p < n_points; p++) {
		const double *row = points->matrix.elements + (size_t)p * (size_t)point_columns;
		double w = point_columns > 3 ? row[3] : 1;
		double eye_space[3];
		for (int j = 0; j < 3; j++)
			eye_space[j] = row[0] * MAT(camera, 0, j) + row[1] * MAT(camera, 1, j) + row[2] * MAT(camera, 2, j) + w * MAT(camera, 3, j);
		aheads[p] = eye_space[2];
		xs[p] = projection->focal * eye_space[0] / eye_space[2] + projection->centre_x;
		ys[p] = projection->centre_y - projection->focal * eye_space[1] / eye_space[2];
	}

	int n_faces = faces->n_faces;
	int centre_columns = centres->matrix.columns;
	int n_facing = 0;
	for (int f = 0; f < n_faces; f++) {
		const double *centre = centres->matrix.elements + (size_t)f * (size_t)centre_columns;
		const double *normal = normals->matrix.elements + (size_t)f * 3;
		double toward = (shading->eye[0] - centre[0]) * normal[0] + (shading->eye[1] - centre[1]) * normal[1]
				+ (shading->eye[2] - centre[2]) * normal[2];
		int is_line = faces->starts[f + 1] - faces->starts[f] == 2;
		if (toward <= 0 && !is_line)
			continue;
		double w = centre_columns > 3 ? centre[3] : 1;
		double depth = centre[0] * MAT(camera, 0, 2) + centre[1] * MAT(camera, 1, 2) + centre[2] * MAT(camera, 2, 2) + w * MAT(camera, 3, 2);
		order[n_facing].depth = depth - leads[f];
		order[n_facing].face = f;
		n_facing++;
	}
	qsort(order, (size_t)n_facing, sizeof order[0], compare_solid_faces);
	int tested = shading->depth_tested && depth_ready(canvas);
	depth.stamp++;

	for (int k = 0; k < n_facing; k++) {
		int f = order[k].face;
		const int *corners = faces->corners + faces->starts[f];
		int n_corners = faces->starts[f + 1] - faces->starts[f];
		int in_front = n_corners > 0;
		for (int c = 0; c < n_corners && in_front; c++)
			in_front = aheads[corners[c]] >= projection->near;
		if (!in_front)
			continue;
		if (n_corners == 2) {
			TPixel ink = pixel_from_rgb(shaded_rgb((unsigned int)colours[f] & 0xFFFFFF, 1, shading));
			solid_line(canvas, xs, ys, aheads, corners[0], corners[1], ink, tested);
			continue;
		}
		DepthPlane plane;
		DepthPlane *surface = NULL;
		if (tested && solid_face_plane(xs, ys, aheads, corners, n_corners, &plane))
			surface = &plane;
		else if (tested)
			continue;
		const double *normal = normals->matrix.elements + (size_t)f * 3;
		double sunlit = normal[0] * shading->sun[0] + normal[1] * shading->sun[1] + normal[2] * shading->sun[2];
		sunlit = fmin(fmax(sunlit, 0), 1);
		double light = (sunlit * (1 - shading->shadow) + shading->shadow) * shading->lighting;
		unsigned int rgb = (unsigned int)colours[f];
		if (rgb & SOLID_UNLIT) {
			rgb &= 0xFFFFFF;
			light = shading->lighting;
		}
		TPixel fill = pixel_from_rgb(shaded_rgb(rgb, light, shading));
		if (surface && n_corners > 3 && solid_face_convex(xs, ys, corners, n_corners))
			solid_fill_fan(canvas, xs, ys, aheads, corners, n_corners, fill);
		else
			fill_polygon_corners(canvas, xs, ys, corners, n_corners, fill, surface);
		if (shading->outline_share < 0)
			continue;
		DepthPlane raised;
		DepthPlane *edge_surface = NULL;
		if (surface) {
			raised = plane;
			raised.bias = SOLID_OUTLINE_LEAD / aheads[corners[0]];
			edge_surface = &raised;
		}
		double left = INFINITY;
		double right = -INFINITY;
		double top = INFINITY;
		double bottom = -INFINITY;
		for (int c = 0; c < n_corners; c++) {
			left = fmin(left, xs[corners[c]]);
			right = fmax(right, xs[corners[c]]);
			top = fmin(top, ys[corners[c]]);
			bottom = fmax(bottom, ys[corners[c]]);
		}
		if (right - left < SOLID_OUTLINE_SPAN && bottom - top < SOLID_OUTLINE_SPAN)
			continue;
		TPixel outline = pixel_from_rgb(shaded_rgb(rgb, light * shading->outline_share, shading));
		for (int c = 0; c < n_corners; c++) {
			int a = corners[c];
			int b = corners[(c + 1) % n_corners];
			double x0 = xs[a];
			double y0 = ys[a];
			double x1 = xs[b];
			double y1 = ys[b];
			if (clip_to_canvas(canvas, &x0, &y0, &x1, &y1))
				opaque_line(canvas, (int)x0, (int)y0, (int)x1, (int)y1, outline, edge_surface);
		}
	}
}

void p_fill_solid(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 9);
	Val points_val = chain_sp[-9];
	REQUIRE_CHAIN_TAG(points_val, T_MATRIX, "fill-solid", "a matrix of points");
	Val faces_val = chain_sp[-8];
	if (VAL_TAG(faces_val) != T_ARRAY && VAL_TAG(faces_val) != T_MATRIX) {
		fail(interp, "expected an array of faces or a matrix of corner indices; got %s", tag_name(VAL_TAG(faces_val)));
		return;
	}
	Val centres_val = chain_sp[-7];
	REQUIRE_CHAIN_TAG(centres_val, T_MATRIX, "fill-solid", "a matrix of face centres");
	Val normals_val = chain_sp[-6];
	REQUIRE_CHAIN_TAG(normals_val, T_MATRIX, "fill-solid", "a matrix of face normals");
	Val colours_val = chain_sp[-5];
	REQUIRE_CHAIN_TAG(colours_val, T_MATRIX, "fill-solid", "a matrix of face colours");
	Val leads_val = chain_sp[-4];
	REQUIRE_CHAIN_TAG(leads_val, T_MATRIX, "fill-solid", "a matrix of face leads");
	Val camera_val = chain_sp[-3];
	REQUIRE_CHAIN_TAG(camera_val, T_MATRIX, "fill-solid", "a 4x3 camera matrix");
	Val projection_val = chain_sp[-2];
	REQUIRE_CHAIN_TAG(projection_val, T_MATRIX, "fill-solid", "a projection vector");
	Val shading_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(shading_val, T_MATRIX, "fill-solid", "a shading vector");
	Object *points = OBJECT_AT(VAL_DATA(points_val));
	Object *face_source = OBJECT_AT(VAL_DATA(faces_val));
	Object *centres = OBJECT_AT(VAL_DATA(centres_val));
	Object *normals = OBJECT_AT(VAL_DATA(normals_val));
	Object *colours = OBJECT_AT(VAL_DATA(colours_val));
	Object *leads = OBJECT_AT(VAL_DATA(leads_val));
	Object *camera = OBJECT_AT(VAL_DATA(camera_val));
	Object *projection_vector = OBJECT_AT(VAL_DATA(projection_val));
	Object *shading_vector = OBJECT_AT(VAL_DATA(shading_val));
	int n_points = points->matrix.rows;
	int n_faces = VAL_TAG(faces_val) == T_ARRAY ? face_source->len : face_source->matrix.rows;

	if (points->matrix.columns < 3) {
		fail(interp, "expected points of 3 or 4 columns; got %dx%d", points->matrix.rows, points->matrix.columns);
		return;
	}
	if (camera->matrix.rows != 4 || camera->matrix.columns != 3) {
		fail(interp, "expected a 4x3 camera matrix; got %dx%d", camera->matrix.rows, camera->matrix.columns);
		return;
	}
	if (centres->matrix.rows != n_faces || centres->matrix.columns < 3 || normals->matrix.rows != n_faces || normals->matrix.columns != 3) {
		fail(interp, "expected a centre and a normal for each of %d faces; got %dx%d and %dx%d", n_faces,
				centres->matrix.rows, centres->matrix.columns, normals->matrix.rows, normals->matrix.columns);
		return;
	}
	if (colours->matrix.rows * colours->matrix.columns != n_faces || leads->matrix.rows * leads->matrix.columns != n_faces) {
		fail(interp, "expected a colour and a lead for each of %d faces; got %d and %d", n_faces,
				colours->matrix.rows * colours->matrix.columns, leads->matrix.rows * leads->matrix.columns);
		return;
	}
	if (projection_vector->matrix.rows * projection_vector->matrix.columns != 4) {
		fail(interp, "expected a projection of 4 elements; got %d", projection_vector->matrix.rows * projection_vector->matrix.columns);
		return;
	}
	int n_shading = shading_vector->matrix.rows * shading_vector->matrix.columns;
	if (n_shading != 12 && n_shading != 13) {
		fail(interp, "expected a shading of 12 or 13 elements; got %d", n_shading);
		return;
	}
	SolidFaces faces;
	if (!solid_faces(interp, faces_val, n_points, &faces))
		return;

	const double *projected = projection_vector->matrix.elements;
	SolidProjection projection = { .focal = projected[0], .centre_x = projected[1], .centre_y = projected[2], .near = projected[3] };
	const double *shaded = shading_vector->matrix.elements;
	SolidShading shading = {
		.eye = { shaded[0], shaded[1], shaded[2] }, .sun = { shaded[3], shaded[4], shaded[5] },
		.shadow = shaded[6], .lighting = shaded[7], .kept = shaded[8], .sky = (unsigned int)shaded[9],
		.outline_share = shaded[10], .grey = shaded[11] != 0, .depth_tested = n_shading < 13 || shaded[12] != 0
	};
	double *xs = malloc((size_t)(n_points + 1) * sizeof(double));
	double *ys = malloc((size_t)(n_points + 1) * sizeof(double));
	double *aheads = malloc((size_t)(n_points + 1) * sizeof(double));
	SolidFace *order = malloc((size_t)(n_faces + 1) * sizeof(SolidFace));
	if (!xs || !ys || !aheads || !order) {
		free(xs);
		free(ys);
		free(aheads);
		free(order);
		free(faces.starts);
		free(faces.corners);
		fail(interp, "out of memory");
		return;
	}

	pthread_mutex_lock(&screen.lock);
	Tigr *canvas = canvas_for_drawing(interp);
	if (canvas) {
		draw_solid(canvas, points, &faces, centres, normals, colours->matrix.elements, leads->matrix.elements, camera,
				&projection, &shading, xs, ys, aheads, order);
		screen.requested = 1;
		screen.dirty = 1;
	}
	pthread_mutex_unlock(&screen.lock);
	free(xs);
	free(ys);
	free(aheads);
	free(order);
	free(faces.starts);
	free(faces.corners);
	if (!canvas)
		return;

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 9);
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

static void blit_glyph_scaled(Tigr *canvas, TigrGlyph *glyph, int x, int y, TPixel tint, int scale) {
	Tigr *font_bitmap = tfont->bitmap;
	for (int row = 0; row < glyph->h; row++) {
		for (int column = 0; column < glyph->w; column++) {
			TPixel source = tigrGet(font_bitmap, glyph->x + column, glyph->y + row);
			TPixel tinted = tigrRGBA(
				(unsigned char)(tint.r * source.r / 255),
				(unsigned char)(tint.g * source.g / 255),
				(unsigned char)(tint.b * source.b / 255),
				(unsigned char)(tint.a * source.a / 255));
			if (tinted.a == 0)
				continue;
			int block_x = x + column * scale;
			int block_y = y + row * scale;
			for (int dy = 0; dy < scale; dy++)
				for (int dx = 0; dx < scale; dx++)
					tigrPlot(canvas, block_x + dx, block_y + dy, tinted);
		}
	}
}

static int lay_out_text(Tigr *canvas, int x, int y, const char *text, int n_bytes, TPixel start_ink) {
	int scale = screen.text_scale;
	int line_height = tigrTextHeight(tfont, "") * scale;
	TPixel pen = start_ink;
	int pen_x = x;
	int pen_y = y;
	int widest = 0;
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
		if (canvas && scale == 1)
			tigrBlitTint(canvas, tfont->bitmap, pen_x, pen_y, glyph->x, glyph->y, glyph->w, glyph->h, pen);
		else if (canvas)
			blit_glyph_scaled(canvas, glyph, pen_x, pen_y, pen, scale);
		pen_x += glyph->w * scale;
		if (pen_x - x > widest)
			widest = pen_x - x;
		cursor = next;
	}
	return widest;
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
		lay_out_text(canvas, (int)VAL_NUMBER(x_val), (int)VAL_NUMBER(y_val), text->bytes, text->len, screen.ink);
		screen.requested = 1;
		screen.dirty = 1;
	}
	pthread_mutex_unlock(&screen.lock);
	if (!canvas)
		return;

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 3);
}

void p_text_width(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val text_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(text_val, T_STRING, "text-width", "a string");
	Object *text = OBJECT_AT(VAL_DATA(text_val));

	pthread_mutex_lock(&screen.lock);
	int width = lay_out_text(NULL, 0, 0, text->bytes, text->len, screen.ink);
	pthread_mutex_unlock(&screen.lock);

	chain_sp[-1] = make_float((double)width);
	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_text_scale(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val scale_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(scale_val, T_FLOAT, "text-scale", "a scale");
	double scale = VAL_NUMBER(scale_val);

	if (scale < 1 || scale > SCREEN_MAX_ZOOM || scale != (double)(int)scale) {
		fail(interp, "text scale must be an integer in [1, %d]; got %g", SCREEN_MAX_ZOOM, scale);
		return;
	}

	pthread_mutex_lock(&screen.lock);
	screen.text_scale = (int)scale;
	pthread_mutex_unlock(&screen.lock);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
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
		memcpy(screen.frame_gamepads, screen.gamepads, sizeof screen.gamepads);
		screen.frame_n_gamepads = screen.n_gamepads;
		for (int pad = 0; pad < GAMEPAD_CAPACITY; pad++)
			memset(screen.gamepads[pad].pressed, 0, sizeof screen.gamepads[pad].pressed);
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

typedef struct {
	const char *name;
	int index;
} GamepadName;

static const GamepadName gamepad_buttons[] = {
	{"south", GAMEPAD_SOUTH}, {"east", GAMEPAD_EAST}, {"west", GAMEPAD_WEST}, {"north", GAMEPAD_NORTH},
	{"back", GAMEPAD_BACK}, {"guide", GAMEPAD_GUIDE}, {"start", GAMEPAD_START},
	{"left-stick", GAMEPAD_LEFT_STICK}, {"right-stick", GAMEPAD_RIGHT_STICK},
	{"left-shoulder", GAMEPAD_LEFT_SHOULDER}, {"right-shoulder", GAMEPAD_RIGHT_SHOULDER},
	{"dpad-left", GAMEPAD_DPAD_LEFT}, {"dpad-right", GAMEPAD_DPAD_RIGHT},
	{"dpad-up", GAMEPAD_DPAD_UP}, {"dpad-down", GAMEPAD_DPAD_DOWN},
	{"left-trigger", GAMEPAD_LEFT_TRIGGER}, {"right-trigger", GAMEPAD_RIGHT_TRIGGER},
	{NULL, 0}
};

static const GamepadName gamepad_axes[] = {
	{"left-x", GAMEPAD_LEFT_X}, {"left-y", GAMEPAD_LEFT_Y}, {"right-x", GAMEPAD_RIGHT_X}, {"right-y", GAMEPAD_RIGHT_Y},
	{"left-trigger", GAMEPAD_LEFT_TRIGGER_AXIS}, {"right-trigger", GAMEPAD_RIGHT_TRIGGER_AXIS},
	{NULL, 0}
};

static void want_gamepads(void) {
	pthread_mutex_lock(&screen.lock);
	if (!screen.gamepads_wanted) {
		screen.gamepads_wanted = 1;
		pthread_cond_signal(&screen.wake);
	}
	pthread_mutex_unlock(&screen.lock);
}

static int gamepad_name_index(Interpreter *interp, Val name_val, const GamepadName *names, const char *kind) {
	if (VAL_TAG(name_val) != T_SYMBOL) {
		fail(interp, "expected a gamepad %s name (a symbol such as :%s); got %s", kind, names[0].name, tag_name(VAL_TAG(name_val)));
		return -1;
	}
	const char *name = &vocab.symbol_pool[VAL_DATA(name_val)];
	for (int i = 0; names[i].name; i++)
		if (strcmp(names[i].name, name) == 0)
			return names[i].index;
	fail(interp, "unknown gamepad %s :%s", kind, name);
	return -1;
}

static int gamepad_number(Interpreter *interp, Val pad_val) {
	if (VAL_TAG(pad_val) != T_FLOAT) {
		fail(interp, "expected a gamepad number; got %s", tag_name(VAL_TAG(pad_val)));
		return -1;
	}
	double pad = VAL_NUMBER(pad_val);
	if (pad < 0 || pad != (double)(int)pad) {
		fail(interp, "expected a gamepad number (an integer from 0); got %g", pad);
		return -1;
	}
	return (int)pad;
}

void p_gamepads(DISPATCH_ARGS) {
	REQUIRE_STACK_ROOM(interp, chain_ip, chain_sp, 1);
	want_gamepads();

	pthread_mutex_lock(&screen.lock);
	int n_gamepads = screen.frame_n_gamepads;
	pthread_mutex_unlock(&screen.lock);

	chain_sp[0] = make_float((double)n_gamepads);
	DISPATCH_REGISTERS(interp, chain_ip, chain_sp + 1);
}

static int gamepad_button_state(Interpreter *interp, Val *chain_sp, int pressed) {
	int button = gamepad_name_index(interp, chain_sp[-2], gamepad_buttons, "button");
	if (button < 0)
		return -1;
	int pad = gamepad_number(interp, chain_sp[-1]);
	if (pad < 0)
		return -1;
	want_gamepads();

	pthread_mutex_lock(&screen.lock);
	int state = 0;
	if (pad < screen.frame_n_gamepads) {
		GamepadState *gamepad = &screen.frame_gamepads[pad];
		state = pressed ? gamepad->pressed[button] : gamepad->held[button];
	}
	pthread_mutex_unlock(&screen.lock);
	return state;
}

void p_gamepad_down(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	int down = gamepad_button_state(interp, chain_sp, 0);
	if (down < 0)
		return;

	chain_sp[-2] = make_float(down ? 1.0 : 0.0);
	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_gamepad_pressed(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	int pressed = gamepad_button_state(interp, chain_sp, 1);
	if (pressed < 0)
		return;

	chain_sp[-2] = make_float(pressed ? 1.0 : 0.0);
	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_gamepad_axis(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	int axis = gamepad_name_index(interp, chain_sp[-2], gamepad_axes, "axis");
	if (axis < 0)
		return;
	int pad = gamepad_number(interp, chain_sp[-1]);
	if (pad < 0)
		return;
	want_gamepads();

	pthread_mutex_lock(&screen.lock);
	double value = pad < screen.frame_n_gamepads ? (double)screen.frame_gamepads[pad].axes[axis] : 0.0;
	pthread_mutex_unlock(&screen.lock);

	chain_sp[-2] = make_float(value);
	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
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
static int gamepads_opened = 0;

static void gamepads_collect(void) {
	pthread_mutex_lock(&screen.lock);
	int wanted = screen.gamepads_wanted;
	pthread_mutex_unlock(&screen.lock);
	if (!wanted && !window)
		return;

	GamepadReading readings[GAMEPAD_CAPACITY];
	memset(readings, 0, sizeof readings);
	int n_gamepads = gamepads_read(readings, GAMEPAD_CAPACITY);
	gamepads_opened = 1;

	pthread_mutex_lock(&screen.lock);
	for (int pad = 0; pad < GAMEPAD_CAPACITY; pad++) {
		GamepadState *state = &screen.gamepads[pad];
		GamepadReading *reading = &readings[pad];
		for (int button = 0; button < GAMEPAD_BUTTON_COUNT; button++) {
			if (reading->held[button] && !state->held[button])
				state->pressed[button] = 1;
			state->held[button] = reading->held[button];
		}
		memcpy(state->axes, reading->axes, sizeof state->axes);
	}
	screen.n_gamepads = n_gamepads;
	pthread_mutex_unlock(&screen.lock);
}

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

	gamepads_collect();

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
	if (gamepads_opened) {
		gamepads_close();
		gamepads_opened = 0;
	}

	return screen.interpreter_status;
}
