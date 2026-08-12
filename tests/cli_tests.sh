#!/bin/sh
# Command-line flag tests for water.
#
# The golden-output harness (run.sh) always invokes the binary with `-b`, so it
# can't exercise flag behavior itself. These cases run the binary directly with
# various flags and check stdout+stderr and exit status. Grow by adding `exact`
# / `has` lines below.
#
# Run standalone with: sh tests/cli_tests.sh   (run.sh also calls it.)

set -u
here=$(cd "$(dirname "$0")" && pwd)
bin="$here/../water"
pass=0
fail=0

ok()  { pass=$((pass + 1)); printf "  ok   %s\n" "$1"; }
bad() { fail=$((fail + 1)); name=$1; shift; printf "  FAIL %s\n" "$name"; for l in "$@"; do printf "       %s\n" "$l"; done; }

# run INPUT FLAGS... -> sets $out (stdout+stderr, trailing newline stripped) and $code
run() {
    input=$1
    shift
    out=$(printf '%s\n' "$input" | "$bin" "$@" 2>&1)
    code=$?
}

# exact NAME INPUT EXPECTED_OUT EXPECTED_CODE FLAGS...
exact() {
    name=$1 input=$2 want=$3 wantcode=$4
    shift 4
    run "$input" "$@"
    if [ "$out" = "$want" ] && [ "$code" = "$wantcode" ]; then ok "$name"
    else bad "$name" "flags: $*" "want (exit $wantcode): [$want]" "got  (exit $code): [$out]"; fi
}

# has NAME INPUT SUBSTRING EXPECTED_CODE FLAGS...
has() {
    name=$1 input=$2 sub=$3 wantcode=$4
    shift 4
    run "$input" "$@"
    case "$out" in *"$sub"*) found=1 ;; *) found=0 ;; esac
    if [ "$found" = 1 ] && [ "$code" = "$wantcode" ]; then ok "$name"
    else bad "$name" "flags: $*" "want substring (exit $wantcode): [$sub]" "got (exit $code): [$out]"; fi
}

# hasnt NAME INPUT SUBSTRING EXPECTED_CODE FLAGS... — asserts the substring is absent
hasnt() {
    name=$1 input=$2 sub=$3 wantcode=$4
    shift 4
    run "$input" "$@"
    case "$out" in *"$sub"*) found=1 ;; *) found=0 ;; esac
    if [ "$found" = 0 ] && [ "$code" = "$wantcode" ]; then ok "$name"
    else bad "$name" "flags: $*" "want ABSENT (exit $wantcode): [$sub]" "got (exit $code): [$out]"; fi
}

printf "CLI flag tests:\n"

# batch mode: only the program's own output, no banner, no prompt
exact "batch (-b) is quiet"             '2 3 + . cr'  "5 " 0 -b
exact "piped default is batch"          '2 3 + . cr'  "5 " 0
# interactive: banner + per-line prompt (banner version not pinned)
has   "interactive (-i) shows banner"   '2 3 + . cr'  "water " 0 -i
has   "interactive (-i) shows the hint" '2 3 + . cr'  "words lists every word; help shows a quick start; bye quits" 0 -i
has   "interactive (-i) shows prompt"   '1 2 . cr'    "ok 1|1"        0 -i
# definition echo: interactive-only (silent under -b), new vs redefined, and the
# immediate-echo path (variable). Anonymous words (a curried xt) never echo.
exact "definition echo silent (-b)"     ': zzz-echo dup * ;'  ""  0 -b
has   "definition echo new (-i)"         ': zzz-echo dup * ;'  "new word: zzz-echo"        0 -i
has   "definition echo redefined (-i)"   ': zzz-echo 1 ; : zzz-echo 2 ;'  "redefined word: zzz-echo"  0 -i
has   "definition echo variable (-i)"    'variable zzz-var'    "new word: zzz-var"         0 -i
# a loaded library's internal word is private to its load unit: unreachable from
# the session (public words of the same lib still resolve — covered by 131_plot)
has   "lib internal is unit-private"     '"lib/plot.h2o" load  "/tmp" next-svg-index'  "unknown word: next-svg-index"  0 -b
# words separates loaded-library words into their own group, apart from session
has   "words groups loaded-library words" '"lib/plot.h2o" load  words'  "library:"  0 -b
# a loaded file echoes only its own definitions, not those of files/libraries it loads
lf=$(mktemp "${TMPDIR:-/tmp}/lf_nest.XXXXXX")
printf '%s\n' '"plot" load-library' ': zzz-mine 1 ;' > "$lf"
has   "load echoes the file's own defs"  ''  "new word: zzz-mine"  0 -i "$lf"
hasnt "load hides nested-load defs"      ''  "new word: figure"    0 -i "$lf"
rm -f "$lf"
# ++ / -- increment a local or global variable in place; unknown/non-variable/top-level errors
has   "++ increments a global"          'variable c 5 to c : b ^c | ++ c ; b b c . cr'  "7"  0 -b
has   "++ needs ^ for a global"         'variable c : b ++ c ;'  "c is a global; declare it in the locals list as ^c"  0 -b
has   "++ rejects an unknown name"      ': u ++ nope ;'  "unknown variable: nope"  0 -b
has   "++ rejects a non-variable"       ': v ++ dup ;'   "dup is not a variable"   0 -b
has   "++ needs a colon definition"     '++ c'           "only valid inside a colon definition"  0 -b
# --max-objects lowers the object ceiling so the limit is reachable cheaply
has   "--max-objects hits ceiling"      '1 200000 range [: drop [< 0 >] :] map drop'  "object registry full" 0 -b --max-objects 100000
# --max-objects argument validation
has   "--max-objects needs a value"     ''  "needs a value"      2 --max-objects
has   "--max-objects rejects 0"         ''  "positive integer"   2 --max-objects 0
has   "--max-objects rejects non-number" '' "positive integer"   2 --max-objects xyz
# unknown flag is rejected, pointing at --help
has   "unknown flag rejected"           ''  "unknown option"     2 --bogus
has   "unknown flag suggests help"      ''  "water --help"       2 --bogus
# timed prints an elapsed line then passes xt's results through (the elapsed
# value is wall-clock-dependent, so only the pass-through result is pinned)
has   "timed passes results through"    '[: 40 2 + :] timed 100 + . cr'  "142" 0 -b

