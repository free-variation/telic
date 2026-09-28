#include "telic.h"

#define SYNTH_SAMPLE_RATE 48000
#define SYNTH_MAX_OPERATORS 8
#define SYNTH_MAX_MODULATIONS 64
#define SYNTH_MAX_PARTS 16
#define SYNTH_MAX_VOICES 32
#define SYNTH_MAX_RENDER_SECONDS 600
#define SYNTH_MAX_STAGE_SECONDS 60
#define SYNTH_MAX_DEPTH 100
#define SYNTH_SILENT_LEVEL 1e-4

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
	STAGE_ATTACK,
	STAGE_DECAY,
	STAGE_SUSTAIN,
	STAGE_RELEASE,
	STAGE_IDLE
} EnvelopeStage;

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
	EnvelopeShape envelope;
} SynthOperator;

typedef struct {
	int source;
	int target;
	double depth;
} SynthModulation;

typedef struct {
	int n_operators;
	SynthOperator operators[SYNTH_MAX_OPERATORS];
	int n_modulations;
	SynthModulation modulations[SYNTH_MAX_MODULATIONS];
	unsigned carrier_mask;
	double level;
	double pan;
	double velocity_sensitivity;
} SynthPatch;

typedef struct {
	double attack_step;
	double decay_factor;
	double sustain_factor;
	double release_factor;
	double sustain;
} EnvelopeRates;

typedef struct {
	int active;
	int part;
	double note;
	int gate;
	long started;
	long released;
	SynthPatch patch;
	double left_gain;
	double right_gain;
	double increments[SYNTH_MAX_OPERATORS];
	double phases[SYNTH_MAX_OPERATORS];
	double outputs[SYNTH_MAX_OPERATORS];
	int n_sources[SYNTH_MAX_OPERATORS];
	int sources[SYNTH_MAX_OPERATORS][SYNTH_MAX_OPERATORS];
	double depths[SYNTH_MAX_OPERATORS][SYNTH_MAX_OPERATORS];
	EnvelopeStage stages[SYNTH_MAX_OPERATORS];
	double levels[SYNTH_MAX_OPERATORS];
	EnvelopeRates rates[SYNTH_MAX_OPERATORS];
	uint64_t noise_states[SYNTH_MAX_OPERATORS];
	double pink_states[SYNTH_MAX_OPERATORS][3];
	double brown_states[SYNTH_MAX_OPERATORS];
} SynthVoice;

static SynthPatch synth_parts[SYNTH_MAX_PARTS];
static SynthVoice synth_voices[SYNTH_MAX_VOICES];
static int synth_parts_ready;
static long synth_clock;

static void envelope_shape_default(EnvelopeShape *shape) {
	shape->attack = 0.005;
	shape->decay = 0.1;
	shape->sustain = 1.0;
	shape->release = 0.1;
	shape->sustain_decay = 0.0;
}

static void operator_default(SynthOperator *operator_) {
	operator_->wave = WAVE_SINE;
	operator_->ratio = 1.0;
	operator_->fixed_hz = 0.0;
	operator_->detune_cents = 0.0;
	operator_->level = 1.0;
	operator_->width = 0.5;
	envelope_shape_default(&operator_->envelope);
}

static void patch_default(SynthPatch *patch) {
	memset(patch, 0, sizeof(SynthPatch));
	patch->n_operators = 1;
	operator_default(&patch->operators[0]);
	patch->carrier_mask = 1;
	patch->level = 1.0;
	patch->pan = 0.0;
	patch->velocity_sensitivity = 1.0;
}

