#include "water.h"

#define EXACT_POWER_BIT_CAP (1 << 20)

static int mag_length(const uint32_t *limbs, int n) {
	while (n > 1 && limbs[n - 1] == 0)
		n--;
	return n;
}

static int mag_is_zero(const uint32_t *limbs, int n) {
	return n == 1 && limbs[0] == 0;
}

static int mag_is_one(const uint32_t *limbs, int n) {
	return n == 1 && limbs[0] == 1;
}

static int mag_cmp(const uint32_t *a, int na, const uint32_t *b, int nb) {
	if (na != nb)
		return na < nb ? -1 : 1;

	for (int i = na - 1; i >= 0; i--)
		if (a[i] != b[i])
			return a[i] < b[i] ? -1 : 1;
	return 0;
}

static int mag_add(const uint32_t *a, int na, const uint32_t *b, int nb, uint32_t *out) {
	uint64_t carry = 0;
	int n = MAX(na, nb);

	for (int i = 0; i < n; i++) {
		uint64_t sum = carry;
		if (i < na)
			sum += a[i];
		if (i < nb)
			sum += b[i];
		out[i] = (uint32_t)sum;
		carry = sum >> 32;
	}
	out[n] = (uint32_t)carry;
	return mag_length(out, n + 1);
}

static int mag_sub(const uint32_t *a, int na, const uint32_t *b, int nb, uint32_t *out) {
	int64_t borrow = 0;

	for (int i = 0; i < na; i++) {
		int64_t diff = (int64_t)a[i] - borrow - (i < nb ? (int64_t)b[i] : 0);
		borrow = diff < 0;
		out[i] = (uint32_t)(diff + (borrow << 32));
	}
	return mag_length(out, na);
}

static int mag_mul(const uint32_t *a, int na, const uint32_t *b, int nb, uint32_t *out) {
	memset(out, 0, ((size_t)na + (size_t)nb) * 4);
	for (int i = 0; i < na; i++) {
		uint64_t carry = 0;
		for (int j = 0; j < nb; j++) {
			uint64_t t = (uint64_t)a[i] * b[j] + out[i + j] + carry;
			out[i + j] = (uint32_t)t;
			carry = t >> 32;
		}
		out[i + nb] = (uint32_t)carry;
	}
	return mag_length(out, na + nb);
}

static int mag_bit(const uint32_t *a, int i) {
	return (a[i >> 5] >> (i & 31)) & 1;
}

static int mag_bit_length(const uint32_t *a, int n) {
	n = mag_length(a, n);
	if (mag_is_zero(a, n))
		return 0;

	uint32_t top = a[n - 1];
	int bits = 0;
	while (top) {
		bits++;
		top >>= 1;
	}
	return (n - 1) * 32 + bits;
}

static int mag_trailing_zero_bits(const uint32_t *a, int n) {
	for (int i = 0; i < n; i++)
		if (a[i])
			return i * 32 + __builtin_ctz(a[i]);
	return 0;
}

static void mag_divmod(const uint32_t *a, int na, const uint32_t *b, int nb,
		uint32_t *quotient, int *n_quotient, uint32_t *remainder, int *n_remainder) {
	memset(quotient, 0, (size_t)na * 4);
	memset(remainder, 0, ((size_t)nb + 1) * 4);
	int nr = 1;

	for (int i = mag_bit_length(a, na) - 1; i >= 0; i--) {
		uint32_t carry = (uint32_t)mag_bit(a, i);
		for (int j = 0; j < nr; j++) {
			uint32_t next = remainder[j] >> 31;
			remainder[j] = (remainder[j] << 1) | carry;
			carry = next;
		}
		if (carry)
			remainder[nr++] = carry;

		if (mag_cmp(remainder, nr, b, nb) >= 0) {
			nr = mag_sub(remainder, nr, b, nb, remainder);
			quotient[i >> 5] |= 1u << (i & 31);
		}
	}

	*n_quotient = mag_length(quotient, na);
	*n_remainder = mag_length(remainder, nr);
}

static int mag_gcd(const uint32_t *a, int na, const uint32_t *b, int nb, uint32_t *out) {
	int cap = MAX(na, nb);
	uint32_t *x = arena_malloc((size_t)cap * 4);
	uint32_t *y = arena_malloc((size_t)cap * 4);
	uint32_t *q = arena_malloc((size_t)cap * 4);
	uint32_t *r = arena_malloc(((size_t)cap + 1) * 4);

	memcpy(x, a, (size_t)na * 4);
	int nx = na;
	memcpy(y, b, (size_t)nb * 4);
	int ny = nb;

	while (!mag_is_zero(y, ny)) {
		int nq;
		int nr;
		mag_divmod(x, nx, y, ny, q, &nq, r, &nr);
		memcpy(x, y, (size_t)ny * 4);
		nx = ny;
		memcpy(y, r, (size_t)nr * 4);
		ny = nr;
	}

	memcpy(out, x, (size_t)nx * 4);
	int n = nx;

	arena_free(x);
	arena_free(y);
	arena_free(q);
	arena_free(r);
	return n;
}

static int mag_shift_left(const uint32_t *a, int na, int bits, uint32_t *out) {
	int limb_shift = bits >> 5;
	int bit_shift = bits & 31;
	int n_out = na + limb_shift + 1;

	memset(out, 0, (size_t)n_out * 4);
	for (int i = 0; i < na; i++) {
		uint64_t t = (uint64_t)a[i] << bit_shift;
		out[i + limb_shift] |= (uint32_t)t;
		out[i + limb_shift + 1] |= (uint32_t)(t >> 32);
	}
	return mag_length(out, n_out);
}

static uint32_t mag_divmod_small(uint32_t *a, int *n, uint32_t divisor) {
	uint64_t remainder = 0;

	for (int i = *n - 1; i >= 0; i--) {
		uint64_t current = (remainder << 32) | a[i];
		a[i] = (uint32_t)(current / divisor);
		remainder = current % divisor;
	}
	*n = mag_length(a, *n);
	return (uint32_t)remainder;
}

