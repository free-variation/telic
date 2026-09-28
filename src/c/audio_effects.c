#include "telic.h"

#define EFFECTS_SAMPLE_RATE 48000.0
#define EFFECTS_RAMP_FRAMES 64
#define CHORUS_BUFFER 2048
#define DELAY_BUFFER 131072
#define REVERB_PREDELAY_BUFFER 16384
#define REVERB_MAX_PREDELAY 9600
#define REVERB_DIFFUSER_BUFFER 2048
#define REVERB_MODULATED_BUFFER 4096
#define REVERB_LONG_BUFFER 16384
#define REVERB_DATTORRO_RATE 29761.0
#define REVERB_FREEZE_DECAY 0.9999
#define REVERB_DENORMAL_GUARD 1e-25
#define SHIFTER_BUFFER 4096

typedef enum {
	EFFECT_CHORUS_WET,
	EFFECT_CHORUS_DRY,
	EFFECT_CHORUS_DELAY,
	EFFECT_CHORUS_DEPTH,
	EFFECT_CHORUS_RATE,
	EFFECT_DELAY_WET,
	EFFECT_DELAY_DRY,
	EFFECT_DELAY_TIME,
	EFFECT_DELAY_FEEDBACK,
	EFFECT_DELAY_MODE,
	EFFECT_REVERB_WET,
	EFFECT_REVERB_DRY,
	EFFECT_REVERB_PREDELAY,
	EFFECT_REVERB_BANDWIDTH,
	EFFECT_REVERB_DECAY,
	EFFECT_REVERB_DAMPING,
	EFFECT_REVERB_SIZE,
	EFFECT_REVERB_MODULATION_RATE,
	EFFECT_REVERB_MODULATION_DEPTH,
	EFFECT_REVERB_SHIMMER1_SHIFT,
	EFFECT_REVERB_SHIMMER1_MIX,
	EFFECT_REVERB_SHIMMER2_SHIFT,
	EFFECT_REVERB_SHIMMER2_MIX,
	EFFECT_REVERB_SHIMMER_IN_LOOP,
	EFFECT_REVERB_FREEZE,
	EFFECT_REVERB_WIDTH,
	EFFECT_REVERB_CROSS_FEED,
	EFFECT_REVERB_LOW_CUT,
	EFFECT_REVERB_HIGH_CUT,
	EFFECT_MASTER_LEVEL
} EffectParameter;

typedef enum {
	VALUE_NUMBER,
	VALUE_FLAG,
	VALUE_DELAY_MODE
} EffectValueKind;

typedef enum {
	DELAY_PING_PONG,
	DELAY_STRAIGHT
} DelayMode;

typedef struct {
	const char *effect;
	const char *key;
	EffectParameter parameter;
	EffectValueKind kind;
	double low;
	double high;
} EffectRow;

typedef struct {
	double current;
	double target;
	double step;
	int remaining;
} EffectRamp;

typedef struct {
	EffectRamp wet;
	EffectRamp dry;
	int dormant;
	double delay;
	double depth;
	double rate;
	double buffers[2][CHORUS_BUFFER];
	int write_position;
	double phase;
} Chorus;

typedef struct {
	EffectRamp wet;
	EffectRamp dry;
	int dormant;
	double time;
	double feedback;
	DelayMode mode;
	double buffers[2][DELAY_BUFFER];
	int cursor;
	double position;
} PingPongDelay;

typedef struct {
	double buffer[REVERB_LONG_BUFFER];
	int main_delay;
	int taps[3];
} ReverbDelayLine;

typedef struct {
	double modulated_diffuser[REVERB_MODULATED_BUFFER];
	int modulated_delay;
	double modulated_output;
	ReverbDelayLine pre_damping;
	double damping_state;
	double decay_diffuser[REVERB_LONG_BUFFER];
	int decay_delay;
	int decay_taps[2];
	ReverbDelayLine post_damping;
} ReverbTankHalf;

typedef struct {
	double predelay[REVERB_PREDELAY_BUFFER];
	double bandwidth_state;
	double diffusers[4][REVERB_DIFFUSER_BUFFER];
	int diffuser_delays[4];
	ReverbTankHalf tank[2];
} ReverbChannel;

typedef struct {
	double buffer[SHIFTER_BUFFER];
	int write_position;
	double read_offsets[4];
	double read_slopes[4];
	double ramps[4];
	double ramp_slopes[4];
	int counter;
	int stage;
} PitchShifter;

typedef struct {
	EffectRamp wet;
	EffectRamp dry;
	int dormant;
	double predelay;
	double bandwidth;
	double decay;
	double damping;
	double size;
	double modulation_rate;
	double modulation_depth;
	double shimmer_shifts[2];
	double shimmer_mixes[2];
	int shimmer_in_loop;
	int freeze;
	double width;
	double cross_feed;
	double low_cut;
	double high_cut;
	double smooth_decay;
	double smooth_size;
	double smooth_freeze;
	double modulation_phases[4];
	double shimmer_phase;
	double dc_states[2];
	double low_cut_states[2];
	double high_cut_states[2];
	uint32_t t;
	ReverbChannel channels[2];
	PitchShifter shifters[2][2];
} Reverb;

