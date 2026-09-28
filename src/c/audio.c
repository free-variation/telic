#include "telic.h"
#include <pthread.h>
#include <signal.h>
#include "miniaudio.h"

#define AUDIO_SAMPLE_RATE 48000
#define AUDIO_CHANNELS 2

static ma_device audio_device;
static int audio_device_open;
static int audio_exit_hook_installed;

static void audio_data_callback(ma_device *device, void *output, const void *input, ma_uint32 n_frames) {
	(void)device;
	(void)input;
	synth_device_render((float *)output, (int)n_frames);
}

static void audio_close(void) {
	if (!audio_device_open)
		return;

	ma_device_uninit(&audio_device);
	audio_device_open = 0;
	synth_set_live(0);
}

static int audio_open(Interpreter *interp) {
	if (audio_device_open)
		return 1;

	ma_device_config config = ma_device_config_init(ma_device_type_playback);
	config.playback.format = ma_format_f32;
	config.playback.channels = AUDIO_CHANNELS;
	config.sampleRate = AUDIO_SAMPLE_RATE;
	config.dataCallback = audio_data_callback;

	sigset_t every_signal;
	sigset_t previous_mask;
	sigfillset(&every_signal);
	pthread_sigmask(SIG_BLOCK, &every_signal, &previous_mask);
	synth_set_live(1);
	ma_result initialized = ma_device_init(NULL, &config, &audio_device);
	ma_result started = initialized == MA_SUCCESS ? ma_device_start(&audio_device) : initialized;
	pthread_sigmask(SIG_SETMASK, &previous_mask, NULL);

	if (initialized != MA_SUCCESS) {
		synth_set_live(0);
		fail(interp, "cannot open the audio output device (%s)", ma_result_description(initialized));
		return 0;
	}
	if (started != MA_SUCCESS) {
		ma_device_uninit(&audio_device);
		synth_set_live(0);
		fail(interp, "cannot start the audio output device (%s)", ma_result_description(started));
		return 0;
	}

	audio_device_open = 1;
	if (!audio_exit_hook_installed) {
		atexit(audio_close);
		audio_exit_hook_installed = 1;
	}
	return 1;
}

void p_audio_on(DISPATCH_ARGS) {
	if (!audio_open(interp))
		return;

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_audio_off(DISPATCH_ARGS) {
	audio_close();

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_play(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val samples_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(samples_val, T_MATRIX, "play", "a matrix of samples (nx1 or nx2)");
	Object *samples = OBJECT_AT(VAL_DATA(samples_val));
	int n_frames = samples->matrix.rows;
	int n_columns = samples->matrix.columns;
	if ((n_columns != 1 && n_columns != 2) || n_frames < 1) {
		fail(interp, "expected a matrix of samples (nx1 or nx2, n >= 1); got %dx%d", n_frames, n_columns);
		return;
	}

	float *interleaved;
	MALLOC_OR_FAIL(interp, interleaved, sizeof(float) * 2 * (size_t)n_frames);
	const double *elements = samples->matrix.elements;
	for (int frame = 0; frame < n_frames; frame++) {
		double left = elements[frame * n_columns];
		double right = elements[frame * n_columns + n_columns - 1];
		interleaved[2 * frame] = (float)left;
		interleaved[2 * frame + 1] = (float)right;
	}

	if (!audio_open(interp)) {
		free(interleaved);
		return;
	}
	if (!synth_submit_play(interp, interleaved, n_frames)) {
		free(interleaved);
		return;
	}

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_wait_audio(DISPATCH_ARGS) {
	if (audio_device_open)
		synth_wait_quiet(interp);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}
