#include "telic.h"
#include "pocketfft.h"

static int complex_length(Interpreter *interp, Object *complex_vector) {
	int n_rows = complex_vector->matrix.rows;
	int n_columns = complex_vector->matrix.columns;

	if (n_columns != 2 || n_rows < 1) {
		fail(interp, "expected a complex vector (nx2, n >= 1); got %dx%d", n_rows, n_columns);
		return -1;
	}
	return n_rows;
}

static int positive_length(Interpreter *interp, Val length_val) {
	if (VAL_TAG(length_val) != T_FLOAT) {
		fail(interp, "expected a positive integer length; got %s", tag_name(VAL_TAG(length_val)));
		return -1;
	}

	double length = VAL_NUMBER(length_val);
	if (!(length >= 1) || length != floor(length) || length > INT_MAX) {
		fail(interp, "expected a positive integer length; got %g", length);
		return -1;
	}
	return (int)length;
}

void p_fft(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val signal_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(signal_val, T_MATRIX, "fft", "a matrix");
	Object *signal = OBJECT_AT(VAL_DATA(signal_val));
	int n_samples = vector_length(interp, signal, "a vector");
	if (n_samples < 0)
		return;
	if (n_samples < 1) {
		fail(interp, "expected a non-empty vector; got %dx%d", signal->matrix.rows, signal->matrix.columns);
		return;
	}

	int n_bins = n_samples / 2 + 1;
	NEW_MATRIX(spectrum_handle, spectrum, n_bins, 2);

	double *packed;
	MALLOC_OR_FAIL(interp, packed, sizeof(double) * (size_t)n_samples);
	memcpy(packed, signal->matrix.elements, sizeof(double) * (size_t)n_samples);

	rfft_plan plan = make_rfft_plan((size_t)n_samples);
	if (!plan) {
		free(packed);
		fail(interp, "out of memory");
		return;
	}
	int status = rfft_forward(plan, packed, 1.0);
	destroy_rfft_plan(plan);
	if (status != 0) {
		free(packed);
		fail(interp, "out of memory");
		return;
	}

	double *bins = spectrum->matrix.elements;
	bins[0] = packed[0];
	bins[1] = 0.0;
	for (int k = 1; k < n_bins; k++) {
		int real_index = 2 * k - 1;
		bins[2 * k] = packed[real_index];
		bins[2 * k + 1] = real_index + 1 < n_samples ? packed[real_index + 1] : 0.0;
	}
	free(packed);

	chain_sp[-1] = make_matrix(spectrum_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_ifft(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val spectrum_val = chain_sp[-2];
	REQUIRE_CHAIN_TAG(spectrum_val, T_MATRIX, "ifft", "a matrix");
	Object *spectrum = OBJECT_AT(VAL_DATA(spectrum_val));
	int n_bins = complex_length(interp, spectrum);
	if (n_bins < 0)
		return;
	int n_samples = positive_length(interp, chain_sp[-1]);
	if (n_samples < 0)
		return;
	if (n_samples / 2 + 1 != n_bins) {
		fail(interp, "expected %d spectrum rows for length %d; got %d", n_samples / 2 + 1, n_samples, n_bins);
		return;
	}

	NEW_MATRIX(signal_handle, signal, n_samples, 1);

	double *packed = signal->matrix.elements;
	const double *bins = spectrum->matrix.elements;
	packed[0] = bins[0];
	for (int k = 1; k < n_bins; k++) {
		int real_index = 2 * k - 1;
		packed[real_index] = bins[2 * k];
		if (real_index + 1 < n_samples)
			packed[real_index + 1] = bins[2 * k + 1];
	}

	rfft_plan plan = make_rfft_plan((size_t)n_samples);
	if (!plan) {
		fail(interp, "out of memory");
		return;
	}
	int status = rfft_backward(plan, packed, 1.0 / (double)n_samples);
	destroy_rfft_plan(plan);
	if (status != 0) {
		fail(interp, "out of memory");
		return;
	}

	chain_sp[-2] = make_matrix(signal_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

static int complex_transform(Interpreter *interp, Val complex_val, int forward) {
	if (VAL_TAG(complex_val) != T_MATRIX) {
		fail(interp, "expected a matrix; got %s", tag_name(VAL_TAG(complex_val)));
		return -1;
	}
	Object *complex_vector = OBJECT_AT(VAL_DATA(complex_val));
	int n_points = complex_length(interp, complex_vector);
	if (n_points < 0)
		return -1;

	int transformed_handle = object_new_matrix(interp, n_points, 2);
	if (interp->error_flag)
		return -1;
	double *interleaved = OBJECT_AT(transformed_handle)->matrix.elements;
	memcpy(interleaved, complex_vector->matrix.elements, sizeof(double) * 2 * (size_t)n_points);

	cfft_plan plan = make_cfft_plan((size_t)n_points);
	if (!plan) {
		fail(interp, "out of memory");
		return -1;
	}
	int status = forward
		? cfft_forward(plan, interleaved, 1.0)
		: cfft_backward(plan, interleaved, 1.0 / (double)n_points);
	destroy_cfft_plan(plan);
	if (status != 0) {
		fail(interp, "out of memory");
		return -1;
	}
	return transformed_handle;
}

void p_cfft(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	int transformed_handle = complex_transform(interp, chain_sp[-1], 1);
	if (transformed_handle < 0)
		return;

	chain_sp[-1] = make_matrix(transformed_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_icfft(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	int transformed_handle = complex_transform(interp, chain_sp[-1], 0);
	if (transformed_handle < 0)
		return;

	chain_sp[-1] = make_matrix(transformed_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_magnitudes(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val spectrum_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(spectrum_val, T_MATRIX, "magnitudes", "a matrix");
	Object *spectrum = OBJECT_AT(VAL_DATA(spectrum_val));
	int n_bins = complex_length(interp, spectrum);
	if (n_bins < 0)
		return;

	NEW_MATRIX(moduli_handle, moduli, n_bins, 1);

	const double *bins = spectrum->matrix.elements;
	double *moduli_elements = moduli->matrix.elements;
	for (int k = 0; k < n_bins; k++)
		moduli_elements[k] = hypot(bins[2 * k], bins[2 * k + 1]);

	chain_sp[-1] = make_matrix(moduli_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_hann(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	int n_points = positive_length(interp, chain_sp[-1]);
	if (n_points < 0)
		return;

	NEW_MATRIX(window_handle, window, n_points, 1);

	double *weights = window->matrix.elements;
	if (n_points == 1)
		weights[0] = 1.0;
	for (int k = 0; k < n_points && n_points > 1; k++)
		weights[k] = 0.5 - 0.5 * cos(2.0 * M_PI * (double)k / (double)n_points);

	chain_sp[-1] = make_matrix(window_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}