static Chorus effects_chorus;
static PingPongDelay effects_delay;
static Reverb effects_reverb;
static EffectRamp effects_master_level;

static const double reverb_modulation_ratios[4] = {1.0, 1.5, 1.2, 1.8};
static const int reverb_diffuser_bases[4] = {142, 107, 379, 277};
static const int reverb_modulated_bases[2] = {672, 908};
static const int reverb_pre_damping_bases[2][4] = {{4453, 353, 3627, 1990}, {4217, 266, 2974, 2111}};
static const int reverb_decay_bases[2][3] = {{1800, 187, 1228}, {2656, 335, 1913}};
static const int reverb_post_damping_bases[2][3] = {{3720, 1066, 2673}, {3163, 121, 1996}};

static const EffectRow effects_rows[] = {
	{"chorus", "wet", EFFECT_CHORUS_WET, VALUE_NUMBER, 0, 1},
	{"chorus", "dry", EFFECT_CHORUS_DRY, VALUE_NUMBER, 0, 1},
	{"chorus", "delay", EFFECT_CHORUS_DELAY, VALUE_NUMBER, 0.005, 0.025},
	{"chorus", "depth", EFFECT_CHORUS_DEPTH, VALUE_NUMBER, 0, 0.005},
	{"chorus", "rate", EFFECT_CHORUS_RATE, VALUE_NUMBER, 0.01, 10},
	{"delay", "wet", EFFECT_DELAY_WET, VALUE_NUMBER, 0, 1},
	{"delay", "dry", EFFECT_DELAY_DRY, VALUE_NUMBER, 0, 1},
	{"delay", "time", EFFECT_DELAY_TIME, VALUE_NUMBER, 0.001, 2},
	{"delay", "feedback", EFFECT_DELAY_FEEDBACK, VALUE_NUMBER, 0, 0.99},
	{"delay", "mode", EFFECT_DELAY_MODE, VALUE_DELAY_MODE, 0, 1},
	{"reverb", "wet", EFFECT_REVERB_WET, VALUE_NUMBER, 0, 1},
	{"reverb", "dry", EFFECT_REVERB_DRY, VALUE_NUMBER, 0, 1},
	{"reverb", "predelay", EFFECT_REVERB_PREDELAY, VALUE_NUMBER, 0, 0.2},
	{"reverb", "bandwidth", EFFECT_REVERB_BANDWIDTH, VALUE_NUMBER, 0, 1},
	{"reverb", "decay", EFFECT_REVERB_DECAY, VALUE_NUMBER, 0, 0.99},
	{"reverb", "damping", EFFECT_REVERB_DAMPING, VALUE_NUMBER, 0, 1},
	{"reverb", "size", EFFECT_REVERB_SIZE, VALUE_NUMBER, 0.1, 2},
	{"reverb", "modulation-rate", EFFECT_REVERB_MODULATION_RATE, VALUE_NUMBER, 0, 10},
	{"reverb", "modulation-depth", EFFECT_REVERB_MODULATION_DEPTH, VALUE_NUMBER, 0, 1},
	{"reverb", "shimmer1-shift", EFFECT_REVERB_SHIMMER1_SHIFT, VALUE_NUMBER, -24, 24},
	{"reverb", "shimmer1-mix", EFFECT_REVERB_SHIMMER1_MIX, VALUE_NUMBER, 0, 1},
	{"reverb", "shimmer2-shift", EFFECT_REVERB_SHIMMER2_SHIFT, VALUE_NUMBER, -24, 24},
	{"reverb", "shimmer2-mix", EFFECT_REVERB_SHIMMER2_MIX, VALUE_NUMBER, 0, 1},
	{"reverb", "shimmer-in-loop", EFFECT_REVERB_SHIMMER_IN_LOOP, VALUE_FLAG, 0, 1},
	{"reverb", "freeze", EFFECT_REVERB_FREEZE, VALUE_FLAG, 0, 1},
	{"reverb", "width", EFFECT_REVERB_WIDTH, VALUE_NUMBER, 0, 2},
	{"reverb", "cross-feed", EFFECT_REVERB_CROSS_FEED, VALUE_NUMBER, 0, 1},
	{"reverb", "low-cut", EFFECT_REVERB_LOW_CUT, VALUE_NUMBER, 0, 1000},
	{"reverb", "high-cut", EFFECT_REVERB_HIGH_CUT, VALUE_NUMBER, 1000, 24000},
	{"master", "level", EFFECT_MASTER_LEVEL, VALUE_NUMBER, 0, 1},
	{NULL, NULL, EFFECT_MASTER_LEVEL, VALUE_NUMBER, 0, 0}
};

static void effect_ramp_set(EffectRamp *ramp, double value) {
	ramp->current = value;
	ramp->target = value;
	ramp->remaining = 0;
}

static void effect_ramp_start(EffectRamp *ramp, double target) {
	ramp->target = target;
	ramp->step = (target - ramp->current) / EFFECTS_RAMP_FRAMES;
	ramp->remaining = EFFECTS_RAMP_FRAMES;
}