static int mag_from_decimal(const char *digits, int n_digits, uint32_t *out) {
	out[0] = 0;
	int n = 1;

	for (int i = 0; i < n_digits; i++) {
		uint64_t carry = (uint32_t)(digits[i] - '0');
		for (int j = 0; j < n; j++) {
			uint64_t t = (uint64_t)out[j] * 10 + carry;
			out[j] = (uint32_t)t;
			carry = t >> 32;
		}
		if (carry)
			out[n++] = (uint32_t)carry;
	}
	return n;
}

static void mag_print(FILE *out, const uint32_t *a, int na) {
	if (mag_is_zero(a, na)) {
		fputc('0', out);
		return;
	}

	uint32_t *work = arena_malloc((size_t)na * 4);
	uint32_t *groups = arena_malloc(((size_t)na * 32 / 29 + 2) * 4);
	memcpy(work, a, (size_t)na * 4);
	int n = na;
	int n_groups = 0;

	while (!mag_is_zero(work, n))
		groups[n_groups++] = mag_divmod_small(work, &n, 1000000000u);

	fprintf(out, "%u", groups[n_groups - 1]);
	for (int i = n_groups - 2; i >= 0; i--)
		fprintf(out, "%09u", groups[i]);

	arena_free(work);
	arena_free(groups);
}

static const uint32_t mag_zero_limb = 0;
static const uint32_t mag_one_limb = 1;

static Val exact_wrap(Interpreter *interp, int handle) {
	(void)interp;
	if (handle < 0)
		return make_tagged(T_NONE, 0);
	return make_exact(handle);
}

static void mag_reduce(uint32_t *numerator, int *n_numerator, uint32_t *denominator, int *n_denominator) {
	if (mag_is_one(denominator, *n_denominator))
		return;

	uint32_t *divisor = arena_malloc((size_t)MAX(*n_numerator, *n_denominator) * 4);
	int n_divisor = mag_gcd(numerator, *n_numerator, denominator, *n_denominator, divisor);

	if (!mag_is_one(divisor, n_divisor)) {
		uint32_t *quotient = arena_malloc((size_t)MAX(*n_numerator, *n_denominator) * 4);
		uint32_t *remainder = arena_malloc(((size_t)n_divisor + 1) * 4);
		int n_quotient;
		int n_remainder;

		mag_divmod(numerator, *n_numerator, divisor, n_divisor, quotient, &n_quotient, remainder, &n_remainder);
		memcpy(numerator, quotient, (size_t)n_quotient * 4);
		*n_numerator = n_quotient;
		mag_divmod(denominator, *n_denominator, divisor, n_divisor, quotient, &n_quotient, remainder, &n_remainder);
		memcpy(denominator, quotient, (size_t)n_quotient * 4);
		*n_denominator = n_quotient;

		arena_free(quotient);
		arena_free(remainder);
	}
	arena_free(divisor);
}

static Val exact_normalized(Interpreter *interp, int sign, uint32_t *numerator, int n_numerator,
		uint32_t *denominator, int n_denominator) {
	n_numerator = mag_length(numerator, n_numerator);
	n_denominator = mag_length(denominator, n_denominator);

	if (mag_is_zero(numerator, n_numerator))
		return exact_wrap(interp, object_new_exact(interp, 0, &mag_zero_limb, 1, &mag_one_limb, 1));

	mag_reduce(numerator, &n_numerator, denominator, &n_denominator);

	return exact_wrap(interp, object_new_exact(interp, sign, numerator, n_numerator, denominator, n_denominator));
}

#define EXACT_PARTS(value, obj_var, sign_var, num_var, n_num_var, den_var, n_den_var) \
	Object *obj_var = OBJECT_AT(VAL_DATA(value)); \
	int sign_var = (obj_var)->exact.sign; \
	const uint32_t *num_var = (obj_var)->exact.limbs; \
	int n_num_var = (obj_var)->exact.n_numerator; \
	const uint32_t *den_var = (obj_var)->exact.limbs + (obj_var)->exact.n_numerator; \
	int n_den_var = (obj_var)->exact.n_denominator

int exact_truthy_value(Val value) {
	return OBJECT_AT(VAL_DATA(value))->exact.sign != 0;
}

int exact_is_integer(Val value) {
	Object *obj = OBJECT_AT(VAL_DATA(value));
	return obj->exact.n_denominator == 1 && obj->exact.limbs[obj->exact.n_numerator] == 1;
}

static void mag_print_fraction(FILE *out, int sign, const uint32_t *num, int n_num,
		const uint32_t *den, int n_den) {
	if (sign < 0)
		fputc('-', out);
	mag_print(out, num, n_num);
	if (!mag_is_one(den, n_den)) {
		fputc('/', out);
		mag_print(out, den, n_den);
	}
}

void exact_print(FILE *out, Val value) {
	EXACT_PARTS(value, obj, sign, num, n_num, den, n_den);

	mag_print_fraction(out, sign, num, n_num, den, n_den);
}

void exact_print_scaled(FILE *out, Val value, long long ratio_numerator, long long ratio_denominator) {
	EXACT_PARTS(value, obj, sign, num, n_num, den, n_den);

	uint32_t top_limbs[2] = { (uint32_t)ratio_numerator, (uint32_t)((uint64_t)ratio_numerator >> 32) };
	int n_top = mag_length(top_limbs, 2);
	uint32_t bottom_limbs[2] = { (uint32_t)ratio_denominator, (uint32_t)((uint64_t)ratio_denominator >> 32) };
	int n_bottom = mag_length(bottom_limbs, 2);

	uint32_t *scaled_numerator = arena_malloc(((size_t)n_num + n_top) * 4);
	uint32_t *scaled_denominator = arena_malloc(((size_t)n_den + n_bottom) * 4);
	int n_scaled_numerator = mag_mul(num, n_num, top_limbs, n_top, scaled_numerator);
	int n_scaled_denominator = mag_mul(den, n_den, bottom_limbs, n_bottom, scaled_denominator);

	mag_reduce(scaled_numerator, &n_scaled_numerator, scaled_denominator, &n_scaled_denominator);
	mag_print_fraction(out, sign, scaled_numerator, n_scaled_numerator, scaled_denominator, n_scaled_denominator);

	arena_free(scaled_numerator);
	arena_free(scaled_denominator);
}

