#include "telic.h"
#include <stdatomic.h>
#include <time.h>

#define SYNTH_SAMPLE_RATE 48000
#define SYNTH_QUEUE_CAPACITY 4096
#define SYNTH_MAX_PLAYS 64
#define SYNTH_DEVICE_CHUNK 512
#define SYNTH_WAIT_NANOSECONDS 1000000
#define SYNTH_MAX_OSCILLATORS 8
#define SYNTH_MAX_MODULATIONS 64
#define SYNTH_MAX_INSTRUMENTS 16
#define SYNTH_MAX_VOICES 32
#define SYNTH_MAX_UNISON 7
#define SYNTH_MAX_RENDER_SECONDS 600
#define SYNTH_MAX_STAGE_SECONDS 60
#define SYNTH_MAX_DEPTH 100
#define SYNTH_SILENT_LEVEL 1e-4
#define SYNTH_RAMP_FRAMES 64
#define SYNTH_MIN_CUTOFF 20.0
#define SYNTH_MAX_CUTOFF 20000.0
#define SYNTH_MAX_SCHEDULED 8192
#define SYNTH_SCHEDULE_LEAD 2048
#define SYNTH_WHOLE_NOTE_BASE 11520000000LL
#define SYNTH_DEFAULT_ARTICULATION 0.875
#define MOOG_VT 0.312
#define MOOG_INPUT_SCALE 0.5
#define MOOG_OVERSAMPLE 2

typedef enum {
	WAVE_SINE,
	WAVE_TRIANGLE,
	WAVE_SAW,
	WAVE_PULSE,
	WAVE_WHITE,
	WAVE_PINK,
	WAVE_BROWN
} SynthWave;

typedef enum {
	LFO_SINE,
	LFO_TRIANGLE,
	LFO_SAW,
	LFO_SQUARE,
	LFO_SAMPLE_AND_HOLD
} LfoShape;

typedef enum {
	STAGE_ATTACK,
	STAGE_DECAY,
	STAGE_SUSTAIN,
	STAGE_RELEASE,
	STAGE_IDLE
} EnvelopeStage;

typedef enum {
	PARAMETER_LEVEL,
	PARAMETER_PAN,
	PARAMETER_CUTOFF,
	PARAMETER_RESONANCE,
	PARAMETER_DRIVE,
	PARAMETER_OSCILLATOR_LEVEL,
	PARAMETER_OSCILLATOR_RATIO,
	PARAMETER_OSCILLATOR_WIDTH
} SynthParameter;

typedef struct {
	double attack;
	double decay;
	double sustain;
	double release;
	double sustain_decay;
} EnvelopeShape;

typedef struct {
	SynthWave wave;
	double ratio;
	double fixed_hz;
	double detune_cents;
	double level;
	double width;
	double lfo_level;
	double lfo_width;
	EnvelopeShape envelope;
} SynthOscillator;

typedef struct {
	int source;
	int target;
	double depth;
} SynthModulation;

typedef struct {
	LfoShape shape;
	double rate;
	double delay;
	double fade;
	int key_sync;
	double pitch_depth;
	double cutoff_depth;
	double pan_depth;
} SynthLfo;

typedef struct {
	int n_oscillators;
	SynthOscillator oscillators[SYNTH_MAX_OSCILLATORS];
	int n_modulations;
	SynthModulation modulations[SYNTH_MAX_MODULATIONS];
	unsigned carrier_mask;
	double level;
	double pan;
	double velocity_sensitivity;
	int n_unison;
	double unison_detune;
	double unison_spread;
	int filter_on;
	double cutoff;
	double resonance;
	double drive;
	EnvelopeShape filter_envelope;
	double filter_amount;
	SynthLfo lfo;
	double glide;
} SynthPatch;

typedef struct {
	double attack_step;
	double decay_factor;
	double sustain_factor;
	double release_factor;
	double sustain;
} EnvelopeRates;

typedef struct {
	double current;
	double target;
	double step;
	int remaining;
} SynthRamp;

typedef struct {
	double v[4][2];
	double dv[4][2];
	double tv[4][2];
	double previous_input[2];
} MoogState;

typedef struct {
	int active;
	int instrument;
	double note;
	int gate;
	long started;
	long released;
	SynthPatch patch;
	double note_hz;
	double velocity_gain;
	double filter_amount;
	int n_copies;
	double copy_ratios[SYNTH_MAX_UNISON];
	double copy_pan_offsets[SYNTH_MAX_UNISON];
	double copy_left[SYNTH_MAX_UNISON];
	double copy_right[SYNTH_MAX_UNISON];
	double copy_scale;
	int stereo;
	double phases[SYNTH_MAX_UNISON][SYNTH_MAX_OSCILLATORS];
	double outputs[SYNTH_MAX_UNISON][SYNTH_MAX_OSCILLATORS];
	uint64_t noise_states[SYNTH_MAX_UNISON][SYNTH_MAX_OSCILLATORS];
	double pink_states[SYNTH_MAX_UNISON][SYNTH_MAX_OSCILLATORS][3];
	double brown_states[SYNTH_MAX_UNISON][SYNTH_MAX_OSCILLATORS];
	double detune_factors[SYNTH_MAX_OSCILLATORS];
	int n_sources[SYNTH_MAX_OSCILLATORS];
	int sources[SYNTH_MAX_OSCILLATORS][SYNTH_MAX_OSCILLATORS];
	double depths[SYNTH_MAX_OSCILLATORS][SYNTH_MAX_OSCILLATORS];
	EnvelopeStage stages[SYNTH_MAX_OSCILLATORS];
	double levels[SYNTH_MAX_OSCILLATORS];
	EnvelopeRates rates[SYNTH_MAX_OSCILLATORS];
	EnvelopeStage filter_stage;
	double filter_level;
	EnvelopeRates filter_rates;
	MoogState moog;
	SynthRamp level_ramp;
	SynthRamp pan_ramp;
	SynthRamp cutoff_ramp;
	SynthRamp resonance_ramp;
	SynthRamp drive_ramp;
	SynthRamp oscillator_level_ramps[SYNTH_MAX_OSCILLATORS];
	SynthRamp oscillator_ratio_ramps[SYNTH_MAX_OSCILLATORS];
	SynthRamp oscillator_width_ramps[SYNTH_MAX_OSCILLATORS];
	int n_ramping;
	double glide_offset;
	double glide_step;
	long glide_remaining;
	double lfo_phase;
	double lfo_held;
	long lfo_age;
	uint64_t lfo_noise;
	int pan_moving;
} SynthVoice;

typedef enum {
	COMMAND_NOTE_ON,
	COMMAND_NOTE_OFF,
	COMMAND_SILENCE,
	COMMAND_PLAY,
	COMMAND_PARAMETER
} SynthCommandKind;

typedef struct {
	SynthCommandKind kind;
	int instrument;
	double note;
	double velocity;
	uint64_t seed;
	float *samples;
	int n_frames;
	SynthParameter parameter;
	int oscillator_index;
	double value;
	int scheduled;
	long at;
} SynthCommand;

typedef struct {
	int64_t numerator;
	int64_t denominator;
} SampleTime;

typedef struct {
	float *samples;
	int n_frames;
	int position;
} SynthPlay;

static SynthPatch synth_instruments[SYNTH_MAX_INSTRUMENTS];
static double synth_last_notes[SYNTH_MAX_INSTRUMENTS];
static SynthVoice synth_voices[SYNTH_MAX_VOICES];
static int synth_instruments_ready;
static long synth_clock;
static SynthPlay synth_plays[SYNTH_MAX_PLAYS];
static int synth_n_plays;

static atomic_int synth_live;
static SynthCommand synth_commands[SYNTH_QUEUE_CAPACITY];
static atomic_int synth_command_head;
static atomic_int synth_command_tail;
static float *synth_finished[SYNTH_QUEUE_CAPACITY];
static atomic_int synth_finished_head;
static atomic_int synth_finished_tail;
static SynthPatch synth_pending_patches[SYNTH_MAX_INSTRUMENTS];
static atomic_int synth_pending_ready[SYNTH_MAX_INSTRUMENTS];
static atomic_int synth_sounding;
static atomic_long synth_n_device_renders;
static SynthCommand synth_schedule[SYNTH_MAX_SCHEDULED];
static int synth_schedule_start;
static int synth_n_scheduled;
static atomic_long synth_n_scheduled_applied;
static atomic_long synth_published_clock;
static long synth_n_scheduled_submitted;
static SampleTime synth_instrument_cursors[SYNTH_MAX_INSTRUMENTS];
static int64_t synth_tempo_milli = 120000;
static double synth_articulation = SYNTH_DEFAULT_ARTICULATION;
static double synth_velocity = 1.0;
static int synth_current_instrument;

static void envelope_shape_default(EnvelopeShape *shape) {
	shape->attack = 0.005;
	shape->decay = 0.1;
	shape->sustain = 1.0;
	shape->release = 0.1;
	shape->sustain_decay = 0.0;
}

static void oscillator_default(SynthOscillator *oscillator) {
	memset(oscillator, 0, sizeof(SynthOscillator));
	oscillator->wave = WAVE_SINE;
	oscillator->ratio = 1.0;
	oscillator->level = 1.0;
	oscillator->width = 0.5;
	envelope_shape_default(&oscillator->envelope);
}

static void patch_default(SynthPatch *patch) {
	memset(patch, 0, sizeof(SynthPatch));
	patch->n_oscillators = 1;
	oscillator_default(&patch->oscillators[0]);
	patch->carrier_mask = 1;
	patch->level = 1.0;
	patch->velocity_sensitivity = 1.0;
	patch->n_unison = 1;
	patch->cutoff = SYNTH_MAX_CUTOFF;
	patch->drive = 1.0;
	envelope_shape_default(&patch->filter_envelope);
	patch->lfo.shape = LFO_SINE;
	patch->lfo.rate = 5.0;
	patch->lfo.key_sync = 1;
}