static void effect_ramp_advance(EffectRamp *ramp) {
	if (ramp->remaining == 0)
		return;
	ramp->current += ramp->step;
	if (--ramp->remaining == 0)
		ramp->current = ramp->target;
}

static int effect_ramp_silent(const EffectRamp *ramp) {
	return ramp->remaining == 0 && ramp->current == 0.0;
}

static double ring_read(const double *buffer, int size, uint32_t t, int delay) {
	return buffer[(t - (uint32_t)delay) & (uint32_t)(size - 1)];
}

static void ring_write(double *buffer, int size, uint32_t t, double value) {
	buffer[t & (uint32_t)(size - 1)] = value;
}

static double allpass(double *buffer, int size, uint32_t t, int delay, double gain, double input) {
	double delayed = ring_read(buffer, size, t, delay);
	double written = input - delayed * gain;
	ring_write(buffer, size, t, written);
	return delayed + written * gain;
}

static double soft_limit(double x) {
	return x * (27.0 + x * x) / (27.0 + 9.0 * x * x);
}

static double cubic_read(const double *buffer, int size, double position) {
	while (position < 0.0)
		position += size;
	int index = (int)position;
	double fraction = position - index;
	int i1 = index % size;
	int i0 = i1 == 0 ? size - 1 : i1 - 1;
	int i2 = (i1 + 1) % size;
	int i3 = (i2 + 1) % size;
	double y0 = buffer[i0];
	double y1 = buffer[i1];
	double y2 = buffer[i2];
	double y3 = buffer[i3];
	double c1 = 0.5 * (y2 - y0);
	double c2 = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3;
	double c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
	return ((c3 * fraction + c2) * fraction + c1) * fraction + y1;
}

static int scaled_delay(int base, double size) {
	return (int)(base * size);
}

static int sample_rate_delay(int dattorro_samples) {
	return (int)(dattorro_samples * EFFECTS_SAMPLE_RATE / REVERB_DATTORRO_RATE);
}

static void chorus_clear(Chorus *chorus) {
	memset(chorus->buffers, 0, sizeof(chorus->buffers));
	chorus->write_position = 0;
	chorus->phase = 0.0;
}

static double chorus_tap(const double *buffer, int write_position, double delay_samples) {
	double position = write_position - delay_samples;
	while (position < 0.0)
		position += CHORUS_BUFFER;
	int index = (int)position;
	double fraction = position - index;
	double early = buffer[index % CHORUS_BUFFER];
	double late = buffer[(index + 1) % CHORUS_BUFFER];
	return early + (late - early) * fraction;
}

static void chorus_process(Chorus *chorus, double *left, double *right) {
	effect_ramp_advance(&chorus->wet);
	effect_ramp_advance(&chorus->dry);
	double dry = chorus->dry.current;
	if (effect_ramp_silent(&chorus->wet)) {
		chorus->dormant = 1;
		*left *= dry;
		*right *= dry;
		return;
	}
	if (chorus->dormant) {
		chorus_clear(chorus);
		chorus->dormant = 0;
	}

	double wet = chorus->wet.current;
	double inputs[2] = {*left, *right};
	double outputs[2];
	for (int channel = 0; channel < 2; channel++) {
		chorus->buffers[channel][chorus->write_position] = inputs[channel];
		double sweep = sin(2.0 * M_PI * (chorus->phase + 0.25 * channel));
		double delay_samples = (chorus->delay + chorus->depth * sweep) * EFFECTS_SAMPLE_RATE;
		double delayed = chorus_tap(chorus->buffers[channel], chorus->write_position, delay_samples);
		outputs[channel] = inputs[channel] * dry + delayed * wet;
	}
	chorus->write_position = (chorus->write_position + 1) % CHORUS_BUFFER;
	chorus->phase += chorus->rate / EFFECTS_SAMPLE_RATE;
	if (chorus->phase >= 1.0)
		chorus->phase -= 1.0;
	*left = outputs[0];
	*right = outputs[1];
}

static int delay_target_frames(const PingPongDelay *delay) {
	return (int)lround(delay->time * EFFECTS_SAMPLE_RATE);
}

static void delay_clear(PingPongDelay *delay) {
	memset(delay->buffers, 0, sizeof(delay->buffers));
	delay->cursor = 0;
	delay->position = delay_target_frames(delay);
}