double exact_to_double(Val value) {
	EXACT_PARTS(value, obj, sign, num, n_num, den, n_den);

	if (sign == 0)
		return 0.0;

	int numerator_bits = mag_bit_length(num, n_num);
	int denominator_bits = mag_bit_length(den, n_den);
	int quotient_exponent = numerator_bits - denominator_bits;
	int shift = 55 - quotient_exponent;

	const uint32_t *dividend = num;
	int n_dividend = n_num;
	const uint32_t *divisor = den;
	int n_divisor = n_den;
	uint32_t *shifted = NULL;
	if (shift > 0) {
		shifted = arena_malloc(((size_t)n_num + (size_t)shift / 32 + 2) * 4);
		n_dividend = mag_shift_left(num, n_num, shift, shifted);
		dividend = shifted;
	} else if (shift < 0) {
		shifted = arena_malloc(((size_t)n_den + (size_t)(-shift) / 32 + 2) * 4);
		n_divisor = mag_shift_left(den, n_den, -shift, shifted);
		divisor = shifted;
	}

	uint32_t *quotient = arena_malloc(((size_t)n_dividend + 1) * 4);
	uint32_t *remainder = arena_malloc(((size_t)n_divisor + 1) * 4);
	int n_quotient;
	int n_remainder;
	mag_divmod(dividend, n_dividend, divisor, n_divisor, quotient, &n_quotient, remainder, &n_remainder);
	int sticky = !mag_is_zero(remainder, n_remainder);

	uint64_t scaled_quotient = quotient[0];
	if (n_quotient > 1)
		scaled_quotient |= (uint64_t)quotient[1] << 32;
	arena_free(shifted);
	arena_free(quotient);
	arena_free(remainder);

	int leading = 64 - __builtin_clzll(scaled_quotient);
	int leading_exponent = quotient_exponent - 55 + leading - 1;

	double magnitude;
	if (leading_exponent > 1023) {
		magnitude = HUGE_VAL;
	} else if (leading_exponent < -1075) {
		magnitude = 0.0;
	} else {
		int precision = leading_exponent >= -1022 ? 53 : leading_exponent + 1075;
		int dropped = leading - precision;
		uint64_t kept = scaled_quotient >> dropped;
		uint64_t guard = (scaled_quotient >> (dropped - 1)) & 1;
		int sticky_below = (scaled_quotient & (((uint64_t)1 << (dropped - 1)) - 1)) != 0 || sticky;
		if (guard && (sticky_below || (kept & 1)))
			kept++;
		magnitude = ldexp((double)kept, leading_exponent - precision + 1);
	}

	return sign < 0 ? -magnitude : magnitude;
}

Val exact_from_double(Interpreter *interp, double value) {
	if (isnan(value) || isinf(value)) {
		fail(interp, "expected a finite float");
		return make_tagged(T_NONE, 0);
	}
	if (value == 0.0) {
		uint32_t zero = 0;
		uint32_t one = 1;
		return exact_normalized(interp, 0, &zero, 1, &one, 1);
	}

	int sign = value < 0 ? -1 : 1;
	int exponent;
	double mantissa_fraction = frexp(fabs(value), &exponent);
	uint64_t mantissa = (uint64_t)ldexp(mantissa_fraction, 53);
	exponent -= 53;

	while ((mantissa & 1) == 0 && exponent < 0) {
		mantissa >>= 1;
		exponent++;
	}

	uint32_t mantissa_limbs[2] = { (uint32_t)mantissa, (uint32_t)(mantissa >> 32) };
	int n_mantissa = mag_length(mantissa_limbs, 2);

	if (exponent >= 0) {
		uint32_t *numerator = arena_malloc(((size_t)n_mantissa + (size_t)exponent / 32 + 2) * 4);
		int n_numerator = mag_shift_left(mantissa_limbs, n_mantissa, exponent, numerator);
		uint32_t one = 1;
		Val result = exact_normalized(interp, sign, numerator, n_numerator, &one, 1);
		arena_free(numerator);
		return result;
	}

	uint32_t one = 1;
	uint32_t *denominator = arena_malloc(((size_t)(-exponent) / 32 + 2) * 4);
	int n_denominator = mag_shift_left(&one, 1, -exponent, denominator);
	Val result = exact_normalized(interp, sign, mantissa_limbs, n_mantissa, denominator, n_denominator);
	arena_free(denominator);
	return result;
}

Val exact_from_int64(Interpreter *interp, int64_t value) {
	int sign = value == 0 ? 0 : (value < 0 ? -1 : 1);
	uint64_t magnitude = value < 0 ? (uint64_t)(-(value + 1)) + 1 : (uint64_t)value;
	uint32_t limbs[2] = { (uint32_t)magnitude, (uint32_t)(magnitude >> 32) };
	uint32_t one = 1;

	return exact_normalized(interp, sign, limbs, mag_length(limbs, 2), &one, 1);
}

static Val exact_reciprocal(Interpreter *interp, Val value) {
	EXACT_PARTS(value, obj, sign, num, n_num, den, n_den);

	uint32_t *flipped_numerator = arena_malloc((size_t)n_den * 4);
	uint32_t *flipped_denominator = arena_malloc((size_t)n_num * 4);
	memcpy(flipped_numerator, den, (size_t)n_den * 4);
	memcpy(flipped_denominator, num, (size_t)n_num * 4);

	Val result = exact_normalized(interp, sign, flipped_numerator, n_den, flipped_denominator, n_num);
	arena_free(flipped_numerator);
	arena_free(flipped_denominator);
	return result;
}

