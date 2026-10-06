#include "telic.h"

#if defined(__APPLE__)
#include <dlfcn.h>
#include <objc/objc.h>
#include <objc/message.h>
#include <objc/runtime.h>

#define GAME_CONTROLLER_FRAMEWORK "/System/Library/Frameworks/GameController.framework/GameController"

static Class gamepad_controller_class = NULL;
static int gamepad_framework_missing = 0;

static const struct {
	int button;
	const char *selector;
} gamepad_button_selectors[] = {
	{GAMEPAD_SOUTH, "buttonA"}, {GAMEPAD_EAST, "buttonB"}, {GAMEPAD_WEST, "buttonX"}, {GAMEPAD_NORTH, "buttonY"},
	{GAMEPAD_BACK, "buttonOptions"}, {GAMEPAD_GUIDE, "buttonHome"}, {GAMEPAD_START, "buttonMenu"},
	{GAMEPAD_LEFT_STICK, "leftThumbstickButton"}, {GAMEPAD_RIGHT_STICK, "rightThumbstickButton"},
	{GAMEPAD_LEFT_SHOULDER, "leftShoulder"}, {GAMEPAD_RIGHT_SHOULDER, "rightShoulder"},
	{GAMEPAD_LEFT_TRIGGER, "leftTrigger"}, {GAMEPAD_RIGHT_TRIGGER, "rightTrigger"},
	{0, NULL}
};

static const struct {
	int button;
	const char *selector;
} gamepad_dpad_selectors[] = {
	{GAMEPAD_DPAD_LEFT, "left"}, {GAMEPAD_DPAD_RIGHT, "right"}, {GAMEPAD_DPAD_UP, "up"}, {GAMEPAD_DPAD_DOWN, "down"},
	{0, NULL}
};

static id gamepad_send(id target, const char *selector) {
	if (!target)
		return NULL;

	return ((id (*)(id, SEL))objc_msgSend)(target, sel_registerName(selector));
}

static unsigned char gamepad_pressed(id button) {
	if (!button)
		return 0;

	return (unsigned char)(((BOOL (*)(id, SEL))objc_msgSend)(button, sel_registerName("isPressed")) != 0);
}

static float gamepad_value(id input) {
	if (!input)
		return 0.0f;

	return ((float (*)(id, SEL))objc_msgSend)(input, sel_registerName("value"));
}

static int gamepads_open(void) {
	if (gamepad_controller_class)
		return 1;
	if (gamepad_framework_missing)
		return 0;

	if (!dlopen(GAME_CONTROLLER_FRAMEWORK, RTLD_LAZY) || !(gamepad_controller_class = objc_getClass("GCController"))) {
		gamepad_framework_missing = 1;
		return 0;
	}

	SEL monitor_background = sel_registerName("setShouldMonitorBackgroundEvents:");
	if (class_respondsToSelector(object_getClass((id)gamepad_controller_class), monitor_background))
		((void (*)(id, SEL, BOOL))objc_msgSend)((id)gamepad_controller_class, monitor_background, YES);
	return 1;
}

static void gamepad_read_extended(id pad, GamepadReading *reading) {
	for (int i = 0; gamepad_button_selectors[i].selector; i++)
		reading->held[gamepad_button_selectors[i].button] = gamepad_pressed(gamepad_send(pad, gamepad_button_selectors[i].selector));

	id dpad = gamepad_send(pad, "dpad");
	for (int i = 0; gamepad_dpad_selectors[i].selector; i++)
		reading->held[gamepad_dpad_selectors[i].button] = gamepad_pressed(gamepad_send(dpad, gamepad_dpad_selectors[i].selector));

	id left_stick = gamepad_send(pad, "leftThumbstick");
	id right_stick = gamepad_send(pad, "rightThumbstick");
	reading->axes[GAMEPAD_LEFT_X] = gamepad_value(gamepad_send(left_stick, "xAxis"));
	reading->axes[GAMEPAD_LEFT_Y] = -gamepad_value(gamepad_send(left_stick, "yAxis"));
	reading->axes[GAMEPAD_RIGHT_X] = gamepad_value(gamepad_send(right_stick, "xAxis"));
	reading->axes[GAMEPAD_RIGHT_Y] = -gamepad_value(gamepad_send(right_stick, "yAxis"));
	reading->axes[GAMEPAD_LEFT_TRIGGER_AXIS] = gamepad_value(gamepad_send(pad, "leftTrigger"));
	reading->axes[GAMEPAD_RIGHT_TRIGGER_AXIS] = gamepad_value(gamepad_send(pad, "rightTrigger"));
}