static void delay_process(PingPongDelay *delay, double *left, double *right) {
	effect_ramp_advance(&delay->wet);
	effect_ramp_advance(&delay->dry);
	double dry = delay->dry.current;
	if (effect_ramp_silent(&delay->wet)) {
		delay->dormant = 1;
		*left *= dry;
		*right *= dry;
		return;
	}
	if (delay->dormant) {
		delay_clear(delay);
		delay->dormant = 0;
	}

	double target = delay_target_frames(delay);
	if (delay->position < target)
		delay->position = MIN(delay->position + 1.0, target);
	else if (delay->position > target)
		delay->position = MAX(delay->position - 1.0, target);
	int delay_frames = (int)delay->position;
	int read_position = (delay->cursor + DELAY_BUFFER - delay_frames) % DELAY_BUFFER;
	double delayed_left = delay->buffers[0][read_position];
	double delayed_right = delay->buffers[1][read_position];
	double feedback = delay->feedback;
	if (delay->mode == DELAY_PING_PONG) {
		delay->buffers[0][delay->cursor] = (*left + *right) * 0.5 + delayed_right * feedback;
		delay->buffers[1][delay->cursor] = delayed_left;
	} else {
		delay->buffers[0][delay->cursor] = *left + delayed_left * feedback;
		delay->buffers[1][delay->cursor] = *right + delayed_right * feedback;
	}
	delay->cursor = (delay->cursor + 1) % DELAY_BUFFER;

	double wet = delay->wet.current;
	*left = *left * dry + delayed_left * wet;
	*right = *right * dry + delayed_right * wet;
}

static void delay_line_prepare(ReverbDelayLine *line, const int *bases, int n_taps) {
	line->main_delay = sample_rate_delay(bases[0]);
	for (int tap = 0; tap < n_taps; tap++)
		line->taps[tap] = sample_rate_delay(bases[tap + 1]);
}

static void shifter_clear(PitchShifter *shifter) {
	int frame_size = SHIFTER_BUFFER / 2;
	double slope = 2.0 / frame_size;
	memset(shifter, 0, sizeof(PitchShifter));
	for (int grain = 0; grain < 4; grain++)
		shifter->read_offsets[grain] = 2.0;
	shifter->ramps[0] = 1.0;
	shifter->ramps[1] = 0.5;
	shifter->ramps[3] = 0.5;
	shifter->ramp_slopes[0] = -slope;
	shifter->ramp_slopes[1] = slope;
	shifter->ramp_slopes[2] = slope;
	shifter->ramp_slopes[3] = -slope;
	shifter->counter = frame_size >> 3;
}

static double shifter_process(PitchShifter *shifter, double input, double semitones, double mix) {
	int mask = SHIFTER_BUFFER - 1;
	shifter->buffer[shifter->write_position & mask] = input;
	if (semitones == 0.0 || mix == 0.0) {
		shifter->write_position++;
		return input;
	}

	int frame_size = SHIFTER_BUFFER / 2;
	double slope = 2.0 / frame_size;
	double ratio_excess = pow(2.0, semitones / 12.0) - 1.0;
	if (shifter->counter == 0) {
		shifter->counter = frame_size >> 2;
		shifter->stage = (shifter->stage + 1) & 3;
		int grain = shifter->stage;
		shifter->read_slopes[grain] = -ratio_excess;
		shifter->read_offsets[grain] = ratio_excess < 0.0 ? 2.0 : frame_size * ratio_excess + 2.0;
		shifter->ramps[grain] = 0.0;
		shifter->ramp_slopes[grain] = slope;
		shifter->ramp_slopes[(grain + 2) & 3] = -slope;
	}

	double shifted = 0.0;
	for (int grain = 0; grain < 4; grain++) {
		shifter->read_offsets[grain] += shifter->read_slopes[grain];
		double position = (double)(shifter->write_position & mask) - shifter->read_offsets[grain];
		shifted += cubic_read(shifter->buffer, SHIFTER_BUFFER, position) * shifter->ramps[grain];
		shifter->ramps[grain] += shifter->ramp_slopes[grain];
		shifter->ramps[grain] = shifter->ramps[grain] < 0.0 ? 0.0 : shifter->ramps[grain] > 1.0 ? 1.0 : shifter->ramps[grain];
	}
	shifter->write_position++;
	shifter->counter--;
	return input + (shifted - input) * mix;
}

static void reverb_clear(Reverb *reverb) {
	memset(reverb->channels, 0, sizeof(reverb->channels));
	for (int c = 0; c < 2; c++) {
		ReverbChannel *channel = &reverb->channels[c];
		for (int d = 0; d < 4; d++)
			channel->diffuser_delays[d] = sample_rate_delay(reverb_diffuser_bases[d]);
		for (int h = 0; h < 2; h++) {
			ReverbTankHalf *half = &channel->tank[h];
			half->modulated_delay = sample_rate_delay(reverb_modulated_bases[h]);
			delay_line_prepare(&half->pre_damping, reverb_pre_damping_bases[h], 3);
			half->decay_delay = sample_rate_delay(reverb_decay_bases[h][0]);
			half->decay_taps[0] = sample_rate_delay(reverb_decay_bases[h][1]);
			half->decay_taps[1] = sample_rate_delay(reverb_decay_bases[h][2]);
			delay_line_prepare(&half->post_damping, reverb_post_damping_bases[h], 2);
		}
		shifter_clear(&reverb->shifters[c][0]);
		shifter_clear(&reverb->shifters[c][1]);
	}
	reverb->smooth_decay = reverb->decay;
	reverb->smooth_size = reverb->size;
	reverb->smooth_freeze = reverb->freeze ? 1.0 : 0.0;
	for (int j = 0; j < 4; j++)
		reverb->modulation_phases[j] = 0.25 * j;
	reverb->shimmer_phase = 0.0;
	for (int c = 0; c < 2; c++) {
		reverb->dc_states[c] = 0.0;
		reverb->low_cut_states[c] = 0.0;
		reverb->high_cut_states[c] = 0.0;
	}
	reverb->t = 0;
}