Val exact_scale_by_ratio(Interpreter *interp, Val value, long long ratio_numerator, long long ratio_denominator) {
	if (ratio_numerator == ratio_denominator)
		return value;

	gc_root_push(interp, value);
	Val numerator_exact = exact_from_int64(interp, ratio_numerator);
	if (interp->error_flag) {
		gc_root_pop(interp);
		return make_tagged(T_NONE, 0);
	}

	Val scaled = exact_binary(interp, value, numerator_exact, EXACT_OP_MUL);
	gc_root_pop(interp);
	if (interp->error_flag)
		return make_tagged(T_NONE, 0);
	if (ratio_denominator == 1)
		return scaled;

	gc_root_push(interp, scaled);
	Val denominator_exact = exact_from_int64(interp, ratio_denominator);
	if (interp->error_flag) {
		gc_root_pop(interp);
		return make_tagged(T_NONE, 0);
	}

	Val quotient = exact_binary(interp, scaled, denominator_exact, EXACT_OP_DIV);
	gc_root_pop(interp);
	return quotient;
}

int exact_fits_int64(Val value, int64_t *out) {
	EXACT_PARTS(value, obj, sign, num, n_num, den, n_den);

	if (!mag_is_one(den, n_den) || mag_bit_length(num, n_num) > 63)
		return 0;

	uint64_t magnitude = num[0];
	if (n_num > 1)
		magnitude |= (uint64_t)num[1] << 32;
	*out = sign < 0 ? -(int64_t)magnitude : (int64_t)magnitude;
	return 1;
}

static Val exact_from_signed_cross(Interpreter *interp, int sign_a, uint32_t *term_a, int n_a,
		int sign_b, uint32_t *term_b, int n_b, uint32_t *denominator, int n_denominator) {
	if (sign_a == 0 || mag_is_zero(term_a, n_a))
		return exact_normalized(interp, sign_b, term_b, n_b, denominator, n_denominator);
	if (sign_b == 0 || mag_is_zero(term_b, n_b))
		return exact_normalized(interp, sign_a, term_a, n_a, denominator, n_denominator);

	if (sign_a == sign_b) {
		uint32_t *sum = arena_malloc(((size_t)MAX(n_a, n_b) + 1) * 4);
		int n_sum = mag_add(term_a, n_a, term_b, n_b, sum);
		Val result = exact_normalized(interp, sign_a, sum, n_sum, denominator, n_denominator);
		arena_free(sum);
		return result;
	}

	int order = mag_cmp(term_a, n_a, term_b, n_b);
	if (order == 0) {
		uint32_t zero = 0;
		uint32_t one = 1;
		return exact_normalized(interp, 0, &zero, 1, &one, 1);
	}
	if (order > 0) {
		int n = mag_sub(term_a, n_a, term_b, n_b, term_a);
		return exact_normalized(interp, sign_a, term_a, n, denominator, n_denominator);
	}
	int n = mag_sub(term_b, n_b, term_a, n_a, term_b);
	return exact_normalized(interp, sign_b, term_b, n, denominator, n_denominator);
}

Val exact_binary(Interpreter *interp, Val left, Val right, int op) {
	EXACT_PARTS(left, left_obj, ls, ln, n_ln, ld, n_ld);
	EXACT_PARTS(right, right_obj, rs, rn, n_rn, rd, n_rd);

	if (op == EXACT_OP_MUL || op == EXACT_OP_DIV) {
		if (op == EXACT_OP_DIV && rs == 0) {
			fail(interp, "division by zero");
			return make_tagged(T_NONE, 0);
		}
		const uint32_t *b_num = op == EXACT_OP_MUL ? rn : rd;
		int n_b_num = op == EXACT_OP_MUL ? n_rn : n_rd;
		const uint32_t *b_den = op == EXACT_OP_MUL ? rd : rn;
		int n_b_den = op == EXACT_OP_MUL ? n_rd : n_rn;

		uint32_t *numerator = arena_malloc(((size_t)n_ln + n_b_num) * 4);
		uint32_t *denominator = arena_malloc(((size_t)n_ld + n_b_den) * 4);
		int n_numerator = mag_mul(ln, n_ln, b_num, n_b_num, numerator);
		int n_denominator = mag_mul(ld, n_ld, b_den, n_b_den, denominator);
		Val result = exact_normalized(interp, ls * rs, numerator, n_numerator, denominator, n_denominator);
		arena_free(numerator);
		arena_free(denominator);
		return result;
	}

	int right_sign = op == EXACT_OP_SUB ? -rs : rs;
	uint32_t *term_left = arena_malloc(((size_t)n_ln + n_rd) * 4);
	uint32_t *term_right = arena_malloc(((size_t)n_rn + n_ld) * 4);
	uint32_t *denominator = arena_malloc(((size_t)n_ld + n_rd) * 4);
	int n_term_left = mag_mul(ln, n_ln, rd, n_rd, term_left);
	int n_term_right = mag_mul(rn, n_rn, ld, n_ld, term_right);
	int n_denominator = mag_mul(ld, n_ld, rd, n_rd, denominator);

	Val result = exact_from_signed_cross(interp, ls, term_left, n_term_left,
			right_sign, term_right, n_term_right, denominator, n_denominator);
	arena_free(term_left);
	arena_free(term_right);
	arena_free(denominator);
	return result;
}

int exact_cmp(Interpreter *interp, Val left, Val right) {
	(void)interp;
	EXACT_PARTS(left, left_obj, ls, ln, n_ln, ld, n_ld);
	EXACT_PARTS(right, right_obj, rs, rn, n_rn, rd, n_rd);

	if (ls != rs)
		return ls < rs ? -1 : 1;
	if (ls == 0)
		return 0;

	uint32_t *cross_left = arena_malloc(((size_t)n_ln + n_rd) * 4);
	uint32_t *cross_right = arena_malloc(((size_t)n_rn + n_ld) * 4);
	int n_cross_left = mag_mul(ln, n_ln, rd, n_rd, cross_left);
	int n_cross_right = mag_mul(rn, n_rn, ld, n_ld, cross_right);
	int order = mag_cmp(cross_left, n_cross_left, cross_right, n_cross_right);
	arena_free(cross_left);
	arena_free(cross_right);

	return ls < 0 ? -order : order;
}