static void synth_ensure_ready(void) {
	if (synth_instruments_ready)
		return;

	for (int instrument = 0; instrument < SYNTH_MAX_INSTRUMENTS; instrument++) {
		patch_default(&synth_instruments[instrument]);
		synth_last_notes[instrument] = -1.0;
	}
	synth_instruments_ready = 1;
}

static const char *symbol_text(cell symbol) {
	return &vocab.symbol_pool[symbol];
}

static int key_allowed(cell key, const char *const *allowed_keys) {
	const char *name = symbol_text(key);
	for (int i = 0; allowed_keys[i]; i++)
		if (strcmp(name, allowed_keys[i]) == 0)
			return 1;
	return 0;
}

static int frame_keys_allowed(Interpreter *interp, Object *frame, const char *const *allowed_keys, const char *what) {
	for (int i = 0; i < frame->len; i++) {
		cell key = frame->frame.keys[i];
		if (!key_allowed(key, allowed_keys)) {
			fail(interp, "unknown %s key :%s", what, symbol_text(key));
			return 0;
		}
	}
	return 1;
}

static int frame_value(Interpreter *interp, Object *frame, const char *key, Val *value) {
	cell symbol = (cell)intern_symbol(interp, key);
	FRAME_LOOKUP(frame, symbol, at, present);
	if (present)
		*value = frame->frame.values[at];
	return present;
}

static int number_in_range(Interpreter *interp, Val value, const char *key, double low, double high, double *target) {
	if (VAL_TAG(value) != T_FLOAT) {
		fail(interp, "expected a float for :%s; got %s", key, tag_name(VAL_TAG(value)));
		return 0;
	}
	double number = VAL_NUMBER(value);
	if (!(number >= low && number <= high)) {
		fail(interp, "expected :%s in [%g, %g]; got %g", key, low, high, number);
		return 0;
	}
	*target = number;
	return 1;
}

static int frame_number(Interpreter *interp, Object *frame, const char *key, double low, double high, double *target) {
	Val value;
	if (!frame_value(interp, frame, key, &value))
		return 1;
	return number_in_range(interp, value, key, low, high, target);
}

static int frame_integer(Interpreter *interp, Object *frame, const char *key, int low, int high, int *target) {
	double number = *target;
	if (!frame_number(interp, frame, key, low, high, &number))
		return 0;
	if (number != floor(number)) {
		fail(interp, "expected an integer for :%s; got %g", key, number);
		return 0;
	}
	*target = (int)number;
	return 1;
}

static int frame_index(Interpreter *interp, Val value, int n_oscillators, const char *what) {
	if (VAL_TAG(value) != T_FLOAT) {
		fail(interp, "expected an oscillator index in %s; got %s", what, tag_name(VAL_TAG(value)));
		return -1;
	}
	double number = VAL_NUMBER(value);
	if (!(number >= 0 && number < n_oscillators) || number != floor(number)) {
		fail(interp, "expected an oscillator index in %s in [0, %d); got %g", what, n_oscillators, number);
		return -1;
	}
	return (int)number;
}

static int frame_choice(Interpreter *interp, Object *frame, const char *key, const char *const *names, int *target) {
	Val value;
	if (!frame_value(interp, frame, key, &value))
		return 1;

	if (VAL_TAG(value) == T_SYMBOL) {
		const char *name = symbol_text(VAL_DATA(value));
		for (int i = 0; names[i]; i++)
			if (strcmp(names[i], name) == 0) {
				*target = i;
				return 1;
			}
	}
	char listing[160] = "";
	for (int i = 0; names[i]; i++) {
		strncat(listing, " :", sizeof listing - strlen(listing) - 1);
		strncat(listing, names[i], sizeof listing - strlen(listing) - 1);
	}
	if (VAL_TAG(value) == T_SYMBOL)
		fail(interp, "expected :%s one of%s; got :%s", key, listing, symbol_text(VAL_DATA(value)));
	else
		fail(interp, "expected :%s one of%s; got %s", key, listing, tag_name(VAL_TAG(value)));
	return 0;
}

static const char *const synth_wave_names[] = {
	"sine", "triangle", "saw", "pulse", "white", "pink", "brown", NULL
};

static const char *const synth_lfo_shape_names[] = {
	"sine", "triangle", "saw", "square", "sample-and-hold", NULL
};

static const char *const synth_oscillator_keys[] = {
	"wave", "ratio", "fixed-hz", "detune", "level", "width", "lfo-level", "lfo-width",
	"attack", "decay", "sustain", "release", "sustain-decay", NULL
};

static const char *const synth_envelope_keys[] = {
	"attack", "decay", "sustain", "release", "sustain-decay", "amount", NULL
};

static const char *const synth_lfo_keys[] = {
	"shape", "rate", "delay", "fade", "key-sync", "pitch-depth", "cutoff-depth", "pan-depth", NULL
};

static const char *const synth_patch_keys[] = {
	"oscillators", "modulation", "carriers", "level", "pan", "velocity-sensitivity",
	"unison", "unison-detune", "unison-spread", "cutoff", "resonance", "drive",
	"filter-envelope", "lfo", "glide", NULL
};

static int envelope_from_frame(Interpreter *interp, Object *frame, EnvelopeShape *envelope) {
	return frame_number(interp, frame, "attack", 0, SYNTH_MAX_STAGE_SECONDS, &envelope->attack)
		&& frame_number(interp, frame, "decay", 0, SYNTH_MAX_STAGE_SECONDS, &envelope->decay)
		&& frame_number(interp, frame, "sustain", 0, 1, &envelope->sustain)
		&& frame_number(interp, frame, "release", 0, SYNTH_MAX_STAGE_SECONDS, &envelope->release)
		&& frame_number(interp, frame, "sustain-decay", 0, SYNTH_MAX_STAGE_SECONDS, &envelope->sustain_decay);
}

static int oscillator_from_frame(Interpreter *interp, Val oscillator_val, SynthOscillator *oscillator) {
	oscillator_default(oscillator);
	if (VAL_TAG(oscillator_val) != T_FRAME) {
		fail(interp, "expected an oscillator frame; got %s", tag_name(VAL_TAG(oscillator_val)));
		return 0;
	}
	Object *frame = OBJECT_AT(VAL_DATA(oscillator_val));
	if (!frame_keys_allowed(interp, frame, synth_oscillator_keys, "oscillator"))
		return 0;

	int wave = (int)oscillator->wave;
	if (!frame_choice(interp, frame, "wave", synth_wave_names, &wave))
		return 0;
	oscillator->wave = (SynthWave)wave;

	return frame_number(interp, frame, "ratio", 1.0 / 64, 64, &oscillator->ratio)
		&& frame_number(interp, frame, "fixed-hz", 0, SYNTH_SAMPLE_RATE / 2.0, &oscillator->fixed_hz)
		&& frame_number(interp, frame, "detune", -1200, 1200, &oscillator->detune_cents)
		&& frame_number(interp, frame, "level", 0, 1, &oscillator->level)
		&& frame_number(interp, frame, "width", 0.01, 0.99, &oscillator->width)
		&& frame_number(interp, frame, "lfo-level", 0, 1, &oscillator->lfo_level)
		&& frame_number(interp, frame, "lfo-width", 0, 0.49, &oscillator->lfo_width)
		&& envelope_from_frame(interp, frame, &oscillator->envelope);
}

static int modulations_from_value(Interpreter *interp, Val modulation_val, SynthPatch *patch) {
	if (VAL_TAG(modulation_val) != T_ARRAY) {
		fail(interp, "expected an array of [ from to depth ] for :modulation; got %s", tag_name(VAL_TAG(modulation_val)));
		return 0;
	}
	Object *triples = OBJECT_AT(VAL_DATA(modulation_val));
	int n_triples = triples->len;
	if (n_triples > SYNTH_MAX_MODULATIONS) {
		fail(interp, "too many :modulation entries (max %d)", SYNTH_MAX_MODULATIONS);
		return 0;
	}

	for (int i = 0; i < n_triples; i++) {
		Val triple_val = triples->items[i];
		if (VAL_TAG(triple_val) != T_ARRAY || OBJECT_AT(VAL_DATA(triple_val))->len != 3) {
			fail(interp, "expected [ from to depth ] in :modulation; got %s", tag_name(VAL_TAG(triple_val)));
			return 0;
		}
		Object *triple = OBJECT_AT(VAL_DATA(triple_val));
		int source = frame_index(interp, triple->items[0], patch->n_oscillators, ":modulation");
		if (source < 0)
			return 0;
		int target = frame_index(interp, triple->items[1], patch->n_oscillators, ":modulation");
		if (target < 0)
			return 0;
		Val depth_val = triple->items[2];
		if (VAL_TAG(depth_val) != T_FLOAT || !(fabs(VAL_NUMBER(depth_val)) <= SYNTH_MAX_DEPTH)) {
			fail(interp, "expected a :modulation depth in [%d, %d]", -SYNTH_MAX_DEPTH, SYNTH_MAX_DEPTH);
			return 0;
		}
		patch->modulations[i].source = source;
		patch->modulations[i].target = target;
		patch->modulations[i].depth = VAL_NUMBER(depth_val);
	}
	patch->n_modulations = n_triples;
	return 1;
}