typedef struct {
	double decay;
	double damping;
	double size;
	int modulation_offsets[2];
	int shimmer_active;
	double shimmer_mix;
	double shimmer_crossfade;
	double shimmer_positions[2];
} ReverbFrameContext;

static void tank_half_process(ReverbTankHalf *half, uint32_t t, double input, const ReverbFrameContext *context, int modulation_offset) {
	double size = context->size;
	int modulated_delay = scaled_delay(half->modulated_delay, size) + modulation_offset;
	modulated_delay = modulated_delay < 1 ? 1 : modulated_delay;
	double x = allpass(half->modulated_diffuser, REVERB_MODULATED_BUFFER, t, modulated_delay, 0.70, input);
	half->modulated_output = x;

	ring_write(half->pre_damping.buffer, REVERB_LONG_BUFFER, t, x);
	x = ring_read(half->pre_damping.buffer, REVERB_LONG_BUFFER, t, scaled_delay(half->pre_damping.main_delay, size));
	half->damping_state += context->damping * (x - half->damping_state);
	x = soft_limit(half->damping_state + REVERB_DENORMAL_GUARD) * context->decay;
	x = allpass(half->decay_diffuser, REVERB_LONG_BUFFER, t, scaled_delay(half->decay_delay, size), 0.50, x);
	ring_write(half->post_damping.buffer, REVERB_LONG_BUFFER, t, x);
}

static double tank_tap(const ReverbDelayLine *line, uint32_t t, int tap, double size) {
	return ring_read(line->buffer, REVERB_LONG_BUFFER, t, scaled_delay(tap, size));
}

static double tank_output(const ReverbTankHalf *near, const ReverbTankHalf *far, uint32_t t, double size) {
	return near->modulated_output
		+ tank_tap(&near->pre_damping, t, near->pre_damping.taps[0], size)
		+ tank_tap(&near->pre_damping, t, near->pre_damping.taps[1], size)
		- ring_read(near->decay_diffuser, REVERB_LONG_BUFFER, t, scaled_delay(near->decay_taps[1], size))
		+ tank_tap(&near->post_damping, t, near->post_damping.taps[1], size)
		- tank_tap(&far->pre_damping, t, far->pre_damping.taps[2], size)
		- ring_read(far->decay_diffuser, REVERB_LONG_BUFFER, t, scaled_delay(far->decay_taps[0], size))
		+ tank_tap(&far->post_damping, t, far->post_damping.taps[0], size);
}

static double tank_feedback(const ReverbTankHalf *source, uint32_t t, const ReverbFrameContext *context) {
	double size = context->size;
	int post_delay = scaled_delay(source->post_damping.main_delay, size);
	double feedback = ring_read(source->post_damping.buffer, REVERB_LONG_BUFFER, t, post_delay);
	if (context->shimmer_active) {
		double base = (double)((t - (uint32_t)post_delay) & (uint32_t)(REVERB_LONG_BUFFER - 1));
		double first = cubic_read(source->post_damping.buffer, REVERB_LONG_BUFFER, base - context->shimmer_positions[0]);
		double second = cubic_read(source->post_damping.buffer, REVERB_LONG_BUFFER, base - context->shimmer_positions[1]);
		double shifted = first * context->shimmer_crossfade + second * (1.0 - context->shimmer_crossfade);
		feedback = feedback * (1.0 - context->shimmer_mix) + shifted * context->shimmer_mix;
	}
	return soft_limit(feedback) * context->decay + REVERB_DENORMAL_GUARD;
}

static void channel_process(ReverbChannel *channel, uint32_t t, double input, int predelay_samples, double bandwidth,
		const ReverbFrameContext *context, double *left, double *right) {
	double size = context->size;
	ring_write(channel->predelay, REVERB_PREDELAY_BUFFER, t, input);
	double x = ring_read(channel->predelay, REVERB_PREDELAY_BUFFER, t, predelay_samples);
	channel->bandwidth_state += bandwidth * (x - channel->bandwidth_state);
	x = channel->bandwidth_state;
	x = allpass(channel->diffusers[0], REVERB_DIFFUSER_BUFFER, t, scaled_delay(channel->diffuser_delays[0], size), 0.75, x);
	x = allpass(channel->diffusers[1], REVERB_DIFFUSER_BUFFER, t, scaled_delay(channel->diffuser_delays[1], size), 0.75, x);
	x = allpass(channel->diffusers[2], REVERB_DIFFUSER_BUFFER, t, scaled_delay(channel->diffuser_delays[2], size), 0.625, x);
	x = allpass(channel->diffusers[3], REVERB_DIFFUSER_BUFFER, t, scaled_delay(channel->diffuser_delays[3], size), 0.625, x);

	double feedback_into_first = tank_feedback(&channel->tank[1], t, context);
	double feedback_into_second = tank_feedback(&channel->tank[0], t, context);
	tank_half_process(&channel->tank[0], t, x + feedback_into_first, context, context->modulation_offsets[0]);
	tank_half_process(&channel->tank[1], t, x + feedback_into_second, context, context->modulation_offsets[1]);

	*left = tank_output(&channel->tank[1], &channel->tank[0], t, size);
	*right = tank_output(&channel->tank[0], &channel->tank[1], t, size);
}