int gamepads_read(GamepadReading *readings, int capacity) {
	if (!gamepads_open())
		return 0;

	id pool = gamepad_send(gamepad_send((id)objc_getClass("NSAutoreleasePool"), "alloc"), "init");
	id controllers = gamepad_send((id)gamepad_controller_class, "controllers");
	long n_controllers = ((long (*)(id, SEL))objc_msgSend)(controllers, sel_registerName("count"));
	SEL object_at_index = sel_registerName("objectAtIndex:");

	int n_gamepads = 0;
	for (long i = 0; i < n_controllers && n_gamepads < capacity; i++) {
		id controller = ((id (*)(id, SEL, unsigned long))objc_msgSend)(controllers, object_at_index, (unsigned long)i);
		id pad = gamepad_send(controller, "extendedGamepad");
		if (!pad)
			continue;
		gamepad_read_extended(pad, &readings[n_gamepads]);
		n_gamepads++;
	}
	gamepad_send(pool, "drain");
	return n_gamepads;
}

void gamepads_close(void) {
}

#elif defined(__linux__)
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/input.h>

#define GAMEPAD_RESCAN_READS 120
#define GAMEPAD_PATH_CAPACITY 64
#define GAMEPAD_BITS_PER_LONG ((int)(sizeof(unsigned long) * 8))

typedef struct {
	int fd;
	char path[GAMEPAD_PATH_CAPACITY];
	int minimum[ABS_CNT];
	int maximum[ABS_CNT];
	GamepadReading reading;
} LinuxGamepad;

static LinuxGamepad gamepad_devices[GAMEPAD_CAPACITY];
static int n_gamepad_devices = 0;
static int gamepad_reads_until_scan = 0;

static int gamepad_button_of(int code) {
	switch (code) {
	case BTN_SOUTH: return GAMEPAD_SOUTH;
	case BTN_EAST: return GAMEPAD_EAST;
	case BTN_WEST: return GAMEPAD_WEST;
	case BTN_NORTH: return GAMEPAD_NORTH;
	case BTN_SELECT: return GAMEPAD_BACK;
	case BTN_MODE: return GAMEPAD_GUIDE;
	case BTN_START: return GAMEPAD_START;
	case BTN_THUMBL: return GAMEPAD_LEFT_STICK;
	case BTN_THUMBR: return GAMEPAD_RIGHT_STICK;
	case BTN_TL: return GAMEPAD_LEFT_SHOULDER;
	case BTN_TR: return GAMEPAD_RIGHT_SHOULDER;
	case BTN_TL2: return GAMEPAD_LEFT_TRIGGER;
	case BTN_TR2: return GAMEPAD_RIGHT_TRIGGER;
	case BTN_DPAD_LEFT: return GAMEPAD_DPAD_LEFT;
	case BTN_DPAD_RIGHT: return GAMEPAD_DPAD_RIGHT;
	case BTN_DPAD_UP: return GAMEPAD_DPAD_UP;
	case BTN_DPAD_DOWN: return GAMEPAD_DPAD_DOWN;
	default: return -1;
	}
}

static int gamepad_has_bit(const unsigned long *bits, int bit) {
	return (int)((bits[bit / GAMEPAD_BITS_PER_LONG] >> (bit % GAMEPAD_BITS_PER_LONG)) & 1UL);
}

static int gamepad_device_open(const char *path) {
	for (int i = 0; i < n_gamepad_devices; i++)
		if (strcmp(gamepad_devices[i].path, path) == 0)
			return 0;
	if (n_gamepad_devices >= GAMEPAD_CAPACITY)
		return 0;

	int fd = open(path, O_RDONLY | O_NONBLOCK);
	if (fd < 0)
		return 0;

	unsigned long key_bits[KEY_CNT / (sizeof(unsigned long) * 8) + 1];
	memset(key_bits, 0, sizeof key_bits);
	if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof key_bits), key_bits) < 0 || !gamepad_has_bit(key_bits, BTN_GAMEPAD)) {
		close(fd);
		return 0;
	}

	LinuxGamepad *device = &gamepad_devices[n_gamepad_devices];
	memset(device, 0, sizeof *device);
	device->fd = fd;
	snprintf(device->path, sizeof device->path, "%s", path);
	for (int axis = 0; axis < ABS_CNT; axis++) {
		struct input_absinfo range;
		if (ioctl(fd, EVIOCGABS(axis), &range) == 0) {
			device->minimum[axis] = range.minimum;
			device->maximum[axis] = range.maximum;
		}
	}
	n_gamepad_devices++;
	return 1;
}