static int carriers_from_value(Interpreter *interp, Val carriers_val, SynthPatch *patch) {
	if (VAL_TAG(carriers_val) != T_ARRAY) {
		fail(interp, "expected an array of oscillator indices for :carriers; got %s", tag_name(VAL_TAG(carriers_val)));
		return 0;
	}
	Object *carriers = OBJECT_AT(VAL_DATA(carriers_val));
	unsigned carrier_mask = 0;
	for (int i = 0; i < carriers->len; i++) {
		int carrier = frame_index(interp, carriers->items[i], patch->n_oscillators, ":carriers");
		if (carrier < 0)
			return 0;
		carrier_mask |= 1u << carrier;
	}
	if (!carrier_mask) {
		fail(interp, "expected at least one carrier in :carriers");
		return 0;
	}
	patch->carrier_mask = carrier_mask;
	return 1;
}

static int subframe(Interpreter *interp, Object *frame, const char *key, const char *const *allowed_keys, Object **subframe_out) {
	Val value;
	*subframe_out = NULL;
	if (!frame_value(interp, frame, key, &value))
		return 1;
	if (VAL_TAG(value) != T_FRAME) {
		fail(interp, "expected a frame for :%s; got %s", key, tag_name(VAL_TAG(value)));
		return 0;
	}
	*subframe_out = OBJECT_AT(VAL_DATA(value));
	return frame_keys_allowed(interp, *subframe_out, allowed_keys, key);
}

static int lfo_from_frame(Interpreter *interp, Object *frame, SynthLfo *lfo) {
	int shape = (int)lfo->shape;
	double key_sync = lfo->key_sync;
	if (!frame_choice(interp, frame, "shape", synth_lfo_shape_names, &shape))
		return 0;
	lfo->shape = (LfoShape)shape;
	if (!frame_number(interp, frame, "key-sync", 0, 1, &key_sync))
		return 0;
	lfo->key_sync = key_sync != 0;
	return frame_number(interp, frame, "rate", 0.01, 100, &lfo->rate)
		&& frame_number(interp, frame, "delay", 0, SYNTH_MAX_STAGE_SECONDS, &lfo->delay)
		&& frame_number(interp, frame, "fade", 0, SYNTH_MAX_STAGE_SECONDS, &lfo->fade)
		&& frame_number(interp, frame, "pitch-depth", 0, 24, &lfo->pitch_depth)
		&& frame_number(interp, frame, "cutoff-depth", 0, 8, &lfo->cutoff_depth)
		&& frame_number(interp, frame, "pan-depth", 0, 1, &lfo->pan_depth);
}

static int patch_from_frame(Interpreter *interp, Object *frame, SynthPatch *patch) {
	patch_default(patch);
	if (!frame_keys_allowed(interp, frame, synth_patch_keys, "patch"))
		return 0;

	Val oscillators_val;
	if (frame_value(interp, frame, "oscillators", &oscillators_val)) {
		if (VAL_TAG(oscillators_val) != T_ARRAY) {
			fail(interp, "expected an array of oscillator frames for :oscillators; got %s", tag_name(VAL_TAG(oscillators_val)));
			return 0;
		}
		Object *oscillators = OBJECT_AT(VAL_DATA(oscillators_val));
		int n_oscillators = oscillators->len;
		if (n_oscillators < 1 || n_oscillators > SYNTH_MAX_OSCILLATORS) {
			fail(interp, "expected 1 to %d oscillators; got %d", SYNTH_MAX_OSCILLATORS, n_oscillators);
			return 0;
		}
		for (int i = 0; i < n_oscillators; i++)
			if (!oscillator_from_frame(interp, oscillators->items[i], &patch->oscillators[i]))
				return 0;
		patch->n_oscillators = n_oscillators;
	}

	Val modulation_val;
	if (frame_value(interp, frame, "modulation", &modulation_val) && !modulations_from_value(interp, modulation_val, patch))
		return 0;
	Val carriers_val;
	if (frame_value(interp, frame, "carriers", &carriers_val) && !carriers_from_value(interp, carriers_val, patch))
		return 0;
	Val cutoff_val;
	patch->filter_on = frame_value(interp, frame, "cutoff", &cutoff_val);

	Object *filter_envelope;
	if (!subframe(interp, frame, "filter-envelope", synth_envelope_keys, &filter_envelope))
		return 0;
	if (filter_envelope && (!envelope_from_frame(interp, filter_envelope, &patch->filter_envelope)
			|| !frame_number(interp, filter_envelope, "amount", -10, 10, &patch->filter_amount)))
		return 0;
	Object *lfo;
	if (!subframe(interp, frame, "lfo", synth_lfo_keys, &lfo))
		return 0;
	if (lfo && !lfo_from_frame(interp, lfo, &patch->lfo))
		return 0;

	return frame_number(interp, frame, "level", 0, 1, &patch->level)
		&& frame_number(interp, frame, "pan", -1, 1, &patch->pan)
		&& frame_number(interp, frame, "velocity-sensitivity", 0, 1, &patch->velocity_sensitivity)
		&& frame_integer(interp, frame, "unison", 1, SYNTH_MAX_UNISON, &patch->n_unison)
		&& frame_number(interp, frame, "unison-detune", 0, 100, &patch->unison_detune)
		&& frame_number(interp, frame, "unison-spread", 0, 1, &patch->unison_spread)
		&& frame_number(interp, frame, "cutoff", SYNTH_MIN_CUTOFF, SYNTH_MAX_CUTOFF, &patch->cutoff)
		&& frame_number(interp, frame, "resonance", 0, 4, &patch->resonance)
		&& frame_number(interp, frame, "drive", 0.1, 10, &patch->drive)
		&& frame_number(interp, frame, "glide", 0, 10, &patch->glide);
}

static int semitone_of_letter(char letter) {
	static const int semitones[] = {9, 11, 0, 2, 4, 5, 7};
	if (letter < 'a' || letter > 'g')
		return -1;
	return semitones[letter - 'a'];
}

static double pitch_note(Interpreter *interp, Val pitch_val) {
	if (VAL_TAG(pitch_val) == T_FLOAT) {
		double note = VAL_NUMBER(pitch_val);
		if (!(note >= 0 && note <= 127)) {
			fail(interp, "expected a MIDI note number in [0, 127]; got %g", note);
			return -1;
		}
		return note;
	}
	if (VAL_TAG(pitch_val) != T_SYMBOL) {
		fail(interp, "expected a pitch (a MIDI note number or a symbol such as :c4 or :f#3); got %s", tag_name(VAL_TAG(pitch_val)));
		return -1;
	}

	const char *name = symbol_text(VAL_DATA(pitch_val));
	int letter_semitone = semitone_of_letter(name[0]);
	double semitone = letter_semitone;
	const char *cursor = name + 1;
	if (letter_semitone >= 0 && *cursor == '#') {
		semitone += 1.0;
		cursor++;
	} else if (letter_semitone >= 0 && *cursor == 'b' && cursor[1] != '\0') {
		semitone -= 1.0;
		cursor++;
	}
	if (letter_semitone >= 0 && *cursor == '+') {
		semitone += 0.5;
		cursor++;
	} else if (letter_semitone >= 0 && *cursor == 'd') {
		semitone -= 0.5;
		cursor++;
	}
	int octave_digits = isdigit((unsigned char)cursor[0]) || (cursor[0] == '-' && isdigit((unsigned char)cursor[1]));
	char *end;
	long octave = strtol(cursor, &end, 10);
	if (letter_semitone < 0 || !octave_digits || *end != '\0') {
		fail(interp, "expected a pitch such as :c4, :f#3, :bb5 or :c+4; got :%s", name);
		return -1;
	}
	double note = (double)(octave + 1) * 12.0 + semitone;
	if (note < 0 || note > 127) {
		fail(interp, "pitch :%s is outside MIDI notes [0, 127]", name);
		return -1;
	}
	return note;
}

static double note_hz(double note) {
	return 440.0 * pow(2.0, (note - 69.0) / 12.0);
}

static int instrument_index(Interpreter *interp, Val instrument_val) {
	if (VAL_TAG(instrument_val) != T_FLOAT) {
		fail(interp, "expected an instrument number; got %s", tag_name(VAL_TAG(instrument_val)));
		return -1;
	}
	double instrument = VAL_NUMBER(instrument_val);
	if (!(instrument >= 0 && instrument < SYNTH_MAX_INSTRUMENTS) || instrument != floor(instrument)) {
		fail(interp, "expected an instrument number in [0, %d); got %g", SYNTH_MAX_INSTRUMENTS, instrument);
		return -1;
	}
	return (int)instrument;
}

static double stage_factor(double seconds) {
	return seconds > 0 ? exp(-1.0 / (seconds * SYNTH_SAMPLE_RATE)) : 0.0;
}

static void envelope_rates_for(const EnvelopeShape *shape, EnvelopeRates *rates) {
	rates->attack_step = shape->attack > 0 ? 1.0 / (shape->attack * SYNTH_SAMPLE_RATE) : 1.0;
	rates->decay_factor = stage_factor(shape->decay);
	rates->sustain_factor = shape->sustain_decay > 0 ? stage_factor(shape->sustain_decay) : 1.0;
	rates->release_factor = stage_factor(shape->release);
	rates->sustain = shape->sustain;
}

static double envelope_step(EnvelopeStage *stage, double *level, const EnvelopeRates *rates) {
	switch (*stage) {
	case STAGE_ATTACK:
		*level += rates->attack_step;
		if (*level >= 1.0) {
			*level = 1.0;
			*stage = STAGE_DECAY;
		}
		break;
	case STAGE_DECAY:
		*level = rates->sustain + (*level - rates->sustain) * rates->decay_factor;
		if (fabs(*level - rates->sustain) < SYNTH_SILENT_LEVEL) {
			*level = rates->sustain;
			*stage = STAGE_SUSTAIN;
		}
		break;
	case STAGE_SUSTAIN:
		*level *= rates->sustain_factor;
		break;
	case STAGE_RELEASE:
		*level *= rates->release_factor;
		if (*level < SYNTH_SILENT_LEVEL) {
			*level = 0.0;
			*stage = STAGE_IDLE;
		}
		break;
	case STAGE_IDLE:
		break;
	}
	return *level;
}