int exact_cmp_double(Val left, double right) {
	EXACT_PARTS(left, left_obj, sign, num, n_num, den, n_den);

	if (isnan(right) || isinf(right))
		return (isnan(right) || right > 0) ? -1 : 1;

	int right_sign = right == 0.0 ? 0 : (right < 0 ? -1 : 1);
	if (sign != right_sign)
		return sign < right_sign ? -1 : 1;
	if (sign == 0)
		return 0;

	int exponent;
	double mantissa_fraction = frexp(fabs(right), &exponent);
	uint64_t mantissa = (uint64_t)ldexp(mantissa_fraction, 53);
	exponent -= 53;
	uint32_t mantissa_limbs[2] = { (uint32_t)mantissa, (uint32_t)(mantissa >> 32) };
	int n_mantissa = mag_length(mantissa_limbs, 2);

	uint32_t *cross_left = arena_malloc(((size_t)n_num + (exponent < 0 ? (size_t)(-exponent) / 32 + 2 : 0) + 1) * 4);
	int n_cross_left;
	if (exponent < 0) {
		n_cross_left = mag_shift_left(num, n_num, -exponent, cross_left);
	} else {
		memcpy(cross_left, num, (size_t)n_num * 4);
		n_cross_left = n_num;
	}

	uint32_t *scaled_mantissa = arena_malloc(((size_t)n_mantissa + (exponent > 0 ? (size_t)exponent / 32 + 2 : 0) + 1) * 4);
	int n_scaled_mantissa;
	if (exponent > 0) {
		n_scaled_mantissa = mag_shift_left(mantissa_limbs, n_mantissa, exponent, scaled_mantissa);
	} else {
		memcpy(scaled_mantissa, mantissa_limbs, (size_t)n_mantissa * 4);
		n_scaled_mantissa = n_mantissa;
	}

	uint32_t *cross_right = arena_malloc(((size_t)n_scaled_mantissa + n_den) * 4);
	int n_cross_right = mag_mul(scaled_mantissa, n_scaled_mantissa, den, n_den, cross_right);

	int order = mag_cmp(cross_left, n_cross_left, cross_right, n_cross_right);
	arena_free(cross_left);
	arena_free(scaled_mantissa);
	arena_free(cross_right);

	return sign < 0 ? -order : order;
}

static Val exact_integer_result(Interpreter *interp, int sign, uint32_t *magnitude, int n) {
	uint32_t one = 1;
	if (mag_is_zero(magnitude, n))
		sign = 0;
	return exact_normalized(interp, sign, magnitude, n, &one, 1);
}

static Val exact_round_family(Interpreter *interp, Val value, int op) {
	EXACT_PARTS(value, obj, sign, num, n_num, den, n_den);

	uint32_t *quotient = arena_malloc((size_t)n_num * 4);
	uint32_t *remainder = arena_malloc(((size_t)n_den + 1) * 4);
	int n_quotient;
	int n_remainder;
	mag_divmod(num, n_num, den, n_den, quotient, &n_quotient, remainder, &n_remainder);

	int bump = 0;
	if (!mag_is_zero(remainder, n_remainder)) {
		if (op == EXACT_OP_ROUND_UP)
			bump = sign > 0;
		else if (op == EXACT_OP_ROUND_DOWN)
			bump = sign < 0;
		else if (op == EXACT_OP_ROUND) {
			uint32_t *doubled = arena_malloc(((size_t)n_remainder + 1) * 4);
			int n_doubled = mag_add(remainder, n_remainder, remainder, n_remainder, doubled);
			bump = mag_cmp(doubled, n_doubled, den, n_den) >= 0;
			arena_free(doubled);
		}
	}

	Val result;
	if (bump) {
		uint32_t one = 1;
		uint32_t *bumped = arena_malloc(((size_t)n_quotient + 1) * 4);
		int n_bumped = mag_add(quotient, n_quotient, &one, 1, bumped);
		result = exact_integer_result(interp, sign, bumped, n_bumped);
		arena_free(bumped);
	} else {
		result = exact_integer_result(interp, sign, quotient, n_quotient);
	}

	arena_free(quotient);
	arena_free(remainder);
	return result;
}

Val exact_unary(Interpreter *interp, Val value, int op) {
	EXACT_PARTS(value, obj, sign, num, n_num, den, n_den);

	switch (op) {
		case EXACT_OP_NEGATE:
		case EXACT_OP_ABS: {
			int result_sign = op == EXACT_OP_ABS ? (sign ? 1 : 0) : -sign;
			return exact_wrap(interp, object_new_exact(interp, result_sign, num, n_num, den, n_den));
		}
		case EXACT_OP_SQUARE: {
			uint32_t *numerator = arena_malloc((size_t)n_num * 2 * 4);
			uint32_t *denominator = arena_malloc((size_t)n_den * 2 * 4);
			int n_numerator = mag_mul(num, n_num, num, n_num, numerator);
			int n_denominator = mag_mul(den, n_den, den, n_den, denominator);
			Val result = exact_wrap(interp, object_new_exact(interp, sign ? 1 : 0,
					numerator, n_numerator, denominator, n_denominator));
			arena_free(numerator);
			arena_free(denominator);
			return result;
		}
		case EXACT_OP_INCREMENT:
		case EXACT_OP_DECREMENT: {
			int one_sign = op == EXACT_OP_INCREMENT ? 1 : -1;
			uint32_t *term_left = arena_malloc((size_t)n_num * 4);
			uint32_t *term_right = arena_malloc((size_t)n_den * 4);
			uint32_t *denominator = arena_malloc((size_t)n_den * 4);
			memcpy(term_left, num, (size_t)n_num * 4);
			memcpy(term_right, den, (size_t)n_den * 4);
			memcpy(denominator, den, (size_t)n_den * 4);
			Val result = exact_from_signed_cross(interp, sign, term_left, n_num,
					one_sign, term_right, n_den, denominator, n_den);
			arena_free(term_left);
			arena_free(term_right);
			arena_free(denominator);
			return result;
		}
		default:
			return exact_round_family(interp, value, op);
	}
}