static void gamepad_scan(void) {
	DIR *directory = opendir("/dev/input");
	if (!directory)
		return;

	struct dirent *entry;
	while ((entry = readdir(directory)) != NULL) {
		if (strncmp(entry->d_name, "event", 5) != 0)
			continue;
		char path[GAMEPAD_PATH_CAPACITY];
		snprintf(path, sizeof path, "/dev/input/%s", entry->d_name);
		gamepad_device_open(path);
	}
	closedir(directory);
}

static float gamepad_span(const LinuxGamepad *device, int axis, int value, int centred) {
	int minimum = device->minimum[axis];
	int maximum = device->maximum[axis];
	if (maximum <= minimum)
		return 0.0f;

	float fraction = (float)(value - minimum) / (float)(maximum - minimum);
	return centred ? fraction * 2.0f - 1.0f : fraction;
}

static void gamepad_apply(LinuxGamepad *device, const struct input_event *event) {
	GamepadReading *reading = &device->reading;
	if (event->type == EV_KEY) {
		int button = gamepad_button_of(event->code);
		if (button >= 0)
			reading->held[button] = (unsigned char)(event->value != 0);
		return;
	}
	if (event->type != EV_ABS)
		return;

	switch (event->code) {
	case ABS_X: reading->axes[GAMEPAD_LEFT_X] = gamepad_span(device, ABS_X, event->value, 1); break;
	case ABS_Y: reading->axes[GAMEPAD_LEFT_Y] = gamepad_span(device, ABS_Y, event->value, 1); break;
	case ABS_RX: reading->axes[GAMEPAD_RIGHT_X] = gamepad_span(device, ABS_RX, event->value, 1); break;
	case ABS_RY: reading->axes[GAMEPAD_RIGHT_Y] = gamepad_span(device, ABS_RY, event->value, 1); break;
	case ABS_Z: reading->axes[GAMEPAD_LEFT_TRIGGER_AXIS] = gamepad_span(device, ABS_Z, event->value, 0); break;
	case ABS_RZ: reading->axes[GAMEPAD_RIGHT_TRIGGER_AXIS] = gamepad_span(device, ABS_RZ, event->value, 0); break;
	case ABS_HAT0X:
		reading->held[GAMEPAD_DPAD_LEFT] = (unsigned char)(event->value < 0);
		reading->held[GAMEPAD_DPAD_RIGHT] = (unsigned char)(event->value > 0);
		break;
	case ABS_HAT0Y:
		reading->held[GAMEPAD_DPAD_UP] = (unsigned char)(event->value < 0);
		reading->held[GAMEPAD_DPAD_DOWN] = (unsigned char)(event->value > 0);
		break;
	default: break;
	}
}

static void gamepad_device_close(int index) {
	close(gamepad_devices[index].fd);
	n_gamepad_devices--;
	if (index < n_gamepad_devices)
		memmove(&gamepad_devices[index], &gamepad_devices[index + 1], sizeof gamepad_devices[0] * (size_t)(n_gamepad_devices - index));
}

int gamepads_read(GamepadReading *readings, int capacity) {
	if (gamepad_reads_until_scan-- <= 0) {
		gamepad_scan();
		gamepad_reads_until_scan = GAMEPAD_RESCAN_READS;
	}

	for (int i = 0; i < n_gamepad_devices; i++) {
		LinuxGamepad *device = &gamepad_devices[i];
		struct input_event events[64];
		ssize_t n_bytes;
		while ((n_bytes = read(device->fd, events, sizeof events)) > 0)
			for (int k = 0; k < (int)(n_bytes / (ssize_t)sizeof events[0]); k++)
				gamepad_apply(device, &events[k]);
		if (n_bytes < 0 && errno != EAGAIN) {
			gamepad_device_close(i);
			i--;
		}
	}

	int n_gamepads = MIN(n_gamepad_devices, capacity);
	for (int i = 0; i < n_gamepads; i++)
		readings[i] = gamepad_devices[i].reading;
	return n_gamepads;
}

void gamepads_close(void) {
	while (n_gamepad_devices > 0)
		gamepad_device_close(n_gamepad_devices - 1);
}

#else
int gamepads_read(GamepadReading *readings, int capacity) {
	(void)readings;
	(void)capacity;
	return 0;
}

void gamepads_close(void) {
}
#endif