static uint64_t noise_next(uint64_t *state) {
	uint64_t z = (*state += 0x9e3779b97f4a7c15ULL);
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}

static double noise_white(uint64_t *state) {
	return (double)(noise_next(state) >> 11) * 0x1.0p-52 - 1.0;
}

static double polyblep(double t, double dt) {
	if (t < dt) {
		double x = t / dt;
		return x + x - x * x - 1.0;
	}
	if (t > 1.0 - dt) {
		double x = (t - 1.0) / dt;
		return x * x + x + x + 1.0;
	}
	return 0.0;
}

static double wrap_phase(double phase) {
	return phase - floor(phase);
}

static double oscillator_wave(SynthVoice *voice, int copy, int j, double phase_offset, double dt, double width) {
	const SynthOscillator *oscillator = &voice->patch.oscillators[j];
	double t = wrap_phase(voice->phases[copy][j] + phase_offset);

	switch (oscillator->wave) {
	case WAVE_SINE:
		return sin(2.0 * M_PI * t);
	case WAVE_TRIANGLE:
		return 4.0 * fabs(wrap_phase(t + 0.75) - 0.5) - 1.0;
	case WAVE_SAW:
		return 2.0 * t - 1.0 - polyblep(t, dt);
	case WAVE_PULSE: {
		double naive = t < width ? 1.0 : -1.0;
		return naive + polyblep(t, dt) - polyblep(wrap_phase(t - width), dt);
	}
	case WAVE_WHITE:
		return noise_white(&voice->noise_states[copy][j]);
	case WAVE_PINK: {
		double white = noise_white(&voice->noise_states[copy][j]);
		double *pink = voice->pink_states[copy][j];
		pink[0] = 0.99765 * pink[0] + white * 0.0990460;
		pink[1] = 0.96300 * pink[1] + white * 0.2965164;
		pink[2] = 0.57000 * pink[2] + white * 1.0526913;
		return (pink[0] + pink[1] + pink[2] + white * 0.1848) * 0.25;
	}
	case WAVE_BROWN: {
		double white = noise_white(&voice->noise_states[copy][j]);
		voice->brown_states[copy][j] = (voice->brown_states[copy][j] + 0.02 * white) / 1.02;
		return voice->brown_states[copy][j] * 3.5;
	}
	}
	return 0.0;
}

static double lfo_wave(SynthVoice *voice, double phase, int wrapped) {
	const SynthLfo *lfo = &voice->patch.lfo;
	switch (lfo->shape) {
	case LFO_SINE:
		return sin(2.0 * M_PI * phase);
	case LFO_TRIANGLE:
		return 4.0 * fabs(wrap_phase(phase + 0.75) - 0.5) - 1.0;
	case LFO_SAW:
		return 2.0 * phase - 1.0;
	case LFO_SQUARE:
		return phase < 0.5 ? 1.0 : -1.0;
	case LFO_SAMPLE_AND_HOLD:
		if (wrapped)
			voice->lfo_held = noise_white(&voice->lfo_noise);
		return voice->lfo_held;
	}
	return 0.0;
}

static void ramp_start(SynthVoice *voice, SynthRamp *ramp, double target) {
	if (ramp->remaining == 0)
		voice->n_ramping++;
	ramp->target = target;
	ramp->step = (target - ramp->current) / SYNTH_RAMP_FRAMES;
	ramp->remaining = SYNTH_RAMP_FRAMES;
}

static void ramp_set(SynthRamp *ramp, double value) {
	ramp->current = value;
	ramp->target = value;
	ramp->remaining = 0;
}

static int ramp_advance(SynthRamp *ramp) {
	if (ramp->remaining == 0)
		return 0;
	ramp->current += ramp->step;
	if (--ramp->remaining == 0) {
		ramp->current = ramp->target;
		return 1;
	}
	return 0;
}

static void voice_ramps_advance(SynthVoice *voice) {
	int n_finished = ramp_advance(&voice->level_ramp)
		+ ramp_advance(&voice->pan_ramp)
		+ ramp_advance(&voice->cutoff_ramp)
		+ ramp_advance(&voice->resonance_ramp)
		+ ramp_advance(&voice->drive_ramp);
	for (int j = 0; j < voice->patch.n_oscillators; j++)
		n_finished += ramp_advance(&voice->oscillator_level_ramps[j])
			+ ramp_advance(&voice->oscillator_ratio_ramps[j])
			+ ramp_advance(&voice->oscillator_width_ramps[j]);
	voice->n_ramping -= n_finished;
}

static void voice_pan_gains(SynthVoice *voice, double lfo_pan) {
	double base_pan = voice->pan_ramp.current + lfo_pan;
	for (int copy = 0; copy < voice->n_copies; copy++) {
		double pan = base_pan + voice->copy_pan_offsets[copy];
		pan = pan < -1.0 ? -1.0 : pan > 1.0 ? 1.0 : pan;
		double angle = (pan + 1.0) * M_PI / 4.0;
		voice->copy_left[copy] = pan == 0.0 ? M_SQRT1_2 : cos(angle);
		voice->copy_right[copy] = pan == 0.0 ? M_SQRT1_2 : sin(angle);
	}
}

static void voice_start(SynthVoice *voice, int instrument, double note, double velocity, uint64_t seed) {
	const SynthPatch *patch = &synth_instruments[instrument];
	int retrigger = voice->active && voice->instrument == instrument && voice->note == note;

	if (!retrigger) {
		memset(voice, 0, sizeof(SynthVoice));
		voice->patch = *patch;
		voice->n_copies = patch->n_unison;
		for (int copy = 0; copy < voice->n_copies; copy++)
			for (int j = 0; j < patch->n_oscillators; j++)
				voice->noise_states[copy][j] = seed + (uint64_t)(copy * SYNTH_MAX_OSCILLATORS + j) * 0x632be59bd9b4e019ULL;
		voice->lfo_noise = seed ^ 0xd6e8feb86659fd93ULL;
		voice->filter_stage = STAGE_ATTACK;
		double previous_note = synth_last_notes[instrument];
		if (patch->glide > 0 && previous_note >= 0 && previous_note != note) {
			voice->glide_offset = previous_note - note;
			voice->glide_remaining = (long)llround(patch->glide * SYNTH_SAMPLE_RATE);
			voice->glide_step = -voice->glide_offset / (double)voice->glide_remaining;
		}
		if (patch->lfo.key_sync)
			voice->lfo_phase = 0.0;
		else
			voice->lfo_phase = wrap_phase((double)synth_clock * patch->lfo.rate / SYNTH_SAMPLE_RATE);
		voice->lfo_held = noise_white(&voice->lfo_noise);
	}
	synth_last_notes[instrument] = note;
	voice->active = 1;
	voice->instrument = instrument;
	voice->note = note;
	voice->gate = 1;
	voice->started = synth_clock;
	voice->lfo_age = 0;

	const SynthPatch *voice_patch = &voice->patch;
	voice->note_hz = note_hz(note);
	for (int j = 0; j < voice_patch->n_oscillators; j++) {
		const SynthOscillator *oscillator = &voice_patch->oscillators[j];
		voice->detune_factors[j] = pow(2.0, oscillator->detune_cents / 1200.0);
		envelope_rates_for(&oscillator->envelope, &voice->rates[j]);
		voice->stages[j] = STAGE_ATTACK;
		voice->n_sources[j] = 0;
		if (!retrigger) {
			ramp_set(&voice->oscillator_level_ramps[j], oscillator->level);
			ramp_set(&voice->oscillator_ratio_ramps[j], oscillator->ratio);
			ramp_set(&voice->oscillator_width_ramps[j], oscillator->width);
		}
	}
	envelope_rates_for(&voice_patch->filter_envelope, &voice->filter_rates);
	voice->filter_stage = STAGE_ATTACK;

	for (int m = 0; m < voice_patch->n_modulations; m++) {
		const SynthModulation *modulation = &voice_patch->modulations[m];
		int target = modulation->target;
		int n_sources = voice->n_sources[target];
		int slot = 0;
		while (slot < n_sources && voice->sources[target][slot] != modulation->source)
			slot++;
		if (slot == n_sources) {
			voice->sources[target][slot] = modulation->source;
			voice->depths[target][slot] = 0.0;
			voice->n_sources[target] = n_sources + 1;
		}
		voice->depths[target][slot] += modulation->depth;
	}

	int n_copies = voice->n_copies;
	for (int copy = 0; copy < n_copies; copy++) {
		double position = n_copies > 1 ? (double)copy / (n_copies - 1) - 0.5 : 0.0;
		voice->copy_ratios[copy] = pow(2.0, voice_patch->unison_detune * position / 1200.0);
		voice->copy_pan_offsets[copy] = voice_patch->unison_spread * 2.0 * position;
	}
	voice->copy_scale = 1.0 / sqrt((double)n_copies);
	voice->stereo = n_copies > 1 && voice_patch->unison_spread > 0;

	double sensitivity = voice_patch->velocity_sensitivity;
	double velocity_scale = 1.0 - sensitivity + sensitivity * velocity;
	voice->velocity_gain = velocity_scale;
	voice->filter_amount = voice_patch->filter_amount * velocity_scale;
	if (!retrigger) {
		ramp_set(&voice->level_ramp, voice_patch->level);
		ramp_set(&voice->pan_ramp, voice_patch->pan);
		ramp_set(&voice->cutoff_ramp, voice_patch->cutoff);
		ramp_set(&voice->resonance_ramp, voice_patch->resonance);
		ramp_set(&voice->drive_ramp, voice_patch->drive);
	}
	voice->pan_moving = voice_patch->lfo.pan_depth > 0;
	voice_pan_gains(voice, 0.0);
}