static void reverb_process(Reverb *reverb, double *left, double *right) {
	effect_ramp_advance(&reverb->wet);
	effect_ramp_advance(&reverb->dry);
	double dry = reverb->dry.current;
	if (effect_ramp_silent(&reverb->wet)) {
		reverb->dormant = 1;
		*left *= dry;
		*right *= dry;
		return;
	}
	if (reverb->dormant) {
		reverb_clear(reverb);
		reverb->dormant = 0;
	}

	int predelay_samples = (int)(reverb->predelay * EFFECTS_SAMPLE_RATE);
	predelay_samples = predelay_samples < 1 ? 1 : predelay_samples > REVERB_MAX_PREDELAY ? REVERB_MAX_PREDELAY : predelay_samples;
	double bandwidth = 0.1 + reverb->bandwidth * 0.89;
	double base_damping = 0.99 - reverb->damping * 0.89;

	double freeze_target = reverb->freeze ? 1.0 : 0.0;
	double freeze_coefficient = reverb->freeze ? 0.002 : 0.0005;
	reverb->smooth_freeze += freeze_coefficient * (freeze_target - reverb->smooth_freeze);
	reverb->smooth_decay += 0.0001 * (reverb->decay - reverb->smooth_decay);
	double unfrozen = 1.0 - reverb->smooth_freeze;
	double input_gain = reverb->freeze ? 0.0 : unfrozen * unfrozen;
	double input_left = *left * input_gain;
	double input_right = *right * input_gain;

	ReverbFrameContext context = {
		.decay = reverb->smooth_decay + (REVERB_FREEZE_DECAY - reverb->smooth_decay) * reverb->smooth_freeze,
		.damping = base_damping + (0.9 - base_damping) * reverb->smooth_freeze,
		.size = reverb->smooth_size,
	};
	int modulation_offsets[4];
	for (int j = 0; j < 4; j++) {
		reverb->modulation_phases[j] += reverb->modulation_rate * reverb_modulation_ratios[j] / EFFECTS_SAMPLE_RATE;
		if (reverb->modulation_phases[j] >= 1.0)
			reverb->modulation_phases[j] -= 1.0;
		modulation_offsets[j] = (int)(sin(2.0 * M_PI * reverb->modulation_phases[j]) * reverb->modulation_depth * 16.0);
	}

	double shimmer_mix = reverb->shimmer_mixes[0];
	context.shimmer_active = reverb->shimmer_in_loop && shimmer_mix > 0.0;
	if (context.shimmer_active && !reverb->freeze) {
		double ratio = pow(2.0, reverb->shimmer_shifts[0] / 12.0);
		double window = 128.0 + (3410.0 - 128.0) * reverb->smooth_size;
		reverb->shimmer_phase += (1.0 - ratio) / window;
		reverb->shimmer_phase -= floor(reverb->shimmer_phase);
		context.shimmer_crossfade = 0.5 - 0.5 * cos(2.0 * M_PI * reverb->shimmer_phase);
		context.shimmer_positions[0] = reverb->shimmer_phase * window;
		context.shimmer_positions[1] = fmod(context.shimmer_positions[0] + window * 0.5, window);
	}
	context.shimmer_mix = shimmer_mix;
	reverb->smooth_size += 0.001 * (reverb->size - reverb->smooth_size);

	double wet_left0;
	double wet_right0;
	double wet_left1;
	double wet_right1;
	uint32_t t = reverb->t;
	context.modulation_offsets[0] = modulation_offsets[0];
	context.modulation_offsets[1] = modulation_offsets[1];
	channel_process(&reverb->channels[0], t, input_left + input_right * reverb->cross_feed, predelay_samples, bandwidth,
		&context, &wet_left0, &wet_right0);
	context.modulation_offsets[0] = modulation_offsets[2];
	context.modulation_offsets[1] = modulation_offsets[3];
	channel_process(&reverb->channels[1], t, input_right + input_left * reverb->cross_feed, predelay_samples, bandwidth,
		&context, &wet_left1, &wet_right1);

	double wet_channels[2] = {(wet_left0 + wet_left1) * 0.5, (wet_right0 + wet_right1) * 0.5};
	for (int c = 0; c < 2; c++) {
		if (!reverb->shimmer_in_loop)
			wet_channels[c] = shifter_process(&reverb->shifters[c][0], wet_channels[c], reverb->shimmer_shifts[0], reverb->shimmer_mixes[0]);
		wet_channels[c] = shifter_process(&reverb->shifters[c][1], wet_channels[c], reverb->shimmer_shifts[1], reverb->shimmer_mixes[1]);
	}

	double width = reverb->width;
	double mid = (wet_channels[0] + wet_channels[1]) * 0.5;
	double side = (wet_channels[0] - wet_channels[1]) * 0.5 * width;
	double width_compensation = 1.0 / sqrt((1.0 + width * width) * 0.5);
	wet_channels[0] = (mid + side) * width_compensation;
	wet_channels[1] = (mid - side) * width_compensation;

	double dc_coefficient = exp(-2.0 * M_PI * 20.0 / EFFECTS_SAMPLE_RATE);
	double low_cut_coefficient = 1.0 - 2.0 * M_PI * reverb->low_cut / EFFECTS_SAMPLE_RATE;
	double high_cut_coefficient = 1.0 - exp(-2.0 * M_PI * reverb->high_cut / EFFECTS_SAMPLE_RATE);
	for (int c = 0; c < 2; c++) {
		reverb->dc_states[c] = dc_coefficient * reverb->dc_states[c] + (1.0 - dc_coefficient) * wet_channels[c];
		wet_channels[c] -= reverb->dc_states[c];
		if (reverb->low_cut > 0.0) {
			reverb->low_cut_states[c] = low_cut_coefficient * reverb->low_cut_states[c] + (1.0 - low_cut_coefficient) * wet_channels[c];
			wet_channels[c] -= reverb->low_cut_states[c];
		}
		if (reverb->high_cut < EFFECTS_SAMPLE_RATE * 0.5) {
			reverb->high_cut_states[c] += high_cut_coefficient * (wet_channels[c] - reverb->high_cut_states[c]);
			wet_channels[c] = reverb->high_cut_states[c];
		}
	}

	double wet = reverb->wet.current;
	*left = *left * dry + wet_channels[0] * wet;
	*right = *right * dry + wet_channels[1] * wet;
	reverb->t++;
}

