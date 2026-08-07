# Water reference

Every entry is derived from reading the C source. Stack effects are exact;
`--` separates the state before (bottom to top, leftmost = deepest) from after.
Shorthand: `f` float, `str` string, `xt` execution token, `mat` matrix, `arr`
array, `set` set, `fr` frame, `sym` symbol, `k` continuation. `mat` and `str`
are spelled out because `m` is the metre unit and `s` the second, and `to`
refuses to shadow a word; `set` is itself the set constructor, so a variable
holding one takes a name of its own.

Three cost columns appear on runtime words:

- **Ops** — an approximate count of primitive operations (stack pushes/pops
  plus the dominant inner work). An integer for constant-time words; a leading
  term such as `n` or `r×c` otherwise. It is a rough constant-factor guide, not
  an instruction count.
- **Alloc** — heap activity. `1o` = one object slot + its payload allocation;
  `1s` = one string; `1a(n)` = one n-element array; `1m(r×c)` = one r×c matrix.
- **O** — asymptotic time.

Compile-time words (control flow, defining words, superwords) carry no cost
columns: their work happens while a definition is being compiled, not at run
time.

**Unsafe** words are marked ⚠. They read the raw `.number` field of a stack
slot with no tag check; a non-float operand yields a garbage float silently.
All `f`-prefixed words and all superwords are unsafe.

