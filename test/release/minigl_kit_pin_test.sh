#!/bin/bash
# util/minigl-kit-pin.sh against a stand-in gh that serves fixture releases.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
PIN_SH="$ROOT/util/minigl-kit-pin.sh"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# Fixture releases: v30.0 has a kit, v30.1-libonly has only library archives.
mkdir -p "$WORK/releases/v30.0" "$WORK/releases/v30.1-libonly" "$WORK/bin"
printf 'module image v30.0\n' > "$WORK/releases/v30.0/MINIGL.ZZM"
printf '{"schema":1}\n' > "$WORK/releases/v30.0/MINIGL.KIT.json"
printf 'source\n' > "$WORK/releases/v30.0/MINIGL-source-abc.tar.gz"
printf 'lha\n' > "$WORK/releases/v30.1-libonly/amiberry-minigl-30.1.lha"
cat > "$WORK/bin/gh" <<'EOF'
#!/bin/bash
# gh release download TAG --repo R --dir D [--clobber] [--pattern P]...
[ "$1 $2" = "release download" ] || exit 2
tag=$3; shift 3; dir=
while [ $# -gt 0 ]; do
    case $1 in --dir) dir=$2; shift 2 ;; --repo|--pattern) shift 2 ;; *) shift ;; esac
done
src="$FIXTURE_RELEASES/$tag"
[ -d "$src" ] || { echo "release not found" >&2; exit 1; }
for f in "$src"/MINIGL*; do [ -e "$f" ] && cp "$f" "$dir/"; done
exit 0
EOF
chmod +x "$WORK/bin/gh"
export PATH="$WORK/bin:$PATH" FIXTURE_RELEASES="$WORK/releases"
export MINIGL_KIT_PIN_FILE="$WORK/minigl-kit.pin"
cp "$ROOT/util/minigl-kit.pin" "$MINIGL_KIT_PIN_FILE"

failures=0
check() { # description, command...
    local what=$1; shift
    if "$@"; then echo "PASS $what"; else echo "FAIL $what" >&2; failures=$((failures + 1)); fi
}
fails() { ! "$@" > /dev/null 2>&1; }

check "committed pin parses" sh "$PIN_SH" current
check "none fetch creates nothing" \
    bash -c "sh '$PIN_SH' set none && sh '$PIN_SH' fetch '$WORK/kit-none' && [ ! -e '$WORK/kit-none' ]"

sh "$PIN_SH" set v30.0
expected="v30.0 $(sha256sum "$WORK/releases/v30.0/MINIGL.ZZM" | cut -d' ' -f1)"
check "set records tag and module sha256" [ "$(sh "$PIN_SH" current)" = "$expected" ]
check "set keeps the comment header" grep -q '^# MiniGL module kit' "$MINIGL_KIT_PIN_FILE"
check "matching fetch downloads the kit" \
    bash -c "sh '$PIN_SH' fetch '$WORK/kit' && [ -f '$WORK/kit/MINIGL.ZZM' ] && [ -f '$WORK/kit/MINIGL.KIT.json' ]"

printf 'replaced module\n' > "$WORK/releases/v30.0/MINIGL.ZZM"
check "replaced module is rejected" fails sh "$PIN_SH" fetch "$WORK/kit-replaced"
check "release without a kit cannot be pinned" fails sh "$PIN_SH" set v30.1-libonly

printf 'v30.0\n' > "$MINIGL_KIT_PIN_FILE"
check "pin without sha256 is rejected" fails sh "$PIN_SH" current
printf 'none\nnone\n' > "$MINIGL_KIT_PIN_FILE"
check "two pin lines are rejected" fails sh "$PIN_SH" current

[ "$failures" -eq 0 ] || exit 1
echo "minigl kit pin: all checks pass"