Val exact_truncated_quotient(Interpreter *interp, Val left, Val right) {
	EXACT_PARTS(left, left_obj, ls, ln, n_ln, ld, n_ld);
	EXACT_PARTS(right, right_obj, rs, rn, n_rn, rd, n_rd);

	uint32_t *cross_left = arena_malloc(((size_t)n_ln + n_rd) * 4);
	uint32_t *cross_right = arena_malloc(((size_t)n_rn + n_ld) * 4);
	int n_cross_left = mag_mul(ln, n_ln, rd, n_rd, cross_left);
	int n_cross_right = mag_mul(rn, n_rn, ld, n_ld, cross_right);

	uint32_t *quotient = arena_malloc((size_t)n_cross_left * 4);
	uint32_t *remainder = arena_malloc(((size_t)n_cross_right + 1) * 4);
	int n_quotient;
	int n_remainder;
	mag_divmod(cross_left, n_cross_left, cross_right, n_cross_right, quotient, &n_quotient, remainder, &n_remainder);

	Val result = exact_integer_result(interp, ls * rs, quotient, n_quotient);
	arena_free(cross_left);
	arena_free(cross_right);
	arena_free(quotient);
	arena_free(remainder);
	return result;
}

Val exact_power(Interpreter *interp, Val base, double exponent) {
	if (exponent != trunc(exponent) || fabs(exponent) > 2147483647.0) {
		fail(interp, "exact ^ needs an integer exponent; got %g", exponent);
		return make_tagged(T_NONE, 0);
	}

	EXACT_PARTS(base, base_obj, sign, num, n_num, den, n_den);
	int64_t e = (int64_t)exponent;

	if (e == 0) {
		uint32_t one = 1;
		return exact_normalized(interp, 1, &one, 1, &one, 1);
	}
	if (sign == 0) {
		if (e < 0) {
			fail(interp, "division by zero");
			return make_tagged(T_NONE, 0);
		}
		uint32_t zero = 0;
		uint32_t one = 1;
		return exact_normalized(interp, 0, &zero, 1, &one, 1);
	}

	int negative_exponent = e < 0;
	if (negative_exponent)
		e = -e;

	int64_t result_bits = (int64_t)(mag_bit_length(num, n_num) + mag_bit_length(den, n_den)) * e;
	if (result_bits > EXACT_POWER_BIT_CAP) {
		fail(interp, "exact ^ result too large (over %d bits)", EXACT_POWER_BIT_CAP);
		return make_tagged(T_NONE, 0);
	}

	size_t cap = (size_t)(result_bits / 32 + 2);
	uint32_t *result_num = arena_malloc(cap * 4);
	uint32_t *result_den = arena_malloc(cap * 4);
	uint32_t *square_num = arena_malloc(cap * 4);
	uint32_t *square_den = arena_malloc(cap * 4);
	uint32_t *scratch = arena_malloc(cap * 4);
	int n_result_num = 1;
	int n_result_den = 1;
	int n_square_num = n_num;
	int n_square_den = n_den;

	result_num[0] = 1;
	result_den[0] = 1;
	memcpy(square_num, num, (size_t)n_num * 4);
	memcpy(square_den, den, (size_t)n_den * 4);

	for (int64_t remaining = e; remaining > 0; remaining >>= 1) {
		if (remaining & 1) {
			int n = mag_mul(result_num, n_result_num, square_num, n_square_num, scratch);
			memcpy(result_num, scratch, (size_t)n * 4);
			n_result_num = n;
			n = mag_mul(result_den, n_result_den, square_den, n_square_den, scratch);
			memcpy(result_den, scratch, (size_t)n * 4);
			n_result_den = n;
		}
		if (remaining > 1) {
			int n = mag_mul(square_num, n_square_num, square_num, n_square_num, scratch);
			memcpy(square_num, scratch, (size_t)n * 4);
			n_square_num = n;
			n = mag_mul(square_den, n_square_den, square_den, n_square_den, scratch);
			memcpy(square_den, scratch, (size_t)n * 4);
			n_square_den = n;
		}
	}

	int result_sign = (sign < 0 && (e & 1)) ? -1 : 1;
	Val result;
	if (negative_exponent)
		result = exact_wrap(interp, object_new_exact(interp, result_sign,
				result_den, n_result_den, result_num, n_result_num));
	else
		result = exact_wrap(interp, object_new_exact(interp, result_sign,
				result_num, n_result_num, result_den, n_result_den));

	arena_free(result_num);
	arena_free(result_den);
	arena_free(square_num);
	arena_free(square_den);
	arena_free(scratch);
	return result;
}

static int decimal_lossless_as_double(const uint32_t *limbs, int n) {
	int bits = mag_bit_length(limbs, n);
	if (bits <= 53)
		return 1;
	if (bits > 1024)
		return 0;
	return bits - mag_trailing_zero_bits(limbs, n) <= 53;
}

int exact_claim_integer_digits(Interpreter *interp, const char *digits, int n_digits, int negative, Val *out) {
	uint32_t *magnitude = arena_malloc(((size_t)n_digits / 9 + 2) * 4);
	int n = mag_from_decimal(digits, n_digits, magnitude);

	if (decimal_lossless_as_double(magnitude, n)) {
		arena_free(magnitude);
		return 0;
	}

	int sign = mag_is_zero(magnitude, n) ? 0 : (negative ? -1 : 1);
	uint32_t one = 1;
	*out = exact_normalized(interp, sign, magnitude, n, &one, 1);
	arena_free(magnitude);
	return 1;
}