static SynthVoice *voice_for_note(int instrument, double note) {
	for (int v = 0; v < SYNTH_MAX_VOICES; v++) {
		SynthVoice *voice = &synth_voices[v];
		if (voice->active && voice->instrument == instrument && voice->note == note)
			return voice;
	}

	SynthVoice *released_longest = NULL;
	SynthVoice *oldest = &synth_voices[0];
	for (int v = 0; v < SYNTH_MAX_VOICES; v++) {
		SynthVoice *voice = &synth_voices[v];
		if (!voice->active)
			return voice;
		if (!voice->gate && (!released_longest || voice->released < released_longest->released))
			released_longest = voice;
		if (voice->started < oldest->started)
			oldest = voice;
	}
	return released_longest ? released_longest : oldest;
}

static void synth_note_on(int instrument, double note, double velocity, uint64_t seed) {
	SynthVoice *voice = voice_for_note(instrument, note);
	if (voice->active && (voice->instrument != instrument || voice->note != note))
		voice->active = 0;
	voice_start(voice, instrument, note, velocity, seed);
}

static void synth_note_off(int instrument, double note) {
	for (int v = 0; v < SYNTH_MAX_VOICES; v++) {
		SynthVoice *voice = &synth_voices[v];
		if (!voice->active || !voice->gate || voice->instrument != instrument || voice->note != note)
			continue;
		voice->gate = 0;
		voice->released = synth_clock;
		for (int j = 0; j < voice->patch.n_oscillators; j++)
			voice->stages[j] = STAGE_RELEASE;
		voice->filter_stage = STAGE_RELEASE;
	}
}

static void voice_parameter(SynthVoice *voice, SynthParameter parameter, int oscillator_index, double value) {
	switch (parameter) {
	case PARAMETER_LEVEL:
		ramp_start(voice, &voice->level_ramp, value);
		break;
	case PARAMETER_PAN:
		ramp_start(voice, &voice->pan_ramp, value);
		break;
	case PARAMETER_CUTOFF:
		if (!voice->patch.filter_on) {
			voice->patch.filter_on = 1;
			ramp_set(&voice->cutoff_ramp, SYNTH_MAX_CUTOFF);
		}
		ramp_start(voice, &voice->cutoff_ramp, value);
		break;
	case PARAMETER_RESONANCE:
		ramp_start(voice, &voice->resonance_ramp, value);
		break;
	case PARAMETER_DRIVE:
		ramp_start(voice, &voice->drive_ramp, value);
		break;
	case PARAMETER_OSCILLATOR_LEVEL:
		if (oscillator_index < voice->patch.n_oscillators)
			ramp_start(voice, &voice->oscillator_level_ramps[oscillator_index], value);
		break;
	case PARAMETER_OSCILLATOR_RATIO:
		if (oscillator_index < voice->patch.n_oscillators)
			ramp_start(voice, &voice->oscillator_ratio_ramps[oscillator_index], value);
		break;
	case PARAMETER_OSCILLATOR_WIDTH:
		if (oscillator_index < voice->patch.n_oscillators)
			ramp_start(voice, &voice->oscillator_width_ramps[oscillator_index], value);
		break;
	}
}

static void patch_parameter(SynthPatch *patch, SynthParameter parameter, int oscillator_index, double value) {
	switch (parameter) {
	case PARAMETER_LEVEL:
		patch->level = value;
		break;
	case PARAMETER_PAN:
		patch->pan = value;
		break;
	case PARAMETER_CUTOFF:
		patch->cutoff = value;
		patch->filter_on = 1;
		break;
	case PARAMETER_RESONANCE:
		patch->resonance = value;
		break;
	case PARAMETER_DRIVE:
		patch->drive = value;
		break;
	case PARAMETER_OSCILLATOR_LEVEL:
		if (oscillator_index < patch->n_oscillators)
			patch->oscillators[oscillator_index].level = value;
		break;
	case PARAMETER_OSCILLATOR_RATIO:
		if (oscillator_index < patch->n_oscillators)
			patch->oscillators[oscillator_index].ratio = value;
		break;
	case PARAMETER_OSCILLATOR_WIDTH:
		if (oscillator_index < patch->n_oscillators)
			patch->oscillators[oscillator_index].width = value;
		break;
	}
}

static void synth_parameter(int instrument, SynthParameter parameter, int oscillator_index, double value) {
	patch_parameter(&synth_instruments[instrument], parameter, oscillator_index, value);
	for (int v = 0; v < SYNTH_MAX_VOICES; v++) {
		SynthVoice *voice = &synth_voices[v];
		if (voice->active && voice->instrument == instrument)
			voice_parameter(voice, parameter, oscillator_index, value);
	}
}

static void moog_stage(MoogState *moog, double input_sample, double g, double vt2, int channel) {
	double sample_rate_2x = (double)SYNTH_SAMPLE_RATE * MOOG_OVERSAMPLE;

	double dv0 = g * (tanh(input_sample / vt2) - moog->tv[0][channel]);
	moog->v[0][channel] += (dv0 + moog->dv[0][channel]) / (2.0 * sample_rate_2x);
	moog->dv[0][channel] = dv0;
	moog->tv[0][channel] = tanh(moog->v[0][channel] / vt2);

	for (int pole = 1; pole < 4; pole++) {
		double dv = g * (moog->tv[pole - 1][channel] - moog->tv[pole][channel]);
		moog->v[pole][channel] += (dv + moog->dv[pole][channel]) / (2.0 * sample_rate_2x);
		moog->dv[pole][channel] = dv;
		moog->tv[pole][channel] = tanh(moog->v[pole][channel] / vt2);
	}
}

static double moog_sample(MoogState *moog, double sample, double cutoff, double resonance, double drive, int channel) {
	double sample_rate_2x = (double)SYNTH_SAMPLE_RATE * MOOG_OVERSAMPLE;
	double vt2 = 2.0 * MOOG_VT;
	double x = M_PI * cutoff / sample_rate_2x;
	double g = 4.0 * M_PI * MOOG_VT * cutoff * (1.0 - x) / (1.0 + x);

	double midpoint = (moog->previous_input[channel] + sample) * 0.5;
	moog_stage(moog, midpoint * MOOG_INPUT_SCALE * drive - resonance * moog->tv[3][channel], g, vt2, channel);
	moog_stage(moog, sample * MOOG_INPUT_SCALE * drive - resonance * moog->tv[3][channel], g, vt2, channel);
	moog->previous_input[channel] = sample;

	return moog->v[3][channel] / MOOG_INPUT_SCALE * (1.0 + 0.5 * resonance);
}

static void voice_sample(SynthVoice *voice, double *left, double *right) {
	const SynthPatch *patch = &voice->patch;
	int n_oscillators = patch->n_oscillators;
	unsigned carrier_mask = patch->carrier_mask;
	int carriers_sounding = 0;
	int pan_ramping = voice->pan_ramp.remaining > 0;

	if (voice->n_ramping)
		voice_ramps_advance(voice);
	if (voice->glide_remaining > 0) {
		voice->glide_offset += voice->glide_step;
		if (--voice->glide_remaining == 0)
			voice->glide_offset = 0.0;
	}

	const SynthLfo *lfo = &patch->lfo;
	double lfo_value = 0.0;
	double lfo_depth = 0.0;
	double lfo_seconds = (double)voice->lfo_age / SYNTH_SAMPLE_RATE;
	if (lfo_seconds >= lfo->delay) {
		lfo_depth = lfo->fade > 0 ? (lfo_seconds - lfo->delay) / lfo->fade : 1.0;
		lfo_depth = lfo_depth > 1.0 ? 1.0 : lfo_depth;
	}
	double next_phase = voice->lfo_phase + lfo->rate / SYNTH_SAMPLE_RATE;
	int wrapped = next_phase >= 1.0;
	lfo_value = lfo_wave(voice, voice->lfo_phase, wrapped || voice->lfo_age == 0);
	voice->lfo_phase = wrap_phase(next_phase);
	voice->lfo_age++;
	double lfo_amount = lfo_value * lfo_depth;

	double levels[SYNTH_MAX_OSCILLATORS];
	double widths[SYNTH_MAX_OSCILLATORS];
	double increments[SYNTH_MAX_OSCILLATORS];
	double pitch_offset = voice->glide_offset + lfo->pitch_depth * lfo_amount;
	double pitch_factor = pitch_offset != 0.0 ? exp2(pitch_offset / 12.0) : 1.0;
	for (int j = 0; j < n_oscillators; j++) {
		const SynthOscillator *oscillator = &patch->oscillators[j];
		double envelope = envelope_step(&voice->stages[j], &voice->levels[j], &voice->rates[j]);
		double tremolo = 1.0 - oscillator->lfo_level * (1.0 - lfo_amount) * 0.5;
		levels[j] = voice->oscillator_level_ramps[j].current * envelope * (oscillator->lfo_level > 0 ? tremolo : 1.0);
		double width = voice->oscillator_width_ramps[j].current + oscillator->lfo_width * lfo_amount;
		widths[j] = width < 0.01 ? 0.01 : width > 0.99 ? 0.99 : width;
		double hz = oscillator->fixed_hz > 0
			? oscillator->fixed_hz
			: voice->note_hz * voice->oscillator_ratio_ramps[j].current * pitch_factor;
		increments[j] = hz * voice->detune_factors[j] / SYNTH_SAMPLE_RATE;
		if (carrier_mask & (1u << j))
			carriers_sounding |= voice->stages[j] != STAGE_IDLE;
	}

	if (voice->pan_moving || pan_ramping)
		voice_pan_gains(voice, lfo->pan_depth * lfo_amount);

	double mono = 0.0;
	double stereo_left = 0.0;
	double stereo_right = 0.0;
	for (int copy = 0; copy < voice->n_copies; copy++) {
		double carrier_sum = 0.0;
		double copy_ratio = voice->copy_ratios[copy];
		for (int j = n_oscillators - 1; j >= 0; j--) {
			double phase_modulation = 0.0;
			int n_sources = voice->n_sources[j];
			for (int s = 0; s < n_sources; s++)
				phase_modulation += voice->depths[j][s] * voice->outputs[copy][voice->sources[j][s]];

			double dt = increments[j] * copy_ratio;
			double wave = oscillator_wave(voice, copy, j, phase_modulation / (2.0 * M_PI), dt, widths[j]);
			voice->outputs[copy][j] = wave * levels[j];
			voice->phases[copy][j] = wrap_phase(voice->phases[copy][j] + dt);
			if (carrier_mask & (1u << j))
				carrier_sum += voice->outputs[copy][j];
		}
		mono += carrier_sum;
		stereo_left += carrier_sum * voice->copy_left[copy];
		stereo_right += carrier_sum * voice->copy_right[copy];
	}

	double gain = voice->level_ramp.current * voice->velocity_gain * voice->copy_scale;
	double filter_level = envelope_step(&voice->filter_stage, &voice->filter_level, &voice->filter_rates);
	if (patch->filter_on) {
		double octaves = voice->filter_amount * filter_level + lfo->cutoff_depth * lfo_amount;
		double cutoff = voice->cutoff_ramp.current * exp2(octaves);
		cutoff = cutoff < SYNTH_MIN_CUTOFF ? SYNTH_MIN_CUTOFF : cutoff > SYNTH_MAX_CUTOFF ? SYNTH_MAX_CUTOFF : cutoff;
		double resonance = voice->resonance_ramp.current;
		double drive = voice->drive_ramp.current;
		if (voice->stereo) {
			stereo_left = moog_sample(&voice->moog, stereo_left, cutoff, resonance, drive, 0);
			stereo_right = moog_sample(&voice->moog, stereo_right, cutoff, resonance, drive, 1);
		} else {
			mono = moog_sample(&voice->moog, mono, cutoff, resonance, drive, 0);
		}
	}

	if (voice->stereo) {
		*left += stereo_left * gain;
		*right += stereo_right * gain;
	} else {
		*left += mono * voice->copy_left[0] * gain;
		*right += mono * voice->copy_right[0] * gain;
	}

	if (!carriers_sounding)
		voice->active = 0;
}