A word's examples follow its section's table as fence pairs: a
` ```forth <word> ` fence holding self-contained code — deterministic (random
draws seeded), newline-terminated, leaving both stacks empty — and a
` ```output ` fence holding exactly what it prints (trailing blanks
insignificant). `make test` extracts and verifies every pair, and
`help <word>` prints them. A ` ```forth-noexec <word> ` fence is shown by
`help` but never run.

Tokens are whitespace-delimited, with self-delimiting punctuation: `;`, `]`,
and `}` always end a token and `[` and `{` always start one (the two-char
openers `[:` `[(` `[>` `[<` and closers `:]` `)]` `>]` stay whole), so
`[1 2 3]`, `{:a 1}`, and `dup *;` parse without inner spaces. A path literal's
predicate brackets (`/a[x>3]`) are kept whole by bracket balance. `<` `>` `<=`
`>=` are ordinary comparison words; set literals `[< … >]` still need spaces
around their contents.

Allocation note: an object slot is a pointer bump into the object table, which
grows on demand (doubling) up to a 64M-entry ceiling; when the ceiling is
reached, a mark-sweep GC runs and the allocation retries. There is no
incremental collection.

---

## Stack manipulation

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `dup` | `( a -- a a )` | Duplicate top | 3 | none | O(1) |
| `drop` | `( a -- )` | Discard top | 1 | none | O(1) |
| `swap` | `( a b -- b a )` | Exchange top two | 4 | none | O(1) |
| `over` | `( a b -- a b a )` | Copy second over top | 5 | none | O(1) |
| `rot` | `( a b c -- b c a )` | Rotate top three | 6 | none | O(1) |
| `-rot` | `( a b c -- c a b )` | core.h2o: reverse rotate — brings the top down under the other two (`rot rot`, inlined) | 12 | none | O(1) |
| `depth` | `( -- n )` | Push current depth | 1 | none | O(1) |
| `pick` | `( xₙ … x₀ n -- xₙ … x₀ xₙ )` | Copy the item n deep to the top, leaving it in place; `0 pick` is `dup` and `1 pick` is `over`, and n counts from the top as `roll`'s does. Reaches a value a caller parked below a combinator's operands — under `map`, which peeks its source, the element is at 0, the source at 1 and a parked value at 2 | 3 | none | O(1) |
| `roll` | `( xₙ … x₀ n -- xₙ₋₁ … x₀ xₙ )` | Move the item n deep to the top; memmoves the n above it down | 2 + n | none | O(n) |
| `clear` | `( … -- )` | Reset data stack depth to 0 | 1 | none | O(1) |
| `2dup` | `( a b -- a b a b )` | core.h2o: `over over` (inlined) | 10 | none | O(1) |
| `2drop` | `( a b -- )` | core.h2o: `drop drop` (inlined) | 2 | none | O(1) |
| `identity` | `( a -- a )` | core.h2o: the value unchanged (inlined) — the no-op xt for a higher-order word that wants "leave it as is" | 1 | none | O(1) |
| `nip` | `( a b -- b )` | Drop the second item, keeping the top — one op, not `swap drop` | 1 | none | O(1) |

```forth dup
3 dup * . cr
```
```output
9
```

```forth drop
1 2 drop . cr
```
```output
1
```

```forth swap
1 2 swap . . cr
```
```output
1 2
```

```forth over
1 2 over . . . cr
```
```output
1 2 1
```

```forth rot
1 2 3 rot . . . cr
```
```output
1 3 2
```

```forth -rot
1 2 3 -rot . . . cr
```
```output
2 1 3
```

```forth depth
1 2 3 depth . clear cr
```
```output
3
```

```forth pick
10 20 30 1 pick . clear cr
```
```output
20
```

```forth roll
10 20 30 2 roll . . . cr
```
```output
10 30 20
```

```forth clear
1 2 3 clear depth . cr
```
```output
0
```

```forth 2dup
3 4 2dup * . + . cr
```
```output
12 7
```

```forth 2drop
1 2 3 2drop . cr
```
```output
1
```

```forth identity
[ 1 2 ] ' identity map . cr
```
```output
[ 1 2 ]
```

```forth nip
1 2 nip . cr
```
```output
2
```

---

## Arithmetic

Polymorphic; dispatch on operand tags at run time. Ops/Alloc/O below give the
float fast path first; the heavy cases are captured by the O column.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `+` | `( a b -- a+b )` | float: add. string+string: concat → new string. set+set: union → new set. matrix+matrix: element-wise → new matrix. scalar+matrix / matrix+scalar: broadcast → new matrix. array+array: defers to `concat`. | 3 (float) | float none; string `1s` + temp buffer; set `1o`; matrix `1m(r×c)`; array `1a(m+n)` | float O(1); string O(\|s\|); set O(n log n); matrix O(r×c); array O(m+n) |
| `-` | `( a b -- a-b )` | float: subtract. set−set: difference. matrix: element-wise. scalar/matrix broadcast. | 3 (float) | as `+` | as `+` |
| `*` | `( a b -- a*b )` | float: multiply. set∩set: intersection. matrix: element-wise. scalar/matrix broadcast. | 3 (float) | as `+` | as `+` |
| `/` | `( a b -- a/b )` | float: divide (errors on zero divisor). matrix÷matrix: element-wise (errors on any zero element). scalar/matrix broadcast. | 3 (float) | matrix `1m(r×c)` | float O(1); matrix O(r×c) |
| `%` | `( a b -- remainder quotient )` | truncating division: pushes `a − trunc(a/b)·b` then `trunc(a/b)`; float or matrix element-wise with scalar broadcast, answering a remainder matrix under a quotient matrix; a zero divisor or zero element errors | 4 | none | O(1) |
| `mod` | `( a b -- remainder )` | remainder with the sign of the dividend (`fmod`); float or matrix element-wise with scalar broadcast; a zero divisor or zero element errors | 3 (float) | matrix `1m(r×c)` | float O(1); matrix O(r×c) |
| `^` | `( a b -- a^b )` | `pow`; float or matrix (element-wise) / scalar broadcast | 3 (float) | matrix `1m(r×c)` | float O(1); matrix O(r×c) |
| `negate` | `( a -- -a )` | float or matrix (element-wise) | 2 (float) | matrix `1m(r×c)` | float O(1); matrix O(r×c) |
| `1+` | `( a -- a+1 )` | float or matrix | 2 (float) | matrix `1m(r×c)` | float O(1); matrix O(r×c) |
| `1-` | `( a -- a-1 )` | float or matrix | 2 (float) | matrix `1m(r×c)` | float O(1); matrix O(r×c) |
| `sq` | `( a -- a² )` | float or matrix | 2 (float) | matrix `1m(r×c)` | float O(1); matrix O(r×c) |
| `min2` | `( a b -- smaller )` | the `val_cmp`-ordered lesser of two values — floats, strings, quantities; NaN orders below every number, so a NaN operand answers NaN. With a matrix operand it is element-wise with scalar broadcast. `min`/`max` reduce one matrix, these order a pair | 3 (float) | matrix `1m(r×c)` | float O(1); matrix O(r×c) |
| `max2` | `( a b -- larger )` | the `val_cmp`-ordered greater, `min2`'s twin; a NaN operand answers the other value | 3 (float) | matrix `1m(r×c)` | float O(1); matrix O(r×c) |

```forth +
3 4 + . cr
"con" "cat" + . cr
[< 1 2 >] [< 2 3 >] + . cr
```
```output
7
concat
[< 1 2 3 >]
```

```forth -
10 3 - . cr
[< 1 2 3 >] [< 2 >] - . cr
```
```output
7
[< 1 3 >]
```

```forth *
6 7 * . cr
[< 1 2 3 >] [< 2 3 4 >] * . cr
```
```output
42
[< 2 3 >]
```

```forth /
10 4 / . cr
```
```output
2.5
```

```forth %
7 3 % . . cr
```
```output
2 1
```

```forth mod
7 3 mod . -7 3 mod . cr
```
```output
1 -1
```

```forth ^
2 10 ^ . cr
```
```output
1024
```

```forth negate
5 negate . cr
```
```output
-5
```

```forth 1+
41 1+ . cr
```
```output
42
```

```forth 1-
43 1- . cr
```
```output
42
```

```forth sq
9 sq . cr
```
```output
81
```

```forth min2
3 7 min2 . cr
```
```output
3
```

```forth max2
3 7 max2 . cr
```
```output
7
```

### In-place matrix arithmetic

Mutate the left operand and return it; no allocation. Programmer is responsible for uniqueness (no implicit refcounting).

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `+!` | `( mat a -- mat )` | matrix+matrix or matrix+scalar (and scalar+matrix, mutating the matrix) in place | 3 + r×c | none | O(r×c) |
| `-!` | `( mat a -- mat )` | in-place subtract | 3 + r×c | none | O(r×c) |
| `*!` | `( mat a -- mat )` | in-place multiply | 3 + r×c | none | O(r×c) |
| `/!` | `( mat a -- mat )` | in-place divide | 3 + r×c | none | O(r×c) |

```forth +!
[ 1 2 ] vector 10 +! matrix>array . cr
```
```output
[ 11 12 ]
```

```forth -!
[ 10 20 ] vector 1 -! matrix>array . cr
```
```output
[ 9 19 ]
```

```forth *!
[ 1 2 ] vector 3 *! matrix>array . cr
```
```output
[ 3 6 ]
```

```forth /!
[ 10 20 ] vector 4 /! matrix>array . cr
```
```output
[ 2.5 5 ]
```

### Float-only arithmetic ⚠

Operate directly on stack slots' `.number`, in place, with only a depth check — no tag check.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `f+` | `( a b -- a+b )` ⚠ | add, result in deeper slot | 2 | none | O(1) |
| `f-` | `( a b -- a-b )` ⚠ | subtract | 2 | none | O(1) |
| `f*` | `( a b -- a*b )` ⚠ | multiply | 2 | none | O(1) |
| `f/` | `( a b -- a/b )` ⚠ | divide; checks divisor ≠ 0 | 2 | none | O(1) |
| `f=` | `( a b -- f )` ⚠ | float `=`, result `1.0`/`0.0`; no type check | 2 | none | O(1) |
| `f<` | `( a b -- f )` ⚠ | float `<`, result `1.0`/`0.0`; no type check | 2 | none | O(1) |
| `f>` | `( a b -- f )` ⚠ | float `>`, result `1.0`/`0.0`; no type check | 2 | none | O(1) |
| `f<=` | `( a b -- f )` ⚠ | float `<=` (≤), result `1.0`/`0.0`; no type check | 2 | none | O(1) |
| `f>=` | `( a b -- f )` ⚠ | float `>=` (≥), result `1.0`/`0.0`; no type check | 2 | none | O(1) |
| `f^` | `( a b -- a^b )` ⚠ | `pow` | 2 | none | O(1) |
| `fmod` | `( a b -- fmod(a,b) )` ⚠ | `fmod` | 2 | none | O(1) |
| `f*+` | `( a b c -- a*b+c )` ⚠ | fused multiply-add; result in slot of `a` | 3 | none | O(1) |
| `f*-` | `( a b c -- c-a*b )` ⚠ | fused multiply-subtract | 3 | none | O(1) |
| `f1+` | `( a -- a+1 )` ⚠ | in place | 1 | none | O(1) |
| `f1-` | `( a -- a-1 )` ⚠ | in place | 1 | none | O(1) |
| `fsq` | `( a -- a² )` ⚠ | in place | 1 | none | O(1) |
| `fnegate` | `( a -- -a )` ⚠ | in place | 1 | none | O(1) |
| `fabs` | `( a -- \|a\| )` ⚠ | in place | 1 | none | O(1) |
| `fsqrt` | `( a -- √a )` ⚠ | in place | 1 | none | O(1) |
| `fexp` | `( a -- eᵃ )` ⚠ | in place | 1 | none | O(1) |
| `flog` | `( a -- log₁₀ a )` ⚠ | base-10 log, in place | 1 | none | O(1) |
| `fln` | `( a -- ln a )` ⚠ | natural log, in place | 1 | none | O(1) |
| `fsin` | `( a -- sin a )` ⚠ | sine (radians), in place | 1 | none | O(1) |
| `fcos` | `( a -- cos a )` ⚠ | cosine (radians), in place | 1 | none | O(1) |
| `ftan` | `( a -- tan a )` ⚠ | tangent (radians), in place | 1 | none | O(1) |
| `ftanh` | `( a -- tanh a )` ⚠ | hyperbolic tangent, in place | 1 | none | O(1) |
| `fasin` | `( a -- asin a )` ⚠ | inverse sine, in place | 1 | none | O(1) |
| `facos` | `( a -- acos a )` ⚠ | inverse cosine, in place | 1 | none | O(1) |
| `fatan` | `( a -- atan a )` ⚠ | inverse tangent, in place | 1 | none | O(1) |
| `fround` | `( a -- round a )` ⚠ | nearest, in place | 1 | none | O(1) |
| `ftruncate` | `( a -- trunc a )` ⚠ | toward zero, in place | 1 | none | O(1) |
| `fround-up` | `( a -- ceil a )` ⚠ | in place | 1 | none | O(1) |
| `fround-down` | `( a -- floor a )` ⚠ | in place | 1 | none | O(1) |

```forth f+
1.5 2.5 f+ . cr
```
```output
4
```

```forth f-
5 1.5 f- . cr
```
```output
3.5
```

```forth f*
2.5 4 f* . cr
```
```output
10
```

```forth f/
7 2 f/ . cr
```
```output
3.5
```

```forth f=
2 2 f= . cr
```
```output
1
```

```forth f<
1 2 f< . cr
```
```output
1
```

```forth f>
1 2 f> . cr
```
```output
0
```

```forth f<=
2 2 f<= . cr
```
```output
1
```

```forth f>=
1 2 f>= . cr
```
```output
0
```

```forth f^
2 0.5 f^ . cr
```
```output
1.41421
```

```forth fmod
7 3 fmod . cr
```
```output
1
```

```forth f*+
2 3 10 f*+ . cr
```
```output
16
```

```forth f*-
2 3 10 f*- . cr
```
```output
4
```

```forth f1+
41 f1+ . cr
```
```output
42
```

```forth f1-
43 f1- . cr
```
```output
42
```

```forth fsq
9 fsq . cr
```
```output
81
```

```forth fnegate
5 fnegate . cr
```
```output
-5
```

```forth fabs
-3.5 fabs . cr
```
```output
3.5
```

```forth fsqrt
2 fsqrt . cr
```
```output
1.41421
```

```forth fexp
1 fexp . cr
```
```output
2.71828
```

```forth flog
1000 flog . cr
```
```output
3
```

```forth fln
E fln . cr
```
```output
1
```

```forth fsin
PI 2 f/ fsin . cr
```
```output
1
```

```forth fcos
0 fcos . cr
```
```output
1
```

```forth ftan
PI 4 f/ ftan . cr
```
```output
1
```

```forth ftanh
0 ftanh . cr
```
```output
0
```

```forth fasin
1 fasin . cr
```
```output
1.5708
```

```forth facos
1 facos . cr
```
```output
0
```

```forth fatan
1 fatan . cr
```
```output
0.785398
```

```forth fround
2.5 fround . cr
```
```output
3
```

```forth ftruncate
2.9 ftruncate . cr
```
```output
2
```

```forth fround-up
2.1 fround-up . cr
```
```output
3
```

```forth fround-down
2.9 fround-down . cr
```
```output
2
```

---

## Constants

constants.h2o, capitalized by convention. Mathematical values are computed at load;
physical values are the exact SI-2019 definitions (G is CODATA 2018, the one
measured value). The dimensioned constants are quantities, so unit algebra
applies: `KB 300 kelvin *` is an energy, `C 2 ^ 1 kg *` is E=mc².

| Word | Stack effect | Behavior |
|------|-------------|----------|
| `PI` | `( -- f )` | π |
| `E` | `( -- f )` | Euler's number e |
| `TAU` | `( -- f )` | 2π |
| `PHI` | `( -- f )` | The golden ratio (1+√5)/2 |
| `C` | `( -- q )` | Speed of light, 299792458 m/s (exact) |
| `G` | `( -- q )` | Gravitational constant, 6.67430×10⁻¹¹ m³·kg⁻¹·s⁻² |
| `H` | `( -- q )` | Planck constant, 6.62607015×10⁻³⁴ J·s (exact) |
| `HBAR` | `( -- q )` | Reduced Planck constant h/2π |
| `KB` | `( -- q )` | Boltzmann constant, 1.380649×10⁻²³ J/K (exact) |
| `NA` | `( -- q )` | Avogadro constant, 6.02214076×10²³ mol⁻¹ (exact) |
| `QE` | `( -- q )` | Elementary charge, 1.602176634×10⁻¹⁹ C (exact) |

```forth PI
PI . cr
```
```output
3.14159
```

```forth E
E . cr
```
```output
2.71828
```

```forth TAU
TAU . cr
```
```output
6.28319
```

```forth PHI
PHI . cr
```
```output
1.61803
```

```forth C
C . cr
```
```output
299792458 m.s^-1
```

```forth G
G . cr
```
```output
6.6743e-11 m^3.s^-2.kg^-1
```

```forth H
H . cr
```
```output
6.62607e-34 m^2.kg.s^-1
```

```forth HBAR
HBAR . cr
```
```output
1.05457e-34 m^2.kg.s^-1
```

```forth KB
KB . cr
```
```output
1.38065e-23 m^2.kg.s^-2.kelvin^-1
```

```forth NA
NA . cr
```
```output
6.02214e+23 mol^-1
```

```forth QE
QE . cr
```
```output
1.60218e-19 coulomb
```

## Unary math (polymorphic: float or matrix)

Tag-checked; safe. Float input → float; matrix input → new matrix, element-wise.
A float result that would be NaN (`-1 sqrt`, `-1 ln`) is `null` — NaN-boxing
reserves NaN bit patterns for tags, so `null` is Water's NaN, and it is falsy,
`none?`, and `= null`. Matrix buffers hold raw NaN elements untouched
(element-wise math writes them, `sort` places them last); a NaN read out of a
matrix (`@i,j`, `@e`) surfaces as `null` the same way.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `abs` | `( a -- \|a\| )` | `fabs` | 2 (float) | matrix `1m(r×c)` | float O(1); matrix O(r×c) |
| `sqrt` | `( a -- √a )` | `sqrt` | 2 | matrix `1m(r×c)` | same |
| `exp` | `( a -- eᵃ )` | `exp` | 2 | matrix `1m(r×c)` | same |
| `log` | `( a -- log₁₀ a )` | `log10` | 2 | matrix `1m(r×c)` | same |
| `ln` | `( a -- ln a )` | `log` — natural log | 2 | matrix `1m(r×c)` | same |
| `lgamma` | `( a -- ln Γ(a) )` | `lgamma` — log of the gamma function, which extends the factorial (`5 lgamma` is `ln 4!`). The log form is what count-model likelihoods use, since Γ overflows a double past 171 while `171 lgamma` is 706.57. Defined for positive arguments: it is `+inf` at zero and the negative integers, and for negative non-integers libm returns ln \|Γ(a)\| with the sign held in a global this word does not expose | 2 | matrix `1m(r×c)` | same |
| `sin` | `( a -- sin a )` | sine (radians) | 2 | matrix `1m(r×c)` | same |
| `cos` | `( a -- cos a )` | cosine (radians) | 2 | matrix `1m(r×c)` | same |
| `tan` | `( a -- tan a )` | tangent (radians) | 2 | matrix `1m(r×c)` | same |
| `tanh` | `( a -- tanh a )` | hyperbolic tangent | 2 | matrix `1m(r×c)` | same |
| `asin` | `( a -- asin a )` | inverse sine | 2 | matrix `1m(r×c)` | same |
| `acos` | `( a -- acos a )` | inverse cosine | 2 | matrix `1m(r×c)` | same |
| `atan` | `( a -- atan a )` | inverse tangent | 2 | matrix `1m(r×c)` | same |
| `round` | `( a -- round a )` | `round` | 2 | matrix `1m(r×c)` | same |
| `truncate` | `( a -- trunc a )` | `trunc` | 2 | matrix `1m(r×c)` | same |
| `round-up` | `( a -- ceil a )` | `ceil` | 2 | matrix `1m(r×c)` | same |
| `round-down` | `( a -- floor a )` | `floor` | 2 | matrix `1m(r×c)` | same |
| `quotient` | `( a b -- quotient )` | core.h2o: `% swap drop`; toward zero | 9 | none | O(1) |

```forth abs
-7 abs . cr
[ -1 2 ] vector abs matrix>array . cr
```
```output
7
[ 1 2 ]
```

```forth sqrt
16 sqrt . cr
-1 sqrt . cr
```
```output
4
null
```

```forth exp
0 exp . cr
```
```output
1
```

```forth log
100 log . cr
```
```output
2
```

```forth ln
E ln . cr
```
```output
1
```

```forth lgamma
5 lgamma . cr
```
```output
3.17805
```

```forth sin
0 sin . cr
```
```output
0
```

```forth cos
PI cos . cr
```
```output
-1
```

```forth tan
0 tan . cr
```
```output
0
```

```forth tanh
100 tanh . cr
```
```output
1
```

```forth asin
0.5 asin . cr
```
```output
0.523599
```

```forth acos
0 acos . cr
```
```output
1.5708
```

```forth atan
0 atan . cr
```
```output
0
```

```forth round
2.5 round . -2.5 round . cr
```
```output
3 -3
```

```forth truncate
-2.9 truncate . cr
```
```output
-2
```

```forth round-up
2.1 round-up . cr
```
```output
3
```

```forth round-down
-2.1 round-down . cr
```
```output
-3
```

```forth quotient
7 3 quotient . -7 3 quotient . cr
```
```output
2 -2
```

---

## Comparison and logic

Result is `1.0` (true) or `0.0` (false), with a float fast path. `=` uses `val_cmp` (structural): matrices compare by shape then row-major contents, so they order for set membership. `<`/`>` are structural too, **except on matrices**, where they compare element-wise and return a 1.0/0.0 matrix (same shape, or a scalar broadcasts over the matrix). A dimensioned matrix on either side of `<`/`>`/`eq` also masks element-wise: the right operand rescales into the left's unit (`prices 10 $ <` works whether prices are in `$` or `¢`), the mask comes back bare, and a quantity against a plain number or a different dimension errors. An array operand masks element-wise too: each element compares by `val_cmp` against the other operand (or pairwise against an equal-length array — unequal lengths error), yielding an n×1 mask, so `names "ann" eq where` filters a text column and string order is lexicographic. Directly before `if`/`while`/`until` a comparison fuses into a compare-and-branch, which stays structural — branching on a matrix result isn't meaningful.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `=` | `( a b -- bool )` | structural equality | 3 (float) | none | float O(1); string O(\|s\|); array/set O(n); frame O(n); matrix O(r×c) |
| `<` | `( a b -- bool )` or `( mat/arr x -- mat )` | less-than; element-wise 1/0 mask on matrix operands (scalar broadcast) and on array operands (`val_cmp` per element, n×1) | 3 (float) | matrix `1m(r×c)` | same; matrix O(r×c) |
| `<=` | `( a b -- bool )` or `( mat/arr x -- mat )` | less-than-or-equal (≤); element-wise 1/0 mask on matrix operands (scalar broadcast) and on array operands (`val_cmp` per element, n×1) | 3 (float) | matrix `1m(r×c)` | same; matrix O(r×c) |
| `true` | `( -- bool )` | core.h2o: pushes 1 (inline) | 1 | none | O(1) |
| `false` | `( -- bool )` | core.h2o: pushes 0 (inline) | 1 | none | O(1) |
| `>` | `( a b -- bool )` or `( mat/arr x -- mat )` | greater-than; element-wise 1/0 mask on matrix operands (scalar broadcast) and on array operands (`val_cmp` per element, n×1) | 3 (float) | matrix `1m(r×c)` | same; matrix O(r×c) |
| `>=` | `( a b -- bool )` or `( mat/arr x -- mat )` | greater-than-or-equal (≥); element-wise 1/0 mask on matrix operands (scalar broadcast) and on array operands (`val_cmp` per element, n×1) | 3 (float) | matrix `1m(r×c)` | same; matrix O(r×c) |
| `eq` | `( a b -- bool )` or `( mat/arr x -- mat )` | equality; element-wise 1/0 mask on matrix and array operands (scalar broadcast; `val_cmp` per array element) — the mask-producing twin of `=`, which stays structural on collections. NaN elements equal nothing | 3 (float) | matrix `1m(r×c)` | same; matrix O(r×c) |
| `nan?` | `( v -- bool )` or `( mat/arr -- mat )` | NaN test: 1/0 mask over a matrix's elements; an array answers an n×1 mask marking `none` elements (a text column's missing cells), composing with `where`/`select-rows`; `1` on `null` itself (a scalar NaN *is* `null`), `0` on any float. The only mask route to NaNs — they compare false under `<`/`>`/`eq` | 2 | `1m(r×c)` | O(1); matrix/array O(n) |
| `0=` | `( a -- bool )` | `!truthy(a)`; any type | 2 | none | O(1) |
| `1=` | `( a -- bool )` | core.h2o: `1 =` (inlined) | 5 | none | O(1) |
| `type-of` | `( a -- sym )` | The value's type as a symbol: `:float` `:string` `:symbol` `:array` `:set` `:pair` `:frame` `:matrix` `:quantity` `:xt` `:continuation` `:stream` `:db` `:ptr` `:segment` `:none` `:wildcard` `:lvar`. A bound logic var reports its value's type; an unbound one is `:lvar` | 2 | none | O(1) |
| `float?` | `( a -- bool )` | core.h2o: `type-of :float =` (inlined) | 5 | none | O(1) |
| `string?` | `( a -- bool )` | core.h2o: `type-of :string =` (inlined) | 5 | none | O(1) |
| `symbol?` | `( a -- bool )` | core.h2o: `type-of :symbol =` (inlined) | 5 | none | O(1) |
| `array?` | `( a -- bool )` | core.h2o: `type-of :array =` (inlined) | 5 | none | O(1) |
| `set?` | `( a -- bool )` | core.h2o: `type-of :set =` (inlined) | 5 | none | O(1) |
| `pair?` | `( a -- bool )` | core.h2o: `type-of :pair =` (inlined) | 5 | none | O(1) |
| `frame?` | `( a -- bool )` | core.h2o: `type-of :frame =` (inlined) | 5 | none | O(1) |
| `matrix?` | `( a -- bool )` | core.h2o: `type-of :matrix =` (inlined) | 5 | none | O(1) |
| `quantity?` | `( a -- bool )` | core.h2o: `type-of :quantity =` (inlined) | 5 | none | O(1) |
| `xt?` | `( a -- bool )` | core.h2o: `type-of :xt =` (inlined) | 5 | none | O(1) |
| `continuation?` | `( a -- bool )` | core.h2o: `type-of :continuation =` (inlined) | 5 | none | O(1) |
| `stream?` | `( a -- bool )` | core.h2o: `type-of :stream =` (inlined) | 5 | none | O(1) |
| `db?` | `( a -- bool )` | core.h2o: `type-of :db =` (inlined) | 5 | none | O(1) |
| `ptr?` | `( a -- bool )` | core.h2o: `type-of :ptr =` (inlined) | 5 | none | O(1) |
| `segment?` | `( a -- bool )` | core.h2o: `type-of :segment =` (inlined) | 5 | none | O(1) |
| `none?` | `( a -- bool )` | True when the value is the none value (`null`) — a single `T_NONE` tag test; a bound logic var reports as its value | 2 | none | O(1) |
| `wildcard?` | `( a -- bool )` | core.h2o: `type-of :wildcard =` (inlined) | 5 | none | O(1) |
| `lvar?` | `( a -- bool )` | core.h2o: `type-of :lvar =` (inlined) | 5 | none | O(1) |
| `and` | `( a b -- bool )` | logical and of truthiness | 3 | none | O(1) |
| `or` | `( a b -- bool )` | logical or of truthiness | 3 | none | O(1) |
| `not` | `( a -- bool )` | logical not of truthiness | 2 | none | O(1) |

`truthy` of a float is `≠ 0.0`; of any heap value, its handle `≠ 0`.

```forth =
1 1 = . "ab" "ab" = . [ 1 2 ] [ 1 2 ] = . cr
```
```output
1 1 1
```

```forth <
1 2 < . cr
[ 1 5 3 ] vector 2 < matrix>array . cr
```
```output
1
[ 1 0 0 ]
```

```forth <=
2 2 <= . cr
```
```output
1
```

```forth true
true . false . cr
```
```output
1 0
```

```forth false
false . true . cr
```
```output
0 1
```

```forth >
3 2 > . cr
```
```output
1
```

```forth >=
2 3 >= . cr
```
```output
0
```

```forth eq
"ab" "ab" eq . cr
[ 1 2 1 ] vector 1 eq matrix>array . cr
```
```output
1
[ 1 0 1 ]
```

```forth nan?
null nan? . cr
[ 1 null 3 ] vector nan? matrix>array . cr
```
```output
1
[ 0 1 0 ]
```

```forth 0=
0 0= . 5 0= . cr
```
```output
1 0
```

```forth 1=
1 1= . cr
```
```output
1
```

```forth type-of
3.5 type-of . "s" type-of . [ ] type-of . cr
```
```output
:float :string :array
```

```forth float?
3 float? . "x" float? . cr
```
```output
1 0
```

```forth string?
"x" string? . cr
```
```output
1
```

```forth symbol?
:a symbol? . cr
```
```output
1
```

```forth array?
[ ] array? . cr
```
```output
1
```

```forth set?
[< 1 >] set? . cr
```
```output
1
```

```forth pair?
1 2 cons pair? . cr
```
```output
1
```

```forth frame?
{ } frame? . cr
```
```output
1
```

```forth matrix?
[ 1 ] vector matrix? . cr
```
```output
1
```

```forth quantity?
3 m quantity? . cr
```
```output
1
```

```forth xt?
' dup xt? . cr
```
```output
1
```

```forth continuation?
[: 5 yield :] start-generator continuation? . drop cr
```
```output
1
```

```forth stream?
stdout stream? . cr
```
```output
1
```

```forth db?
":memory:" db-open dup db? . db-close cr
```
```output
1
```

```forth ptr?
3 ptr? . cr
```
```output
0
```

```forth segment?
4 int-segment segment? . cr
```
```output
1
```

```forth none?
null none? . 0 none? . cr
```
```output
1 0
```

```forth wildcard?
_ wildcard? . cr
```
```output
1
```

```forth lvar?
lvar lvar? . cr
```
```output
1
```

```forth and
1 0 and . 1 2 and . cr
```
```output
0 1
```

```forth or
0 0 or . 0 3 or . cr
```
```output
0 1
```

```forth not
0 not . 5 not . cr
```
```output
1 0
```

### Bitwise

Each operand is read as a two's-complement integer (exact within the double's
53-bit integer range), the operation is applied, and the result is pushed as a
float. `rshift` is arithmetic (sign-preserving).

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `bit-and` | `( a b -- f )` | bitwise AND | 2 | none | O(1) |
| `bit-or` | `( a b -- f )` | bitwise OR | 2 | none | O(1) |
| `bit-xor` | `( a b -- f )` | bitwise XOR | 2 | none | O(1) |
| `bit-not` | `( a -- f )` | two's-complement complement | 1 | none | O(1) |
| `lshift` | `( a n -- f )` | left shift `a` by `n` bits | 2 | none | O(1) |
| `rshift` | `( a n -- f )` | arithmetic right shift, = `floor(a / 2ⁿ)` | 2 | none | O(1) |
| `lowest-bit` | `( a -- i )` | 0-indexed position of the lowest set bit (`-1` if `a` is 0) | 1 | none | O(1) |

```forth bit-and
12 10 bit-and . cr
```
```output
8
```

```forth bit-or
12 10 bit-or . cr
```
```output
14
```

```forth bit-xor
12 10 bit-xor . cr
```
```output
6
```

```forth bit-not
0 bit-not . cr
```
```output
-1
```

```forth lshift
1 10 lshift . cr
```
```output
1024
```

```forth rshift
-16 2 rshift . cr
```
```output
-4
```

```forth lowest-bit
12 lowest-bit . cr
```
```output
2
```

---

## Dimensioned quantities

A quantity is a magnitude (a float or a matrix) carrying a unit. Units are
rational-exponent vectors over user-declared base dimensions, each with a
rational scale relative to its dimension's base; arithmetic propagates and
checks them. Same-dimension units at any rational scale coexist and convert
(`$`/`¢`, `kg`/`g`, `inch`/`cm`). What's excluded is affine offsets — `°C`/`°F`
need an added zero, not just a scale factor.

Declare with `base` and `unit`:

    base unit m   base unit s   base unit kg      \ base dimensions
    1 kg 1 m * 1 s / 1 s / unit newton            \ derived, named
    base unit $   1 $ 100 / unit ¢                \ scaled sub-unit (1/100)

A unit word is postfix — it attaches its unit to the number before it (`10 m`,
`3 newton`). Attaching a unit to a matrix makes a dimensioned matrix (`M m`).

- `*` / `/` — multiply/divide magnitudes; combine unit exponents and scales. A
  dimensionless result collapses back to a bare float or matrix, folding the
  scale ratio into the magnitude (`3 m 3 m / → 1`, `1 hour 1 s / → 3600`).
- `+` / `-` — require the same dimension; a different-scale operand is rescaled
  into the left operand's unit (`1 $ 50 ¢ + → 1.5 $`). Different
  dimension, or a quantity ± a bare number, errors.
- `^` — rational exponent only; raises the unit's exponents (`q 2 ^`, `q 0.5 ^`).
- `sqrt` — halves the unit's exponents (`sqrt(m²) → m`).
- `negate` / `abs` — keep the unit; transcendentals (`sin`, `log`, …) reject a quantity.
- `=` `<` `>` — compare by value, normalizing scale within a dimension (`100 ¢ 1 $ = → 1`). On a dimensioned matrix, `<`/`>`/`eq` answer an element-wise bare mask with the same normalization (`prices 10 $ <`); `=` stays structural everywhere.

Printing shows magnitude then unit: a named unit prints its name (`3 newton`); an
unnamed compound prints its dimensional form with the scale folded into the
magnitude (`10 m 2 s / → 5 m.s^-1`), positive exponents first. Quantities,
units, and unit words round-trip through `save-image`/`load-image`.

The matrix statistics accept a dimensioned matrix and keep its unit: `sum`
`mean` `max` `min` `quantile` (so `median` `percentile` `iqr` `ci`) `row-sums`
`column-sums` `cumulative-sum` `norm` `reshape` `transpose` `select-rows`
answer in the operand's unit, `matrix>array` yields per-element quantities,
`var` answers in the unit squared (`std`/`se` return to the unit through
`sqrt`), and the index and count words (`argsort` `argmax` `argmin` `size`
`dim`) and the correlations answer bare — correlation is scale-invariant, so
dimensioned inputs are computed over their magnitudes.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `magnitude` | `( v -- v' )` | A quantity's bare magnitude (float or matrix, the unit dropped); any other value passes through unchanged | 2 | none | O(1) |
| `unit-of` | `( v -- q\|1 )` | A quantity's unit as the quantity `1` in that unit (`10 km` → `1 km`, a matrix column in `m` → `1 m`, computed units in dimensional form — `1 m.s^-1`); a bare value answers `1.0`. Composes: `x unit-of *` attaches x's unit, `1 s =` tests for a unit | 2 | 1 pair | O(1) |

`units.h2o` predeclares a standard set (names spelled out and lowercase):
length `m` (`km`), time `s` (`minute`, `hour`, `day`, `week`), mass `kg`, current `ampere`,
temperature `kelvin`, amount `mol`; derived `hertz` `newton` `pascal` `joule`
`watt` `coulomb` `volt`; and three currencies, each its own dimension —
`$`/`¢`, `£`/`penny`, `€`/`eurocent`.

```forth magnitude
10 km magnitude . cr
```
```output
10
```

```forth unit-of
10 km unit-of . cr
```
```output
1 km
```

## Return stack

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `>r` | `( a -- )` → return stack | Move top to return stack | 2 | none | O(1) |
| `r>` | return stack → `( -- a )` | Move return-stack top to data stack | 2 | none | O(1) |
| `r@` | `( -- a )` | Copy return-stack top to data stack | 2 | none | O(1) |

```forth >r
1 2 >r . r> . cr
```
```output
1 2
```

```forth r>
5 >r r> . cr
```
```output
5
```

```forth r@
7 >r r@ . r> . cr
```
```output
7 7
```

---

## Side stack

A third stack (depth 1024) for stashing values out of the way; used by `try-catch` to hold the handler.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `>side` | `( a -- )` | Push to side stack | 2 | none | O(1) |
| `side>` | `( -- a )` | Pop from side stack | 2 | none | O(1) |
| `side-drop` | `( -- )` | Discard side-stack top | 1 | none | O(1) |
| `side-peek` | `( -- a )` | Copy side-stack top to the data stack | 1 | none | O(1) |
| `side-depth` | `( -- n )` | Push side-stack depth | 1 | none | O(1) |

```forth >side
10 20 >side . side> . cr
```
```output
10 20
```

```forth side>
7 >side side> . cr
```
```output
7
```

```forth side-drop
1 >side 2 >side side-drop side> . cr
```
```output
1
```

```forth side-peek
9 >side side-peek . side-drop cr
```
```output
9
```

```forth side-depth
1 >side 2 >side side-depth . side-drop side-drop cr
```
```output
2
```

---

## Control flow (compile-time)

Immediate words that emit branch instructions into the current definition. Outside a definition or quotation there is nothing to emit into, and the openers (`if`, `?if`, `begin`) error: `only valid inside a colon definition or quotation`. A top-level conditional goes in a quotation, run on the spot: `[: … if … then :] execute`.

| Word | Runtime effect | Behavior |
|------|---------------|----------|
| `if` | `( flag -- )` | Branch past the `then`/`else` if flag is falsy |
| `?if` | `( flag -- flag )` | Like `if`, but peeks the flag instead of consuming it — the flag stays on the stack in both branches |
| `else` | — | Separate the true and false arms |
| `then` | — | Close an `if`/`if…else`; patches the forward branch |
| `begin` | — | Mark a loop top |
| `until` | `( flag -- )` | Branch back to `begin` if flag is falsy |
| `again` | — | Unconditional branch back to `begin` |
| `while` | `( flag -- )` | Exit the loop forward if flag is falsy (`begin … while … repeat`) |
| `repeat` | — | Branch back to `begin`; patches the `while` exit |
| `leave` | — | Branch past the innermost loop's closing word; conditional form is `if leave then` |
| `continue` | — | Branch back to the innermost loop's `begin`: a `while` loop re-runs its test; an `until` loop skips its trailing test and repeats unconditionally |
| `exit` | `( -- )` | Return early from the current definition (this one runs at run time) |

`leave` and `continue` are plain compiled branches — zero runtime cost. Both
are compile errors outside a loop, and a quotation opens its own frame, so a
`[: leave :]` inside a loop body does not see that loop. A `begin` with no
`until`/`again`/`repeat` is a compile error at `;` or `:]` (an unpatched
`leave` would otherwise be a wild branch); the partial definition rolls back.
In `times` / `i-times` quotations, `exit` already ends the current iteration.

```forth if
: absolute dup 0 < if negate then ; -7 absolute . cr
```
```output
7
```

```forth ?if
: keep-if-truthy ?if "kept" . then . cr ; 5 keep-if-truthy 0 keep-if-truthy
```
```output
kept 5
0
```

```forth else
: parity 2 mod 0= if "even" else "odd" then . cr ; 7 parity
```
```output
odd
```

```forth then
: past-ten 10 > if "big" . then "done" . cr ; 42 past-ten
```
```output
big done
```

```forth begin
: countdown begin dup . 1- dup 0= until drop cr ; 3 countdown
```
```output
3 2 1
```

```forth until
: triple-count 1 begin dup . 3 + dup 9 > until drop cr ; triple-count
```
```output
1 4 7
```

```forth again
: to-five 1 begin dup . dup 5 = if leave then 1+ again drop cr ; to-five
```
```output
1 2 3 4 5
```

```forth while
: halves begin dup 0 > while dup . 2 quotient repeat drop cr ; 20 halves
```
```output
20 10 5 2 1
```

```forth repeat
: powers 1 begin dup 100 < while dup . 2 * repeat drop cr ; powers
```
```output
1 2 4 8 16 32 64
```

```forth leave
: stop-at-zero begin dup . 1- dup 0 < if leave then again drop cr ; 2 stop-at-zero
```
```output
2 1 0
```

```forth continue
: odds-to-nine 0 begin 1+ dup 9 > if leave then dup 2 mod 0= if continue then dup . again drop cr ; odds-to-nine
```
```output
1 3 5 7 9
```

```forth exit
: early dup 0 < if drop "neg" . cr exit then drop "pos" . cr ; -3 early
```
```output
neg
```

---

## Delimiters

Literal-building words. Each is an ordinary dictionary word; the openers and
closers are self-delimiting tokens (see the note in the introduction).

| Word | Stack effect | Behavior |
|------|-------------|----------|
| `[` | — | Open an array literal; `]` closes it |
| `]` | — | Close an array literal |
| `{` | — | Open a frame literal (alternating key/value); `}` closes it |
| `}` | — | Close a frame literal |
| `[<` | — | Open a set literal; `>]` closes it (both need surrounding spaces) |
| `>]` | — | Close a set literal |
| `[(` | — | Open a cons-list literal; `)]` closes it |
| `)]` | — | Close a cons-list literal |
| `:]` | — | Close a quotation (either `[:` / `[>` form; `[:` itself is under Defining) |
| `[>` | — | Open a quotation whose locals list receives every slot from the stack |
| `\|` | — | Declare word-locals at a definition's head: `\| x y \|` |
| `\|>` | — | Locals list in which every slot receives from the stack |

```forth [
[ 1 2 3 ] . cr
```
```output
[ 1 2 3 ]
```

```forth ]
[ 1 2 3 ] . cr
```
```output
[ 1 2 3 ]
```

```forth {
{ :a 1 :b 2 } frame>array . cr
```
```output
[ :a 1 :b 2 ]
```

```forth }
{ :x 9 } :x @ . cr
```
```output
9
```

```forth [<
[< 3 1 2 >] . cr
```
```output
[< 1 2 3 >]
```

```forth >]
[< 3 1 2 >] . cr
```
```output
[< 1 2 3 >]
```

```forth [(
[( 1 2 null )] . cr
```
```output
[( 1 2 null )]
```

```forth )]
[( 1 2 null )] . cr
```
```output
[( 1 2 null )]
```

```forth :]
5 [: 2 * :] execute . cr
```
```output
10
```

```forth [>
3 4 [> a b | a b - :] execute . cr
```
```output
-1
```

```forth |
: hyp | a b | to b to a a a * b b * + sqrt ; 3 4 hyp . cr
: discounted | >price rate | 0.2 to rate price price rate * - ; 100 discounted . cr
```
```output
5
80
```

```forth |>
: diff |> a b | a b - ; 10 3 diff . cr
```
```output
7
```

## Defining and compiling words

These parse following tokens and/or compile code. Costs are dominated by compilation, not by a stack effect, so no cost columns.

| Word | Stack effect | Behavior |
|------|-------------|----------|
| `:` | — | Begin a colon definition; read the following name; enter compile mode |
| `;` | — | End a colon definition; emit `exit`; store the source text for `see`. Self-delimiting: `dup *;` parses |
| `recurse` | — | Compile a call to the innermost definition being compiled — the enclosing quotation, else the enclosing colon word — so an anonymous quotation can self-call. An ordinary recursive call (grows the return stack); compile error outside a definition |
| `variable` | — | Read the following name; declare a global variable initialized to `0.0` |
| `constant` | `( val -- )` | Pop a value and read the following name; define an inline word that pushes it as a literal, so call sites fold to the literal with no run-time fetch. Fixed at definition — `to` cannot reassign it |
| `to` | `( val -- )` | Assign to the named local (in a definition) or global. At interpreted top level — the REPL, a program file, a `load`ed file — it auto-creates an absent global. In a compiled body, a colon definition or a quotation alike, the global must already exist: `to: unknown variable: <name>; declare it with variable`. The name must also be free: `to` refuses to shadow an existing word, `to: <name> is already a word, not a variable` — `to m` fails because `m` is the metre unit. May trigger superword store-fusion while compiling. |
| `symbol` | — | Read the following name; declare a word that pushes a specific interned symbol |
| `defer` | `( "name" -- )` | Read the following name; declare a forward-referenced word with no target. Calling it before a target is installed throws `unresolved deferred word`. Enables mutual recursion and late binding; set the target with `embodies` or `embodies!` |
| `embodies` | `( xt "name" -- )` | Pop an xt (a colon word or quotation) and read the following name; install it as the named deferred word's target. Retargetable — each later call re-reads it — so a call to the deferred word forwards through one dispatch. Top-level only |
| `embodies!` | `( xt "name" -- )` | Like `embodies`, but finalizing: rewrite every existing call site of the deferred word to call the target directly (no forwarding cost), then turn the word into an ordinary word. Not retargetable afterward; a further `embodies`/`embodies!` reports it is no longer deferred. Top-level only |
| `base` | `( -- q )` | Push a base quantity — a fresh dimension with its base unit, magnitude `1.0`. Paired with `unit` to declare a base dimension (`base unit m`) |
| `unit` | `( q -- )` | Read the following name; pop a quantity whose magnitude is a positive whole number, and define a postfix word attaching that unit. The magnitude is the unit's integer scale relative to its dimension's base (`100 cent unit dollar`). A single unnamed base dimension gets named after the word |
| `:name` | `( -- sym )` | Symbol literal; interns the name at read time |
| `string>symbol` | `( str -- sym )` | Intern a computed string as a symbol |
| `[:` | `( -- xt )` | Open an anonymous quotation (closed by `:]`); compiles its body and pushes its xt |
| `'` | `( "name" -- xt )` | Parse the following word at compile time and push its xt (immediate; folds the xt in as a literal) |
| `lookup` | `( "name" -- xt )` | Parse the following word at run time and push its xt — the non-immediate counterpart of `'` |
| `execute` | `( xt -- … )` | Call the word at xt |
| `curry` | `( value xt -- xt' )` | Bind a value into a curried token: a heap value carrying the target xt and the bound value, accepted wherever an xt is. At invocation the bound value is pushed, then the target runs — so currying binds a word's **trailing** parameters. Collected like any array, so a word may curry on every call; usable inside a parallel region | 3 | `1o` | O(n bound) |
| `2curry` | `( a b xt -- xt' )` | Bind two values into a curried token: at invocation it pushes `a`, then `b`, then calls xt. Same result as `curry curry` in one call | 4 | `1o` | O(n bound) |
| `ncurry` | `( v₁ … v_N xt N -- xt' )` | Bind N values into a curried token: at invocation it pushes `v₁` … `v_N` in that order, then calls xt. `N` of 0 answers a token with the target's own bindings. The arity form of `curry`/`2curry`; the count is a value, so a caller binds as many as it has. Currying a token flattens — the outer values push first | 3 + N | `1o` | O(n bound) |
| `inline` | — | Mark the most recent definition inline; future calls splice its body. A body containing a quotation is not spliced — such calls compile as plain calls, since a copied quotation header would have no recorded span |
| `internal` | — | Mark the most recent definition internal: hidden from `words`, `apropos`, and completion, and resolvable — by name or tick — only within its own load unit (the embedded library, one `load`/`load-library` file, or the REPL session); unreachable from another unit |
| `forget` | — | Read the following name; truncate the dictionary back to before it |

```forth :
: twice 2 * ; 21 twice . cr
```
```output
42
```

```forth ;
: greet "hi" . ; greet greet cr
```
```output
hi hi
```

```forth recurse
: fact dup 1 > if dup 1- recurse * then ; 5 fact . cr
```
```output
120
```

```forth variable
variable counter 5 to counter counter . cr
```
```output
5
```

```forth constant
42 constant answer answer . cr
```
```output
42
```

```forth to
variable total 1 to total total 10 + to total total . cr
```
```output
11
```

```forth symbol
symbol blue blue . cr
```
```output
:blue
```

```forth defer
defer greeting : hello-word "hello" . cr ; ' hello-word embodies greeting greeting
```
```output
hello
```

```forth embodies
defer greeting : hello-word "hello" . cr ; ' hello-word embodies greeting greeting
```
```output
hello
```

```forth embodies!
defer farewell : bye-word "bye" . cr ; ' bye-word embodies! farewell farewell
```
```output
bye
```

```forth base
base unit point 12 point unit pica 3 pica . cr
```
```output
3 pica
```

```forth unit
base unit inch 12 inch unit foot 2 foot 6 inch + . cr
```
```output
2.5 foot
```

```forth :name
:north . cr
```
```output
:north
```

```forth string>symbol
"dyn" "amic" + string>symbol . cr
```
```output
:dynamic
```

```forth [:
5 [: 2 * :] execute . cr
```
```output
10
```

```forth '
16 ' sqrt execute . cr
```
```output
4
```

```forth lookup
lookup sqrt 9 swap execute . cr
```
```output
3
```

```forth execute
7 ' 1+ execute . cr
```
```output
8
```

```forth curry
3 ' + curry 4 swap execute . cr
```
```output
7
```

```forth 2curry
1 2 ' - 2curry execute . cr
```
```output
-1
```

```forth ncurry
1 2 ' swap 2 ncurry execute . . cr
```
```output
1 2
```

```forth inline
: double 2 * ; inline : quadruple double double ; 5 quadruple . cr
```
```output
20
```

```forth internal
: helper-word 3 ; internal : public-word helper-word 2 * ; public-word . cr
```
```output
6
```

```forth forget
: era 1 ; era . cr forget era : era 2 ; era . cr
```
```output
1
2
```

### Locals

Declared only at the **head** of a definition or quotation body, and in a **single list**: a receive list followed by a scratch list — `|> items | | total i |` — is a compile error, `locals are declared in one list; mark individual receive slots with a > prefix (| >a b c |)`. A body mixing received and uninitialized slots writes one list with `>` on the names that receive, as the mixed row below shows. Live on the return stack: up to 128 names across up to 64 nested scopes. A body reads the locals **it declares itself** and nothing else: a reference to a name declared in an enclosing definition or an enclosing quotation is a compile error — `x is not bound in this quotation; pass it in or use pick` — and the partial definition rolls back. Values reach a quotation three ways: received into its own slots (`[>` receive-all, `[: | >x |` selective), parked on the stack below the combinator's operands and read by depth with `pick`, or bound into a curried token by `curry`/`2curry`/`ncurry`.

The mechanism: a local reference compiles to the **slot index** in the frame that declares it, always the innermost locals-bearing scope, and the op reads `local_base + slot` with no frame walk. Names are discarded after compilation and no value is bound then. Because every reference is depth 0, a quotation's meaning does not depend on which frames happen to be live when it runs — it reads the same slots under `map`, under `i-times`, through `execute`, inside another word's frame, or after a continuation capture and resume. The rejected alternative was resolving a reference as `(frames-up, slot)` against the live frame chain, which made a quotation's reads depend on its caller's frames and silently returned another word's slots when it travelled.

| Syntax | Behavior |
|--------|----------|
| `\| x y z \|` | Declare x, y, z, **uninitialized** (slots keep stale return-stack contents — deliberately no per-call zeroing; assign with `to` before reading — at `;`/`:]` the compiler rejects a slot fetched but stored nowhere, naming a shadowed word when one exists); read by bare name |
| `\|> x y z \|` | Declare and receive from the stack: z ← top, y ← second, x ← third |
| `\| x >y z \|` | Mixed: a `>` prefix marks an individual name as a receive slot; the rest are uninitialized |
| `\| ?x \|` | A `?` prefix marks a slot initialized with a fresh logic variable per call; read by bare name. Cannot combine with `>`, and not allowed in the all-receive `\|>` / `[>` forms |
| `[> x y z \| … :]` | Lambda sugar for the receive-all case: `[>` fuses `[:` and `\|>`, so x, y, z are received from the stack |

These compile-time words read a following local name and emit a single fused depth-0 instruction:

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `++` | `( -- )` | Increment the named local or global variable by 1 in place; only inside a colon definition; errors on an unknown or non-variable name | 1 | none | O(1) |
| `--` | `( -- )` | Decrement the named local or global variable by 1 in place; only inside a colon definition; errors on an unknown or non-variable name | 1 | none | O(1) |
| `f++` | `( -- )` ⚠ | Unsafe float increment: raw `.number` mutation, no tag check, for a local known to hold a float | 1 | none | O(1) |
| `f--` | `( -- )` ⚠ | Unsafe float decrement: raw `.number` mutation, no tag check | 1 | none | O(1) |

```forth ++
: count-up | n | 0 to n ++ n ++ n n ; count-up . cr
```
```output
2
```

```forth --
: count-down | n | 5 to n -- n n ; count-down . cr
```
```output
4
```

```forth f++
: fast-up | x | 1.5 to x f++ x x ; fast-up . cr
```
```output
2.5
```

```forth f--
: fast-down | x | 1.5 to x f-- x x ; fast-down . cr
```
```output
0.5
```

---

## I/O and printing

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `.` | `( a -- )` | Print value then a space; matrices print as a grid, frames pretty-print | 1 + print | none | O(size printed) |
| `.a` | `( a -- )` | Like `.` but shows everything: no element truncation, and floats print at full round-trip precision (`%.17g`) instead of `.`'s 6 significant figures. Matrix/vector columns lose their fixed-width alignment when values render at full precision | 1 + print | none | O(size printed) |
| `render` | `( a -- str )` | The text `.` would print, returned as a string instead of printed: no truncation, no trailing separator (a matrix grid's final newline is dropped). Strings render raw, symbols by name, collections/frames/matrices in their laid-out form | 1 + size | `1o` | O(size) |
| `.s` | `( -- )` | Print every stack value, bottom to top; leaves the stack intact | print | none | O(depth) |
| `peek` | `( a -- a )` | core.h2o: print the top value then a space without consuming it (`dup .`, inlined) — a stack probe | 1 + print | none | O(size printed) |
| `,` | `( a -- a )` | core.h2o: `peek` under a one-character name, for splicing probes into a pipeline (inlined) | 1 + print | none | O(size printed) |
| `print` | `( x -- )` | core.h2o: alias for `.` | 1 + print | none | O(size printed) |
| `print-stack` | `( -- )` | core.h2o: alias for `.s` | print | none | O(depth) |
| `cr` | `( -- )` | Print a newline | 1 | none | O(1) |
| `emit` | `( code -- )` | Print the character with codepoint `code`, UTF-8 encoded (1–4 bytes); range-checked `[0, 0x10FFFF]` | 1 | none | O(1) |
| `tty?` | `( -- bool )` | Whether stdout is a terminal (`isatty`) — printing words branch on it to emit styling only for a person at a terminal, so piped and batch output stays plain (`help` dims its prose this way) | 1 | none | O(1) |

String literals `"…"` are **raw**: bytes between the quotes are copied verbatim and an embedded newline is kept; the only escape is a doubled `""`, which yields one `"` (a lone `"` closes the string). There is no `{n}` substitution — a regex `\d{3}` literal is safe, and template-filling is the explicit word `format` (in String operations below).

```forth .
PI . cr
```
```output
3.14159
```

```forth .a
PI .a cr
```
```output
3.1415926535897931
```

```forth render
{ :a 1 } render print cr
```
```output
{
  :a 1
}
```

```forth .s
1 2 3 .s cr clear
```
```output
1 2 3
```

```forth peek
5 peek 1+ . cr
```
```output
5 6
```

```forth ,
1 2 , + . cr
```
```output
2 3
```

```forth print
"hello" print cr
```
```output
hello
```

```forth print-stack
7 8 print-stack cr clear
```
```output
7 8
```

```forth cr
"a" . cr "b" . cr
```
```output
a
b
```

```forth emit
87 emit 9731 emit cr
```
```output
W☃
```

```forth tty?
tty? . cr
```
```output
0
```

---

## String operations

Regex words run on PCRE2 with JIT-compiled patterns. Each distinct pattern is compiled once and cached (a 1024-slot hash table keyed on the pattern bytes, bounded probe window), so reusing a pattern costs a hash plus one comparison, then the match. Patterns are PCRE syntax in raw `"…"` literals — PCRE itself interprets `\n`, `\t`, `\d`, `\x22`, and the rest. Matching is multiline: `^` and `$` bind to line boundaries. Patterns and subjects are treated as **UTF-8**: `.` matches one codepoint, and `\w` `\d` `\s` `\b` use Unicode properties (accented letters, non-ASCII digits). Invalid byte sequences are tolerated — they simply fail to match rather than raising an error. Match offsets are **byte** offsets (pair them with `byte-substring`). Captures come back as strings; an optional group that didn't participate is `0.0`. Booleans are `1.0`/`0.0`. In the cost columns `n` is the subject length.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `match` | `( str pat -- [ whole cap… ] \| 0 )` | First (leftmost) match as a flat array: whole match then each capture; no match returns `0` | n | `1a` + captures | O(n) |
| `match-all` | `( str pat -- [ [whole cap…] … ] \| 0 )` | Every non-overlapping leftmost match, each a flat sub-array; a zero-width match advances one byte; no match returns `0` | n | `1a` per match + captures | O(n + m·g) |
| `replace` | `( str pat rep -- str' )` | Replace **all** matches; in `rep`, `&` or `\0` is the whole match, `\1`–`\9` a capture, `\&` and `\\` literals | n | `1o` + buffer growth | O(n) |
| `xml-escape` | `( str -- str' )` | strings.h2o: `&` `<` `>` `'` to their XML entities, for element text and single-quoted attributes; four `replace` passes | 4n | 4 strings | O(n) |
| `basename` | `( path -- filename )` | strings.h2o: the path's last component (`"^.*/" "" replace`, inlined); a path with no `/` passes through | n | `1o` | O(n) |
| `split` | `( str pat -- [ piece… ] )` | Split `str` at each non-overlapping match of `pat`; the pieces are the gaps between matches, empty fields kept; no match → `[ str ]` | n | `1a` + pieces | O(n) |
| `substring` | `( str start end -- sub )` | Half-open **codepoint** range `[start, end)`; bounds-checked against the codepoint count | 2 + n | `1o` | O(n) |
| `byte-substring` | `( str start end -- sub )` | Half-open **byte** range `[start, end)`; bounds-checked. Pairs with byte offsets from `match`/`match-all` | 2 + k | `1o` | O(k), k = end − start |
| `char-at` | `( str index -- char )` | The one-character string at codepoint `index`; bounds-checked against the codepoint count | 2 + n | `1o` | O(n) |
| `codepoint-at` | `( str index -- code )` | The integer codepoint at codepoint `index`; bounds-checked | 2 + n | none | O(n) |
| `string>chars` | `( str -- [ char… ] )` | Array of one-character strings, one per codepoint | n | `1a` + `1o`/char | O(n) |
| `string>codepoints` | `( str -- [ code… ] )` | Array of integer codepoints, one per codepoint | n | `1a` | O(n) |
| `codepoint>char` | `( code -- char )` | One-character string for codepoint `code`; range-checked `[0, 0x10FFFF]` | 1 | `1o` | O(1) |
| `codepoints>string` | `( [ code… ] -- str )` | Encode each codepoint to UTF-8 and concatenate; per-element type- and range-checked | n | `1o` | O(n) |
| `trim` | `( str -- str' )` | Strip leading and trailing ASCII whitespace (`' ' \t \n \v \f \r`); a backward/forward byte-scan, one allocation of the surviving span | n | `1o` | O(n) |
| `join` | `( arr sep -- str )` | Concatenate the string elements of `arr` separated by `sep`; errors on a non-string element | 2 + total | `1o` | O(total) |
| `index-of` | `( str pat -- i )` | strings.h2o: codepoint index of `pat`'s first regex match in `str`, or `-1` if none (`split 0 @i size` guarded by `has?`) | n | `1a` + pieces | O(n) |
| `spaces` | `( k -- str )` | strings.h2o: a string of k spaces (`" " swap array-of "" join`) | k | `1a` + `1o` | O(k) |
| `pad-left` | `( str width -- str' )` | strings.h2o: s left-padded with spaces to width (unchanged when already that wide; codepoint widths) | n | `1a` + `1o` | O(n) |
| `pad-right` | `( str width -- str' )` | strings.h2o: s right-padded with spaces to width (unchanged when already that wide; codepoint widths) | n | `1a` + `1o` | O(n) |
| `string>number` | `( str -- n \| none )` | Parse a decimal/float string (via `strtod`, like a numeric literal) to a float, ignoring surrounding whitespace; the none value if `str` is not entirely a number | n | none | O(n) |
| `edit-distance` | `( a b -- n )` | Edit distance between two strings over codepoints: insertions, deletions, substitutions, and adjacent transpositions each cost 1 (Levenshtein with transpositions — optimal string alignment); symmetric | n·m | none | O(n·m) |
| `format` | `( … template -- str )` | Fill `template`'s `{n}` (or `{n:spec}`) placeholders with the nth-from-top stack value, then drop exactly the referenced positions (unreferenced values stay); renders floats/strings/symbols. `{nl}` and `{tab}` emit a newline and a tab — string literals have no escapes, so format is where control characters come from. When stdout is a terminal, the ink directives `{black}` `{red}` `{green}` `{yellow}` `{blue}` `{magenta}` `{cyan}` `{white}` and `{bold}` `{dim}` emit the SGR escape styling the following text until `{plain}` reverts to plain ink; when it is not (piped, batch), they vanish, so redirected output carries no escape bytes. Only these directives substitute; other brace content is left literal | len + refs | `1o` | O(len) |

A placeholder may carry a format spec after a colon — `{n:spec}` — a printf-style mini-language controlling how the value renders. `spec` is optional flags (`-`, `+`, space, `#`, `0`), an optional field width, an optional `.precision`, and an optional conversion letter:

- `f` `e` `g` (and `F` `E` `G`) render the value as a float: `{0:.2f}` fixes the precision, `{0:8.2f}` also pads to a field width.
- `d` / `i` render it as an integer, truncated toward zero: `{0:04d}`.
- `s`, or no conversion letter, places the value's default rendering in a field: `{0:8}` right-justifies, `{0:-8}` left-justifies, `{0:.3}` truncates to three characters.

A float or integer conversion requires a float operand; a non-float operand, an unknown conversion letter, or trailing characters in the spec is an error. With no colon, `{n}` renders the value in its default form.

`first match` and `findall` are spelled `match` and `match-all`; there is no separate search/match/fullmatch split. Anchor with `^`/`$` (or `\A`/`\z`) when you need it.

```forth match
"x=42" "(\w+)=(\d+)" match . cr
```
```output
[ "x=42" "x" "42" ]
```

```forth match-all
"a1 b2" "\w(\d)" match-all . cr
```
```output
[ [ "a1" "1" ]
  [ "b2" "2" ] ]
```

```forth replace
"hello world" "o" "0" replace . cr
```
```output
hell0 w0rld
```

```forth xml-escape
"a < b & c" xml-escape . cr
```
```output
a &lt; b &amp; c
```

```forth basename
"/usr/local/bin/water" basename . cr
```
```output
water
```

```forth split
"a,b,,c" "," split . cr
```
```output
[ "a" "b" "" "c" ]
```

```forth substring
"water" 1 3 substring . cr
```
```output
at
```

```forth byte-substring
"héllo" 0 3 byte-substring . cr
```
```output
hé
```

```forth char-at
"héllo" 1 char-at . cr
```
```output
é
```

```forth codepoint-at
"A" 0 codepoint-at . cr
```
```output
65
```

```forth string>chars
"abc" string>chars . cr
```
```output
[ "a" "b" "c" ]
```

```forth string>codepoints
"AB" string>codepoints . cr
```
```output
[ 65 66 ]
```

```forth codepoint>char
9731 codepoint>char . cr
```
```output
☃
```

```forth codepoints>string
[ 87 111 87 ] codepoints>string . cr
```
```output
WoW
```

```forth trim
"  pad  " trim "|" + . cr
```
```output
pad|
```

```forth join
[ "a" "b" "c" ] "-" join . cr
```
```output
a-b-c
```

```forth index-of
"hello" "l+" index-of . cr
```
```output
2
```

```forth spaces
3 spaces byte-size . cr
```
```output
3
```

```forth pad-left
"7" 3 pad-left "|" + . cr
```
```output
  7|
```

```forth pad-right
"7" 3 pad-right "|" + . cr
```
```output
7  |
```

```forth string>number
"3.5" string>number . "x" string>number none? . cr
```
```output
3.5 1
```

```forth edit-distance
"kitten" "sitting" edit-distance . cr
```
```output
3
```

```forth format
1 2 "{1} then {0}" format . cr
PI "{0:.2f}" format . cr
"{bold}{red}alert{plain} normal" format . cr
```
```output
1 then 2
3.14
alert normal
```

---

## Sets

Sorted `Val` arrays with binary-search insertion; equality is structural. `+`/`*`/`-` on two sets are union/intersection/difference.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `[< v… >]` | `( -- set )` | Set literal; `[<` pushes a mark, `>]` gathers everything above it in one sort-and-dedup pass, like `set` | n log n | `1o` + realloc | O(n log n) |
| `set` | `( v₀ … vₙ₋₁ n -- set )` | Gather the top n values into a new set (the set analog of `array`) | 2 + n log n | `1o` + reallocs | O(n log n) |
| `union` | `( set₁ set₂ -- set₃ )` | Union into a new set, merging the two sorted arrays | m+n | `1o` + reallocs | O(m+n) |
| `intersection` | `( set₁ set₂ -- set₃ )` | Intersection into a new set, merging the two sorted arrays | m+n | `1o` + reallocs | O(m+n) |
| `difference` | `( set₁ set₂ -- set₃ )` | set₁ − set₂ into a new set, merging the two sorted arrays | m+n | `1o` + reallocs | O(m+n) |
| `set-add!` | `( set v -- set )` | Insert v in sorted position if absent (dedups); leaves set on the stack | log n + n | reallocs | O(n) |
| `set-remove!` | `( set v -- set )` | Remove v if present (no-op if absent); leaves set on the stack | log n + n | none | O(n) |
| `member?` | `( set v -- bool )` | Binary-search membership | 3 + log n | none | O(log n) |
| `array>set` | `( array -- set )` | Sort a copy of the array once and dedup into a set — the fast bulk constructor (one sort, not n inserts); the source array is unchanged | n log n | `1o` + realloc | O(n log n) |
| `set>array` | `( set -- arr )` | arrays.h2o: the elements as an array in `val_cmp` order — `sort`'s set branch, which copies rather than compares | 1 | `1o` | O(n) |
| `group-by` | `( array col -- frame )` | Group an array of frames by their symbol-valued `col` into a frame from each value to a set of the matching rows; one sorted pass, distinct values sorted | n log n | frame + sets | O(n log n) |
| `size` | `( coll -- n )` | Element count: set/array members, **codepoints** of a string, pair count of a frame; a string's codepoint count is computed on first use and memoized on the object | 2 | none | O(1); a string's first `size` is O(n) |
| `byte-size` | `( str -- n )` | Byte length of a string | 2 | none | O(1) |

```forth set
10 20 20 3 set . cr
```
```output
[< 10 20 >]
```

```forth union
[< 1 2 >] [< 2 3 >] union . cr
```
```output
[< 1 2 3 >]
```

```forth intersection
[< 1 2 3 >] [< 2 3 4 >] intersection . cr
```
```output
[< 2 3 >]
```

```forth difference
[< 1 2 3 >] [< 2 >] difference . cr
```
```output
[< 1 3 >]
```

```forth set-add!
[< 1 3 >] 2 set-add! . cr
```
```output
[< 1 2 3 >]
```

```forth set-remove!
[< 1 2 3 >] 2 set-remove! . cr
```
```output
[< 1 3 >]
```

```forth member?
[< 1 2 3 >] 2 member? . [< 1 2 3 >] 9 member? . cr
```
```output
1 0
```

```forth array>set
[ 3 1 3 2 ] array>set . cr
```
```output
[< 1 2 3 >]
```

```forth set>array
[< 3 1 2 >] set>array . cr
```
```output
[ 1 2 3 ]
```

```forth group-by
[ { :name "ann" :team :red } { :name "bo" :team :blue } { :name "cy" :team :red } ] :team group-by /red @ size . cr
```
```output
2
```

```forth size
[ 1 2 3 ] size . "héllo" size . { :a 1 } size . cr
```
```output
3 5 1
```

```forth byte-size
"héllo" byte-size . cr
```
```output
6
```

---

## Arrays

0-indexed, elements of any type. Grows at the end in amortized O(1) over a backing buffer that doubles on demand; indexing stays O(1).

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `[ v… ]` | `( -- arr )` | Array literal; `[` marks, `]` gathers above the mark | n | `1a(n)` | O(n) |
| `array` | `( v₀ … vₙ₋₁ n -- arr )` | Gather the top n values into an array | 2 + n | `1a(n)` | O(n) |
| `array-of` | `( val n -- arr )` | New n-element array, every slot = val | 3 + n | `1a(n)` | O(n) |
| `@i` | `( arr i -- val )` | Array element; on a matrix returns row i as a 1×c matrix | 3 (array) | matrix `1m(1×c)` | O(1) array; O(c) matrix |
| `!i` | `( arr i val -- arr )` | Store val at index i in place; leaves arr on the stack | 4 | none | O(1) |
| `add-last!` | `( arr v -- arr )` | Append v at the end, doubling the backing buffer when full; leaves arr on the stack | 2 | `≤1a` on grow | amortized O(1) |
| `remove-last!` | `( arr -- v )` | Remove and return the last element; errors on an empty array | 2 | none | O(1) |
| `take` | `( arr/set n -- arr )` | First n elements (clamped) | 2 + n | `1a(n)` | O(n) |
| `reverse` | `( arr/set -- arr )` | Reversed copy | 1 + n | `1a(n)` | O(n) |
| `concat` | `( arr/set arr/set -- arr )` | Concatenated copy | 2 + m + n | `1a(m+n)` | O(m+n) |
| `range` | `( from to -- arr )` | Inclusive integer range, step ±1 | 3 + n | `1a(n)` | O(n) |
| `destruct` | `( arr/set/fr -- v… )` | Spread elements onto the stack; a frame spreads alternating sym/value | 1 + n | none | O(n) |
| `destruct-to` | `( source targets -- )` | source and target arrays; assign each source element to the variable named by the corresponding target (symbol or xt), creating it if needed | 2 + n | may create variables | O(n) |
| `slice!` | `( arr tstart src sstart sstep slen -- arr )` | Copy `slen` elements `src[sstart], src[sstart+sstep], …` into `arr[tstart…]` in place | 6 + slen | self-overlap may malloc slen | O(slen) |
| `to-slice!` | `( v₀ … vₙ₋₁ arr offset n -- arr )` | Store the n values just below `arr` into `arr[offset…offset+n)`; leaves arr | 2 + n | none | O(n) |
| `last` | `( arr n -- arr )` | arrays.h2o: `swap reverse swap take reverse` | 3n | 3×`1a(n)` | O(n) |
| `first` | `( arr/pair -- v )` | core.h2o: element 0 of an array, or a cons's head — reads pairs-shaped results (`count`, `group-indices`) and logic pairs alike | 9 | none | O(1) |
| `second` | `( arr/pair -- v )` | core.h2o: element 1 of an array, or a cons's tail (`5 6 cons second` → 6; on a list literal the rest, not the next element) | 9 | none | O(1) |
| `skip` | `( arr n -- arr )` | arrays.h2o: `over size swap - swap reverse swap take reverse` | 3n | 3×`1a(n)` | O(n) |
| `sort` | `( arr/set/v -- arr/v )` | Sorted copy: an array orders by `val_cmp`; a set projects its already-ordered elements to an array; an nx1 or 1xn vector sorts ascending with NaNs last (other matrix shapes error) | 1 + n log n | `1a(n)` / `1m(n)` | O(n log n); vectors above 8k elements O(n) radix |
| `flatten-array` | `( arr -- arr )` | Flatten one level; returns the input unchanged if no element is itself an array | 1 + m | `1a(m)` | O(m) |
| `sample` | `( arr/set count repl -- arr )` | Draw `count` elements; `repl` truthy = with replacement, else without (count ≤ len) | 3 + n | `1a(count)` (+ `malloc(n)` without replacement) | O(n) |
| `shuffle` | `( arr -- arr )` | datasets.h2o: new array, elements uniformly permuted (a full `sample` without replacement); input untouched | 3 + n | as `sample` | O(n) |
| `resample` | `( arr/set -- arr )` | datasets.h2o: same-size draw with replacement (a full `sample` with replacement, the bootstrap draw); input untouched — the value-space sibling of `resample-indices` | 3 + n | `1a(n)` | O(n) |
| `iota` | `( n -- arr )` | arrays.h2o: `[0…n−1]`, empty when n ≤ 0 | 3 + n | `1a(n)` | O(n) |

```forth array
1 2 3 3 array . cr
```
```output
[ 1 2 3 ]
```

```forth array-of
0 4 array-of . cr
```
```output
[ 0 0 0 0 ]
```

```forth @i
[ 10 20 30 ] 1 @i . cr
```
```output
20
```

```forth !i
[ 1 2 3 ] 1 99 !i . cr
```
```output
[ 1 99 3 ]
```

```forth add-last!
[ 1 2 ] 3 add-last! . cr
```
```output
[ 1 2 3 ]
```

```forth remove-last!
[ 1 2 3 ] remove-last! . cr
```
```output
3
```

```forth take
[ 1 2 3 4 ] 2 take . cr
```
```output
[ 1 2 ]
```

```forth reverse
[ 1 2 3 ] reverse . cr
```
```output
[ 3 2 1 ]
```

```forth concat
[ 1 2 ] [ 3 4 ] concat . cr
```
```output
[ 1 2 3 4 ]
```

```forth range
3 7 range . cr
```
```output
[ 3 4 5 6 7 ]
```

```forth destruct
[ 1 2 3 ] destruct . . . cr
```
```output
3 2 1
```

```forth destruct-to
[ 10 20 ] [ :low :high ] destruct-to low . high . cr
```
```output
10 20
```

```forth slice!
[ 0 0 0 0 0 ] 1 [ 10 20 30 ] 0 1 3 slice! . cr
```
```output
[ 0 10 20 30 0 ]
```

```forth to-slice!
7 8 [ 0 0 0 0 ] 1 2 to-slice! . cr
```
```output
[ 0 7 8 0 ]
```

```forth last
[ 1 2 3 4 ] 2 last . cr
```
```output
[ 3 4 ]
```

```forth first
[ 7 8 9 ] first . cr
```
```output
7
```

```forth second
[ 7 8 9 ] second . cr
```
```output
8
```

```forth skip
[ 1 2 3 4 ] 1 skip . cr
```
```output
[ 2 3 4 ]
```

```forth sort
[ 3 1 2 ] sort . cr
```
```output
[ 1 2 3 ]
```

```forth flatten-array
[ [ 1 2 ] [ 3 ] ] flatten-array . cr
```
```output
[ 1 2 3 ]
```

```forth sample
42 seed [ 1 2 3 4 5 ] 3 false sample . cr
```
```output
[ 3 4 5 ]
```

```forth shuffle
42 seed [ 1 2 3 4 5 ] shuffle . cr
```
```output
[ 3 4 5 1 2 ]
```

```forth resample
42 seed [ 1 2 3 ] resample . cr
```
```output
[ 1 1 3 ]
```

```forth iota
4 iota . cr
```
```output
[ 0 1 2 3 ]
```

---

## Pairs (cons lists)

Cons cells in a dense, GC'd table — the linked, recursively-decomposable counterpart to arrays (O(1) prepend, tail-sharing, head/tail recursion). A list is a chain of pairs; `null` is the empty list and the terminator. The `[( … )]` reader takes the **last element as the tail**, so `[( a b c )]` is `cons(a, cons(b, c))` and a proper list is written `[( a b c null )]`. That makes `[( H T )]` exactly Prolog's `[H|T]` under `unify`. Printing resolves bound vars; output round-trips.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `[( v… )]` | `( -- list )` | List literal; the last element is the tail (`[( a b c )]` = `cons(a, cons(b, c))`; `[( )]` = `null`; one element = itself) | n | `n−1` pairs | O(n) |
| `cons` | `( head tail -- pair )` | Build a cons cell | 2 | `1 pair` | O(1) |
| `head-tail` | `( pair -- head tail )` | Split a pair — head under, tail on top; no auto-deref; errors on a non-pair | 1 | none | O(1) |
| `array>cons` | `( arr -- list )` | Cons chain from an array's elements (last element becomes the tail; `[ ]` → `null`) | n | `n−1` pairs | O(n) |
| `cons>array` | `( list -- arr )` | Walk a cons chain into an array, **dereferencing** the spine and each element and including the terminal (works on relational results) | n | `1a(n)` | O(n) |

```forth cons
1 2 cons . cr
```
```output
[( 1 2 )]
```

```forth head-tail
[( 1 2 3 null )] head-tail . . cr
```
```output
[( 2 3 null )] 1
```

```forth array>cons
[ 1 2 3 ] array>cons . cr
```
```output
[( 1 2 3 )]
```

```forth cons>array
[( 4 5 6 null )] cons>array . cr
```
```output
[ 4 5 6 null ]
```

`unify` decomposes/builds pairs (head then tail), and `=` compares them structurally — see Logic.

---

## Frames

Symbol-keyed sorted maps; binary-search lookup. A **path** is an array of steps; a plain *locator* is all symbols, and the literal `/a/b/c` is a compile-time constant array that allocates nothing at run time. A path may instead be a **search path** matching a set of nodes (see Path queries below). The single-target words (`@`, `!`, `delete-at`, `update-at`) require a locator and reject a search path, pointing the caller at `select-values`/`select-keys`; `has?` accepts either. `d` = path depth, `n` = frame size.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `{ :k v … }` | `( -- fr )` | Frame literal from alternating key/value pairs above the `{` mark; a path key (`/a/b/c`) vivifies nested frames. Built by sorted insertion — a binary search plus a shift per pair; `frame` / `array>frame` are the sort-once bulk constructors | n·(log n + n) | `1o` + reallocs | O(n²) |
| `frame` | `( keys values -- fr )` | Build from parallel key and value arrays of equal length | 2 + n log n | `1o` + reallocs | O(n log n) |
| `array>frame` | `( arr -- fr )` | Build from an even-length alternating-kv array; a path key (`/a/b/c`) vivifies nested frames | 1 + n log n | `1o` + reallocs | O(n log n) |
| `frame>array` | `( fr -- arr )` | Flatten to an alternating-kv array in the frame's own key order (symbol id, i.e. interning order, not alphabetical); inverse of `array>frame` | 1 + n | `1o` | O(n) |
| `@` | `( fr sym/path -- val )` | Get by key or path; errors if absent or if the path is a search path | 3 + d log n | none | O(d log n) |
| `name@key` | `( -- val )` | Get in one token: the part left of `@` is a local, else a defined word, and it supplies the frame; `key` is interned at read time and compiled as the operand of one op, so `row@price` is a local fetch plus `(@key)` with no symbol on the stack. Chains left to right — `row@address@city` walks two frames. An empty left part takes the frame from the stack, `( fr -- val )`, so `@price` is the postfix form. Errors as `@` does when the key is absent or the value is not a frame. The rule applies only to tokens that are not defined words, so `@i`, `@or` and any word named with an `@` keep their meanings, and defining `row@total` shadows the token form for that spelling | 2 + log n | none | O(log n) |
| `name!key` | `( val -- )` | Set in one token, dropping the frame `!` returns: `99 row!price` stores 99 at `:price` in `row`'s frame and leaves the stack empty. Same resolution of the left part as `name@key`, and an empty left part takes the frame from above the value, `( val fr -- )`. A chain may end in a set — `row@address!city` — but only its last step may, since a set leaves no frame to walk | 2 + log n | none | O(log n) |
| `@or` | `( fr sym/path fallback -- val )` | Get by key or path, the fallback when absent — `has? if @` in one probe, no error on miss; the fallback is already evaluated, so it suits values, not expensive computations | 4 + d log n | none | O(d log n) |
| `!` | `( fr sym/path val -- fr )` | Set by key or path, vivifying intermediates; mutates fr; errors on a search path | d log n | realloc on growth; `1o` per vivified frame | O(d log n) amortized |
| `has?` | `( fr sym/path -- bool )` | Existence test for a frame key or path, no error on miss; a search path is true if any node matches (short-circuits at the first); on a string `( str pat -- bool )`, true if regex `pat` matches anywhere | 3 + d log n | none | O(d log n) |
| `delete-at` | `( fr sym/path -- fr )` | Remove a key (errors if absent or on a search path); mutates fr | n | none | O(n) |
| `update-at` | `( fr sym/path xt -- fr )` | Apply xt to the value at the key, store the result back; errors on a search path | d log n + xt | none | O(d log n + xt) |
| `keys` | `( fr -- arr )` | Keys (symbols) in sorted order | 1 + n | `1a(n)` | O(n) |
| `values` | `( fr -- arr )` | Values in key order | 1 + n | `1a(n)` | O(n) |
| `merge` | `( fr₁ fr₂ -- fr )` | New frame with all keys; fr₂ wins collisions. A linear two-pointer merge of the two sorted key arrays | m+n | `1o` | O(m+n) |
| `copy` | `( a -- a' )` | Deep copy of any value, `copy_term`-style: dereferences bound logic vars to their values and gives each unbound var a fresh shared var; recurses into frames, arrays, matrices, strings, sets, continuations, pairs; identity for scalars. Defined generally, not frame-specific. | tree size | one object per node | O(tree size) |
| `reify` | `( a -- a' )` | Like `copy`, but each unbound var becomes a canonical inert symbol `:_0`, `:_1`, … numbered by first appearance — a ground, storable, comparable snapshot. | tree size | one object per node | O(tree size) |

```forth frame
[ :a :b ] [ 1 2 ] frame frame>array . cr
```
```output
[ :a 1 :b 2 ]
```

```forth array>frame
[ :x 1 :y 2 ] array>frame frame>array . cr
```
```output
[ :x 1 :y 2 ]
```

```forth frame>array
{ :a 1 :b 2 } frame>array . cr
```
```output
[ :a 1 :b 2 ]
```

```forth @
{ :a { :b 5 } } /a/b @ . cr
```
```output
5
```

```forth name@key
: price-of |> row | row@price ; { :price 9 } price-of . cr
```
```output
9
```

```forth name!key
: mark-sold |> row | 0 row!price row ; { :price 9 } mark-sold frame>array . cr
```
```output
[ :price 0 ]
```

```forth @or
{ :a 1 } :b 99 @or . cr
```
```output
99
```

```forth !
{ } /a/b 5 ! /a/b @ . cr
```
```output
5
```

```forth has?
{ :a 1 } :a has? . { :a 1 } :b has? . cr
```
```output
1 0
```

```forth delete-at
{ :a 1 :b 2 } :a delete-at frame>array . cr
```
```output
[ :b 2 ]
```

```forth update-at
{ :n 10 } :n ' 1+ update-at frame>array . cr
```
```output
[ :n 11 ]
```

```forth keys
{ :a 1 :b 2 } keys . cr
```
```output
[ :a :b ]
```

```forth values
{ :a 1 :b 2 } values . cr
```
```output
[ 1 2 ]
```

```forth merge
{ :a 1 :b 2 } { :b 20 :c 30 } merge frame>array . cr
```
```output
[ :a 1 :b 20 :c 30 ]
```

```forth copy
{ :a 1 } dup copy :a 2 ! drop :a @ . cr
```
```output
1
```

```forth reify
[ lvar lvar 5 ] reify . cr
```
```output
[ :_0 :_1 5 ]
```

### Path queries

A search path generalizes a locator with three step kinds, matching a set of nodes instead of one. Descent is through nested frames only; an array, set, or scalar is a leaf, and `//` is depth-capped against cycles.

- `*` — any one child at this level.
- `//` — descendant-or-self: any depth at or below the current node.
- `[…]` — a predicate filtering the current node: `[k]` (key `k` exists), `[k=v]`, `[k<v]`, `[k>v]` (compare key `k`'s value to `v`), `[.=v]`/`[.<v]`/`[.>v]` (compare the node itself, via `.`), or `[a/b op v]` (a sub-path subject). Several predicates on one step chain: `[role=admin][age>45]`.

So `/users/*/name` is the `:name` of every child of `:users`, `/root//city` is every `:city` at any depth, and `/people/*[age>30]` filters by predicate. `s` = nodes visited.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `select-values` | `( fr path -- arr )` | Every matched value, in document (pre-order) order, duplicates kept; no path built per match | s | `1a` + reallocs | O(s) |
| `select-keys` | `( fr path -- arr )` | The full root-to-match path (a symbol array) for every match, document order; each round-trips through `@` | s | `1a` + `1a` per match | O(s + total path length) |

`select-values` is the cheaper word (it captures the node directly, no per-match path array); `array>set` the result when distinct values are wanted, or `array>cons` to feed matches to `choose` as backtracking choice points.

```forth select-values
{ :a { :n 1 } :b { :n 2 } } /*/n select-values . cr
```
```output
[ 1 2 ]
```

```forth select-keys
{ :a { :n 1 } :b { :n 2 } } //n select-keys . cr
```
```output
[ [ :a :n ]
  [ :b :n ] ]
```

---

## JSON

Objects ↔ frames (keys interned as symbols), arrays ↔ arrays, strings ↔ strings, numbers ↔ floats. JSON `true`/`false` ↔ the reserved `:1`/`:0` symbols; `null` ↔ the none value.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `json>frame` | `( str -- val )` | Parse a JSON string. Escapes and `\uXXXX` (with surrogate pairs) decode to UTF-8; recursive-descent, depth-guarded; rejects trailing non-whitespace. Each object's keys are sorted after collection | scan + build | one object per node | O(\|s\| log \|s\|) |
| `frame>json` | `( val -- str )` | Serialize a value to JSON. Floats use the shortest round-trip form; strings are escaped (non-ASCII emitted raw); object keys are the symbol names | walk + build | `1o` string | O(tree size) |
| `null` | `( -- none )` | Push the none value (`T_NONE`) — what JSON `null` parses to, and what an unset `env` returns | 1 | none | O(1) |

```forth json>frame
"{""a"": [1, 2]}" json>frame /a @ . cr
```
```output
[ 1 2 ]
```

```forth frame>json
{ :a 1 } frame>json . cr
```
```output
{"a": 1}
```

```forth null
null . null none? . cr
```
```output
null 1
```

---

## Matrices

Row-major `double` storage. `r` rows, `c` columns.

### Construction

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `0-matrix` | `( r c -- mat )` | r×c zero matrix (calloc) | 3 | `1m(r×c)` | O(1)+ |
| `matrix` | `( arr r c -- mat )` or `( arr r -- mat )` | Build from a float array; two-arg form takes r = rows and infers columns | 3 + r×c | `1m(r×c)` | O(r×c) |
| `vector` | `( arr -- v )` | matrix.h2o: the array as an nx1 matrix, length inferred (`dup size 1 matrix`, inlined) | 3 + n | `1m(n)` | O(n) |
| `diagonal-matrix` | `( fill n -- mat )` | n×n matrix with `fill` on the diagonal | 2 + n | `1m(n×n)` | O(n) |
| `identity-matrix` | `( n -- mat )` | matrix.h2o: `1 swap diagonal-matrix` | n | `1m(n×n)` | O(n) |
| `matrix-range` | `( start end step -- mat )` | 1×N row of evenly spaced values | 3 + N | `1m(1×N)` | O(N) |

```forth 0-matrix
2 3 0-matrix dim swap . . cr
```
```output
2 3
```

```forth matrix
[ 1 2 3 4 ] 2 2 matrix render print cr
```
```output
<matrix 2x2>
          1          2
          3          4
```

```forth vector
[ 1 2 3 ] vector dim swap . . cr
```
```output
3 1
```

```forth diagonal-matrix
7 2 diagonal-matrix render print cr
```
```output
<matrix 2x2>
          7          0
          0          7
```

```forth identity-matrix
2 identity-matrix render print cr
```
```output
<matrix 2x2>
          1          0
          0          1
```

```forth matrix-range
0 1 0.25 matrix-range matrix>array . cr
```
```output
[ 0 0.25 0.5 0.75 1 ]
```

### Shape and indexing

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `@j` | `( mat j -- col )` | Column j as an r×1 matrix (copy) | 2 + r | `1m(r×1)` | O(r) |
| `@i,j` | `( mat i j -- f )` | Single element as a float | 4 | none | O(1) |
| `@e` | `( mat i -- f )` | Element at flat row-major index i as a float — consumes `argmin`/`argmax`/`where` indices; the same access on n×1 and 1×n vectors | 3 | none | O(1) |
| `!e` | `( mat i v -- mat )` | Store v (a float, or `null` for NaN) at flat row-major index i, in place | 4 | none | O(1) |
| `!i,j` | `( mat i j v -- mat )` | Store v (a float, or `null` for NaN) at row i, column j, in place | 5 | none | O(1) |
| `dim` | `( mat/dataset -- r c )` | Push rows then columns; datasets.h2o extends it to a dataset — rows from the first column's length, columns from the key count | 3 | none | O(1) |
| `reshape` | `( mat r c -- mat' )` | Same elements, new shape (must match); memcpy | 3 + r×c | `1m(r×c)` | O(r×c) |
| `transpose` | `( mat -- mat' )` | Rows/columns swapped | 1 + r×c | `1m(c×r)` | O(r×c) |
| `diagonal` | `( mat -- mat' )` | Diagonal as a 1×min(r,c) matrix | 1 + min(r,c) | `1m(1×min)` | O(min(r,c)) |
| `flatten` | `( mat -- mat' )` | matrix.h2o: 1×(r·c) reshape | r×c | `1m(1×r·c)` | O(r×c) |
| `as-column` | `( v -- v' )` | matrix.h2o: any vector shape as n×1 (`dup dim * 1 reshape`, inlined) | r×c | `1m(n×1)` | O(n) |
| `matrix>array` | `( mat -- arr )` | The elements as an array in row-major order: floats from a bare matrix; a dimensioned matrix yields one quantity per element in its unit; a NaN element becomes `null` either way | 1 + r×c | `1a(r×c)`; dimensioned + 1 pair per non-NaN element | O(r×c) |
| `num-elements` | `( mat -- n )` | matrix.h2o: `dim *` (inlined) | 5 | none | O(1) |
| `n-rows` | `( mat/dataset -- n )` | datasets.h2o: `dim drop` | 6 | none | O(1) |
| `n-columns` | `( mat/dataset -- n )` | datasets.h2o: `dim nip` | 8 | none | O(1) |

```forth @j
[ 1 2 3 4 ] 2 2 matrix 1 @j matrix>array . cr
```
```output
[ 2 4 ]
```

```forth @i,j
[ 1 2 3 4 ] 2 2 matrix 1 0 @i,j . cr
```
```output
3
```

```forth @e
[ 1 2 3 4 ] 2 2 matrix 2 @e . cr
```
```output
3
```

```forth !e
[ 1 2 3 4 ] 2 2 matrix 0 99 !e matrix>array . cr
```
```output
[ 99 2 3 4 ]
```

```forth !i,j
[ 1 2 3 4 ] 2 2 matrix 1 1 99 !i,j matrix>array . cr
```
```output
[ 1 2 3 99 ]
```

```forth dim
[ 1 2 3 4 5 6 ] 2 3 matrix dim swap . . cr
```
```output
2 3
```

```forth reshape
[ 1 2 3 4 5 6 ] 2 3 matrix 3 2 reshape dim swap . . cr
```
```output
3 2
```

```forth transpose
[ 1 2 3 4 ] 2 2 matrix transpose matrix>array . cr
```
```output
[ 1 3 2 4 ]
```

```forth diagonal
[ 1 2 3 4 ] 2 2 matrix diagonal matrix>array . cr
```
```output
[ 1 4 ]
```

```forth flatten
[ 1 2 3 4 ] 2 2 matrix flatten dim swap . . cr
```
```output
1 4
```

```forth as-column
[ 1 2 3 ] 1 3 matrix as-column dim swap . . cr
```
```output
3 1
```

```forth matrix>array
[ 1 2 3 4 ] 2 2 matrix matrix>array . cr
```
```output
[ 1 2 3 4 ]
```

```forth num-elements
[ 1 2 3 4 5 6 ] 2 3 matrix num-elements . cr
```
```output
6
```

```forth n-rows
[ 1 2 3 4 5 6 ] 2 3 matrix n-rows . cr
```
```output
2
```

```forth n-columns
[ 1 2 3 4 5 6 ] 2 3 matrix n-columns . cr
```
```output
3
```

### Multiplication and reductions

`dgemm` variants do real matrix multiply; element-wise `*` does not.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `dgemm-nn` | `( α A B β C -- R )` | `R = α·A·B + β·C`, ikj fast path | 5 + m·k·n | `1m(m×n)` | O(m·k·n) |
| `dgemm-tn` | `( α A B β C -- R )` | `R = α·Aᵀ·B + β·C` | 5 + m·k·n | `1m(m×n)` | O(m·k·n) |
| `dgemm-nt` | `( α A B β C -- R )` | `R = α·A·Bᵀ + β·C` | 5 + m·k·n | `1m(m×n)` | O(m·k·n) |
| `dgemm-tt` | `( α A B β C -- R )` | `R = α·Aᵀ·Bᵀ + β·C` | 5 + m·k·n | `1m(m×n)` | O(m·k·n) |
| `sum` | `( mat -- f )` | Sum of all elements (4-way unrolled, fast-math) | 1 + r×c | none | O(r×c) |
| `max` | `( mat -- f )` | Maximum element | 1 + r×c | none | O(r×c) |
| `min` | `( mat -- f )` | Minimum element | 1 + r×c | none | O(r×c) |
| `argmax` | `( mat -- f )` | Flat row-major index of the maximum element (first on ties) | 1 + r×c | none | O(r×c) |
| `argmin` | `( mat -- f )` | Flat row-major index of the minimum element (first on ties) | 1 + r×c | none | O(r×c) |
| `row-sums` | `( mat -- mat' )` | r×1 of per-row sums | 1 + r×c | `1m(r×1)` | O(r×c) |
| `row-maxes` | `( mat -- mat' )` | r×1 of per-row maxima | 1 + r×c | `1m(r×1)` | O(r×c) |
| `row-mins` | `( mat -- mat' )` | r×1 of per-row minima | 1 + r×c | `1m(r×1)` | O(r×c) |
| `column-sums` | `( mat -- mat' )` | 1×c of per-column sums | 1 + r×c | `1m(1×c)` | O(r×c) |
| `column-maxes` | `( mat -- mat' )` | 1×c of per-column maxima | 1 + r×c | `1m(1×c)` | O(r×c) |
| `column-mins` | `( mat -- mat' )` | 1×c of per-column minima | 1 + r×c | `1m(1×c)` | O(r×c) |
| `mean` | `( mat -- f )` | matrix.h2o: sum ÷ element count | r×c | none | O(r×c) |
| `row-means` | `( mat -- mat' )` | matrix.h2o: `row-sums` then scalar ÷ | r×c | 2×`1m(r×1)` | O(r×c) |
| `column-means` | `( mat -- mat' )` | matrix.h2o: `column-sums` then scalar ÷ | r×c | 2×`1m(1×c)` | O(r×c) |

```forth dgemm-nn
1 [ 1 2 3 4 ] 2 2 matrix 2 identity-matrix 0 2 2 0-matrix dgemm-nn matrix>array . cr
```
```output
[ 1 2 3 4 ]
```

```forth dgemm-tn
1 [ 1 2 3 4 ] 2 2 matrix dup 0 2 2 0-matrix dgemm-tn matrix>array . cr
```
```output
[ 10 14 14 20 ]
```

```forth dgemm-nt
1 [ 1 2 3 4 ] 2 2 matrix dup 0 2 2 0-matrix dgemm-nt matrix>array . cr
```
```output
[ 5 11 11 25 ]
```

```forth dgemm-tt
1 [ 1 2 3 4 ] 2 2 matrix dup 0 2 2 0-matrix dgemm-tt matrix>array . cr
```
```output
[ 7 15 10 22 ]
```

```forth sum
[ 1 2 3 4 ] 2 2 matrix sum . cr
```
```output
10
```

```forth max
[ 1 2 3 4 ] 2 2 matrix max . cr
```
```output
4
```

```forth min
[ 1 2 3 4 ] 2 2 matrix min . cr
```
```output
1
```

```forth argmax
[ 3 9 4 1 ] vector argmax . cr
```
```output
1
```

```forth argmin
[ 3 9 4 1 ] vector argmin . cr
```
```output
3
```

```forth row-sums
[ 1 2 3 4 ] 2 2 matrix row-sums matrix>array . cr
```
```output
[ 3 7 ]
```

```forth row-maxes
[ 1 2 3 4 ] 2 2 matrix row-maxes matrix>array . cr
```
```output
[ 2 4 ]
```

```forth row-mins
[ 1 2 3 4 ] 2 2 matrix row-mins matrix>array . cr
```
```output
[ 1 3 ]
```

```forth column-sums
[ 1 2 3 4 ] 2 2 matrix column-sums matrix>array . cr
```
```output
[ 4 6 ]
```

```forth column-maxes
[ 1 2 3 4 ] 2 2 matrix column-maxes matrix>array . cr
```
```output
[ 3 4 ]
```

```forth column-mins
[ 1 2 3 4 ] 2 2 matrix column-mins matrix>array . cr
```
```output
[ 1 2 ]
```

```forth mean
[ 2 4 6 ] vector mean . cr
```
```output
4
```

```forth row-means
[ 1 2 3 4 ] 2 2 matrix row-means matrix>array . cr
```
```output
[ 1.5 3.5 ]
```

```forth column-means
[ 1 2 3 4 ] 2 2 matrix column-means matrix>array . cr
```
```output
[ 2 3 ]
```

### Reshaping, selection, statistics

The statistics treat NaN elements as missing values and skip them: `sum` and
`norm` count nothing missing as 0; `mean` `var` `std` `se` `min` `max`
`quantile` `median` `percentile` `iqr` `ci` `argmax` `argmin` error with "all
elements are NaN (missing)" (`var`: "needs at least 2 non-NaN elements") when
too little remains; the correlations delete row i from both vectors when
either element i is NaN, and error below 2 complete pairs; `regress-with`
deletes incomplete rows of its design matrix before fitting. The positional
operations (`cumulative-sum`, the row/column reductions, `dot`, `dgemm-*`)
keep NaN in place.

`c` below is the output column count; `k` the index count; `n = r×c`.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `augment` | `( a b -- mat )` | Concatenate two matrices column-wise; errors unless row counts match | 2 + r·c | `1m(r×c)` | O(r·c) |
| `vstack` | `( a b -- mat )` | Stack two matrices row-wise (a on top of b); errors unless column counts match | 2 + r·c | `1m(r×c)` | O(r·c) |
| `hstack` | `( a b -- mat )` | matrix.h2o: `augment` under its numpy name (inlined) | 2 + r·c | `1m(r×c)` | O(r·c) |
| `submatrix` | `( mat rs re cs ce -- mat )` | Copy the half-open block rows [rs,re) × cols [cs,ce); errors out of bounds or start > end | 5 + r·c | `1m(r×c)` | O(r·c) |
| `select-rows` | `( mat/dataset/arr idx -- same )` | New matrix of the rows named by `idx` — a float index array or an index vector (nx1 or 1xn, as `where`/`argsort` return); a dimensioned matrix keeps its unit; errors on a non-float or out-of-range index. datasets.h2o extends it to a dataset (every column gathered by the same indices — matrix and dimensioned columns through the matrix path, array columns element-wise) and to a bare array (elements gathered by index) | 2 + k·c | `1m(k×c)`; dataset one column each; array `1a(k)` | O(k·c) |
| `mesh` | `( v mask b -- v' )` | Masked substitution: element i of the result is `b`'s where `mask[i]` is a definite nonzero, `v`'s where it is 0 **or NaN** (an unknown mask cell changes nothing). `v` is a matrix, dimensioned matrix, or array; the mask a bare matrix of `v`'s shape (element count, for an array). `b` is shape-matched same-representation, or broadcasts: a float, `null` (→ NaN), a quantity, or — for an array subject — any single value. Units reconcile as `+`: `b` rescales into `v`'s unit, which the result keeps; a quantity against a bare number errors. Conditional-mutate idioms: `dup nan? 0 mesh` fills NaNs, `dup -1 eq null mesh` turns a sentinel into NaN, `dup 100 > 100 mesh` caps | 3 + n | `1m(r×c)` / `1a(n)` | O(n) |
| `argsort` | `( v -- v' )` or `( arr -- arr )` | The sorting permutation of a vector, shape preserved: element i is the source index of the i-th smallest value; ties keep index order, NaNs go last in index order. An array operand answers the permutation under `val_cmp` (structural, so mixed types order), ties in index order, as a float-index array | 1 + n log n | `1m(n)` + `malloc(16n)`; array `1a(n)` + `malloc(4n)` | O(n log n); vectors above 8k elements O(n) radix |
| `ranks` | `( v -- v' )` | statistics.h2o: 0-based midranks as nx1 — tied values share the mean of their sorted positions, NaNs rank last in index order; one `argsort`, a gather, and a linear run walk | n log n + 2n | `3m(n)` + `malloc(16n)` | O(n log n) |
| `where` | `( mat -- v )` | Flat row-major indices of the nonzero elements, as a k×1 index vector (1×k for a 1×n mask); composes with the `<`/`>` masks and `select-rows` | 1 + n | `1m(k)` | O(n) |
| `drop-nans` | `( v -- v' )` | matrix.h2o: the finite elements of a vector, NaNs dropped (`dup nan? 0 eq where select-rows`, inlined) | 4n | mask + index + `1m(k)` | O(n) |
| `cumulative-sum` | `( mat -- mat' )` | Running sum over the elements in row-major order, shape preserved — a vector's prefix sums (ecdf, ROC, and calibration plumbing) | 1 + n | `1m(r×c)` | O(n) |
| `var` | `( mat -- f )` | Sample variance (÷ n−1) over all elements; errors with fewer than 2 | 1 + n | none | O(n) |
| `quantile` | `( mat p -- f )` | Linearly-interpolated quantile at p ∈ [0,1] over all elements (sorts a copy); errors if p out of range or empty | 2 + n log n | `malloc(n)` | O(n log n) |
| `quantiles` | `( mat probs -- v )` | statistics.h2o: `quantile` at each probability in the `probs` array, as a vector in that order — R's `quantile(x, probs)` (type 7). Sorts a copy per probability | k·(2 + n log n) | `1a(k)` + `1m(k)` | O(k·n log n) |
| `histogram-table` | `( v n-bins -- fr )` | statistics.h2o: equal-width bin counts over a vector's value range, as `{ :counts (n-bins×1) :low :bin-width }`. NaNs dropped, the top value lands in the last bin, a constant vector takes the range value ± 1; errors on n-bins < 1 or no finite values | n + n-bins | `1m(n-bins)` + `1fr` | O(n + n-bins) |
| `ecdf` | `( v -- xs ys )` | statistics.h2o: the empirical CDF as two n×1 vectors — the finite elements sorted ascending, and the cumulative fractions (i+1)/n, so `ys` at index i is F(`xs` at i). Ties stay as consecutive points; NaNs are excluded from the points and from n; errors when no finite values remain | 2n log n | `2m(n)` + `1a(n)` | O(n log n) |
| `binomial-deviance` | `( y p -- dev )` | statistics.h2o: −2 Σ[y ln p + (1−y) ln(1−p)] over n×1 vectors — the proper scoring rule for probability models; p is clamped to [1e-12, 1−1e-12], so an overconfident prediction scores finitely bad rather than losing its ln 0 term to `sum`'s NaN skipping | 10n | clamp + term vectors | O(n) |
| `brier` | `( outcomes probabilities -- f )` | statistics.h2o: Brier score — mean of (probability − outcome)² over n×1 vectors; NaN elements are skipped by `mean` | 3n | `2m(n)` | O(n) |
| `auc` | `( outcomes scores -- f )` | statistics.h2o: area under the ROC curve = P(a random positive scores above a random negative), ties counted half (Mann–Whitney). outcomes n×1 in {0,1}, scores real; a NaN score drops its row (outcomes stay aligned); throws if either class is absent. Ties are handled exactly, so the result is independent of row order | n_pos·n_neg | index vectors + per-positive masks | O(n_pos·n_neg) |
| `cv-folds` | `( units n-folds -- folds )` | statistics.h2o: deal `units` round-robin into `n-folds` `[ train test ]` index-array pairs in the given order — the split `cross-validate` runs on; errors on n-folds < 2 or fewer units than folds | n | fold index arrays | O(n) |
| `cross-validate` | `( units n-folds fit-xt score-xt -- fold-losses )` | statistics.h2o: k-fold cross-validation — units deal round-robin into folds in the order given (shuffle first when order matters; reuse one shuffle to compare configurations on the same folds); per fold, `fit` `( train-units -- model )` then `score` `( model test-units -- loss )`, both taking everything from the stack since they run in this word's frame; answers the per-fold losses as an n-folds vector (`mean` it for the CV estimate, `std` for the standard error). A unit is whatever the array holds — rows, or per-cluster index arrays for cluster CV | k·(fit + score) + n·k | fold index arrays | O(k·(fit + score)) |
| `ks-distance` | `( a b -- d )` | Two-sample Kolmogorov–Smirnov statistic: the largest absolute gap between the two samples' ECDFs, both advanced past each pooled value before measuring (ties). Symmetric; d ∈ [0, 1]; NaNs excluded per sample, each sample's own n; dimensioned inputs are computed over their magnitudes; errors when either sample has no finite values | (n+m) log(n+m) | `malloc(n)` + `malloc(m)` | O((n+m) log(n+m)); above 8k elements the sorts are O(n) radix |
| `std` | `( mat -- f )` | statistics.h2o: standard deviation, `var sqrt` (inlined) | n | none | O(n) |
| `se` | `( mat -- f )` | statistics.h2o: standard error of the mean, `std / sqrt(n)` | n | none | O(n) |
| `median` | `( mat -- f )` | statistics.h2o: `0.5 quantile` (inlined) | n log n | `malloc(n)` | O(n log n) |
| `percentile` | `( mat pct -- f )` | statistics.h2o: `quantile` at pct ∈ [0,100] (inlined) | n log n | `malloc(n)` | O(n log n) |
| `iqr` | `( mat -- f )` | statistics.h2o: interquartile range, Q3 − Q1 | 2n log n | `malloc(n)` ×2 | O(n log n) |
| `nonmissing-count` | `( mat -- n )` | The number of non-NaN elements — the divisor `mean` and `se` use | 1 + n | none | O(n) |
| `summary` | `( v/dataset -- fr )` | statistics.h2o: a vector answers `{ :min :q1 :median :mean :q3 :max }` over its finite elements — a dimensioned vector in its unit, an instant vector (unit exactly `s`) with each statistic rendered through `time>iso` — plus `:missing` with the NaN count when any; an all-missing vector answers `{ :missing n }`, an empty one `{ }`. A dataset answers that frame per numeric column and `{ :distinct }` (distinct non-missing cells, plus `:missing`) per text column, keyed by column name; any other column value errors naming the column | 4n log n (per column) | `malloc(n)` ×4 + `1fr` (per column) | O(n log n) |
| `ci` | `( mat level -- low high )` | statistics.h2o: percentile confidence interval — level 0.95 gives the 0.025 and 0.975 quantiles | 2n log n | `malloc(n)` ×2 | O(n log n) |
| `complete-cases` | `( xs ys -- xs' ys' )` | statistics.h2o: drop the paired rows where either vector is NaN, keeping the two aligned and returned as n×1; magnitudes are taken, so dimensioned inputs come back unitless | 4n | index vector + 2 gathers | O(n) |
| `correlation-pearson` | `( xs ys -- f )` | statistics.h2o: Pearson r — center both vectors, then `dot` products for covariance and the two variances; accepts nx1 or 1xn; `null` when either vector is constant (R's NA) | 12n | `6m(n)` | O(n) |
| `correlation-spearman` | `( xs ys -- f )` | statistics.h2o: Spearman rho — `correlation-pearson` on the midrank `ranks` of both vectors (inlined); `null` when either vector is constant (R's NA) | 2n log n | `6m(n)` + `malloc(16n)` ×2 | O(n log n) |
| `correlation-kendall` | `( xs ys -- f )` | Kendall tau-b: concordant minus discordant pairs over sqrt of tie-corrected pair counts, via one (x,y) sort and a merge-sort exchange count; NaN when all x or all y are tied; errors on length mismatch or fewer than 2 elements | 2n log n | `malloc(16n)` ×2–3 | O(n log n); above 8k elements the pair sort is O(n) radix |
| `correlate-with` | `( xs ys xt B -- fr )` | statistics.h2o: bootstrap 95% CI for the correlation word at xt — resamples (x, y) pairs jointly, B refits via a curried fit through `pbootstrap`, as `{ :estimate :se :bias :ci-low :ci-high }`; deterministic under a fixed seed | B·(n + xt) | pairs matrix + per-worker resample + `1fr` | O(B·(n + xt) / cores) |
| `cor` | `( xs ys -- fr )` | statistics.h2o: `correlation-kendall` with a 500-replicate bootstrap CI — `' correlation-kendall 500 correlate-with` (inlined) | as `correlate-with` | as `correlate-with` | as `correlate-with` |
| `qnorm` | `( p -- z )` | statistics.h2o: standard normal quantile (inverse CDF), Acklam's rational approximation — relative error below 1.15e-9, matching R's qnorm to 1e-8 over both tails; errors unless p strictly inside (0, 1) | 30 | none | O(1) |
| `sample-without-replacement` | `( arr n -- arr )` | statistics.h2o: `false sample` (inlined) | n | as `sample` | O(n) |
| `sample-with-replacement` | `( arr n -- arr )` | statistics.h2o: `true sample` (inlined) | n | as `sample` | O(n) |
| `bootstrap` | `( data fit-xt B -- arr )` | statistics.h2o: B refits of fit-xt over resamples of data — dataset/matrix rows, or an array's elements. One serial draw sets the run seed; replicate i draws its indices via `resample-indices-ext` at run-seed + i, so no resample outlives its fit and results don't depend on scheduling — deterministic under a fixed seed | B(n + fit) | per-fit resample + `1a(B)` | O(B·(n + fit)) |
| `pbootstrap` | `( data fit-xt B -- arr )` | statistics.h2o: `bootstrap` with the fits run under `pmap` — identical results (per-replicate seeding), parallel resample+fit | as `bootstrap` | as `bootstrap` | O(B·(n + fit) / cores) |
| `bootstrap-with` | `( data fit-xt B mapper-xt -- arr )` | statistics.h2o: the bootstrap skeleton `bootstrap`/`pbootstrap` instantiate; mapper-xt is `map`-shaped | as `bootstrap` | as `bootstrap` | as `bootstrap` |
| `column>indicators` | `( column -- mat )` | statistics.h2o: one 0/1 indicator column per distinct value above the first (the reference) — an n×(k−1) matrix from a numeric vector or text array column, levels in `val_cmp` order (`column>set` lists them); a missing cell lands in no column; errors on fewer than 2 distinct values | n·k + n log n | level masks + `1m` per fold | O(n·k + n log n) |
| `indicators!` | `( design column sym -- design )` | statistics.h2o: `column>indicators`' named twin for a design dataset (a frame of columns): adds one 0/1 column per distinct value above the first (the reference), keyed `sym=level`, mutating and returning the frame — keys and columns derive from the same data, so a level change grows both together; `keys` then names the design and `dataset>matrix` over them is the aligned matrix; errors on fewer than 2 distinct values | n·k + n log n | level masks + frame growth | O(n·k + n log n) |
| `with-intercept` | `( X/design -- X'/design )` | statistics.h2o: a matrix gets a prepended column of ones, so a fit's beta[0] is the intercept; a design dataset gets an `:intercept` ones column keyed like any term (errors on an empty design — the rows are read from it) | r×c | matrix `1m(r×(c+1))`; design `1m(r×1)` | O(r×c) |
| `sigmoid` | `( mat -- mat' )` | statistics.h2o: elementwise logistic 1/(1+e⁻ˣ), mapping reals to (0,1) | 4n | `1m(r×c)` | O(n) |
| `regress-with` | `( dataset predictors response B fit-xt -- arr )` | statistics.h2o: the shared regression pipeline — design matrix with intercept, point estimate, then B bootstrap refits for per-coefficient `{ :estimate :se :bias :ci-low :ci-high }` frames; the loadable statistics library's `linear-regression`/`logistic-regression` pass the fit | fit + B·fit | matrices + B refits + `1a(k)` | O(B·fit) |
| `norm` | `( mat -- f )` | matrix.h2o: `frobenius-norm` under the short name (inlined) | 1 + n | none | O(n) |
| `dot` | `( v w -- f )` | matrix.h2o: inner product (`* sum`, inlined); shapes must broadcast, so match the vectors | 2 + 2n | `1m(n)` | O(n) |
| `frobenius-norm` | `( mat -- f )` | Euclidean (L2) norm: √(Σ aᵢⱼ²) over all elements — a vector's length; for a matrix the Frobenius (entrywise 2-)norm, not the spectral norm | 1 + n | none | O(n) |
| `fit-tree` | `( features y params -- tree )` | CART regression tree. `features` is a frame of typed columns — a numeric vector splits at a midpoint `:threshold`, an array column is categorical and splits on a mean-ordered subset stored as `:categories`; `y` is a numeric response vector. Returns a nested frame: every node carries `:prediction` (mean of its rows) and `:n_rows`, an internal node adds `:feature` and either `:threshold` or `:categories` plus `:left`/`:right`, a leaf optionally carries `:responses`. Splits maximize S_L²/n_L + S_R²/n_R (squared-error reduction), each numeric column presorted once. Params frame: `:max-depth` (default unlimited), `:min-samples` (minimum rows on each side of a split, default 1), `:store-leaf-responses` (default off). A numeric split learns a default direction for rows missing that feature (NaN): the side that maximizes the split criterion, stored on the node as `:default` (`:left`/`:right`) — present only when the node saw missing rows | features·n·depth | `malloc(24n)` per numeric column + node buffer + tree frame | O(features·n·depth) |
| `predict` | `( tree features -- yhat )` | statistics.h2o: apply a `fit-tree` tree to a features frame keyed as at training, walking each row from the root to a leaf — a `:threshold` node sends value ≤ threshold left, a `:categories` node sends set membership left (an unseen value goes right), a NaN feature value follows the node's `:default` (left when the node has none) — and answer the leaf `:prediction`s as an n×1 vector | n·depth | `1a(n)` + `1m(n)` | O(n·depth) |
| `feature-importance` | `( tree -- fr )` | statistics.h2o: normalized impurity-reduction importance from a `fit-tree` tree — each split's squared-error reduction (`n_L·pred_L² + n_R·pred_R² − n_P·pred_P²`) summed per `:feature` and scaled to sum 1, as a frame keyed by feature symbol over the features actually split on; a stump gives `{ }` | nodes | `1fr` | O(nodes) |
| `prune` | `( tree alpha -- tree )` | statistics.h2o: cost-complexity prune in place — collapse every subtree whose total split-gain per extra leaf is at most `alpha` (bottom-up, so each collapse sees already-pruned children); `alpha` 0 leaves the tree unchanged, large `alpha` reduces it to the root stump. Mutates the input tree | nodes | leaf frames | O(nodes) |
| `prune-cv` | `( features y params -- tree )` | statistics.h2o: fit a `fit-tree`, then `prune` at the `alpha` the 1-SE rule picks — the largest `alpha` (smallest tree) whose mean k-fold CV mean-squared-error is within one standard error of the minimum, over the weakest-link `alpha` sequence; `:folds` in params sets k (default 5). Fits k×(sequence length) trees | k·seq·(fit + n) | trees + fold data | O(k·seq·fit) |
| `draw-tree` | `( tree -- )` | statistics.h2o: print a `fit-tree` tree as indented split rules — each internal node's condition (`feature <= threshold`, or `feature in <categories>`), left (condition-true) branch first, and each leaf's `predict <value> (n <rows>)` | nodes | strings | O(nodes) |

```forth augment
[ 1 2 ] vector [ 3 4 ] vector augment matrix>array . cr
```
```output
[ 1 3 2 4 ]
```

```forth vstack
[ 1 2 ] vector [ 3 4 ] vector vstack matrix>array . cr
```
```output
[ 1 2 3 4 ]
```

```forth hstack
[ 1 2 ] vector [ 3 4 ] vector hstack matrix>array . cr
```
```output
[ 1 3 2 4 ]
```

```forth submatrix
[ 1 2 3 4 5 6 7 8 9 ] 3 3 matrix 0 2 1 3 submatrix matrix>array . cr
```
```output
[ 2 3 5 6 ]
```

```forth select-rows
[ 10 20 30 40 ] vector [ 2 0 ] select-rows matrix>array . cr
```
```output
[ 30 10 ]
```

```forth mesh
[ 1 -1 3 ] vector dup -1 eq null mesh matrix>array . cr
```
```output
[ 1 null 3 ]
```

```forth argsort
[ 30 10 20 ] vector argsort matrix>array . cr
```
```output
[ 1 2 0 ]
```

```forth ranks
[ 10 20 20 30 ] vector ranks matrix>array . cr
```
```output
[ 0 1.5 1.5 3 ]
```

```forth where
[ 5 0 7 ] vector where matrix>array . cr
```
```output
[ 0 2 ]
```

```forth drop-nans
[ 1 null 3 ] vector drop-nans matrix>array . cr
```
```output
[ 1 3 ]
```

```forth cumulative-sum
[ 1 2 3 ] vector cumulative-sum matrix>array . cr
```
```output
[ 1 3 6 ]
```

```forth var
[ 2 4 4 4 5 5 7 9 ] vector var . cr
```
```output
4.57143
```

```forth quantile
[ 1 2 3 4 ] vector 0.5 quantile . cr
```
```output
2.5
```

```forth quantiles
[ 1 2 3 4 ] vector [ 0 0.5 1 ] quantiles matrix>array . cr
```
```output
[ 1 2.5 4 ]
```

```forth histogram-table
[ 1 1 2 3 3 3 ] vector 3 histogram-table :counts @ matrix>array . cr
```
```output
[ 2 1 3 ]
```

```forth ecdf
[ 3 1 2 ] vector ecdf matrix>array . matrix>array . cr
```
```output
[ 0.333333 0.666667 1 ] [ 1 2 3 ]
```

```forth binomial-deviance
[ 1 0 1 ] vector [ 0.9 0.1 0.8 ] vector binomial-deviance . cr
```
```output
0.867729
```

```forth brier
[ 1 0 ] vector [ 0.8 0.3 ] vector brier . cr
```
```output
0.065
```

```forth auc
[ 1 0 1 0 ] vector [ 0.9 0.2 0.7 0.4 ] vector auc . cr
```
```output
1
```

```forth cv-folds
[ 0 1 2 3 ] 2 cv-folds . cr
```
```output
[ [ [ 1 3 ]
    [ 0 2 ] ]
  [ [ 0 2 ]
    [ 1 3 ] ] ]
```

```forth cross-validate
[ 1 2 3 4 ] 2 [: vector mean :] [: vector mean swap - abs :] cross-validate matrix>array . cr
```
```output
[ 1 1 ]
```

```forth ks-distance
[ 1 2 3 ] vector [ 1 2 3 ] vector ks-distance . cr
[ 1 2 3 ] vector [ 4 5 6 ] vector ks-distance . cr
```
```output
0
1
```

```forth std
[ 2 4 4 4 5 5 7 9 ] vector std . cr
```
```output
2.13809
```

```forth se
[ 2 4 4 4 5 5 7 9 ] vector se . cr
```
```output
0.755929
```

```forth median
[ 1 2 3 4 ] vector median . cr
```
```output
2.5
```

```forth percentile
[ 1 2 3 4 ] vector 25 percentile . cr
```
```output
1.75
```

```forth iqr
[ 1 2 3 4 ] vector iqr . cr
```
```output
1.5
```

```forth nonmissing-count
[ 1 null 3 ] vector nonmissing-count . cr
```
```output
2
```

```forth summary
[ 1 2 3 4 ] vector summary /median @ . cr
```
```output
2.5
```

```forth ci
[ 1 2 3 4 5 6 7 8 9 10 ] vector 0.8 ci . . cr
```
```output
9.1 1.9
```

```forth complete-cases
[ 1 null 3 ] vector [ 4 5 null ] vector complete-cases matrix>array . matrix>array . cr
```
```output
[ 4 ] [ 1 ]
```

```forth correlation-pearson
[ 1 2 3 4 5 ] vector [ 2 4 6 8 10 ] vector correlation-pearson . cr
```
```output
1
```

```forth correlation-spearman
[ 1 2 2 3 ] vector [ 1 3 2 4 ] vector correlation-spearman . cr
```
```output
0.948683
```

```forth correlation-kendall
[ 1 2 3 4 5 ] vector [ 2 1 4 3 5 ] vector correlation-kendall . cr
```
```output
0.6
```

```forth correlate-with
7 seed [ 1 2 3 4 5 6 7 8 9 10 ] vector [ 2 1 4 3 5 7 6 9 8 10 ] vector ' correlation-pearson 100 correlate-with :estimate @ . cr
```
```output
0.951515
```

```forth cor
7 seed [ 1 2 3 4 5 6 7 8 9 10 ] vector [ 2 1 4 3 5 7 6 9 8 10 ] vector cor :estimate @ . cr
```
```output
0.822222
```

```forth qnorm
0.975 qnorm . cr
```
```output
1.95996
```

```forth sample-without-replacement
42 seed [ 1 2 3 4 5 ] 2 sample-without-replacement . cr
```
```output
[ 3 4 ]
```

```forth sample-with-replacement
42 seed [ 1 2 3 ] 4 sample-with-replacement . cr
```
```output
[ 1 1 3 3 ]
```

```forth bootstrap
42 seed [ 1 2 3 4 5 ] [: vector mean :] 3 bootstrap . cr
```
```output
[ 2.2 2.4 2.6 ]
```

```forth pbootstrap
42 seed [ 1 2 3 4 5 ] [: vector mean :] 3 pbootstrap . cr
```
```output
[ 2.2 2.4 2.6 ]
```

```forth bootstrap-with
42 seed [ 1 2 3 ] [: vector mean :] 2 ' map bootstrap-with . cr
```
```output
[ 1.66667 1 ]
```

```forth column>indicators
[ "a" "b" "a" "c" ] column>indicators matrix>array . cr
```
```output
[ 0 0 1 0 0 0 0 1 ]
```

```forth indicators!
{ } [ "r" "g" "r" ] :color indicators! keys . cr
```
```output
[ :color=r ]
```

```forth with-intercept
[ 1 2 ] vector with-intercept matrix>array . cr
```
```output
[ 1 1 1 2 ]
```

```forth sigmoid
[ 0 ] vector sigmoid matrix>array . cr
```
```output
[ 0.5 ]
```

```forth-noexec regress-with
\ the loadable statistics library passes its LAPACK fit:
adult [ :age :education-num ] :income 200 ' fit-linear regress-with
```
```output
[ per-coefficient { :estimate :se :bias :ci-low :ci-high } frames ]
```

```forth norm
[ 3 4 ] vector norm . cr
```
```output
5
```

```forth dot
[ 1 2 3 ] vector [ 4 5 6 ] vector dot . cr
```
```output
32
```

```forth frobenius-norm
[ 3 4 ] vector frobenius-norm . cr
```
```output
5
```

```forth fit-tree
{ :x [ 1 2 3 4 ] vector } [ 10 10 20 20 ] vector { } fit-tree :feature @ . cr
```
```output
:x
```

```forth predict
{ :x [ 1 2 3 4 ] vector } [ 10 10 20 20 ] vector { } fit-tree { :x [ 1 4 ] vector } predict matrix>array . cr
```
```output
[ 10 20 ]
```

```forth feature-importance
{ :x [ 1 2 3 4 ] vector } [ 10 10 20 20 ] vector { } fit-tree feature-importance keys . cr
```
```output
[ :x ]
```

```forth prune
{ :x [ 1 2 3 4 ] vector } [ 10 10 20 20 ] vector { } fit-tree 1000 prune :prediction @ . cr
```
```output
15
```

```forth prune-cv
{ :x [ 1 2 3 4 5 6 ] vector } [ 1 1 1 9 9 9 ] vector { :folds 2 } prune-cv :prediction @ . cr
```
```output
5
```

```forth draw-tree
{ :x [ 1 2 3 4 ] vector } [ 10 10 20 20 ] vector { } fit-tree draw-tree
```
```output
x <= 2.5
  predict 10  (n 2)
x > 2.5
  predict 20  (n 2)
```

---

## Segments

Flat, fixed-length typed numeric buffers stored off the arena (one `calloc`, freed by GC). Both `int-segment` and `double-segment` store values as doubles internally; `@i` reads a float and `!i` stores a float, so they share the array index ops. Use them for FFI scratch (`segment>pointer`) and dense numeric data without per-element boxing.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `int-segment` | `( n -- seg )` | n-element int segment, zero-filled; errors if n < 0 | 1 | `1seg(n)` | O(n) |
| `double-segment` | `( n -- seg )` | n-element double segment, zero-filled; errors if n < 0 | 1 | `1seg(n)` | O(n) |
| `@i` | `( seg i -- f )` | Read element i as a float (see Arrays) | 3 | none | O(1) |
| `!i` | `( seg i f -- seg )` | Store float f at index i in place; leaves seg | 4 | none | O(1) |
| `segment>pointer` | `( seg -- ptr )` | Intern the backing buffer and return an FFI pointer handle (no copy; see Foreign function interface) | 1 | none | O(1)† |

`†` amortized; the pointer-intern table grows occasionally.

```forth int-segment
2 int-segment 1 7 !i dup 0 @i . 1 @i . cr
```
```output
0 7
```

```forth double-segment
2 double-segment 0 1.5 !i 0 @i . cr
```
```output
1.5
```

```forth @i
3 int-segment 0 @i . cr
```
```output
0
```

```forth-noexec segment>pointer
4 double-segment segment>pointer ptr? . cr
```
```output
1
```

---

## Random

A thread-local xoshiro256\*\* stream; each worker derives its own stream from the shared base seed, so parallel runs are deterministic per worker.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `seed` | `( n -- )` | Set the global base seed and reset the stream counter; per-thread streams derive from it | 1 | none | O(1) |
| `random` | `( -- f )` | Uniform float in [0,1) | 1 | none | O(1) |
| `random-int` | `( bound -- f )` | Uniform integer in [0,bound) as a float, by rejection sampling; errors if bound ≤ 0 | 1 | none | O(1)† |

`†` expected O(1); rejection sampling may retry. `sample` (Arrays) and `resample-indices` (Datasets and TSV) draw on this stream.

```forth seed
42 seed random . cr
```
```output
0.083863
```

```forth random
42 seed random . random . cr
```
```output
0.083863 0.37898
```

```forth random-int
42 seed 10 random-int . 10 random-int . cr
```
```output
2 2
```

---

## Wall-clock time and dates

An *instant* is epoch seconds as a quantity in `s`, anchored at
1970-01-01T00:00:00Z, so the units machinery is the date arithmetic:
`wall-now 2 hour +` is an instant, instant − instant is a duration, and
`… 1 day /` counts days. (`now`, under REPL and introspection, is the
monotonic interval clock; `wall-now` is the absolute one.) A *date* is a frame
`{ :year :month :day :hour :minute :second :weekday :yearday }` — `:second`
carries fractions, `:weekday` is 0–6 with 0 = Sunday, `:yearday` is 1-based.
Composition accepts a partial frame — `:year` required, `:month`/`:day`
default 1, clock fields 0, other keys ignored — and out-of-range fields carry
mktime-style: `:month 13` is next January, `:day 0` the last day of the
previous month. Unsuffixed words are UTC and pure Gregorian arithmetic,
identical on every platform; the `-local` twins go through libc in the
process's timezone, re-reading `TZ` on every call. WASI has no timezone
machinery, so there the `-local` words behave as UTC and `parse-time` lacks
`%z`.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `wall-now` | `( -- instant )` | units.h2o: CLOCK_REALTIME epoch seconds as a quantity in `s`; steps when the system clock is adjusted, so time intervals with `now` | 2 | 1 pair | O(1) |
| `epoch>date` | `( instant -- date )` | Decompose an instant into a date frame, UTC | 40 | `1o` | O(1) |
| `epoch>date-local` | `( instant -- date )` | Decompose in the process's timezone | 40 | `1o` | O(1) |
| `date>epoch` | `( date -- instant )` | Compose an instant from a date frame, UTC; `:year` required, absent fields default, out-of-range fields carry | 30 | 1 pair | O(1) |
| `date>epoch-local` | `( date -- instant )` | Compose via `mktime` in the process's timezone; the zone rules resolve DST | 30 | 1 pair | O(1) |
| `format-time` | `( instant format -- string )` | Render with strftime, UTC | len | `1s` | O(len) |
| `format-time-local` | `( instant format -- string )` | Render with strftime in the process's timezone | len | `1s` | O(len) |
| `parse-time` | `( string format -- instant )` | Parse with strptime; uncaptured fields default to 1970-01-01 00:00:00, read as UTC unless the format captures an offset with `%z`; errors on a mismatch | len | 1 pair | O(len) |
| `time>iso` | `( instant -- string )` | units.h2o: `"%Y-%m-%dT%H:%M:%SZ" format-time` | len | `1s` | O(1) |
| `iso>time` | `( string -- instant )` | units.h2o: parse the Z form `time>iso` emits | len | 1 pair | O(1) |
| `days-in-month` | `( year month -- days )` | units.h2o: length of the month, leap-aware (first of next month minus first of this) | 60 | frames | O(1) |
| `date-shift` | `( instant delta -- instant )` | units.h2o: calendar shift, UTC. `:years`/`:months` step the calendar with the day clamped to the target month (Jan 31 + 1 month = Feb 28/29); `:weeks` `:days` `:hours` `:minutes` `:seconds` add exact durations. Components combine and may be negative | 200 | frames + pairs | O(1) |

```forth-noexec wall-now
wall-now time>iso . cr
```
```output
2026-08-06T23:14:09Z
```

```forth epoch>date
0 s epoch>date :year @ . cr
```
```output
1970
```

```forth epoch>date-local
"TZ" "UTC" env! 0 s epoch>date-local :year @ . cr
```
```output
1970
```

```forth date>epoch
{ :year 2000 } date>epoch 1 day / round . cr
```
```output
10957
```

```forth date>epoch-local
"TZ" "UTC" env! { :year 2000 } date>epoch-local { :year 2000 } date>epoch = . cr
```
```output
1
```

```forth format-time
0 s "%Y-%m-%d" format-time . cr
```
```output
1970-01-01
```

```forth format-time-local
"TZ" "UTC" env! 0 s "%H:%M" format-time-local . cr
```
```output
00:00
```

```forth parse-time
"2001-02-03" "%Y-%m-%d" parse-time time>iso . cr
```
```output
2001-02-03T00:00:00Z
```

```forth time>iso
0 s time>iso . cr
```
```output
1970-01-01T00:00:00Z
```

```forth iso>time
"2020-01-02T03:04:05Z" iso>time epoch>date :day @ . cr
```
```output
2
```

```forth days-in-month
2024 2 days-in-month . cr
```
```output
29
```

```forth date-shift
"2026-01-31T09:00:00Z" iso>time { :months 1 } date-shift time>iso . cr
```
```output
2026-02-28T09:00:00Z
```

---

## Datasets and TSV

*Rows* are an array of row-arrays (as `load-tsv` returns) — the raw I/O interchange, the only form preserving a file's physical column order. A *dataset* is a column-oriented frame; a *relation* is a deduped, indexed fact set (see Fact database). `r`/`c` are rows/columns, `n`/`k` observations/selected columns. `select-rows` (under Matrices) accepts a dataset, gathering every column by one index array or vector — with `where` masks and `argsort` that is filtering and sorting.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `load-tsv` | `( path -- rows )` | Read a TSV file into an array of row-arrays; an empty cell → `none`, a numeric cell → float, else a string. No header handling | 1 + bytes | `1a(r)` + one array per row + a string per text cell | O(bytes) |
| `read-tsv` | `( path -- dataset )` | datasets.h2o: a TSV file with a header row as a column-oriented dataset (`load-tsv true rows>dataset`, inlined), columns typed as `rows>dataset` types them; a headerless file goes through `load-tsv` + `rows>dataset` | bytes + 2·r·c | rows + one array per column + `1m` per numeric column + `1fr` | O(bytes + r·c) |
| `write-tsv` | `( dataset path -- )` | datasets.h2o: write a dataset as a TSV with a header row — `dataset>rows` then `save-tsv` (inlined), `read-tsv`'s inverse; a dimensioned column errors in `save-tsv` (strip with `magnitude` first) | 2·r·c | transient rows | O(r·c) |
| `save-tsv` | `( rows path -- )` | Write an array of row-arrays as TSV; `none` → empty, a whole-number float → integer, strings raw; errors on a tab/newline inside a string or a non-array row | 2 + r·c | none (to file) | O(r·c) |
| `rows>dataset` | `( rows header? -- dataset )` | datasets.h2o: column-oriented frame from rows with typed columns — uniformly float-or-`none` cells become an n×1 vector (`none` → NaN), uniform-unit quantity cells a dimensioned vector, anything else stays the cell array; keys come from row 0 when header? is true, else `:col1…` are synthesized | 2·r·c | `k×1a(r)` + `1m` per numeric column + `1fr` | O(r·c) |
| `rows>relation` | `( rows index-cols header? -- relation )` | datasets.h2o: deduped relation indexed on `index-cols` (coerced to symbols) | r·c | one frame per row + relation + index buckets | O(r·c) |
| `dataset>rows` | `( dataset -- rows )` | datasets.h2o: the inverse of `true rows>dataset` — an array of row-arrays led by a header row of the column names as strings, columns in key order, cells through `column>array` (NaN → `null`, dimensioned cells as quantities); feeds `save-tsv` directly (`1 skip` for headerless rows) | r·c | header + one array per row + `1a(r·c)` cells | O(r·c) |
| `headn` | `( dataset n leading-columns -- )` | datasets.h2o: print the first min(n, rows) rows as an aligned table — the `leading-columns` symbols appear first in the given order, the remaining columns alphabetical by name (an empty `leading-columns` orders every column alphabetically); column names as the header line, two-space gutter, numeric/quantity columns right-aligned, text left, `:datetime` columns through `time>iso`, other cells through `render`; empty dataset prints nothing | r·c | rendered cells | O(r·c) |
| `head` | `( dataset -- )` | datasets.h2o: `10 [ ] headn` — the first 10 rows with columns alphabetical | r·c | rendered cells | O(r·c) |
| `dataset>matrix` | `( dataset cols -- mat )` | datasets.h2o: build an n×k matrix from the named numeric columns (rows are observations) | n·k | flat `1a(n·k)` + `2m(n×k)` | O(n·k) |
| `column-type` | `( dataset sym -- sym )` | datasets.h2o: the named column's type from its representation — matrix `:numeric`, quantity in exactly `s` `:datetime`, other quantity `:quantity`, array `:text`; a missing key errors through `@` | 8 | 1 pair | O(log c) |
| `column>array` | `( column -- arr )` | datasets.h2o: a column in any representation as an array of its values — arrays pass through unchanged, matrix/quantity columns go through `matrix>array` (NaN → `null`, dimensioned elements become quantities) | n | `1a(n)` for matrix columns, none for arrays | O(n) |
| `column>set` | `( column -- set )` | datasets.h2o: the set of the column's distinct values — `column>array array>set` | 2n log n | `1a(n)` + `1o` | O(n log n) |
| `select-columns` | `( dataset cols -- dataset )` | datasets.h2o: the named columns as a new dataset (a fresh frame sharing the column values); a missing name errors through `@` | k log c | `1a(k)` + `1o` | O(k log c) |
| `count` | `( arr/v/dataset -- pairs )` | datasets.h2o: occurrences of each distinct value as `[ [ value n ] … ]`, most frequent first, ties in value order (`val_cmp`); a vector counts its elements (a dimensioned one counts quantities), a dataset counts whole rows, each a frame keyed by column name | 2n log n | rows + pairs + 3×`1a` | O(n log n) |
| `group-indices` | `( column -- pairs )` | datasets.h2o: `[ [ value [indices] ] … ]` per distinct value in `val_cmp` order — each index array holds the value's row positions, ascending (one `argsort`, the permutation cut at run boundaries); `count`'s shape with positions instead of tallies, so one pass replaces a per-value `eq where` scan | 2n log n | permutation + one pair and array per value | O(n log n) |
| `frames>dataset` | `( rows -- dataset )` | datasets.h2o: an array of row frames (as `query`, `db-query` `:rows`, or `map` over a dataset produce) as a column-oriented dataset, keys from row 0 — differing keys throw. Each column's representation is inferred: all-float cells (`none` → NaN) become an n×1 vector, uniform-unit quantities a dimensioned vector, anything else stays an array | n·k log k | one column per key + `1o` | O(n·k log k) |
| `replace-where` | `( dataset sym pred replacement -- )` | datasets.h2o: replace the named column's cells passing `pred` `( column -- mask )`, in place — `update-at` around `mesh`, so the replacement broadcasts and units reconcile: `pipeline :rep_touches [: -1 eq :] null replace-where` nulls a sentinel, `[: nan? :] 0` fills missing, `[: 10 $ < :] 5 $` floors prices | pred + n | mask + one column | O(n) |
| `resample-indices` | `( n -- arr )` | datasets.h2o: n indices drawn from [0,n) with replacement (bootstrap), from the global stream | 2n | `2×1a(n)` | O(n) |
| `resample-indices-ext` | `( n seed -- arr )` | n indices drawn from [0,n) with replacement by a private generator seeded from `seed` (splitmix64-expanded) — same draw for the same seed regardless of thread or stream position; the bootstrap words seed replicate i at run-seed + i | n | `1a(n)` | O(n)† |

```forth load-tsv
[ [ "a" "b" ] [ 1 2 ] ] "/tmp/docs-example.tsv" save-tsv "/tmp/docs-example.tsv" load-tsv . cr
```
```output
[ [ "a" "b" ]
  [ 1 2 ] ]
```

```forth read-tsv
[ [ "x" ] [ 1 ] [ 2 ] ] true rows>dataset "/tmp/docs-example2.tsv" write-tsv "/tmp/docs-example2.tsv" read-tsv :x @ mean . cr
```
```output
1.5
```

```forth write-tsv
[ [ "x" ] [ 1 ] [ 2 ] ] true rows>dataset "/tmp/docs-example2.tsv" write-tsv "/tmp/docs-example2.tsv" read-tsv :x @ mean . cr
```
```output
1.5
```

```forth save-tsv
[ [ "a" "b" ] [ 1 2 ] ] "/tmp/docs-example.tsv" save-tsv "/tmp/docs-example.tsv" load-tsv . cr
```
```output
[ [ "a" "b" ]
  [ 1 2 ] ]
```

```forth rows>dataset
[ [ "n" "v" ] [ "a" 1 ] [ "b" 2 ] ] true rows>dataset :v @ matrix>array . cr
```
```output
[ 1 2 ]
```

```forth rows>relation
[ [ "team" ] [ "red" ] [ "red" ] ] [ :team ] true rows>relation { } query size . cr
```
```output
1
```

```forth dataset>rows
[ [ "x" ] [ 7 ] ] true rows>dataset dataset>rows . cr
```
```output
[ [ "x" ]
  [ 7 ] ]
```

```forth head
[ [ "name" "age" ] [ "ann" 34 ] [ "bo" 25 ] ] true rows>dataset head
```
```output
age  name
 34  ann
 25  bo
```

```forth headn
[ [ "name" "age" ] [ "ann" 34 ] [ "bo" 25 ] ] true rows>dataset 1 [ :name ] headn
```
```output
name  age
ann    34
```

```forth dataset>matrix
[ [ "x" "y" ] [ 1 10 ] [ 2 20 ] ] true rows>dataset [ :x :y ] dataset>matrix matrix>array . cr
```
```output
[ 1 10 2 20 ]
```

```forth column-type
[ [ "x" ] [ 1 ] ] true rows>dataset :x column-type . cr
```
```output
:numeric
```

```forth column>array
[ [ "x" ] [ 1 ] [ 2 ] ] true rows>dataset :x @ column>array . cr
```
```output
[ 1 2 ]
```

```forth column>set
[ "b" "a" "b" ] column>set . cr
```
```output
[< "a" "b" >]
```

```forth select-columns
[ [ "a" "b" ] [ 1 2 ] ] true rows>dataset [ :b ] select-columns keys . cr
```
```output
[ :b ]
```

```forth count
[ :b :a :b ] count . cr
```
```output
[ [ :b 2 ]
  [ :a 1 ] ]
```

```forth group-indices
[ :x :y :x ] group-indices . cr
```
```output
[ [ :x
    [ 0 2 ] ]
  [ :y
    [ 1 ] ] ]
```

```forth frames>dataset
[ { :a 1 } { :a 2 } ] frames>dataset :a @ matrix>array . cr
```
```output
[ 1 2 ]
```

```forth replace-where
[ [ "v" ] [ -1 ] [ 5 ] ] true rows>dataset dup :v [: -1 eq :] null replace-where :v @ nonmissing-count . cr
```
```output
1
```

```forth resample-indices
42 seed 4 resample-indices . cr
```
```output
[ 2 2 1 1 ]
```

```forth resample-indices-ext
5 99 resample-indices-ext . cr
```
```output
[ 3 1 2 1 0 ]
```

---

## Higher-order

The quotation/predicate cost dominates; `xt` denotes one call.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `map` | `( arr/set xt -- arr )` or `( dataset xt -- dataset )` | Apply xt to each element; xt must net exactly one value. datasets.h2o extends it to a dataset: xt maps each row frame to a new row frame — derive, rename, or drop fields — and the returned frames rebuild through `frames>dataset`, so all rows must share keys and columns re-infer their representation | 2 + n·xt | `1a(n)`; dataset rows + new columns | O(n·xt); dataset O(n·(xt + k log k)) |
| `nmap` | `( arr₁ … arr_N xt N -- arr )` | N-ary zip-map over equal-length arrays | rows·(N+xt) | `1a(rows)` | O(rows·xt) |
| `filter` | `( arr/set xt -- arr )` or `( dataset xt -- dataset )` | Keep elements where xt is truthy. datasets.h2o extends it to a dataset: xt sees each row as a frame keyed by column name and answers a bool (1.0/0.0); the kept rows come back through `select-rows`, so every column keeps its representation | 2 + n·xt | malloc(n) flags + `1a(k)`; dataset rows + mask + one column each | O(n·xt) |
| `reduce` | `( arr/set init xt -- val )` | Left fold; xt is `( acc elem -- acc )` | 3 + n·xt | none | O(n·xt) |
| `times` | `( xt n -- )` | Run xt n times, no index pushed | 2 + n·xt | none | O(n·xt) |
| `sum-times` | `( xt n -- total )` | arrays.h2o: `fold-times` with 0 and `' f+` — the sum of `xt` `( i -- term )` over i in 0..n-1 | 3 + n·(1+xt) | none | O(n·xt) |
| `product-times` | `( xt n -- product )` | arrays.h2o: `fold-times` with 1 and `' f*` — the product of `xt` `( i -- term )` over i in 0..n-1 | 3 + n·(1+xt) | none | O(n·xt) |
| `i-times` | `( xt n -- )` | Run xt n times, pushing index 0..n-1 first | 2 + n·(1+xt) | none | O(n·xt) |
| `fold-times` | `( acc map-xt combine-xt n -- acc' )` | Counted map-fold, the serial counterpart of `pmap-reduce`: for i in 0..n-1 push i, run `map-xt` `( i -- term )`, then combine the accumulator with the term. The accumulator never reaches the data stack — with `' f+`, `' f-`, `' f*`, `' f/` or their polymorphic twins the arithmetic runs inside the loop with no dispatch, and any other combiner is invoked as `( acc term -- acc' )`. `0 [: dup f* :] ' f+ 5 fold-times` answers 30; values the body needs beyond the index are parked below and read with `pick`. Per element it costs slightly less than `i-times` with an equivalent body, since the index leaves through the combinator rather than a dispatched `drop` | 4 + n·(1+xt) | none | O(n·xt) |
| `find-first` | `( items pred -- element )` | The first element for which pred is truthy, or the none value; short-circuits at the first hit (does not run pred over the rest) | n·xt | none | O(n·xt) |
| `any?` | `( items pred -- bool )` | arrays.h2o: `find-first none? not` — short-circuits, since `find-first` stops at the first hit | n·xt | none | O(n·xt) |
| `all?` | `( items pred -- bool )` | arrays.h2o: `map 1 [: * :] reduce` — true when every element satisfies pred, vacuously true on empty. Runs pred over **every** element (it maps then folds), so it does not short-circuit and a side-effecting pred fires n times | 2n·xt | `1a(n)` | O(n·xt) |
| `each` | `( items xt -- )` | Run xt `( element -- )` on every element for its side effects; the element is the only thing the quotation may consume, and it must leave nothing. No result, no allocation | 2 + n·xt | none | O(n·xt) |
| `flat-map` | `( items xt -- arr )` | arrays.h2o: `map flatten-array`; xt returns an array per element, results concatenated | n·xt + total | `1a(n)` + `1a(total)` | O(n·xt + total) |
| `sort-by` | `( items xt -- arr )` | arrays.h2o: sorted by the key xt `( element -- key )` extracts, one evaluation per element; the keys are `argsort`ed and the elements gathered by that permutation, so equal keys keep index order | n·xt + n log n | 3×`1a(n)` + `malloc(4n)` | O(n·xt + n log n) |
| `partition` | `( items pred -- matches rest )` | arrays.h2o: the elements satisfying pred and the others, one pass, input order kept | n·xt | 2 arrays + the curried predicate token | O(n·xt) |
| `group-with` | `( items xt -- fr )` | arrays.h2o: group elements into `{ key → set }` by the symbol key xt `( element -- sym )` computes — the quotation-keyed kin of `group-by` | n·(xt + log n) | frame + sets | O(n·xt + n log n) |

```forth map
[ 1 2 3 ] [: dup * :] map . cr
```
```output
[ 1 4 9 ]
```

```forth nmap
[ 1 2 ] [ 10 20 ] ' + 2 nmap . cr
```
```output
[ 11 22 ]
```

```forth filter
[ 1 2 3 4 ] [: 2 mod 0= :] filter . cr
```
```output
[ 2 4 ]
```

```forth reduce
[ 1 2 3 4 ] 0 ' + reduce . cr
```
```output
10
```

```forth times
[: "ho" . :] 3 times cr
```
```output
ho ho ho
```

```forth sum-times
[: dup * :] 4 sum-times . cr
```
```output
14
```

```forth product-times
' 1+ 4 product-times . cr
```
```output
24
```

```forth i-times
[: . :] 3 i-times cr
```
```output
0 1 2
```

```forth fold-times
0 [: dup f* :] ' f+ 5 fold-times . cr
```
```output
30
```

```forth find-first
[ 3 8 5 ] [: 4 > :] find-first . cr
```
```output
8
```

```forth any?
[ 1 3 5 ] [: 2 mod 0= :] any? . cr
```
```output
0
```

```forth all?
[ 2 4 ] [: 2 mod 0= :] all? . cr
```
```output
1
```

```forth each
[ 1 2 3 ] [: . :] each cr
```
```output
1 2 3
```

```forth flat-map
[ 1 2 ] [: dup 1 + 2 array :] flat-map . cr
```
```output
[ 1 2 2 3 ]
```

```forth sort-by
[ "bb" "a" "ccc" ] ' size sort-by . cr
```
```output
[ "a" "bb" "ccc" ]
```

```forth partition
[ 1 2 3 4 ] [: 2 mod :] partition . . cr
```
```output
[ 2 4 ] [ 1 3 ]
```

```forth group-with
[ 1 2 3 4 ] [: 2 mod 0= if :even else :odd then :] group-with frame>array . cr
```
```output
[ :even [< 2 4 >] :odd [< 1 3 >] ]
```

### Parallel

Run the xt across worker threads over the shared heap; `w` worker threads, `c` items per claim. The bare forms default to `num-cores` workers and claim 1. The worker contract: the xt produces fresh values — reading shared inputs is fine, writing into them from several workers is an unguarded race — and it must not print (workers share one stdout; print from the coordinator after the join). Allocation of any type inside a worker is unrestricted — strings, arrays, sets, frames, matrices, segments, cons cells, interned symbols all take the per-thread path. A faulting xt (a throw, wrong arity, out-allocating the region) aborts the whole region and raises a clean error, never a partial result. Each worker's logic-variable bindings are private, so unification inside a parallel quotation is local to its worker and does not compose into a shared search.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `pmap` | `( arr xt -- arr )` | Parallel `map` (num-cores workers, claim 1) | 2 + n·xt | `1a(n)` | O(n·xt / w) |
| `pmap-ext` | `( arr w c xt -- arr )` | `pmap` with explicit worker count and items-per-claim | 2 + n·xt | `1a(n)` | O(n·xt / w) |
| `pfilter` | `( arr pred -- arr )` | Parallel `filter`, order preserved | 2 + n·xt | malloc(n) flags + `1a(k)` | O(n·xt / w) |
| `pfilter-ext` | `( arr w c pred -- arr )` | `pfilter` with explicit worker count and items-per-claim | 2 + n·xt | malloc(n) flags + `1a(k)` | O(n·xt / w) |
| `pmap-reduce` | `( arr id map-xt combine-xt -- val )` | Fused parallel map+fold; `combine-xt` must be associative with `id` as neutral element | 2 + n·xt | per-worker partials | O(n·xt / w) |
| `pmap-reduce-ext` | `( arr w c id map-xt combine-xt -- val )` | `pmap-reduce` with explicit worker count and items-per-claim | 2 + n·xt | per-worker partials | O(n·xt / w) |
| `num-cores` | `( -- n )` | Online CPU count (`sysconf`) | 1 | none | O(1) |

```forth pmap
[ 1 2 3 4 ] [: dup * :] pmap . cr
```
```output
[ 1 4 9 16 ]
```

```forth pmap-ext
[ 1 2 3 4 ] 2 1 [: 10 * :] pmap-ext . cr
```
```output
[ 10 20 30 40 ]
```

```forth pfilter
[ 1 2 3 4 5 ] [: 2 mod 0= :] pfilter . cr
```
```output
[ 2 4 ]
```

```forth pfilter-ext
[ 1 2 3 4 5 ] 2 1 [: 3 > :] pfilter-ext . cr
```
```output
[ 4 5 ]
```

```forth pmap-reduce
[ 1 2 3 4 ] 0 [: dup * :] ' + pmap-reduce . cr
```
```output
30
```

```forth pmap-reduce-ext
[ 1 2 3 4 ] 2 1 0 ' identity ' + pmap-reduce-ext . cr
```
```output
10
```

```forth num-cores
num-cores 0 > . cr
```
```output
1
```

---

## Delimited continuations

The substrate for exceptions, coroutines, generators. See `docs/continuations.md`. `L` = captured return-stack length.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `reset` | `( -- )` | Push a unique mark on the return stack, delimiting the captured region | 1 | none | O(1) |
| `shift` | `( -- k )` | Capture the return-stack slice up to the nearest `reset`, remove the mark and that slice, push k | L | `1o` (cont) | O(L) |
| `shift-with` | `( xt -- )` | Capture as `shift`, then run xt in the outer context with k on the stack and begin unwinding | L + xt | `1o` (cont) | O(L + xt) |
| `resume` | `( k -- … )` | Pop k and re-enter it (multi-shot — the continuation object survives, so a retained copy can be resumed again); pushes whatever the resumed code yields | L + resumed | none | O(L + resumed) |
| `throw` | `( exc -- )` | Unwind to the nearest exception prompt, leaving `exc 1` (what `catch` consumes); with no enclosing prompt it is an interpreter error, `uncaught exception: <value>`, the trace captured at the throw site. The prompt search skips locals regions, so stale bytes in uninitialized local slots are never read as prompts | L | none | O(L) |
| `catch` | `( xt -- result 0 \| exc 1 )` | exceptions.h2o: `reset (execute-catching) 0`; `(result 0)` on success, `(exc 1)` on a `throw` **or** an interpreter error (an error frame `{ :message :trace }` becomes the exception value) | — | cont if thrown; `1f` + `2s` on a caught interpreter error | O(xt) |
| `try-catch` | `( normal-xt err-xt -- … )` | exceptions.h2o: run normal-xt; on a `throw` or interpreter error, run err-xt with the exception (the `{ :message :trace }` error frame, for an interpreter error) on the stack | — | cont if thrown; `1f` + `2s` on a caught interpreter error | O(normal-xt) |
| `ensure` | `( body-xt cleanup-xt -- … )` | exceptions.h2o: run cleanup-xt (stack-neutral) whether body-xt returns normally or throws/errors, then re-raise on the throw path | — | cont if thrown | O(body-xt) |
| `expect` | `( flag -- )` | test.h2o: pass silently when flag is truthy; else throw `expectation was false` | — | `1s` on fail | O(1) |
| `expect=` | `( actual expected -- )` | test.h2o: pass when `actual = expected` (deep structural `=`); else throw `expected <expected>, got <actual>` | — | `1s` on fail | O(n) |
| `expect-near` | `( actual expected tolerance -- )` | test.h2o: pass when `\|actual − expected\| <= tolerance`; else throw the expected-range message | — | `1s` on fail | O(1) |
| `expect-throws` | `( xt -- )` | test.h2o: run xt under `catch`; pass iff it throws, else throw `expected a throw` | — | cont if thrown | O(xt) |
| `test` | `( name xt -- )` | test.h2o: run xt under `catch`; print `ok <name>` or `FAIL <name>: <reason>` (a runtime error's `:message`, else the thrown value), tally it, restore the stack, continue past a failure | — | prints | O(xt) |
| `test-report` | `( -- )` | test.h2o: print `<n> passed, <m> failed`; throw when any failed so a program-file run exits nonzero | — | prints | O(1) |
| `new-tests` | `( -- )` | test.h2o: zero the passed and failed counters `test` tallies into, so the next `test-report` covers only the tests run after it — one file's independent groups, or a re-run suite in a session | — | none | O(1) |
| `with-db` | `( path body-xt -- … )` | exceptions.h2o: `db-open` the path, run body-xt `( db -- … )` with the handle, `db-close` on either exit | — | 1 db + cont if thrown | O(body-xt) |
| `with-stream` | `( stream body-xt -- … )` | exceptions.h2o: run body-xt `( stream -- … )` over an already-open stream, `close` it on either exit | — | cont if thrown | O(body-xt) |

```forth reset
: two-step reset 1 . shift 2 . cr ;
two-step "mid" . cr resume "end" . cr
```
```output
1 2
mid
2
end
```

```forth shift
: two-step reset 1 . shift 2 . cr ;
two-step "mid" . cr resume "end" . cr
```
```output
1 2
mid
2
end
```

```forth shift-with
: risky reset "a" . [: drop "b" . cr :] shift-with "c" . cr ;
risky
```
```output
a b
```

```forth resume
: two-step reset 1 . shift 2 . cr ;
two-step "mid" . cr resume "end" . cr
```
```output
1 2
mid
2
end
```

```forth throw
: catch-demo [: "boom" throw :] catch if "caught" . . cr then ; catch-demo
```
```output
caught boom
```

```forth catch
[: 42 :] catch . . cr
```
```output
0 42
```

```forth try-catch
[: 1 0 / :] [: :message @ . cr :] try-catch
```
```output
division by zero
```

```forth ensure
[: "body" . :] [: "cleanup" . :] ensure cr
```
```output
body cleanup
```

```forth expect
1 expect "ok" . cr
```
```output
ok
```

```forth expect=
2 2 expect= "same" . cr
```
```output
same
```

```forth expect-near
3.14 PI 0.01 expect-near "near" . cr
```
```output
near
```

```forth expect-throws
[: "x" throw :] expect-throws "threw" . cr
```
```output
threw
```

```forth test
new-tests "adds" [: 3 4 + 7 expect= :] test test-report
```
```output
ok adds
1 passed, 0 failed
```

```forth test-report
new-tests "adds" [: 3 4 + 7 expect= :] test test-report
```
```output
ok adds
1 passed, 0 failed
```

```forth new-tests
new-tests "adds" [: 3 4 + 7 expect= :] test test-report
```
```output
ok adds
1 passed, 0 failed
```

```forth with-db
":memory:" [: "create table t(x)" [ ] db-exec . :] with-db cr
```
```output
0
```

```forth-noexec with-stream
"echo hi" run :out @ [: read print :] with-stream
```
```output
hi
```

---

## Generators

Coroutines over the continuation substrate: a producer `yield`s values one at a time and a driver `resume`s it for the next. All generators.h2o on `shift`/`reset`/`resume`. `L` = captured return-stack length per step.

A producer drops nothing after `yield`. The word does not consume the value it emits, and what stands on the stack when the producer resumes is whatever the driver left there: under `gen-take` the emitted value itself, since that word gathers the run of them into an array at the end; under `gen-each` the `:gen-end` sentinel, the consumer having already taken the value. A `drop` after `yield` therefore eats a collected value under `gen-take` (`count N out of range`) and eats the sentinel under `gen-each`, which ends the iteration early and silently.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `yield` | `( v -- … )` | generators.h2o: `shift` — emit v to the driver and suspend. What stands on the stack when the producer resumes is the driver's doing, so a producer loop drops nothing after `yield` (see the note above the table) | L | `1o` (cont) | O(L) |
| `start-generator` | `( producer -- value generator )` | generators.h2o: `reset execute` — run producer to its first `yield`; leaves the yielded value and a resumable continuation | L | `1o` (cont) | O(producer to first yield) |
| `gen-take` | `( producer count -- array )` | generators.h2o: the first `count` values the producer yields, collected into an array | — | `1a(count)` + cont/step | O(count · L) |
| `gen-each` | `( producer consumer -- )` | generators.h2o: run consumer on each value the producer yields until the producer falls off (a `:gen-end` sentinel marks exhaustion) | — | cont/step | O(values · consumer) |

```forth yield
: nums 1 yield 2 yield ; ' nums 2 gen-take . cr
: countdown-gen | n | 3 to n begin n 0 > while n yield n 1- to n repeat ;
' countdown-gen 3 gen-take . cr
: naturals | n | 0 to n begin n yield n 1+ to n again ;
' naturals 4 gen-take . cr
```
```output
[ 1 2 ]
[ 3 2 1 ]
[ 0 1 2 3 ]
```

```forth start-generator
[: 5 yield drop :] start-generator drop . cr
```
```output
5
```

```forth gen-take
: odds 1 yield 3 yield 5 yield ; ' odds 3 gen-take . cr
```
```output
[ 1 3 5 ]
```

```forth gen-each
: pair-gen 10 yield 20 yield ; ' pair-gen [: . :] gen-each cr
```
```output
10 20
```

---

## Logic

Logic variables, unification, and committed choice, built on the trail and a `PROMPT_CHOICE` prompt. The primer behind these words — unknowns and substitutions, unification, the trail, search, reification, and the fact database as a worked application — is docs/logic.md. A logic var is always created explicitly: `lvar` pushes a fresh one, `lvar to x` names a persistent global (`to` auto-creates the global at the top level), and a `?` prefix in a locals list (`| ?x |`) declares a fresh per-call local. Capitalizing logic-var names (`X`, `Hs`) is stylistic convention, not syntax — case carries no meaning. `unify` records every binding on the trail; a `unify` mismatch or an explicit `fail` backtracks to the nearest `amb`. Lists are cons pairs (see Pairs): `[( H T )]` is the `[H|T]` head/tail pattern under `unify`. To keep a result past backtracking, snapshot it with `copy` (fresh vars) or `reify` (canonical `:_N`). A logic var prints by the name of a variable that holds it — `?x` while free (the `?` marks the hole, echoing the `| ?x |` declaration form), `x=value` once bound — or `_N` when anonymous; an anonymous bound var prints its value.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `lvar` | `( -- v )` | Push a fresh, unbound logic variable | 2 | `1 lvar` | O(1) |
| `_` | `( -- wild )` | The anonymous wildcard — unifies with anything, binds nothing, allocates nothing (a constant, not a fresh var) | 2 | none | O(1) |
| `unify` | `( a b -- term )` | Unify a and b, binding logic vars (recorded on the trail) so the two match, then leave the dereffed left term. Atoms by value; pairs head then tail; arrays element-wise; frames as open records — shared keys must unify, extra keys on either side allowed. A `_` on either side matches anything and binds nothing. On a mismatch, `fail`s. | n | none | O(n) |
| `~` | `( a b -- term )` | C primitive alias of `unify`, so `cons ~` fuses to `(cons~)` | n | none | O(n) |
| `deref` | `( v -- val )` | Follow a logic var's binding chain to the first non-variable value (v itself if unbound). Shallow — a returned structure still has bound vars inside; for a fully resolved snapshot use `reify` or `copy` | d | none | O(d) |
| `?` | `( v -- val )` | logic.h2o: `deref` (inlined) | d | none | O(d) |
| `amb` | `( xt1 xt2 -- … )` | Run xt1; if it fails (a `unify` mismatch or `fail`), roll its bindings back through the trail and run xt2. Commits to the first branch that succeeds. | xt1 | none | O(xt1 + xt2) |
| `fail` | `( -- )` | Backtrack to the nearest enclosing `amb`, failing the current branch; with no enclosing `amb`, an error | 1 | none | O(L) |
| `choose` | `( list cont -- )` | logic.h2o: run cont with each element of a cons list in turn, committing to the first for which it succeeds; `fail` if none do (n-way `amb` over a list) | n·cont | none | O(n·cont) |
| `matches?` | `( a b -- flag )` | Non-destructive unify test: mark the trail, unify a and b, roll the trail back, push whether they unified. Leaves no bindings and never backtracks (so it composes in straight-line code, unlike `unify`) | n | none | O(n) |

```forth lvar
lvar dup 5 ~ drop ? . cr
```
```output
5
```

```forth _
[ 1 2 ] [ _ 2 ] matches? . cr
```
```output
1
```

```forth unify
lvar to Q [ 1 Q ] [ 1 2 ] unify . cr
```
```output
[ 1 Q=2 ]
```

```forth ~
[ 1 2 ] [ 1 2 ] ~ . cr
```
```output
[ 1 2 ]
```

```forth deref
lvar dup 7 ~ drop deref . cr
```
```output
7
```

```forth ?
lvar dup 9 ~ drop ? . cr
```
```output
9
```

```forth amb
[: 1 :] [: 2 :] amb . cr
```
```output
1
```

```forth fail
[: fail :] [: "fallback" :] amb . cr
```
```output
fallback
```

```forth choose
[( 1 2 3 null )] [: dup 2 < if fail then . cr :] choose
```
```output
2
```

```forth matches?
[ 1 _ ] [ 1 5 ] matches? . cr
```
```output
1
```

---

## Fact database

A relational store built entirely from frames and sets — no new type. A **relation** is `{ :rows <set of rows> :index <index> }`; a **row** is a frame keyed by column name; a **database**, if you want several relations, is just a frame keyed by relation name (`db :father @` reaches one — no words of its own). The same shape describes a SQLite query result, so a fetched table and a hand-built relation are interchangeable (see the SQLite section below).

Rows live in a set, so an identical row asserted twice dedups to one (a relation is a set of tuples). A caller-supplied `:id` column keeps otherwise-identical rows distinct. Indexed columns are declared at creation and must be symbol-valued; `:index` maps each to a `{ value → <rows> }` frame whose buckets share the row frames in `:rows`.

`query` is unification: a pattern frame unifies against rows as an open record — shared keys must match, a logic var matches anything (projection), extra columns are ignored — which is SQL selection and projection. It collects every match (returning an array of the matching rows) by testing each candidate with `matches?` and rolling bindings back, so the pattern is left unbound. Candidates come from the index when the pattern grounds an indexed column to a symbol (intersecting buckets across several such columns, empty when a value was never asserted); otherwise it scans `:rows`.

The relation/query machinery is built from logic.h2o helpers (`bucket-of`, `candidates`, `covering?`, `smallest-set`, `tsv-keys`, `retract-row`, `update-row!`) that are internal implementation details and are not listed individually.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `relation` | `( [cols] -- rel )` | New empty relation; `cols` is an array of column symbols to index | k | frames + sets | O(k) |
| `assert` | `( rel row -- rel )` | Add row to `:rows` and to each indexed column's bucket; identical row is a no-op. Mutates rel in place, returns it | k + n | reallocs | O(n) |
| `retract` | `( rel pattern -- rel )` | Remove every row matching pattern from `:rows` and all buckets. Mutates rel, returns it | matches·(k+n) | `1a` | O(matches·n) |
| `query` | `( rel pattern -- [rows] )` | Array of rows matching pattern; uses an index when the pattern grounds an indexed column, else scans. When every constraint is a ground indexed column the narrowed bucket *is* the answer, so the per-row `matches?` is skipped (covering query) | candidates·n | `1a` + set ops | O(candidates·n) |
| `count-matches` | `( rel pattern -- n )` | How many rows match; for a covering query this is the bucket's `size` with no scan, otherwise `query size` | — | (covering: none) | O(candidates) |
| `inner-join` | `( driver probed col -- [rows] )` | Inner join: each `driver` row merged (`probed` columns win collisions) with each `probed` row sharing `col`'s value; `probed` must index `col` | — | `1a` | O(driver·log probed) |
| `bulk-load` | `( rel rows-array -- rel )` | Load all rows at once: builds `:rows` (a deduped set) and each declared column's index, instead of row-by-row | — | sets + frame | O(n log n) |
| `load-bag` | `( rel rows-array -- rel )` | Like `bulk-load`, but `:rows` stays a **bag** (the array, duplicates kept) rather than a deduped set; only `:index` is built | n | frame + sets | O(n) |
| `create-index` | `( rel cols -- rel )` | Index a relation on the symbol columns `cols`: intern each indexed column's value to a symbol (so it keys the bucket and matches a `{ :col :val }` pattern), then `load-bag` into a `cols`-indexed relation. Other columns keep their type; `:rows` stays a bag. The explicit bridge from a `db-query` result to an indexed relation | n | frame + sets | O(n) |

These are logic.h2o over the C primitives `matches?`, `set-add!`, `set-remove!`, `array>set`, and `group-by`, plus the `symbol?` type predicate. Building a relation with one `assert` per row is super-linear (each insert shifts the sorted `:rows` set, and per-value frames grow the same way); `bulk-load` avoids that with `array>set` for `:rows` (one sort) and a one-pass `group-by` per indexed column (which buckets by the interned symbol value, then sorts each small bucket — no global sort). `load-bag` and `create-index` skip the `:rows` dedup entirely, keeping a bag; `create-index` also interns the indexed columns to symbols. Candidate narrowing drives from the smallest matching bucket.

```forth relation
[ :name ] relation { :name :ann :age 34 } assert { :name :ann } query first frame>array . cr
```
```output
[ :name :ann :age 34 ]
```

```forth assert
[ :name ] relation { :name :ann :age 34 } assert { :name :ann } query first frame>array . cr
```
```output
[ :name :ann :age 34 ]
```

```forth retract
[ ] relation { :x 1 } assert { :x 1 } retract { } query size . cr
```
```output
0
```

```forth query
[ :name ] relation { :name :ann :age 34 } assert { :name :ann } query first frame>array . cr
```
```output
[ :name :ann :age 34 ]
```

```forth count-matches
[ :t ] relation { :t :a :id 1 } assert { :t :a :id 2 } assert { :t :a } count-matches . cr
```
```output
2
```

```forth inner-join
[ :dept ] relation { :dept :eng :floor 3 } assert to floors
[ :dept ] relation { :name :bo :dept :eng } assert floors :dept inner-join first frame>array . cr
```
```output
[ :name :bo :dept :eng :floor 3 ]
```

```forth bulk-load
[ :k ] relation [ { :k :a } { :k :b } { :k :a } ] bulk-load { :k :a } count-matches . cr
```
```output
1
```

```forth load-bag
[ :k ] relation [ { :k :a :n 1 } { :k :a :n 2 } { :k :b :n 3 } ] load-bag { :k :a } count-matches . cr
```
```output
2
```

```forth create-index
[ ] relation [ { :city "nyc" :id 1 } { :city "sf" :id 2 } { :city "nyc" :id 3 } ] load-bag [ :city ] create-index { :city :nyc } count-matches . cr
```
```output
2
```

---

## Superwords (compile-time fusion) ⚠

Immediate compiler words usable only inside a definition. They detect a preceding variable-load and emit a single fused instruction that reads the variable's dict slot directly. All read `.number` without a tag check. Followed by `to dest`, they fuse further into a store variant that writes the result straight to the destination slot.

| Word | Syntax | Behavior |
|------|--------|----------|
| `vvf+` | `vvf+ a b` | Load variables a and b, add, push the result |
| `vvf-` | `vvf- a b` | Load variables a and b, subtract (a−b), push the result |
| `vvf*` | `vvf* a b` | Load variables a and b, multiply, push the result |
| `vvf/` | `vvf/ a b` | Load variables a and b, divide (a/b), push the result |
| `vf+` | `vf+ a` | Add variable a to the stack top, in place |
| `vf-` | `vf- a` | Subtract variable a from the stack top, in place |
| `vf*` | `vf* a` | Multiply the stack top by variable a, in place |
| `vf/` | `vf/ a` | Divide the stack top by variable a, in place |
| `vfsq` | `vfsq a` | Square variable a, push the result |
| `vfneg` | `vfneg a` | Negate variable a, push the result |
| `vfabs` | `vfabs a` | Absolute value of variable a, push the result |
| `vfsqrt` | `vfsqrt a` | Square root of variable a, push the result |
| `vfexp` | `vfexp a` | eᵃ of variable a, push the result |
| `vflog` | `vflog a` | base-10 log of variable a, push the result |
| `vfsin` | `vfsin a` | sine of variable a, push the result |
| `vfcos` | `vfcos a` | cosine of variable a, push the result |
| `vftan` | `vftan a` | tangent of variable a, push the result |
| `vftanh` | `vftanh a` | hyperbolic tangent of variable a, push the result |
| `vvf*+` | `vvf*+ b c` | `( t -- t*b+c )`, reading variables b and c |
| `vvf*-` | `vvf*- b c` | `( t -- c-t*b )`, reading variables b and c |

These are normally produced by the compiler's auto-fuser rather than typed by hand; `see-compiled` reveals them. The fuser triggers only on the unsafe f-words (`f+`, `fsqrt`, `fexp`, …) — the polymorphic names (`+`, `sqrt`, `exp`) never fuse, so their tag dispatch (matrix, quantity) is never bypassed.

The auto-fuser also collapses a comparison immediately before a branch — `= if`, `> while`, `0= until` — into a single compare-and-branch instruction (shown by `see-compiled` as `(=0branch)`, `(>0branch)`, and the like). These are internal and never typed; the source stays the plain comparison followed by the control word.

Stack reads fuse too, which is what makes a body reading parked values cost the same as one reading locals. A literal depth before `pick` becomes one op (`(pick.n) 2`); a `pick` immediately before an unsafe float op becomes a depth-addressed arithmetic op that reads the slot in place (`2 pick f+` → `(f+.d) 2`, and `over f+` → `(f+.d) 1`); and two picks feeding `@i` become a single indexed read (`3 pick 2 pick @i` → `(@i.dd) 3 1`, the index operand adjusted for the copy the first pick would have pushed). `see-compiled` shows all three, and they are what a `times` / `i-times` / `fold-times` body compiles to when it reads values a caller parked below the combinator's operands.

Word-locals fuse the same way, which is what makes a locals-based numeric loop compile tightly. A float op over two locals, or a local and a float literal, becomes one instruction that reads the slots directly (`(ll*0)`, `(ll.lit+0)`); a following `to name` fuses into it, so `zr zr f* to zr2` is a single instruction that reads two slots and writes a third (`(ll*0!)`). An op taking one operand from the stack and one from a local fuses with its store the same way — `ci f+ to zi` is one instruction (`(sl+!0)`) — and when the destination is also the operand, `total x f+ to total` becomes an accumulate (`(acc+0)`). `++ name` / `f++ name` are the one-instruction forms of incrementing a local, so `iter 1+ to iter` written as `f++ iter` compiles to `(local f+!0)`. Sources are read before the destination is written, so a slot may be both.

```forth vvf+
variable a 3 to a variable b 4 to b
: sum-ab vvf+ a b ; sum-ab . cr
```
```output
7
```

```forth vvf-
variable a 3 to a variable b 4 to b
: diff-ab vvf- a b ; diff-ab . cr
```
```output
-1
```

```forth vvf*
variable a 3 to a variable b 4 to b
: prod-ab vvf* a b ; prod-ab . cr
```
```output
12
```

```forth vvf/
variable a 3 to a variable b 4 to b
: quot-ab vvf/ a b ; quot-ab . cr
```
```output
0.75
```

```forth vf+
variable a 3 to a
: plus-a vf+ a ; 10 plus-a . cr
```
```output
13
```

```forth vf-
variable a 3 to a
: minus-a vf- a ; 10 minus-a . cr
```
```output
7
```

```forth vf*
variable a 3 to a
: times-a vf* a ; 10 times-a . cr
```
```output
30
```

```forth vf/
variable a 3 to a
: over-a vf/ a ; 12 over-a . cr
```
```output
4
```

```forth vfsq
variable a 3 to a
: sq-a vfsq a ; sq-a . cr
```
```output
9
```

```forth vfneg
variable a 3 to a
: neg-a vfneg a ; neg-a . cr
```
```output
-3
```

```forth vfabs
variable m -5 to m
: abs-m vfabs m ; abs-m . cr
```
```output
5
```

```forth vfsqrt
variable n 9 to n
: root-n vfsqrt n ; root-n . cr
```
```output
3
```

```forth vfexp
variable z 0 to z
: exp-z vfexp z ; exp-z . cr
```
```output
1
```

```forth vflog
variable h 100 to h
: log-h vflog h ; log-h . cr
```
```output
2
```

```forth vfsin
variable z 0 to z
: sin-z vfsin z ; sin-z . cr
```
```output
0
```

```forth vfcos
variable z 0 to z
: cos-z vfcos z ; cos-z . cr
```
```output
1
```

```forth vftan
variable z 0 to z
: tan-z vftan z ; tan-z . cr
```
```output
0
```

```forth vftanh
variable z 0 to z
: tanh-z vftanh z ; tanh-z . cr
```
```output
0
```

```forth vvf*+
variable b 4 to b variable c 10 to c
: fma-bc vvf*+ b c ; 2 fma-bc . cr
```
```output
18
```

```forth vvf*-
variable b 4 to b variable c 10 to c
: fms-bc vvf*- b c ; 2 fms-bc . cr
```
```output
2
```

---

## REPL and introspection

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `words` | `( -- )` | List all non-internal words in aligned columns, grouped by section, alphabetical within a group: words defined this session first, then words loaded from a library file, then the reference sections | dict scan | none | O(\|dict\| log \|dict\|) |
| `variables` | `( -- arr )` | core.h2o: one `{ :name :value :type }` frame per global (`variable`-declared or `to`-auto-created), oldest first — the name symbol, the live value (shared reference for collections), and its `type-of` symbol. `variables [: :name @ :] map` is the name list; `variables frames>dataset head` a table | dict scan | `1a` + one frame per global | O(\|dict\|) |
| `vars` | `( -- )` | repl.h2o: pretty-print every global, one `variables` frame per block (`variables ' print each`) | dict scan + print | `1a` + frames | O(\|dict\|) |
| `water` | `( -- )` | Print the water logo and the interpreter version | print | none | O(1) |
| `apropos` | `( str -- )` | Print every word whose name or reference summary contains s (case-insensitive): name, stack effect, summary per line; session-defined words match by name | table scan | none | O(entries) |
| `see` | `( xt -- )` | Print a word's source (`: name … ;`), a quotation's `[: … :]` text from its recorded span, or `variable`/`symbol`/primitive form; a curried token prints its bound values, then its target | dict scan | none | O(\|dict\|) |
| `see>string` | `( xt -- str )` | The text `see` would print, returned as a string (trailing newline stripped) | dict scan | `1o` | O(\|dict\|) |
| `see-compiled` | `( xt -- )` | Disassemble a colon definition's compiled cells; a curried token prints its bound values, then disassembles its target | body scan | none | O(body) |
| `see-compiled>string` | `( xt -- str )` | The text `see-compiled` would print, returned as a string (trailing newline stripped) | body scan | `1o` | O(body) |
| `see-tree` | `( xt -- )` | Like `see-compiled`, but each colon-word call is expanded inline, indented two spaces, recursively down to primitives; recursive calls print as `name ...` | body scan | none | O(expanded body) |
| `see-tree>string` | `( xt -- str )` | The text `see-tree` would print, returned as a string (trailing newline stripped) | body scan | `1o` | O(expanded body) |
| `man` | `( xt -- fr )` | Frame of a word's reference entry (`:word :effect :summary`, plus `:ops :alloc :order` for runtime words); a unit word synthesizes its entry from the unit's definition (`unit: m × 1000`); `T_NONE` if undocumented | dict scan + log n | `1o` + strings | O(\|dict\|) |
| `help` | `( "name" -- )` | repl.h2o: parse the next word and print its `man` frame; bare `help` (no name on the line) prints a starter cheat sheet, and an unknown name prints `unknown word: <name>` without erroring. Distinguishes the three cases by `catch`ing `lookup`'s message | dict scan + log n | `1o` + strings + print | O(\|dict\|) |
| `gc` | `( -- )` | Force a mark-sweep now | walks stacks + dict + roots, frees unmarked | none | O(objects + dict) |
| `alloc-stats` | `( -- )` | Print and reset the allocation counters since the last call (`lvars=… arrays=…`) | 2 | none | O(1) |
| `bye` | `( -- )` | `exit(0)` | — | — | — |
| `now` | `( -- f )` | `CLOCK_MONOTONIC` seconds as a float | 1 | none | O(1) |
| `sleep` | `( seconds -- )` | Block for the given float seconds (sub-second supported); `nanosleep` | blocks | none | O(1) |
| `timed` | `( xt -- … )` | Run xt, print its elapsed `now` (`CLOCK_MONOTONIC`) seconds, then pass through whatever it left on the stack | 2 + xt + print | none | O(xt) |

```forth-noexec words
words
```
```output
Stack manipulation:
  -rot      2drop     2dup      clear     depth     drop      dup
  identity  nip       over      pick      roll      rot       swap
Arithmetic:
...
```

```forth variables
42 to answer-var variables [: :name @ :] map dup size 1- @i . cr
```
```output
:answer-var
```

```forth-noexec vars
7 to speed vars
```
```output
{
  :name :speed
  :value 7
  :type :float
}
```

```forth-noexec water
water
```
```output
                                          water 0.26.0
                              https://github.com/free-variation/water
```

```forth-noexec apropos
"kendall" apropos
```
```output
cor              ( xs ys -- fr )          statistics.h2o: correlation-kendall with a 500-replicate bootstrap CI — ' correlation-kendall 500 correlate-with (inlined)
correlation-kendall ( xs ys -- f )           Kendall tau-b: concordant minus discordant pairs over sqrt of tie-corrected pair counts, via one (x,y) sort and a merge-sort exchange count; NaN when all x or all y are tied; errors on length mismatch or fewer than 2 elements
```

```forth see
: sq-see dup * ; ' sq-see see
```
```output
: sq-see dup * ;
```

```forth see>string
: sq-see2 dup * ; ' sq-see2 see>string print cr
```
```output
: sq-see2 dup * ;
```

```forth see-compiled
: sc-demo 1.5 2.5 f+ ; ' sc-demo see-compiled
```
```output
: sc-demo   \ 5 cells
 0: (lit) 1.5
 2: (lf+) 2.5
 4: exit
;
```

```forth see-compiled>string
: sc-demo2 1.5 2.5 f+ ; ' sc-demo2 see-compiled>string print cr
```
```output
: sc-demo2   \ 5 cells
 0: (lit) 1.5
 2: (lf+) 2.5
 4: exit
;
```

```forth see-tree
: st-inner 1 ; : st-outer st-inner 2 * ; ' st-outer see-tree
```
```output
: st-outer
  0: st-inner:
    0: (lit) 1
    2: exit
  2: (lit) 2
  4: *
  5: exit
;
```

```forth see-tree>string
: st-inner 1 ; : st-outer2 st-inner 3 * ; ' st-outer2 see-tree>string print cr
```
```output
: st-outer2
  0: st-inner:
    0: (lit) 1
    2: exit
  2: (lit) 3
  4: *
  5: exit
;
```

```forth man
' dup man :effect @ . cr
```
```output
( a -- a a )
```

```forth help
help nip
```
```output
nip ( a b -- b )
  Drop the second item, keeping the top — one op, not swap drop
  ops 1, alloc none, O(1)

  > 1 2 nip . cr
  2
```

```forth gc
gc "collected" . cr
```
```output
collected
```

```forth-noexec alloc-stats
alloc-stats
```
```output
lvars=0 arrays=0
```

```forth-noexec bye
bye
```
```output
```

```forth-noexec now
now . cr
```
```output
1.48012e+06
```

```forth sleep
0 sleep "woke" . cr
```
```output
woke
```

```forth-noexec timed
[: [ 1 2 3 ] ' 1+ map :] timed . cr
```
```output
2.1e-06
[ 2 3 4 ]
```

---

## Persistence

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `load` | `( str -- )` | Run a source file as if typed; record it for `reload`. Resolves the path as given (relative to the current directory, or absolute); if that open fails, retries relative to the directory of the file that ran the `load`. An error raised while loading is prefixed `file:line: ` (the line of the failing token); a nested `load` locates to the innermost file | file read + run | input buffer | O(file) |
| `load-library` | `( name -- )` | core.h2o: `load` `lib/<name>` from beside the water binary (`binary-dir`), so `"plot" load-library` works from any cwd; a name without `.h2o` gains it | file read + run | input buffer | O(file) |
| `reload` | `( -- )` | Truncate user state, re-run every loaded file in order | forget + N loads | — | O(Σ files) |
| `save` | `( str -- )` | Write all user words as re-loadable `.h2o` source | dict scan + write | file I/O | O(\|user dict\|) |
| `save-image` | `( str -- )` | Binary snapshot of full state (dict, objects, stacks, continuations) | serialize all | file I/O | O(objects + dict) |
| `load-image` | `( str -- )` | Restore a binary snapshot, replacing current state | deserialize all | reallocates all objects | O(objects) |

```forth load
": loaded-word 11 ;" "/tmp/docs-load.h2o" write-file "/tmp/docs-load.h2o" load loaded-word . cr
```
```output
11
```

```forth load-library
"plot" load-library ' scatter xt? . cr
```
```output
1
```

```forth save
: keep-me 5 ; "/tmp/docs-save.h2o" save "/tmp/docs-save.h2o" read-file ": keep-me" has? . cr
```
```output
1
```

```forth save-image
"/tmp/docs-image.img" save-image "/tmp/docs-image.img" file-exists? . cr
```
```output
1
```

```forth-noexec load-image
"/tmp/docs-image.img" load-image
```
```output
```

```forth-noexec reload
reload
```
```output
```

---

## Files and environment

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `read-file` | `( path -- str )` | Read a whole file as one string (byte-safe); errors if it can't be opened | file read | `1o` + buffer | O(file) |
| `write-file` | `( str path -- )` | Create or truncate the file, then write the string's bytes | file write | none | O(\|s\|) |
| `append-file` | `( str path -- )` | Open in append mode, write the string's bytes | file write | none | O(\|s\|) |
| `file-exists?` | `( path -- bool )` | Whether the path exists (`access` with `F_OK`); follows symlinks, tests any file type, not just regular files | 1 | none | O(1) |
| `env` | `( name -- val )` | Environment variable as a string, or the none value if unset (so set-empty `""` and unset stay distinct) | 1 | `1o` on hit | O(\|val\|) |
| `env!` | `( name value -- )` | Set an environment variable (overwriting); process-wide, so subsequent `start-process` children inherit it | 1 | none | O(1) |
| `cwd` | `( -- path )` | The interpreter's current working directory as a string (`getcwd`) | 1 | `1o` | O(\|path\|) |
| `binary-dir` | `( -- str )` | The directory holding the running water binary, symlinks resolved (`realpath`), so an installation's resources are reachable from any cwd; errors on the wasm build (no executable path) | 1 | `1o` | O(\|path\|) |
| `cd` | `( path -- )` | Change the interpreter's working directory (`chdir`); process-wide, so it moves the base for relative file I/O and is inherited by subsequent `start-process` children | 1 | none | O(1) |
| `find-executable` | `( name -- path\|none )` | `io.h2o`: the absolute path of `name` on `$PATH` (first directory holding it), or the none value if unset or not found; a name containing `/` is not special-cased (it just won't match a bare `PATH` entry) | split + probe | `1o` per candidate | O(dirs) |

```forth read-file
"hello" "/tmp/docs-file.txt" write-file "/tmp/docs-file.txt" read-file . cr
```
```output
hello
```

```forth write-file
"hello" "/tmp/docs-file.txt" write-file "/tmp/docs-file.txt" read-file . cr
```
```output
hello
```

```forth append-file
"a" "/tmp/docs-app.txt" write-file "b" "/tmp/docs-app.txt" append-file "/tmp/docs-app.txt" read-file . cr
```
```output
ab
```

```forth file-exists?
"/tmp" file-exists? . "/no/such/path" file-exists? . cr
```
```output
1 0
```

```forth env
"DOCS_VAR" "42" env! "DOCS_VAR" env . cr
"NO_SUCH_VAR_XYZ" env none? . cr
```
```output
42
1
```

```forth env!
"DOCS_VAR" "42" env! "DOCS_VAR" env . cr
```
```output
42
```

```forth cwd
cwd file-exists? . cr
```
```output
1
```

```forth binary-dir
binary-dir file-exists? . cr
```
```output
1
```

```forth cd
cwd "/tmp" cd cwd "tmp" has? . cd cr
```
```output
1
```

```forth find-executable
"sh" find-executable none? 0= . cr
```
```output
1
```

---

## Subprocesses and streams

A stream (`T_STREAM`) wraps an OS file descriptor — a pipe to a child process. `start-process` launches a program directly from an argv array (no shell, so no quoting or injection surface) and returns a frame `{ :pid :in :out :err }` whose `:in`/`:out`/`:err` are streams. The lifecycle is: `write` input → `close` `:in` (sends EOF) → `read` the output → `wait`. `SIGPIPE` is ignored process-wide, so a `write` to a child that has exited returns an error rather than killing the interpreter. Bytes are raw and length-counted, so streams are binary-safe.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `start-process` | `( argv -- proc )` | fork/exec `argv[0]` with `argv` as its arguments; return `{ :pid :in :out :err }` (the three streams are `T_STREAM`) | fork + 3 pipes | `1o` frame + 3 streams | O(argc) |
| `run-result` | `( argv -- frame )` | subprocess.h2o: run `argv` to completion and return `{ :out :err :status }`, closing the streams and reaping the child | fork + drain | `1fr` + output strings | O(output) |
| `write` | `( str stream -- )` | Write the string's bytes to the stream; loops over partial writes, retries `EINTR` | write syscalls | none | O(\|s\|) |
| `read` | `( stream -- str )` | Read the stream to EOF into one string | read syscalls | `1o` + buffer growth | O(bytes) |
| `close` | `( stream -- )` | Close the fd; closing a child's `:in` sends it EOF | 1 syscall | none | O(1) |
| `stdin` | `( -- stream )` | Standard input as a `T_STREAM` over fd 0; `stdin read` slurps it. (Conflicts with the REPL reading its own program from stdin — for file-loaded programs.) | 1 | none | O(1) |
| `stdout` | `( -- stream )` | Standard output as a `T_STREAM` over fd 1; `s stdout write` emits | 1 | none | O(1) |
| `stderr` | `( -- stream )` | Standard error as a `T_STREAM` over fd 2; composes with `write`/`close` like any stream | 1 | none | O(1) |
| `wait` | `( pid -- status )` | Block until the child exits; return its exit code, or `128 + signo` if it was killed by a signal | blocks | none | O(1) |
| `stop` | `( pid -- status )` | `SIGKILL` the child then reap it (137 = 128+9, or its code if it had already exited) | 2 syscalls | none | O(1) |
| `running?` | `( pid -- bool )` | Non-blocking liveness via `waitid`+`WNOHANG`+`WNOWAIT`; true while running, false once exited. Non-reaping, so a later `wait` still returns the status | 1 syscall | none | O(1) |
| `open-app-window` | `( path -- )` | browser.h2o: open `path` in a detached browser application window (Chromium `--app`), falling back to the system `open` / `xdg-open`; the mechanism behind `show-figure` | fork | none | O(1) |
| `run` | `( str -- proc )` | subprocess.h2o: split a command string on runs of spaces and `start-process` it (`" +" split start-process`) | split + fork | `1a` + `1o` frame + 3 streams | O(\|s\| + argc) |
| `write-in` | `( str proc -- )` | subprocess.h2o: write the string to the child's `:in` stream | write syscalls | none | O(\|s\|) |
| `read-out` | `( proc -- str )` | subprocess.h2o: read the child's `:out` stream to EOF | read syscalls | `1o` + buffer growth | O(bytes) |
| `read-err` | `( proc -- str )` | subprocess.h2o: read the child's `:err` stream to EOF | read syscalls | `1o` + buffer growth | O(bytes) |
| `end-process` | `( proc -- )` | subprocess.h2o: the teardown mirror of `start-process` — close `:in`/`:out`/`:err` and `wait` `:pid` (graceful, blocks until exit) | 3 closes + wait | none | O(1) |
| `parallel-run` | `( commands width -- results )` | subprocess.h2o: run each argv array in `commands` as a subprocess, at most `width` at once; collect `{ :out :err :status }` per command in input order, refilling a slot as each child finishes | fork per command + poll | `1a` + per-child frames/streams | O(critical path) |

Line access is `read "\n" split`.

```forth start-process
[ "echo" "hi" ] start-process dup read-out trim . :pid @ wait . cr
```
```output
hi 0
```

```forth run-result
[ "echo" "ok" ] run-result :out @ trim . cr
```
```output
ok
```

```forth write
[ "cat" ] start-process dup :in @ "ping" swap write dup :in @ close dup read-out trim . :pid @ wait drop cr
```
```output
ping
```

```forth read
[ "echo" "data" ] start-process :out @ read trim . cr
```
```output
data
```

```forth close
[ "cat" ] start-process dup :in @ "ping" swap write dup :in @ close dup read-out trim . :pid @ wait drop cr
```
```output
ping
```

```forth-noexec stdin
stdin read size . cr
```
```output
42
```

```forth stdout
"direct" stdout write cr
```
```output
direct
```

```forth stderr
stderr stream? . cr
```
```output
1
```

```forth wait
[ "true" ] start-process :pid @ wait . cr
```
```output
0
```

```forth stop
[ "sleep" "5" ] start-process :pid @ stop . cr
```
```output
137
```

```forth running?
[ "true" ] start-process :pid @ dup wait drop running? . cr
```
```output
0
```

```forth-noexec open-app-window
"figures/plot.svg" open-app-window
```
```output
```

```forth run
"echo hi" run read-out trim . cr
```
```output
hi
```

```forth write-in
"cat" run dup "ping" swap write-in dup :in @ close dup read-out trim . end-process cr
```
```output
ping
```

```forth read-out
"echo hi" run read-out trim . cr
```
```output
hi
```

```forth read-err
[ "sh" "-c" "echo oops >&2" ] start-process read-err trim . cr
```
```output
oops
```

```forth end-process
"cat" run dup "ping" swap write-in dup :in @ close dup read-out trim . end-process cr
```
```output
ping
```

```forth parallel-run
[ [ "echo" "a" ] [ "echo" "b" ] ] 2 parallel-run [: :out @ trim . :] each cr
```
```output
a b
```

---

## SQLite

Embedded relational storage via the vendored SQLite amalgamation, built into the binary. A database is a `T_DB` value — an inline handle into a per-interpreter registry of open connections, like a stream. `db-exec` and `db-query` take a `params` array bound positionally to the statement's `?` placeholders (`[ ]` for none): a float binds as a double, a string or symbol as text, `null` as NULL, anything else errors — so string parameters need no hand-escaping. A `db-query` result is a fact-database relation (see Fact database), so it drops straight into `query` / `inner-join` and is indexed with `create-index`. `n` = rows returned, `c` = columns.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `db-open` | `( path -- db )` | Open (creating if absent) the database file at `path` and push a handle; `":memory:"` is a private in-memory database. Errors if it can't be opened | open | 1 connection (not GC'd) | O(1)+ |
| `db-close` | `( db -- )` | Close the connection and free its registry slot. Idempotent — closing an already-closed handle is a no-op. A handle that is dropped without closing leaks the connection until process exit | 1 syscall | none | O(1) |
| `db-exec` | `( db statement params -- n )` | Bind `params` to the statement's `?` placeholders and run it with no result set (INSERT / UPDATE / DELETE / CREATE / …); return the affected-row count as a float (0 for DDL). One statement per call. On a bad statement, errors with SQLite's message | per statement | none | O(statement) |
| `db-query` | `( db query params -- rel )` | Bind `params` to the query's `?` placeholders and run it; return an index-less relation `{ :rows <array of row frames> :index { } }`. Each row is a frame keyed by column-name symbols, with INTEGER/REAL → float, TEXT → string, NULL → `null`, BLOB → string of raw bytes. `:rows` is a **bag** — duplicates kept, in result order. On a bad query, errors with SQLite's message | n·c | `1o` relation + `1a(n)` + `1o`/row + a string per text/blob cell | O(n·c) |
| `db-query>dataset` | `( db query params -- dataset )` | database.h2o: the same query, returned as a column-oriented dataset with **typed columns**: a column whose every cell is numeric or NULL becomes an n×1 vector (NULL → NaN), a column declared DATE/DATETIME/TIMESTAMP becomes a vector of instants in `s` (numeric cells read as epoch seconds, text cells parsed as ISO Z), and anything else stays an array with `none` for NULL. An empty column declared numeric stays an empty vector, so the type survives an empty result; a repeated column name keeps its last occurrence. The C primitive `(db-query>dataset)` returns the raw columns plus each column's declared type from the same prepared statement | n·c | `1o` frame + `1a`/column + `1m` per numeric column + a string per text cell | O(n·c) |
| `tsv>db` | `( tsv-path db table -- info )` | database.h2o: import a TSV file into a new table. The header row names the columns (identifiers quoted, so any header text works); a column whose every non-empty cell is numeric is REAL, else TEXT; empty cells insert as NULL; all rows go in one transaction. `info` is `{ :n-rows N :columns [ … ] }` — a `:real` column carries `{ :name :type :summary }` with a `:summary` from `summary`, a `:text` column `{ :name :type :distinct }` with `COUNT(DISTINCT)` (NULLs uncounted). Errors before creating anything on a missing or ragged file; an existing table errors on the CREATE, leaving it untouched | r·c | rows + dataset + `1s`/statement | O(r·c) |

Using a closed handle errors (`database is closed`). Do selection, projection, and joins in the SQL itself; Water materializes the result. Indexing a result is a separate, explicit step — `create-index` (see Fact database) — because it interns the indexed columns to symbols, which only makes sense for low-cardinality categorical columns you choose.

```forth db-open
":memory:" db-open db? . cr
```
```output
1
```

```forth db-close
":memory:" db-open dup db-close db-close "closed twice" . cr
```
```output
closed twice
```

```forth db-exec
":memory:" db-open dup "create table t(x)" [ ] db-exec . dup "insert into t values (?)" [ 5 ] db-exec . db-close cr
```
```output
0 1
```

```forth db-query
":memory:" db-open dup "select 1 as n" [ ] db-query :rows @ first :n @ . db-close cr
```
```output
1
```

```forth db-query>dataset
":memory:" db-open dup "select 2 as v" [ ] db-query>dataset :v @ matrix>array . db-close cr
```
```output
[ 2 ]
```

```forth tsv>db
[ [ "x" ] [ 1 ] [ 2 ] ] "/tmp/docs-db.tsv" save-tsv ":memory:" db-open dup "/tmp/docs-db.tsv" swap "t" tsv>db :n-rows @ . db-close cr
```
```output
2
```

---

## Foreign function interface

Call C functions in any shared library at runtime via `libdl` + `libffi` — no per-library glue. `ffi-open` loads a library; `ffi-function` / `ffi-variadic` resolve a symbol and define a Water word that marshals its arguments and result. Types are symbols: `:void :int :long :double :ptr :string` — Water floats marshal to/from C `int`/`long`/`double`, strings pass as `const char*` (a returned `char*` is copied into a Water string), and `:ptr` is an opaque C pointer held as a `T_PTR` handle (a registry index, since a 64-bit pointer doesn't fit a Val's 44-bit payload). FFI is unsafe: a wrong signature corrupts or crashes — argument *count* is checked, types are the caller's responsibility.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `ffi-open` | `( path -- lib )` | `dlopen` the library at `path` and push a `T_PTR` handle; `""` opens the running process itself (`dlopen(NULL)`) for already-linked symbols. Errors if not found | dlopen | 1 handle (not GC'd) | O(1) |
| `ffi-function` | `( lib symbol arg-types ret-type -- ) <name>` | Resolve `symbol` in `lib`, build a libffi call interface, and define the following word `<name>` to call it. `arg-types` is an array of type symbols, `ret-type` a single symbol. The interface is prepared once; calls are ~30–100 ns | dlsym + prep_cif | 1 binding | O(argc) |
| `matrix>pointer` | `( mat -- ptr )` | Intern the matrix's row-major element buffer and return a `T_PTR` handle to pass as a `:ptr` argument; no copy — aliases the live buffer (amortized intern) | 1 | none | O(1) |
| `segment>pointer` | `( seg -- ptr )` | Intern a segment's data buffer and return a `T_PTR` handle (no copy) | 1 | none | O(1) |
| `pointer-cell` | `( -- ptr )` | Allocate a zeroed pointer-sized cell and return a `T_PTR` handle to it, for use as a C out-parameter slot (`&out`) or a one-element handle array; a callee writes a pointer or integer into it, read back with `pointer-deref`. Freed by `ffi-free` | malloc | 1 cell (not GC'd) | O(1) |
| `pointer-deref` | `( ptr -- ptr' )` | Load the pointer stored at cell `ptr` (`*(void**)ptr`) and return it as a `T_PTR` handle — reads a handle a C call wrote into an out-parameter cell, or steps through a `T**` | 1 | 1 handle | O(1) |
| `pointer-long` | `( ptr -- n )` | Load the 64-bit integer stored at cell `ptr` (`*(int64_t*)ptr`) as a float — reads a `bst_ulong`/`long` out-value a C call wrote into a cell; errors above 2^53 (not float-exact) | 1 | none | O(1) |
| `pointer-string-at` | `( ptr i -- str )` | Copy the C string at index `i` of a `char**` at `ptr` (`ptr[i]`, NUL-terminated) into a Water string — reads one entry of a returned string array (e.g. `XGBoosterFeatureScore`'s feature names) | 1 + \|s\| | `1o` | O(\|s\|) |
| `pointer>address` | `( ptr -- n )` | The pointer's numeric address as a float, for embedding in an `__array_interface__` JSON string; errors if the address exceeds 2^53 (not float-exact — macOS arm64 user addresses are well under it) | 1 | none | O(1) |
| `floats>matrix` | `( ptr n -- mat )` | Copy `n` 32-bit floats from foreign memory at `ptr` into a fresh n×1 double matrix — the read-back for a C call that returns a `float const*` result buffer (e.g. predictions); errors if `n < 1` | n | `1m(n×1)` | O(n) |
| `ffi-variadic` | `( lib symbol arg-types ret-type n-fixed -- ) <name>` | Like `ffi-function` for a variadic C function: `n-fixed` leading arguments use the fixed convention, the rest the variadic one (`ffi_prep_cif_var`). Variadic argument types are fixed per binding, so declare one word per type combination (e.g. a `:string` `setopt` and a `:long` `setopt`) | dlsym + prep_cif_var | 1 binding | O(argc) |
| `ffi-free` | `( ptr -- )` | `free` a C buffer held as a `T_PTR` (e.g. from `malloc`) and clear its registry slot. Not for library handles | free | none | O(1) |

A defined FFI word pops its arguments, marshals each per the declared signature, calls through libffi, and pushes the marshalled return (`:void` pushes nothing). The build links `-lffi`; `dlopen` is in libSystem. Callbacks (C → Water), struct-by-value, varargs-per-call, and finer numeric types (`float`, unsigned) are not yet supported.

```forth ffi-open
"" ffi-open "cos" [ :double ] :double ffi-function c-cos 0 c-cos . cr
```
```output
1
```

```forth ffi-function
"" ffi-open "cos" [ :double ] :double ffi-function c-cos 0 c-cos . cr
```
```output
1
```

```forth matrix>pointer
[ 1 2 ] vector matrix>pointer ptr? . cr
```
```output
1
```

```forth pointer-cell
pointer-cell ptr? . cr
```
```output
1
```

```forth pointer-deref
pointer-cell pointer-deref ptr? . cr
```
```output
1
```

```forth pointer-long
pointer-cell pointer-long . cr
```
```output
0
```

```forth-noexec pointer-string-at
names-cell pointer-deref 0 pointer-string-at . cr
```
```output
age
```

```forth pointer>address
pointer-cell pointer>address 0 > . cr
```
```output
1
```

```forth-noexec floats>matrix
predictions-pointer 3 floats>matrix matrix>array . cr
```
```output
[ 0.12 0.87 0.44 ]
```

```forth-noexec ffi-variadic
"" ffi-open "printf" [ :string :double ] :int 1 ffi-variadic c-printf
```
```output
```

```forth ffi-free
pointer-cell ffi-free "freed" . cr
```
```output
freed
```

---

## Type tags

| Tag | Description |
|-----|-------------|
| `T_FLOAT` | 64-bit double; any bit pattern that is not a boxed NaN |
| `T_STRING` | heap object; NUL-terminated UTF-8 bytes, `len` = byte count |
| `T_SYMBOL` | symbol-pool offset; equal names share one offset |
| `T_ARRAY` | heap object; `Val[]` |
| `T_SET` | heap object; sorted `Val[]`, binary-search membership |
| `T_PAIR` | cons cell in the dense, GC'd pair table; `{head, tail}`. Lists are `null`-terminated chains |
| `T_FRAME` | heap object; parallel keys (`cell[]`) and values (`Val[]`) ordered by symbol id — interning order, not alphabetical — for binary-search lookup |
| `T_MATRIX` | heap object; r×c row-major `double[]` |
| `T_QUANTITY` | a magnitude (float or matrix) plus a unit id, in a pair-table slot `{magnitude, unit}`; see Dimensioned quantities. Dimensionless results collapse away, so a live quantity always carries a real unit |
| `T_XT` | execution token (dict index); first-class callable |
| `T_CURRIED` | heap object; a curried token — `items[0]` is the target xt, `items[1..]` the bound values pushed at invocation. Accepted wherever `T_XT` is, and `type-of` calls both `:xt` |
| `T_ADDR` | dict index; used internally for return-stack frames |
| `T_STREAM` | OS file descriptor (a pipe end to a child process); an inline `int`, like `T_ADDR` |
| `T_DB` | inline handle into the per-interpreter registry of open SQLite connections; not GC'd (closed with `db-close`) |
| `T_PTR` | opaque C pointer from the FFI (library handle or data pointer); a registry index, not the raw 64-bit address; not GC'd |
| `T_CONT` | heap object; a captured return-stack slice plus a resume IP |
| `T_MARK` | ephemeral sentinel from `[<`, `[`, `{`, `reset`; not user-visible |
| `T_LOGIC_VAR` | index into the logic-var stack; unbound, or bound to a Val (resolve with `deref`) |
| `T_UNBOUND` | binding sentinel for an unbound logic var; also the `_` wildcard value when on the stack |
| `T_NONE` | uninitialized / sentinel; the empty list and `null` |

Boolean convention: `1.0` true, `0.0` false.

---

## Object allocation

Most heap values use one slot in the `objects[]` table (pointer-bump, grown on demand to a 64M-entry ceiling, GC on exhaustion) plus a `calloc`'d `Object` struct plus one payload allocation. Two types are exceptions: **pairs** live in a separate dense, GC'd table (`{head, tail}` inline, no payload), and **logic vars** on a bump-allocated stack reclaimed by truncation on backtrack.

| Type | Payload |
|------|---------|
| String | `len + 1` bytes (NUL-terminated) |
| Array | `max(n,1) × sizeof(Val)` |
| Set | 4 × `sizeof(Val)` initial, doubles on overflow |
| Frame | 4 × (`sizeof(cell)` keys + `sizeof(Val)` values), doubles on overflow |
| Matrix | `r × c × sizeof(double)` (calloc, zero-filled) |
| Continuation | `max(L,1) × sizeof(Val)` |