void effects_reset(void) {
	Chorus *chorus = &effects_chorus;
	effect_ramp_set(&chorus->wet, 0.0);
	effect_ramp_set(&chorus->dry, 1.0);
	chorus->delay = 0.015;
	chorus->depth = 0.003;
	chorus->rate = 0.8;
	chorus->dormant = 1;

	PingPongDelay *delay = &effects_delay;
	effect_ramp_set(&delay->wet, 0.0);
	effect_ramp_set(&delay->dry, 1.0);
	delay->time = 0.375;
	delay->feedback = 0.5;
	delay->mode = DELAY_PING_PONG;
	delay->dormant = 1;

	Reverb *reverb = &effects_reverb;
	effect_ramp_set(&reverb->wet, 0.0);
	effect_ramp_set(&reverb->dry, 1.0);
	reverb->predelay = 0.02;
	reverb->bandwidth = 0.7;
	reverb->decay = 0.5;
	reverb->damping = 0.5;
	reverb->size = 1.0;
	reverb->modulation_rate = 0.5;
	reverb->modulation_depth = 0.5;
	reverb->shimmer_shifts[0] = 0.0;
	reverb->shimmer_shifts[1] = 0.0;
	reverb->shimmer_mixes[0] = 0.0;
	reverb->shimmer_mixes[1] = 0.0;
	reverb->shimmer_in_loop = 0;
	reverb->freeze = 0;
	reverb->width = 1.0;
	reverb->cross_feed = 0.15;
	reverb->low_cut = 80.0;
	reverb->high_cut = 12000.0;
	reverb->dormant = 1;

	effect_ramp_set(&effects_master_level, 1.0);
}

void effects_clear(void) {
	effects_chorus.dormant = 1;
	effects_delay.dormant = 1;
	effects_reverb.dormant = 1;
}

void effects_process(double *left, double *right) {
	chorus_process(&effects_chorus, left, right);
	delay_process(&effects_delay, left, right);
	reverb_process(&effects_reverb, left, right);
	effect_ramp_advance(&effects_master_level);
	double level = effects_master_level.current;
	*left *= level;
	*right *= level;
}