static void play_finished(float *samples) {
	if (!atomic_load_explicit(&synth_live, memory_order_relaxed)) {
		free(samples);
		return;
	}

	int tail = atomic_load_explicit(&synth_finished_tail, memory_order_relaxed);
	int next = (tail + 1) % SYNTH_QUEUE_CAPACITY;
	if (next == atomic_load_explicit(&synth_finished_head, memory_order_acquire))
		return;
	synth_finished[tail] = samples;
	atomic_store_explicit(&synth_finished_tail, next, memory_order_release);
}

static void plays_mix(double *left, double *right) {
	int kept = 0;
	for (int p = 0; p < synth_n_plays; p++) {
		SynthPlay *play = &synth_plays[p];
		*left += play->samples[2 * play->position];
		*right += play->samples[2 * play->position + 1];
		play->position++;
		if (play->position < play->n_frames)
			synth_plays[kept++] = *play;
		else
			play_finished(play->samples);
	}
	synth_n_plays = kept;
}

static void schedule_insert(const SynthCommand *command) {
	if (synth_n_scheduled == SYNTH_MAX_SCHEDULED) {
		atomic_fetch_add_explicit(&synth_n_scheduled_applied, 1, memory_order_release);
		return;
	}
	if (synth_schedule_start + synth_n_scheduled == SYNTH_MAX_SCHEDULED) {
		memmove(synth_schedule, synth_schedule + synth_schedule_start, sizeof(SynthCommand) * (size_t)synth_n_scheduled);
		synth_schedule_start = 0;
	}

	int slot = synth_schedule_start + synth_n_scheduled;
	while (slot > synth_schedule_start && synth_schedule[slot - 1].at > command->at) {
		synth_schedule[slot] = synth_schedule[slot - 1];
		slot--;
	}
	synth_schedule[slot] = *command;
	synth_n_scheduled++;
}

static void schedule_fire_due(void) {
	while (synth_n_scheduled && synth_schedule[synth_schedule_start].at <= synth_clock) {
		const SynthCommand *due = &synth_schedule[synth_schedule_start];
		if (due->kind == COMMAND_NOTE_ON)
			synth_note_on(due->instrument, due->note, due->velocity, due->seed);
		else
			synth_note_off(due->instrument, due->note);
		synth_schedule_start++;
		synth_n_scheduled--;
		atomic_fetch_add_explicit(&synth_n_scheduled_applied, 1, memory_order_release);
	}
	if (!synth_n_scheduled)
		synth_schedule_start = 0;
}

static void schedule_clear(void) {
	atomic_fetch_add_explicit(&synth_n_scheduled_applied, synth_n_scheduled, memory_order_release);
	synth_n_scheduled = 0;
	synth_schedule_start = 0;
}

static void synth_render(double *interleaved, int n_frames) {
	for (int frame = 0; frame < n_frames; frame++) {
		double left = 0.0;
		double right = 0.0;
		if (synth_n_scheduled)
			schedule_fire_due();
		for (int v = 0; v < SYNTH_MAX_VOICES; v++) {
			SynthVoice *voice = &synth_voices[v];
			if (voice->active)
				voice_sample(voice, &left, &right);
		}
		if (synth_n_plays)
			plays_mix(&left, &right);
		interleaved[2 * frame] = left;
		interleaved[2 * frame + 1] = right;
		synth_clock++;
	}
}

static void synth_silence_all(void) {
	for (int v = 0; v < SYNTH_MAX_VOICES; v++)
		synth_voices[v].active = 0;
	for (int instrument = 0; instrument < SYNTH_MAX_INSTRUMENTS; instrument++)
		synth_last_notes[instrument] = -1.0;
	schedule_clear();
	for (int p = 0; p < synth_n_plays; p++)
		play_finished(synth_plays[p].samples);
	synth_n_plays = 0;
}

static void synth_apply(const SynthCommand *command) {
	if (command->scheduled) {
		schedule_insert(command);
		return;
	}
	switch (command->kind) {
	case COMMAND_NOTE_ON:
		synth_note_on(command->instrument, command->note, command->velocity, command->seed);
		break;
	case COMMAND_NOTE_OFF:
		synth_note_off(command->instrument, command->note);
		break;
	case COMMAND_SILENCE:
		synth_silence_all();
		break;
	case COMMAND_PLAY:
		if (synth_n_plays < SYNTH_MAX_PLAYS) {
			SynthPlay *play = &synth_plays[synth_n_plays++];
			play->samples = command->samples;
			play->n_frames = command->n_frames;
			play->position = 0;
		} else {
			play_finished(command->samples);
		}
		break;
	case COMMAND_PARAMETER:
		synth_parameter(command->instrument, command->parameter, command->oscillator_index, command->value);
		break;
	}
}

static void synth_wait_briefly(void) {
	struct timespec pause = {.tv_sec = 0, .tv_nsec = SYNTH_WAIT_NANOSECONDS};
	nanosleep(&pause, NULL);
}

void synth_collect_finished(void) {
	int head = atomic_load_explicit(&synth_finished_head, memory_order_relaxed);
	int tail = atomic_load_explicit(&synth_finished_tail, memory_order_acquire);
	while (head != tail) {
		free(synth_finished[head]);
		head = (head + 1) % SYNTH_QUEUE_CAPACITY;
	}
	atomic_store_explicit(&synth_finished_head, head, memory_order_release);
}

static int synth_submit(Interpreter *interp, const SynthCommand *command) {
	synth_collect_finished();
	if (!atomic_load_explicit(&synth_live, memory_order_acquire)) {
		synth_apply(command);
		return 1;
	}

	int tail = atomic_load_explicit(&synth_command_tail, memory_order_relaxed);
	int next = (tail + 1) % SYNTH_QUEUE_CAPACITY;
	if (next == atomic_load_explicit(&synth_command_head, memory_order_acquire)) {
		fail(interp, "audio command queue full (max %d)", SYNTH_QUEUE_CAPACITY);
		return 0;
	}
	synth_commands[tail] = *command;
	atomic_store_explicit(&synth_command_tail, next, memory_order_release);
	return 1;
}

int synth_submit_play(Interpreter *interp, float *samples, int n_frames) {
	SynthCommand command = {.kind = COMMAND_PLAY, .samples = samples, .n_frames = n_frames};
	return synth_submit(interp, &command);
}

static void synth_take_pending_patches(void) {
	for (int instrument = 0; instrument < SYNTH_MAX_INSTRUMENTS; instrument++) {
		if (!atomic_load_explicit(&synth_pending_ready[instrument], memory_order_acquire))
			continue;
		synth_instruments[instrument] = synth_pending_patches[instrument];
		atomic_store_explicit(&synth_pending_ready[instrument], 0, memory_order_release);
	}
}