static void synth_ensure_ready(void) {
	if (synth_parts_ready)
		return;

	for (int part = 0; part < SYNTH_MAX_PARTS; part++)
		patch_default(&synth_parts[part]);
	synth_parts_ready = 1;
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

static int frame_number(Interpreter *interp, Object *frame, const char *key, double low, double high, double *target) {
	Val value;
	if (!frame_value(interp, frame, key, &value))
		return 1;

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

static int frame_index(Interpreter *interp, Val value, int n_operators, const char *what) {
	if (VAL_TAG(value) != T_FLOAT) {
		fail(interp, "expected an operator index in %s; got %s", what, tag_name(VAL_TAG(value)));
		return -1;
	}
	double number = VAL_NUMBER(value);
	if (!(number >= 0 && number < n_operators) || number != floor(number)) {
		fail(interp, "expected an operator index in %s in [0, %d); got %g", what, n_operators, number);
		return -1;
	}
	return (int)number;
}

static const struct {
	const char *name;
	SynthWave wave;
} synth_wave_names[] = {
	{"sine", WAVE_SINE},
	{"triangle", WAVE_TRIANGLE},
	{"saw", WAVE_SAW},
	{"pulse", WAVE_PULSE},
	{"white", WAVE_WHITE},
	{"pink", WAVE_PINK},
	{"brown", WAVE_BROWN},
	{NULL, WAVE_SINE}
};

static const char *const synth_operator_keys[] = {
	"wave", "ratio", "fixed", "detune", "level", "width",
	"attack", "decay", "sustain", "release", "sustain-decay", NULL
};

static const char *const synth_patch_keys[] = {
	"operators", "modulation", "carriers", "level", "pan", "velocity", NULL
};

static int operator_from_frame(Interpreter *interp, Val operator_val, SynthOperator *operator_) {
	operator_default(operator_);
	if (VAL_TAG(operator_val) != T_FRAME) {
		fail(interp, "expected an operator frame; got %s", tag_name(VAL_TAG(operator_val)));
		return 0;
	}
	Object *frame = OBJECT_AT(VAL_DATA(operator_val));
	if (!frame_keys_allowed(interp, frame, synth_operator_keys, "operator"))
		return 0;

	Val wave_val;
	if (frame_value(interp, frame, "wave", &wave_val)) {
		if (VAL_TAG(wave_val) != T_SYMBOL) {
			fail(interp, "expected a wave symbol for :wave; got %s", tag_name(VAL_TAG(wave_val)));
			return 0;
		}
		const char *wave_name = symbol_text(VAL_DATA(wave_val));
		int wave_index = 0;
		while (synth_wave_names[wave_index].name && strcmp(synth_wave_names[wave_index].name, wave_name) != 0)
			wave_index++;
		if (!synth_wave_names[wave_index].name) {
			fail(interp, "expected :wave one of :sine :triangle :saw :pulse :white :pink :brown; got :%s", wave_name);
			return 0;
		}
		operator_->wave = synth_wave_names[wave_index].wave;
	}

	EnvelopeShape *envelope = &operator_->envelope;
	return frame_number(interp, frame, "ratio", 1.0 / 64, 64, &operator_->ratio)
		&& frame_number(interp, frame, "fixed", 0, SYNTH_SAMPLE_RATE / 2.0, &operator_->fixed_hz)
		&& frame_number(interp, frame, "detune", -1200, 1200, &operator_->detune_cents)
		&& frame_number(interp, frame, "level", 0, 1, &operator_->level)
		&& frame_number(interp, frame, "width", 0.01, 0.99, &operator_->width)
		&& frame_number(interp, frame, "attack", 0, SYNTH_MAX_STAGE_SECONDS, &envelope->attack)
		&& frame_number(interp, frame, "decay", 0, SYNTH_MAX_STAGE_SECONDS, &envelope->decay)
		&& frame_number(interp, frame, "sustain", 0, 1, &envelope->sustain)
		&& frame_number(interp, frame, "release", 0, SYNTH_MAX_STAGE_SECONDS, &envelope->release)
		&& frame_number(interp, frame, "sustain-decay", 0, SYNTH_MAX_STAGE_SECONDS, &envelope->sustain_decay);
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
		int source = frame_index(interp, triple->items[0], patch->n_operators, ":modulation");
		if (source < 0)
			return 0;
		int target = frame_index(interp, triple->items[1], patch->n_operators, ":modulation");
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
		fail(interp, "expected an array of operator indices for :carriers; got %s", tag_name(VAL_TAG(carriers_val)));
		return 0;
	}
	Object *carriers = OBJECT_AT(VAL_DATA(carriers_val));
	unsigned carrier_mask = 0;
	for (int i = 0; i < carriers->len; i++) {
		int carrier = frame_index(interp, carriers->items[i], patch->n_operators, ":carriers");
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

static int patch_from_frame(Interpreter *interp, Object *frame, SynthPatch *patch) {
	patch_default(patch);
	if (!frame_keys_allowed(interp, frame, synth_patch_keys, "patch"))
		return 0;

	Val operators_val;
	if (frame_value(interp, frame, "operators", &operators_val)) {
		if (VAL_TAG(operators_val) != T_ARRAY) {
			fail(interp, "expected an array of operator frames for :operators; got %s", tag_name(VAL_TAG(operators_val)));
			return 0;
		}
		Object *operators = OBJECT_AT(VAL_DATA(operators_val));
		int n_operators = operators->len;
		if (n_operators < 1 || n_operators > SYNTH_MAX_OPERATORS) {
			fail(interp, "expected 1 to %d operators; got %d", SYNTH_MAX_OPERATORS, n_operators);
			return 0;
		}
		for (int i = 0; i < n_operators; i++)
			if (!operator_from_frame(interp, operators->items[i], &patch->operators[i]))
				return 0;
		patch->n_operators = n_operators;
	}

	Val modulation_val;
	if (frame_value(interp, frame, "modulation", &modulation_val) && !modulations_from_value(interp, modulation_val, patch))
		return 0;
	Val carriers_val;
	if (frame_value(interp, frame, "carriers", &carriers_val) && !carriers_from_value(interp, carriers_val, patch))
		return 0;

	return frame_number(interp, frame, "level", 0, 1, &patch->level)
		&& frame_number(interp, frame, "pan", -1, 1, &patch->pan)
		&& frame_number(interp, frame, "velocity", 0, 1, &patch->velocity_sensitivity);
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
	int semitone = semitone_of_letter(name[0]);
	const char *cursor = name + 1;
	if (semitone >= 0 && *cursor == '#') {
		semitone++;
		cursor++;
	} else if (semitone >= 0 && *cursor == 'b' && cursor[1] != '\0') {
		semitone--;
		cursor++;
	}
	char *end;
	long octave = strtol(cursor, &end, 10);
	if (semitone < 0 || end == cursor || *end != '\0') {
		fail(interp, "expected a pitch such as :c4, :f#3 or :bb5; got :%s", name);
		return -1;
	}
	long note = (octave + 1) * 12 + semitone;
	if (note < 0 || note > 127) {
		fail(interp, "pitch :%s is outside MIDI notes [0, 127]", name);
		return -1;
	}
	return (double)note;
}

static double note_hz(double note) {
	return 440.0 * pow(2.0, (note - 69.0) / 12.0);
}

static int part_index(Interpreter *interp, Val part_val) {
	if (VAL_TAG(part_val) != T_FLOAT) {
		fail(interp, "expected a part number; got %s", tag_name(VAL_TAG(part_val)));
		return -1;
	}
	double part = VAL_NUMBER(part_val);
	if (!(part >= 0 && part < SYNTH_MAX_PARTS) || part != floor(part)) {
		fail(interp, "expected a part number in [0, %d); got %g", SYNTH_MAX_PARTS, part);
		return -1;
	}
	return (int)part;
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

static double operator_wave(SynthVoice *voice, int j, double phase_offset) {
	const SynthOperator *operator_ = &voice->patch.operators[j];
	double t = wrap_phase(voice->phases[j] + phase_offset);
	double dt = voice->increments[j];

	switch (operator_->wave) {
	case WAVE_SINE:
		return sin(2.0 * M_PI * t);
	case WAVE_TRIANGLE:
		return 4.0 * fabs(wrap_phase(t + 0.75) - 0.5) - 1.0;
	case WAVE_SAW:
		return 2.0 * t - 1.0 - polyblep(t, dt);
	case WAVE_PULSE: {
		double width = operator_->width;
		double naive = t < width ? 1.0 : -1.0;
		return naive + polyblep(t, dt) - polyblep(wrap_phase(t - width), dt);
	}
	case WAVE_WHITE:
		return noise_white(&voice->noise_states[j]);
	case WAVE_PINK: {
		double white = noise_white(&voice->noise_states[j]);
		double *pink = voice->pink_states[j];
		pink[0] = 0.99765 * pink[0] + white * 0.0990460;
		pink[1] = 0.96300 * pink[1] + white * 0.2965164;
		pink[2] = 0.57000 * pink[2] + white * 1.0526913;
		return (pink[0] + pink[1] + pink[2] + white * 0.1848) * 0.25;
	}
	case WAVE_BROWN: {
		double white = noise_white(&voice->noise_states[j]);
		voice->brown_states[j] = (voice->brown_states[j] + 0.02 * white) / 1.02;
		return voice->brown_states[j] * 3.5;
	}
	}
	return 0.0;
}

static void voice_start(SynthVoice *voice, int part, double note, double velocity, uint64_t seed) {
	const SynthPatch *patch = &synth_parts[part];
	int retrigger = voice->active && voice->part == part && voice->note == note;

	if (!retrigger) {
		memset(voice, 0, sizeof(SynthVoice));
		voice->patch = *patch;
		for (int j = 0; j < patch->n_operators; j++) {
			voice->stages[j] = STAGE_ATTACK;
			voice->noise_states[j] = seed + (uint64_t)j * 0x632be59bd9b4e019ULL;
		}
	}
	voice->active = 1;
	voice->part = part;
	voice->note = note;
	voice->gate = 1;
	voice->started = synth_clock;

	const SynthPatch *voice_patch = &voice->patch;
	double hz = note_hz(note);
	for (int j = 0; j < voice_patch->n_operators; j++) {
		const SynthOperator *operator_ = &voice_patch->operators[j];
		double operator_hz = operator_->fixed_hz > 0 ? operator_->fixed_hz : hz * operator_->ratio;
		operator_hz *= pow(2.0, operator_->detune_cents / 1200.0);
		voice->increments[j] = operator_hz / SYNTH_SAMPLE_RATE;
		envelope_rates_for(&operator_->envelope, &voice->rates[j]);
		voice->stages[j] = STAGE_ATTACK;
		voice->n_sources[j] = 0;
	}

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

	double sensitivity = voice_patch->velocity_sensitivity;
	double gain = voice_patch->level * (1.0 - sensitivity + sensitivity * velocity);
	double angle = (voice_patch->pan + 1.0) * M_PI / 4.0;
	voice->left_gain = gain * cos(angle);
	voice->right_gain = gain * sin(angle);
}

static SynthVoice *voice_for_note(int part, double note) {
	for (int v = 0; v < SYNTH_MAX_VOICES; v++) {
		SynthVoice *voice = &synth_voices[v];
		if (voice->active && voice->part == part && voice->note == note)
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

static void synth_note_on(int part, double note, double velocity, uint64_t seed) {
	SynthVoice *voice = voice_for_note(part, note);
	if (voice->active && (voice->part != part || voice->note != note))
		voice->active = 0;
	voice_start(voice, part, note, velocity, seed);
}

static void synth_note_off(int part, double note) {
	for (int v = 0; v < SYNTH_MAX_VOICES; v++) {
		SynthVoice *voice = &synth_voices[v];
		if (!voice->active || !voice->gate || voice->part != part || voice->note != note)
			continue;
		voice->gate = 0;
		voice->released = synth_clock;
		for (int j = 0; j < voice->patch.n_operators; j++)
			voice->stages[j] = STAGE_RELEASE;
	}
}

static double voice_sample(SynthVoice *voice) {
	const SynthPatch *patch = &voice->patch;
	int n_operators = patch->n_operators;
	unsigned carrier_mask = patch->carrier_mask;
	double carrier_sum = 0.0;
	int carriers_sounding = 0;

	for (int j = n_operators - 1; j >= 0; j--) {
		double phase_modulation = 0.0;
		int n_sources = voice->n_sources[j];
		for (int s = 0; s < n_sources; s++)
			phase_modulation += voice->depths[j][s] * voice->outputs[voice->sources[j][s]];

		double level = envelope_step(&voice->stages[j], &voice->levels[j], &voice->rates[j]);
		double wave = operator_wave(voice, j, phase_modulation / (2.0 * M_PI));
		voice->outputs[j] = wave * patch->operators[j].level * level;
		voice->phases[j] = wrap_phase(voice->phases[j] + voice->increments[j]);

		if (carrier_mask & (1u << j)) {
			carrier_sum += voice->outputs[j];
			carriers_sounding |= voice->stages[j] != STAGE_IDLE;
		}
	}

	if (!carriers_sounding)
		voice->active = 0;
	return carrier_sum;
}

static void synth_render(double *interleaved, int n_frames) {
	for (int frame = 0; frame < n_frames; frame++) {
		double left = 0.0;
		double right = 0.0;
		for (int v = 0; v < SYNTH_MAX_VOICES; v++) {
			SynthVoice *voice = &synth_voices[v];
			if (!voice->active)
				continue;
			double sample = voice_sample(voice);
			left += sample * voice->left_gain;
			right += sample * voice->right_gain;
		}
		interleaved[2 * frame] = left;
		interleaved[2 * frame + 1] = right;
		synth_clock++;
	}
}

static uint64_t synth_seed(void) {
	uint64_t high = (uint64_t)random_below(1 << 30);
	uint64_t low = (uint64_t)random_below(1 << 30);
	return (high << 30) | low;
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

void p_patch_store(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val patch_val = chain_sp[-2];
	REQUIRE_CHAIN_TAG(patch_val, T_FRAME, "patch!", "a patch frame");
	int part = part_index(interp, chain_sp[-1]);
	if (part < 0)
		return;

	synth_ensure_ready();
	SynthPatch patch;
	if (!patch_from_frame(interp, OBJECT_AT(VAL_DATA(patch_val)), &patch))
		return;
	synth_parts[part] = patch;

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 2);
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
	int part = part_index(interp, chain_sp[-1]);
	if (part < 0)
		return;

	synth_ensure_ready();
	synth_note_on(part, note, velocity, synth_seed());

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 3);
}

void p_note_off(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	double note = pitch_note(interp, chain_sp[-2]);
	if (note < 0)
		return;
	int part = part_index(interp, chain_sp[-1]);
	if (part < 0)
		return;

	synth_note_off(part, note);

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

	int n_frames = (int)llround(seconds * SYNTH_SAMPLE_RATE);
	NEW_MATRIX(samples_handle, samples, n_frames, 2);
	synth_ensure_ready();
	synth_render(samples->matrix.elements, n_frames);

	chain_sp[-1] = make_matrix(samples_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_silence(DISPATCH_ARGS) {
	for (int v = 0; v < SYNTH_MAX_VOICES; v++)
		synth_voices[v].active = 0;

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}