void effects_apply(int parameter, double value) {
	Chorus *chorus = &effects_chorus;
	PingPongDelay *delay = &effects_delay;
	Reverb *reverb = &effects_reverb;
	switch ((EffectParameter)parameter) {
	case EFFECT_CHORUS_WET:
		effect_ramp_start(&chorus->wet, value);
		break;
	case EFFECT_CHORUS_DRY:
		effect_ramp_start(&chorus->dry, value);
		break;
	case EFFECT_CHORUS_DELAY:
		chorus->delay = value;
		break;
	case EFFECT_CHORUS_DEPTH:
		chorus->depth = value;
		break;
	case EFFECT_CHORUS_RATE:
		chorus->rate = value;
		break;
	case EFFECT_DELAY_WET:
		effect_ramp_start(&delay->wet, value);
		break;
	case EFFECT_DELAY_DRY:
		effect_ramp_start(&delay->dry, value);
		break;
	case EFFECT_DELAY_TIME:
		delay->time = value;
		break;
	case EFFECT_DELAY_FEEDBACK:
		delay->feedback = value;
		break;
	case EFFECT_DELAY_MODE:
		delay->mode = (DelayMode)(int)value;
		break;
	case EFFECT_REVERB_WET:
		effect_ramp_start(&reverb->wet, value);
		break;
	case EFFECT_REVERB_DRY:
		effect_ramp_start(&reverb->dry, value);
		break;
	case EFFECT_REVERB_PREDELAY:
		reverb->predelay = value;
		break;
	case EFFECT_REVERB_BANDWIDTH:
		reverb->bandwidth = value;
		break;
	case EFFECT_REVERB_DECAY:
		reverb->decay = value;
		break;
	case EFFECT_REVERB_DAMPING:
		reverb->damping = value;
		break;
	case EFFECT_REVERB_SIZE:
		reverb->size = value;
		break;
	case EFFECT_REVERB_MODULATION_RATE:
		reverb->modulation_rate = value;
		break;
	case EFFECT_REVERB_MODULATION_DEPTH:
		reverb->modulation_depth = value;
		break;
	case EFFECT_REVERB_SHIMMER1_SHIFT:
		reverb->shimmer_shifts[0] = value;
		break;
	case EFFECT_REVERB_SHIMMER1_MIX:
		reverb->shimmer_mixes[0] = value;
		break;
	case EFFECT_REVERB_SHIMMER2_SHIFT:
		reverb->shimmer_shifts[1] = value;
		break;
	case EFFECT_REVERB_SHIMMER2_MIX:
		reverb->shimmer_mixes[1] = value;
		break;
	case EFFECT_REVERB_SHIMMER_IN_LOOP:
		reverb->shimmer_in_loop = (int)value;
		break;
	case EFFECT_REVERB_FREEZE:
		reverb->freeze = (int)value;
		break;
	case EFFECT_REVERB_WIDTH:
		reverb->width = value;
		break;
	case EFFECT_REVERB_CROSS_FEED:
		reverb->cross_feed = value;
		break;
	case EFFECT_REVERB_LOW_CUT:
		reverb->low_cut = value;
		break;
	case EFFECT_REVERB_HIGH_CUT:
		reverb->high_cut = value;
		break;
	case EFFECT_MASTER_LEVEL:
		effect_ramp_start(&effects_master_level, value);
		break;
	}
}

static const char *effect_symbol_text(Val symbol_val) {
	return &vocab.symbol_pool[VAL_DATA(symbol_val)];
}

static int effect_value(Interpreter *interp, const EffectRow *row, Val value_val, double *value) {
	if (row->kind == VALUE_DELAY_MODE) {
		const char *mode = VAL_TAG(value_val) == T_SYMBOL ? effect_symbol_text(value_val) : NULL;
		if (mode && strcmp(mode, "ping-pong") == 0) {
			*value = DELAY_PING_PONG;
			return 1;
		}
		if (mode && strcmp(mode, "straight") == 0) {
			*value = DELAY_STRAIGHT;
			return 1;
		}
		if (mode)
			fail(interp, "expected :mode :ping-pong or :straight; got :%s", mode);
		else
			fail(interp, "expected :mode :ping-pong or :straight; got %s", tag_name(VAL_TAG(value_val)));
		return 0;
	}
	if (VAL_TAG(value_val) != T_FLOAT) {
		fail(interp, "expected a float for :%s; got %s", row->key, tag_name(VAL_TAG(value_val)));
		return 0;
	}
	double number = VAL_NUMBER(value_val);
	if (row->kind == VALUE_FLAG && number != 0.0 && number != 1.0) {
		fail(interp, "expected :%s 0 or 1; got %g", row->key, number);
		return 0;
	}
	if (!(number >= row->low && number <= row->high)) {
		fail(interp, "expected :%s in [%g, %g]; got %g", row->key, row->low, row->high, number);
		return 0;
	}
	*value = number;
	return 1;
}

int effects_parameter_parse(Interpreter *interp, Val value_val, Val key_val, Val effect_val, int *parameter, double *value) {
	if (VAL_TAG(effect_val) != T_SYMBOL) {
		fail(interp, "expected an effect (:chorus :delay :reverb :master); got %s", tag_name(VAL_TAG(effect_val)));
		return 0;
	}
	const char *effect = effect_symbol_text(effect_val);
	int row = 0;
	while (effects_rows[row].effect && strcmp(effects_rows[row].effect, effect) != 0)
		row++;
	if (!effects_rows[row].effect) {
		fail(interp, "expected an effect (:chorus :delay :reverb :master); got :%s", effect);
		return 0;
	}
	if (VAL_TAG(key_val) != T_SYMBOL) {
		fail(interp, "expected a :%s key; got %s", effect, tag_name(VAL_TAG(key_val)));
		return 0;
	}
	const char *key = effect_symbol_text(key_val);
	while (effects_rows[row].effect && strcmp(effects_rows[row].effect, effect) == 0 && strcmp(effects_rows[row].key, key) != 0)
		row++;
	if (!effects_rows[row].effect || strcmp(effects_rows[row].effect, effect) != 0) {
		fail(interp, "unknown :%s key :%s", effect, key);
		return 0;
	}
	if (!effect_value(interp, &effects_rows[row], value_val, value))
		return 0;
	*parameter = (int)effects_rows[row].parameter;
	return 1;
}