static void synth_drain_commands(void) {
	int head = atomic_load_explicit(&synth_command_head, memory_order_relaxed);
	int tail = atomic_load_explicit(&synth_command_tail, memory_order_acquire);
	while (head != tail) {
		synth_apply(&synth_commands[head]);
		head = (head + 1) % SYNTH_QUEUE_CAPACITY;
	}
	atomic_store_explicit(&synth_command_head, head, memory_order_release);
}

void synth_device_render(float *interleaved, int n_frames) {
	static double chunk[2 * SYNTH_DEVICE_CHUNK];

	synth_take_pending_patches();
	synth_drain_commands();
	for (int done = 0; done < n_frames; ) {
		int n_chunk = MIN(SYNTH_DEVICE_CHUNK, n_frames - done);
		synth_render(chunk, n_chunk);
		for (int i = 0; i < 2 * n_chunk; i++)
			interleaved[2 * done + i] = (float)chunk[i];
		done += n_chunk;
	}

	int n_sounding = synth_n_plays;
	for (int v = 0; v < SYNTH_MAX_VOICES; v++)
		n_sounding += synth_voices[v].active;
	atomic_store_explicit(&synth_sounding, n_sounding, memory_order_release);
	atomic_store_explicit(&synth_published_clock, synth_clock, memory_order_release);
	atomic_fetch_add_explicit(&synth_n_device_renders, 1, memory_order_release);
}

void synth_set_live(int live) {
	synth_ensure_ready();
	if (live) {
		atomic_store_explicit(&synth_command_head, 0, memory_order_relaxed);
		atomic_store_explicit(&synth_command_tail, 0, memory_order_relaxed);
		atomic_store_explicit(&synth_published_clock, synth_clock, memory_order_relaxed);
		atomic_store_explicit(&synth_live, 1, memory_order_release);
		return;
	}

	atomic_store_explicit(&synth_live, 0, memory_order_release);
	synth_take_pending_patches();
	synth_drain_commands();
	synth_collect_finished();
	for (int p = 0; p < synth_n_plays; p++)
		free(synth_plays[p].samples);
	synth_n_plays = 0;
}

int synth_is_live(void) {
	return atomic_load_explicit(&synth_live, memory_order_acquire);
}

int synth_wait_quiet(Interpreter *interp) {
	long renders_at_start = atomic_load_explicit(&synth_n_device_renders, memory_order_acquire);
	for (;;) {
		synth_collect_finished();
		int queue_empty = atomic_load_explicit(&synth_command_head, memory_order_acquire)
			== atomic_load_explicit(&synth_command_tail, memory_order_acquire);
		long renders = atomic_load_explicit(&synth_n_device_renders, memory_order_acquire);
		int n_sounding = atomic_load_explicit(&synth_sounding, memory_order_acquire);
		if (queue_empty && renders > renders_at_start + 1 && n_sounding == 0)
			return 1;
		if (interp->gc_pending & INTERRUPT_PENDING)
			return 0;
		synth_wait_briefly();
	}
}

static uint64_t synth_seed(void) {
	uint64_t high = (uint64_t)random_below(1 << 30);
	uint64_t low = (uint64_t)random_below(1 << 30);
	return (high << 30) | low;
}

typedef struct {
	const char *key;
	SynthParameter parameter;
	double low;
	double high;
} ParameterRow;

static const ParameterRow synth_instrument_parameters[] = {
	{"level", PARAMETER_LEVEL, 0, 1},
	{"pan", PARAMETER_PAN, -1, 1},
	{"cutoff", PARAMETER_CUTOFF, SYNTH_MIN_CUTOFF, SYNTH_MAX_CUTOFF},
	{"resonance", PARAMETER_RESONANCE, 0, 4},
	{"drive", PARAMETER_DRIVE, 0.1, 10},
	{NULL, PARAMETER_LEVEL, 0, 0}
};

static const ParameterRow synth_oscillator_parameters[] = {
	{"level", PARAMETER_OSCILLATOR_LEVEL, 0, 1},
	{"ratio", PARAMETER_OSCILLATOR_RATIO, 1.0 / 64, 64},
	{"width", PARAMETER_OSCILLATOR_WIDTH, 0.01, 0.99},
	{NULL, PARAMETER_LEVEL, 0, 0}
};

static int parameter_submit(Interpreter *interp, Val value_val, Val key_val, int instrument, int oscillator_index,
		const ParameterRow *table, const char *keys_phrase) {
	if (VAL_TAG(key_val) != T_SYMBOL) {
		fail(interp, "expected a parameter key (%s); got %s", keys_phrase, tag_name(VAL_TAG(key_val)));
		return 0;
	}
	const char *key = symbol_text(VAL_DATA(key_val));
	int row = 0;
	while (table[row].key && strcmp(table[row].key, key) != 0)
		row++;
	if (!table[row].key) {
		fail(interp, "expected a parameter key (%s); got :%s", keys_phrase, key);
		return 0;
	}
	double value;
	if (!number_in_range(interp, value_val, key, table[row].low, table[row].high, &value))
		return 0;

	SynthCommand command = {
		.kind = COMMAND_PARAMETER,
		.instrument = instrument,
		.parameter = table[row].parameter,
		.oscillator_index = oscillator_index,
		.value = value,
	};
	return synth_submit(interp, &command);
}

static int64_t gcd64(int64_t a, int64_t b) {
	a = a < 0 ? -a : a;
	b = b < 0 ? -b : b;
	while (b) {
		int64_t remainder = a % b;
		a = b;
		b = remainder;
	}
	return a;
}

static void time_reduce(SampleTime *time) {
	int64_t divisor = gcd64(time->numerator, time->denominator);
	if (divisor > 1) {
		time->numerator /= divisor;
		time->denominator /= divisor;
	}
}

static long time_floor(SampleTime time) {
	return (long)(time.numerator / time.denominator);
}

static void time_add(SampleTime *time, SampleTime addend) {
	int64_t divisor = gcd64(time->denominator, addend.denominator);
	int64_t left_scale = addend.denominator / divisor;
	int64_t right_scale = time->denominator / divisor;
	int64_t left;
	int64_t right;
	int64_t numerator;
	int64_t denominator;
	if (__builtin_mul_overflow(time->numerator, left_scale, &left)
			|| __builtin_mul_overflow(addend.numerator, right_scale, &right)
			|| __builtin_add_overflow(left, right, &numerator)
			|| __builtin_mul_overflow(time->denominator, left_scale, &denominator)) {
		double samples = (double)time->numerator / (double)time->denominator
			+ (double)addend.numerator / (double)addend.denominator;
		time->numerator = llround(samples);
		time->denominator = 1;
		return;
	}
	time->numerator = numerator;
	time->denominator = denominator;
	time_reduce(time);
}

static SampleTime length_samples(int64_t numerator, int64_t denominator) {
	int64_t base = SYNTH_WHOLE_NOTE_BASE;
	int64_t tempo = synth_tempo_milli;
	int64_t base_divisor = gcd64(base, denominator);
	base /= base_divisor;
	denominator /= base_divisor;
	int64_t tempo_divisor = gcd64(numerator, tempo);
	numerator /= tempo_divisor;
	tempo /= tempo_divisor;

	SampleTime length;
	if (__builtin_mul_overflow(numerator, base, &length.numerator)
			|| __builtin_mul_overflow(denominator, tempo, &length.denominator)) {
		double samples = (double)numerator / (double)denominator * (double)base / (double)tempo;
		length.numerator = llround(samples);
		length.denominator = 1;
		return length;
	}
	time_reduce(&length);
	return length;
}

static long synth_now(void) {
	if (synth_is_live())
		return atomic_load_explicit(&synth_published_clock, memory_order_acquire) + SYNTH_SCHEDULE_LEAD;
	return synth_clock;
}

static SampleTime *cursor_for(int instrument) {
	SampleTime *cursor = &synth_instrument_cursors[instrument];
	long now = synth_now();
	if (cursor->denominator == 0 || time_floor(*cursor) < now) {
		cursor->numerator = now;
		cursor->denominator = 1;
	}
	return cursor;
}

static int length_fraction(Interpreter *interp, Val numerator_val, Val denominator_val, int64_t *numerator, int64_t *denominator) {
	int64_t numerator_value;
	int64_t denominator_value;
	int numerator_ok = VAL_TAG(numerator_val) == T_EXACT && exact_fits_int64(numerator_val, &numerator_value);
	int denominator_ok = VAL_TAG(denominator_val) == T_EXACT && exact_fits_int64(denominator_val, &denominator_value);
	if (!numerator_ok || !denominator_ok) {
		fail(interp, "expected a note length as an exact numerator and denominator (use sequence-note, sequence-chord or sequence-rest)");
		return 0;
	}
	if (numerator_value <= 0 || denominator_value <= 0) {
		fail(interp, "expected a positive note length; got %lld/%lld", (long long)numerator_value, (long long)denominator_value);
		return 0;
	}
	*numerator = numerator_value;
	*denominator = denominator_value;
	return 1;
}

static int sequence_notes(Interpreter *interp, const double *notes, int n_notes, int64_t numerator, int64_t denominator) {
	long outstanding = synth_n_scheduled_submitted - atomic_load_explicit(&synth_n_scheduled_applied, memory_order_acquire);
	if (outstanding + 2L * n_notes > SYNTH_MAX_SCHEDULED) {
		fail(interp, "sequencer schedule full (max %d events)", SYNTH_MAX_SCHEDULED);
		return 0;
	}

	int instrument = synth_current_instrument;
	SampleTime *cursor = cursor_for(instrument);
	long onset = time_floor(*cursor);
	SampleTime length = length_samples(numerator, denominator);
	double length_in_samples = (double)length.numerator / (double)length.denominator;
	long gate = llround(length_in_samples * synth_articulation);
	gate = gate < 1 ? 1 : gate;

	for (int i = 0; i < n_notes; i++) {
		SynthCommand note_on = {
			.kind = COMMAND_NOTE_ON, .instrument = instrument, .note = notes[i], .velocity = synth_velocity,
			.seed = synth_seed(), .scheduled = 1, .at = onset,
		};
		SynthCommand note_off = {.kind = COMMAND_NOTE_OFF, .instrument = instrument, .note = notes[i], .scheduled = 1, .at = onset + gate};
		if (!synth_submit(interp, &note_on) || !synth_submit(interp, &note_off))
			return 0;
		synth_n_scheduled_submitted += 2;
	}
	time_add(cursor, length);
	return 1;
}