int parse_exact_literal(Interpreter *interp, const char *token, Val *out) {
	const char *cursor = token;
	int negative = *cursor == '-';
	if (negative)
		cursor++;

	const char *num_start = cursor;
	while (*cursor >= '0' && *cursor <= '9')
		cursor++;
	int n_num_digits = (int)(cursor - num_start);
	if (n_num_digits == 0)
		return 0;

	const char *den_start = NULL;
	int n_den_digits = 0;
	if (*cursor == '/') {
		den_start = ++cursor;
		while (*cursor >= '0' && *cursor <= '9')
			cursor++;
		n_den_digits = (int)(cursor - den_start);
		if (n_den_digits == 0)
			return 0;
	}
	if (*cursor != 0)
		return 0;

	if (!den_start)
		return exact_claim_integer_digits(interp, num_start, n_num_digits, negative, out);

	uint32_t *numerator = arena_malloc(((size_t)n_num_digits / 9 + 2) * 4);
	int n_numerator = mag_from_decimal(num_start, n_num_digits, numerator);

	uint32_t *denominator = arena_malloc(((size_t)n_den_digits / 9 + 2) * 4);
	int n_denominator = mag_from_decimal(den_start, n_den_digits, denominator);
	if (mag_is_zero(denominator, n_denominator)) {
		arena_free(numerator);
		arena_free(denominator);
		fail(interp, "exact literal %s has a zero denominator", token);
		*out = make_tagged(T_NONE, 0);
		return 1;
	}

	int sign = mag_is_zero(numerator, n_numerator) ? 0 : (negative ? -1 : 1);
	*out = exact_normalized(interp, sign, numerator, n_numerator, denominator, n_denominator);
	arena_free(numerator);
	arena_free(denominator);
	return 1;
}

int exact_mod_word(Interpreter *interp, Val dividend, Val divisor, int also_quotient) {
	int left_exact = VAL_TAG(dividend) == T_EXACT;
	int right_exact = VAL_TAG(divisor) == T_EXACT;

	if (!left_exact && !right_exact)
		return 0;
	if (!(left_exact && right_exact)) {
		if (VAL_TAG(dividend) == T_FLOAT || VAL_TAG(divisor) == T_FLOAT) {
			fail(interp, "exact and float do not mix; convert with float>exact or exact>float");
			return 1;
		}
		return 0;
	}
	if (!exact_truthy_value(divisor)) {
		fail(interp, "division by zero");
		return 1;
	}

	Val quotient = exact_truncated_quotient(interp, dividend, divisor);
	if (interp->error_flag)
		return 1;
	gc_root_push(interp, quotient);
	Val product = exact_binary(interp, quotient, divisor, EXACT_OP_MUL);
	if (interp->error_flag) {
		gc_root_pop(interp);
		return 1;
	}
	gc_root_push(interp, product);
	Val remainder = exact_binary(interp, dividend, product, EXACT_OP_SUB);
	gc_root_pop(interp);
	gc_root_pop(interp);
	if (interp->error_flag)
		return 1;

	interp->data_stack[interp->dsp - 2] = remainder;
	if (also_quotient)
		interp->data_stack[interp->dsp - 1] = quotient;
	else
		interp->dsp--;
	return 1;
}

int exact_binary_word(Interpreter *interp, Val left, Val right, int op) {
	int left_exact = VAL_TAG(left) == T_EXACT;
	int right_exact = VAL_TAG(right) == T_EXACT;

	if (!left_exact && !right_exact)
		return 0;

	if (left_exact && right_exact) {
		Val result = exact_binary(interp, left, right, op);
		if (interp->error_flag)
			return 1;
		interp->data_stack[interp->dsp - 2] = result;
		interp->dsp--;
		return 1;
	}

	if (VAL_TAG(left) == T_FLOAT || VAL_TAG(right) == T_FLOAT) {
		fail(interp, "exact and float do not mix; convert with float>exact or exact>float");
		return 1;
	}

	return 0;
}

void p_float_to_exact(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val value = chain_sp[-1];
	if (VAL_TAG(value) == T_EXACT)
		DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
	REQUIRE_CHAIN_TAG(value, T_FLOAT, "float>exact", "a float");

	SYNC_REGISTERS(interp, chain_ip, chain_sp);
	Val result = exact_from_double(interp, VAL_NUMBER(value));
	if (interp->error_flag)
		return;
	interp->data_stack[interp->dsp - 1] = result;

	DISPATCH(interp);
}

static Val rationalize_cleanup(Interpreter *interp, int base_dsp, int n_roots) {
	interp->dsp = base_dsp;
	for (int i = 0; i < n_roots; i++)
		gc_root_pop(interp);
	return make_tagged(T_NONE, 0);
}