# -h / --help print usage and exit 0
has   "--help prints usage"             ''  "usage: water"       0 --help
has   "-h prints usage"                 ''  "usage: water"       0 -h

# -w / --words print the word listing and exit 0
has   "--words prints the listing"      ''  "Stack manipulation:" 0 --words
has   "-w prints the listing"           ''  "Stack manipulation:" 0 -w

# a positional argument runs a program file and exits; stdin is not read
prog=$(mktemp "${TMPDIR:-/tmp}/lf_prog.XXXXXX")
printf '2 3 + . cr\n' > "$prog"
exact "positional arg runs a program file"  ''  "5 "           0  "$prog"
has   "missing program file reported"       ''  "cannot open"  1  /no/such/file.h2o
rm -f "$prog"

# -e runs a code string and exits without reading stdin (implies -b)
exact "-e runs a code string"           '5 . cr'  "7 "    0 -e '3 4 + . cr'
exact "-e is repeatable, in order"      ''        "1 2 "  0 -e '1 .' -e '2 . cr'
has   "-e reports errors"               ''  "unknown word: bogus"  1 -e 'bogus'
has   "-e needs a code string"          ''  "needs a code string"  2 -e
prog=$(mktemp "${TMPDIR:-/tmp}/lf_prog.XXXXXX")
printf '2 . \n' > "$prog"
exact "-e composes with files in argument order"  ''  "1 2 3 "  0 -e '1 .' "$prog" -e '3 . cr'
rm -f "$prog"

# a string of bare UTF-8 continuation bytes must decode without a heap overflow:
# the codepoint buffer once used the codepoint count (which skips continuation
# bytes) for its size but was filled one int per byte. 100000 such bytes decode
# to 100000 codepoints and the interpreter keeps computing (40 2 + = 42).
cont=$(mktemp "${TMPDIR:-/tmp}/lf_cont.XXXXXX")
head -c 100000 /dev/zero | tr '\000' '\200' > "$cont"
out=$(printf '"%s" read-file string>codepoints size .  40 2 + . cr\n' "$cont" | "$bin" -b 2>&1)
code=$?
case "$out" in
    "100000 42 ") ok "continuation-byte string: no overflow + recovery" ;;
    *) bad "continuation-byte string: no overflow + recovery" "want [100000 42 ] (exit 0)" "got (exit $code): [$out]" ;;
esac
rm -f "$cont"

# `load` resolves the path as given, then falls back to the loading file's own
# directory; an unresolved path still reports the original name.
lfdir=$(mktemp -d "${TMPDIR:-/tmp}/lf_dir.XXXXXX")
printf ': from-sibling 42 ;\n' > "$lfdir/sib.h2o"
printf '"sib.h2o" load  from-sibling . cr\n' > "$lfdir/main.h2o"
exact "load falls back to the loading file's directory"  ''  "42 "  0  "$lfdir/main.h2o"
printf '"nope.h2o" load\n' > "$lfdir/bad.h2o"
has   "unresolved load reports the original name"  ''  "cannot open nope.h2o"  1  "$lfdir/bad.h2o"
rm -rf "$lfdir"

# --arena overrides the reservation (gigabytes, optional g suffix)
exact "--arena accepts a size"      '2 3 + . cr'  "5 "  0 -b --arena 2g
has   "--arena needs a value"       ''  "needs a size"   2 --arena
has   "--arena rejects junk"        ''  "takes gigabytes" 2 --arena xyz
has   "--arena rejects sub-1g"      ''  "takes gigabytes" 2 --arena 0.5g

# `water` prints the logo and the version from water.h
ver=$(sed -n 's/#define VERSION "\(.*\)".*/\1/p' "$here/../src/c/water.h")
has "water prints the logo"    'water' "++++++"       0 -b
has "water prints the version" 'water' "water $ver"   0 -b

printf "%d passed, %d failed\n" "$pass" "$fail"
[ "$fail" -eq 0 ]