void p_sequence_note(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 3);
	double note = pitch_note(interp, chain_sp[-3]);
	if (note < 0)
		return;
	int64_t numerator;
	int64_t denominator;
	if (!length_fraction(interp, chain_sp[-2], chain_sp[-1], &numerator, &denominator))
		return;

	synth_ensure_ready();
	if (!sequence_notes(interp, &note, 1, numerator, denominator))
		return;

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 3);
}

#define SYNTH_MAX_CHORD 32

void p_sequence_chord(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 3);
	Val pitches_val = chain_sp[-3];
	REQUIRE_CHAIN_TAG(pitches_val, T_ARRAY, "sequence-chord", "an array of pitches");
	Object *pitches = OBJECT_AT(VAL_DATA(pitches_val));
	int n_pitches = pitches->len;
	if (n_pitches < 1 || n_pitches > SYNTH_MAX_CHORD) {
		fail(interp, "expected 1 to %d pitches in a chord; got %d", SYNTH_MAX_CHORD, n_pitches);
		return;
	}
	double notes[SYNTH_MAX_CHORD];
	for (int i = 0; i < n_pitches; i++) {
		notes[i] = pitch_note(interp, pitches->items[i]);
		if (notes[i] < 0)
			return;
	}
	int64_t numerator;
	int64_t denominator;
	if (!length_fraction(interp, chain_sp[-2], chain_sp[-1], &numerator, &denominator))
		return;

	synth_ensure_ready();
	if (!sequence_notes(interp, notes, n_pitches, numerator, denominator))
		return;

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 3);
}

void p_sequence_rest(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	int64_t numerator;
	int64_t denominator;
	if (!length_fraction(interp, chain_sp[-2], chain_sp[-1], &numerator, &denominator))
		return;

	time_add(cursor_for(synth_current_instrument), length_samples(numerator, denominator));

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 2);
}

static int scalar_in_range(Interpreter *interp, Val value, const char *what, double low, double high, double *target) {
	if (VAL_TAG(value) != T_FLOAT && VAL_TAG(value) != T_EXACT) {
		fail(interp, "expected %s as a number; got %s", what, tag_name(VAL_TAG(value)));
		return 0;
	}
	double number = VAL_TAG(value) == T_EXACT ? exact_to_double(value) : VAL_NUMBER(value);
	if (!(number > low && number <= high)) {
		fail(interp, "expected %s in (%g, %g]; got %g", what, low, high, number);
		return 0;
	}
	*target = number;
	return 1;
}

void p_sequence_tempo(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	double bpm;
	if (!scalar_in_range(interp, chain_sp[-1], "a tempo in beats per minute", 0, 1000, &bpm))
		return;

	synth_tempo_milli = llround(bpm * 1000.0);
	synth_tempo_milli = synth_tempo_milli < 1 ? 1 : synth_tempo_milli;

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_sequence_instrument(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	int instrument = instrument_index(interp, chain_sp[-1]);
	if (instrument < 0)
		return;

	synth_current_instrument = instrument;

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_sequence_articulation(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	if (!scalar_in_range(interp, chain_sp[-1], "an articulation (the sounding fraction of a note)", 0, 1, &synth_articulation))
		return;

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_sequence_velocity(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val velocity_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(velocity_val, T_FLOAT, "sequence-velocity", "a float velocity");
	double velocity = VAL_NUMBER(velocity_val);
	if (!(velocity >= 0 && velocity <= 1)) {
		fail(interp, "expected a velocity in [0, 1]; got %g", velocity);
		return;
	}

	synth_velocity = velocity;

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_sequence_end(DISPATCH_ARGS) {
	REQUIRE_STACK_ROOM(interp, chain_ip, chain_sp, 1);
	long clock = synth_is_live() ? atomic_load_explicit(&synth_published_clock, memory_order_acquire) : synth_clock;
	long last = clock;
	for (int instrument = 0; instrument < SYNTH_MAX_INSTRUMENTS; instrument++) {
		SampleTime cursor = synth_instrument_cursors[instrument];
		if (cursor.denominator != 0 && time_floor(cursor) > last)
			last = time_floor(cursor);
	}

	chain_sp[0] = make_float((double)(last - clock) / SYNTH_SAMPLE_RATE);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp + 1);
}

void p_wait_sequence(DISPATCH_ARGS) {
	if (synth_is_live()) {
		while (atomic_load_explicit(&synth_n_scheduled_applied, memory_order_acquire) < synth_n_scheduled_submitted
				&& !(interp->gc_pending & INTERRUPT_PENDING))
			synth_wait_briefly();
		synth_wait_quiet(interp);
	}

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_pitch_to_midi(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	double note = pitch_note(interp, chain_sp[-1]);
	if (note < 0)
		return;

	chain_sp[-1] = make_float(note);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_pitch_to_hz(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	double note = pitch_note(interp, chain_sp[-1]);
	if (note < 0)
		return;

	chain_sp[-1] = make_float(note_hz(note));

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_instrument_patch_store(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val patch_val = chain_sp[-2];
	REQUIRE_CHAIN_TAG(patch_val, T_FRAME, "instrument-patch!", "a patch frame");
	int instrument = instrument_index(interp, chain_sp[-1]);
	if (instrument < 0)
		return;

	synth_ensure_ready();
	SynthPatch patch;
	if (!patch_from_frame(interp, OBJECT_AT(VAL_DATA(patch_val)), &patch))
		return;
	if (!synth_is_live()) {
		synth_instruments[instrument] = patch;
		DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 2);
	}

	while (atomic_load_explicit(&synth_pending_ready[instrument], memory_order_acquire) && synth_is_live())
		synth_wait_briefly();
	synth_pending_patches[instrument] = patch;
	atomic_store_explicit(&synth_pending_ready[instrument], 1, memory_order_release);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 2);
}

void p_instrument_store(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 3);
	int instrument = instrument_index(interp, chain_sp[-1]);
	if (instrument < 0)
		return;

	synth_ensure_ready();
	if (!parameter_submit(interp, chain_sp[-3], chain_sp[-2], instrument, -1, synth_instrument_parameters,
			":level :pan :cutoff :resonance :drive"))
		return;

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 3);
}

void p_oscillator_store(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 4);
	int instrument = instrument_index(interp, chain_sp[-1]);
	if (instrument < 0)
		return;
	int oscillator_index = frame_index(interp, chain_sp[-2], SYNTH_MAX_OSCILLATORS, "oscillator!");
	if (oscillator_index < 0)
		return;

	synth_ensure_ready();
	if (!parameter_submit(interp, chain_sp[-4], chain_sp[-3], instrument, oscillator_index, synth_oscillator_parameters,
			":level :ratio :width"))
		return;

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 4);
}

void p_note_on(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 3);
	double note = pitch_note(interp, chain_sp[-3]);
	if (note < 0)
		return;
	Val velocity_val = chain_sp[-2];
	REQUIRE_CHAIN_TAG(velocity_val, T_FLOAT, "note-on", "a float velocity");
	double velocity = VAL_NUMBER(velocity_val);
	if (!(velocity >= 0 && velocity <= 1)) {
		fail(interp, "expected a velocity in [0, 1]; got %g", velocity);
		return;
	}
	int instrument = instrument_index(interp, chain_sp[-1]);
	if (instrument < 0)
		return;

	synth_ensure_ready();
	SynthCommand command = {.kind = COMMAND_NOTE_ON, .instrument = instrument, .note = note, .velocity = velocity, .seed = synth_seed()};
	if (!synth_submit(interp, &command))
		return;

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 3);
}

void p_note_off(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	double note = pitch_note(interp, chain_sp[-2]);
	if (note < 0)
		return;
	int instrument = instrument_index(interp, chain_sp[-1]);
	if (instrument < 0)
		return;

	SynthCommand command = {.kind = COMMAND_NOTE_OFF, .instrument = instrument, .note = note};
	if (!synth_submit(interp, &command))
		return;

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 2);
}

void p_render_audio(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val seconds_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(seconds_val, T_FLOAT, "render-audio", "a float number of seconds");
	double seconds = VAL_NUMBER(seconds_val);
	if (!(seconds >= 0 && seconds <= SYNTH_MAX_RENDER_SECONDS)) {
		fail(interp, "expected seconds in [0, %d]; got %g", SYNTH_MAX_RENDER_SECONDS, seconds);
		return;
	}
	if (synth_is_live()) {
		fail(interp, "the synthesizer is playing live; audio-off first");
		return;
	}

	int n_frames = (int)llround(seconds * SYNTH_SAMPLE_RATE);
	NEW_MATRIX(samples_handle, samples, n_frames, 2);
	synth_ensure_ready();
	synth_render(samples->matrix.elements, n_frames);

	chain_sp[-1] = make_matrix(samples_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_silence_audio(DISPATCH_ARGS) {
	SynthCommand command = {.kind = COMMAND_SILENCE};
	if (!synth_submit(interp, &command))
		return;
	for (int instrument = 0; instrument < SYNTH_MAX_INSTRUMENTS; instrument++)
		synth_instrument_cursors[instrument].denominator = 0;

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}