static Val exact_rationalize(Interpreter *interp, double value) {
	if (isnan(value) || isinf(value)) {
		fail(interp, "expected a finite float");
		return make_tagged(T_NONE, 0);
	}
	if (value == 0.0)
		return exact_from_int64(interp, 0);

	double magnitude = fabs(value);
	uint64_t bits;
	memcpy(&bits, &magnitude, 8);
	int closed = (bits & 1) == 0;
	int base_dsp = interp->dsp;

	Val center = exact_from_double(interp, magnitude);
	if (interp->error_flag)
		return center;
	gc_root_push(interp, center);

	Val below_exact = exact_from_double(interp, nextafter(magnitude, 0.0));
	if (interp->error_flag)
		return rationalize_cleanup(interp, base_dsp, 1);
	gc_root_push(interp, below_exact);

	Val lo = exact_binary(interp, center, below_exact, EXACT_OP_ADD);
	if (!interp->error_flag)
		lo = exact_scale_by_ratio(interp, lo, 1, 2);
	if (interp->error_flag)
		return rationalize_cleanup(interp, base_dsp, 2);
	gc_root_push(interp, lo);

	double above = nextafter(magnitude, INFINITY);
	Val hi;
	if (isinf(above)) {
		hi = exact_binary(interp, center, below_exact, EXACT_OP_SUB);
		if (!interp->error_flag)
			hi = exact_scale_by_ratio(interp, hi, 1, 2);
		if (!interp->error_flag)
			hi = exact_binary(interp, center, hi, EXACT_OP_ADD);
	} else {
		hi = exact_from_double(interp, above);
		if (!interp->error_flag)
			hi = exact_binary(interp, center, hi, EXACT_OP_ADD);
		if (!interp->error_flag)
			hi = exact_scale_by_ratio(interp, hi, 1, 2);
	}
	if (interp->error_flag)
		return rationalize_cleanup(interp, base_dsp, 3);

	gc_root_pop(interp);
	gc_root_pop(interp);
	gc_root_pop(interp);
	gc_root_push(interp, lo);
	gc_root_push(interp, hi);

	int n_parked = 0;
	Val candidate = make_tagged(T_NONE, 0);
	for (int step = 0; ; step++) {
		if (step == 200) {
			fail(interp, "rationalize did not converge");
			return rationalize_cleanup(interp, base_dsp, 2);
		}

		Val lo_floor = exact_unary(interp, lo, EXACT_OP_ROUND_DOWN);
		if (interp->error_flag)
			return rationalize_cleanup(interp, base_dsp, 2);
		int lo_is_integer = exact_cmp(interp, lo, lo_floor) == 0;

		if (lo_is_integer && closed) {
			candidate = lo_floor;
		} else {
			gc_root_push(interp, lo_floor);
			candidate = exact_unary(interp, lo_floor, EXACT_OP_INCREMENT);
			gc_root_pop(interp);
			if (interp->error_flag)
				return rationalize_cleanup(interp, base_dsp, 2);
		}

		int order = exact_cmp(interp, candidate, hi);
		if (order < 0 || (order == 0 && closed))
			break;

		if (lo_is_integer) {
			fail(interp, "rationalize did not converge");
			return rationalize_cleanup(interp, base_dsp, 2);
		}

		push(interp, lo_floor);
		n_parked++;

		Val hi_minus = exact_binary(interp, hi, lo_floor, EXACT_OP_SUB);
		if (interp->error_flag)
			return rationalize_cleanup(interp, base_dsp, 2);
		gc_root_push(interp, hi_minus);

		Val lo_minus = exact_binary(interp, lo, lo_floor, EXACT_OP_SUB);
		gc_root_pop(interp);
		if (interp->error_flag)
			return rationalize_cleanup(interp, base_dsp, 2);
		gc_root_push(interp, lo_minus);

		Val new_lo = exact_reciprocal(interp, hi_minus);
		if (interp->error_flag)
			return rationalize_cleanup(interp, base_dsp, 3);
		gc_root_push(interp, new_lo);

		Val new_hi = exact_reciprocal(interp, lo_minus);
		if (interp->error_flag)
			return rationalize_cleanup(interp, base_dsp, 4);

		gc_root_pop(interp);
		gc_root_pop(interp);
		gc_root_pop(interp);
		gc_root_pop(interp);
		lo = new_lo;
		hi = new_hi;
		gc_root_push(interp, lo);
		gc_root_push(interp, hi);
	}

	gc_root_pop(interp);
	gc_root_pop(interp);

	Val result = candidate;
	while (n_parked-- > 0) {
		Val coefficient = interp->data_stack[interp->dsp - 1];
		Val inverse = exact_reciprocal(interp, result);
		if (interp->error_flag)
			return rationalize_cleanup(interp, base_dsp, 0);

		result = exact_binary(interp, coefficient, inverse, EXACT_OP_ADD);
		if (interp->error_flag)
			return rationalize_cleanup(interp, base_dsp, 0);
		interp->dsp--;
	}

	if (value < 0.0) {
		result = exact_unary(interp, result, EXACT_OP_NEGATE);
		if (interp->error_flag)
			return make_tagged(T_NONE, 0);
	}
	return result;
}

void p_rationalize(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val value = chain_sp[-1];
	if (VAL_TAG(value) == T_EXACT)
		DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
	REQUIRE_CHAIN_TAG(value, T_FLOAT, "rationalize", "a float");

	SYNC_REGISTERS(interp, chain_ip, chain_sp);
	Val result = exact_rationalize(interp, VAL_NUMBER(value));
	if (interp->error_flag)
		return;
	interp->data_stack[interp->dsp - 1] = result;

	DISPATCH(interp);
}

void p_exact_to_float(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val value = chain_sp[-1];
	if (VAL_TAG(value) == T_FLOAT)
		DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
	REQUIRE_CHAIN_TAG(value, T_EXACT, "exact>float", "an exact");

	chain_sp[-1] = make_float(exact_to_double(value));

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_numerator(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val value = chain_sp[-1];
	REQUIRE_CHAIN_TAG(value, T_EXACT, "numerator", "an exact");

	SYNC_REGISTERS(interp, chain_ip, chain_sp);
	Object *obj = OBJECT_AT(VAL_DATA(value));
	int handle = object_new_exact(interp, obj->exact.sign, obj->exact.limbs, obj->exact.n_numerator,
			&mag_one_limb, 1);
	if (interp->error_flag)
		return;
	interp->data_stack[interp->dsp - 1] = make_exact(handle);

	DISPATCH(interp);
}

void p_denominator(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val value = chain_sp[-1];
	REQUIRE_CHAIN_TAG(value, T_EXACT, "denominator", "an exact");

	SYNC_REGISTERS(interp, chain_ip, chain_sp);
	Object *obj = OBJECT_AT(VAL_DATA(value));
	int handle = object_new_exact(interp, 1, obj->exact.limbs + obj->exact.n_numerator,
			obj->exact.n_denominator, &mag_one_limb, 1);
	if (interp->error_flag)
		return;
	interp->data_stack[interp->dsp - 1] = make_exact(handle);

	DISPATCH(interp);
}
