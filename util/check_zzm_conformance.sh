#!/bin/sh
# Validate the post-link ELF consumed by zzm_pack.  libzzm_rt is linked into
# every module as an archive, so its provenance is represented by a required
# identity symbol rather than by an external shared-library dependency.
set -eu

usage() {
    cat >&2 <<EOF
usage: $0 [--runtime-symbol SYMBOL] MODULE.elf MODULE_ID MODULE_NAME SLOT_ADDRESS SLOT_SIZE

MODULE_ID, SLOT_ADDRESS, and SLOT_SIZE accept decimal or 0x-prefixed unsigned
32-bit values.  SYMBOL defaults to zzm_rt_identity.
EOF
    exit 2
}

die() {
    printf '%s: %s\n' "$0" "$*" >&2
    exit 2
}

parse_u32() {
    value=$1
    awk '
function digit(c) {
    if (c >= "0" && c <= "9") return c + 0
    c = tolower(c)
    if (c >= "a" && c <= "f") return 10 + index("abcdef", c) - 1
    return -1
}
{
    text = $0
    base = 10
    if (text ~ /^0[xX]/) {
        base = 16
        text = substr(text, 3)
    }
    if (text == "") exit 1
    number = 0
    for (i = 1; i <= length(text); i++) {
        n = digit(substr(text, i, 1))
        if (n < 0 || n >= base) exit 1
        number = number * base + n
        if (number > 4294967295) exit 1
    }
    printf "%.0f\n", number
    exit 0
}
END { if (NR != 1) exit 1 }
' <<EOF
$value
EOF
}

runtime_symbol=zzm_rt_identity
while [ "$#" -gt 0 ]; do
    case $1 in
    --runtime-symbol)
        [ "$#" -ge 2 ] || usage
        runtime_symbol=$2
        shift 2
        ;;
    --help)
        usage
        ;;
    --)
        shift
        break
        ;;
    -*)
        usage
        ;;
    *)
        break
        ;;
    esac
done

[ "$#" -eq 5 ] || usage
elf=$1
module_id=$2
module_name=$3
slot_address=$4
slot_size=$5

[ -r "$elf" ] || die "cannot read ELF: $elf"
[ -n "$module_name" ] || die "MODULE_NAME must not be empty"
case $runtime_symbol in
    *[!A-Za-z0-9_]* | [0-9]* | '') die "runtime symbol must be an ELF symbol name" ;;
esac

module_id_value=$(parse_u32 "$module_id") || die "invalid MODULE_ID: $module_id"
slot_value=$(parse_u32 "$slot_address") || die "invalid SLOT_ADDRESS: $slot_address"
slot_size_value=$(parse_u32 "$slot_size") || die "invalid SLOT_SIZE: $slot_size"
[ "$module_id_value" -ne 0 ] || die "MODULE_ID must not be zero"
[ "$slot_size_value" -ne 0 ] || die "SLOT_SIZE must not be zero"

# The arithmetic is deliberately in awk: POSIX shells are not required to
# represent the full unsigned 32-bit range.
awk -v slot="$slot_value" -v size="$slot_size_value" 'BEGIN {
    if (slot + size > 4294967296) exit 1
}' || die "slot range overflows 32-bit address space"

: "${READELF:=arm-none-eabi-readelf}"
tmp=$(mktemp -d "${TMPDIR:-/tmp}/check-zzm-conformance.XXXXXX") || die "cannot create temporary directory"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
header=$tmp/header
symbols=$tmp/symbols
sections=$tmp/sections
entry_file=$tmp/entry
errors=$tmp/errors
: > "$errors"

"$READELF" -W -h "$elf" > "$header" || die "cannot read ELF header: $elf"
"$READELF" -W -s "$elf" > "$symbols" || die "cannot read ELF symbols: $elf"
"$READELF" -W -S "$elf" > "$sections" || die "cannot read ELF sections: $elf"

awk -v entry_file="$entry_file" '
function hex(text,    i, c, digit, value) {
    sub(/^0[xX]/, "", text)
    value = 0
    for (i = 1; i <= length(text); i++) {
        c = tolower(substr(text, i, 1))
        digit = index("0123456789abcdef", c) - 1
        if (digit < 0) return -1
        value = value * 16 + digit
    }
    return value
}
BEGIN {
    entry = -1
}

/^  Class:/ {
    class = $2
}
/^  Data:/ {
    little_endian = ($0 ~ /little endian/)
}
/^  Type:/ {
    type = $2
}
/^  Machine:/ {
    machine = $0
}
/^  Entry point address:/ {
    entry = hex($4)
}
END {
    failed = 0
    if (class != "ELF32") {
        print "module is not ELF32" > "/dev/stderr"
        failed = 1
    }
    if (!little_endian) {
        print "module is not little-endian" > "/dev/stderr"
        failed = 1
    }
    if (type != "EXEC") {
        print "module is not an executable ELF" > "/dev/stderr"
        failed = 1
    }
    if (machine !~ /ARM/) {
        print "module machine is not ARM" > "/dev/stderr"
        failed = 1
    }
    if (entry < 0) {
        print "module has no valid ELF entry address" > "/dev/stderr"
        failed = 1
    } else {
        printf "%.0f\n", entry > entry_file
    }
    exit failed
}' "$header" || exit 1
entry_value=$(cat "$entry_file")

# All entries here must arrive from the mandatory, whole-archived libzzm_rt.
# The final ELF has no archive provenance, so a section-defined identity marker
# is the stable link-time proof that the runtime was included.
awk -v runtime_symbol="$runtime_symbol" -v entry="$entry_value" -v errors="$errors" '
function hex(text,    i, c, digit, value) {
    sub(/^0[xX]/, "", text)
    value = 0
    for (i = 1; i <= length(text); i++) {
        c = tolower(substr(text, i, 1))
        digit = index("0123456789abcdef", c) - 1
        if (digit < 0) return -1
        value = value * 16 + digit
    }
    return value
}
function section_index(ndx) {
    return ndx ~ /^[0-9]+$/
}
function report(key, message) {
    if (!(key in reported)) {
        print message > errors
        reported[key] = 1
        failed = 1
    }
}
function required_symbol(name) {
    required[name] = 1
}
function cpp_required_symbol(name) {
    cpp_required[name] = 1
}
BEGIN {
    split("malloc free calloc realloc memalign posix_memalign aligned_alloc", c_alloc, " ")
    split("_malloc_r _free_r _calloc_r _realloc_r _memalign_r", reentrant_alloc, " ")
    split("_Znwj _Znaj _ZdlPv _ZdaPv _ZdlPvj _ZdaPvj _ZnwjSt11align_val_t _ZnajSt11align_val_t _ZdlPvSt11align_val_t _ZdaPvSt11align_val_t", cpp_alloc, " ")
    split("__cxa_atexit __cxa_guard_acquire __cxa_guard_release __cxa_guard_abort __dso_handle", cpp_runtime, " ")
    for (i in c_alloc) required_symbol(c_alloc[i])
    for (i in reentrant_alloc) required_symbol(reentrant_alloc[i])
    for (i in cpp_alloc) cpp_required_symbol(cpp_alloc[i])
    for (i in cpp_runtime) cpp_required_symbol(cpp_runtime[i])
}
$1 ~ /^[0-9]+:$/ {
    value = hex($2)
    type = $4
    binding = $5
    ndx = $7
    name = $8
    sub(/@@?.*$/, "", name)
    if (name == "") next

    if (ndx == "UND")
        report("undefined:" name, "undefined ELF symbol: " name)

    if ((name == "__cxa_throw" || name == "__gxx_personality_v0" || name ~ /^_Unwind_/) &&
        section_index(ndx))
        report("exception:" name, "unwinding/throw support is not allowed: " name)

    if ((name == "_sbrk" || name == "_sbrk_r" || name ~ /^__malloc_av_/) && section_index(ndx))
        report("heap:" name, "module-local newlib heap symbol: " name)

    if (name ~ /^_Z/ || name == "__cxa_atexit" || name == "__cxa_guard_acquire" ||
        name == "__cxa_guard_release" || name == "__cxa_guard_abort" ||
        name == "__dso_handle")
        cpp_requested = 1

    if (name == "zzm_module_entry" && section_index(ndx) && binding != "LOCAL" && value == entry)
        entry_marker = 1
    if (name == "zzm_module_init" && section_index(ndx) && binding != "LOCAL")
        init_marker = 1
    if (name == "zzm_module_descriptor" && section_index(ndx) && binding != "LOCAL")
        descriptor_marker = 1
    if (name == runtime_symbol && section_index(ndx) && binding != "LOCAL")
        runtime_marker = 1

    if (name in required || name in cpp_required) {
        if (section_index(ndx) && binding != "LOCAL")
            resolved[name] = 1
        else if (ndx != "UND")
            report("allocator-origin:" name, "allocator/runtime symbol is not section-defined by libzzm_rt: " name)
    }
}
END {
    if (!entry_marker || !init_marker)
        report("entry", "missing zzm_module_entry/zzm_module_init entry markers")
    if (!descriptor_marker)
        report("descriptor", "missing zzm_module_descriptor module header marker")
    if (!runtime_marker)
        report("runtime", "missing section-defined libzzm_rt identity symbol: " runtime_symbol)
    for (name in required) {
        if (!(name in resolved))
            report("allocator:" name, "allocator/runtime symbol is not resolved from libzzm_rt: " name)
    }
    if (cpp_requested) {
        for (name in cpp_required) {
            if (!(name in resolved))
                report("cpp-allocator:" name, "C++ allocator/runtime symbol is not resolved from libzzm_rt: " name)
        }
    }
    exit failed
}' "$symbols" || {
    [ -s "$errors" ] || die "cannot inspect ELF symbols: $elf"
}

awk -v slot="$slot_value" -v size="$slot_size_value" -v entry="$entry_value" -v errors="$errors" '
function hex(text,    i, c, digit, value) {
    value = 0
    for (i = 1; i <= length(text); i++) {
        c = tolower(substr(text, i, 1))
        digit = index("0123456789abcdef", c) - 1
        if (digit < 0) return -1
        value = value * 16 + digit
    }
    return value
}
function report(key, message) {
    if (!(key in reported)) {
        print message > errors
        reported[key] = 1
        failed = 1
    }
}
/^[[:space:]]*\[[[:space:]]*[0-9]+\]/ {
    if ($1 == "[") {
        name = $3
        type = $4
        address = hex($5)
        section_size = hex($7)
        flags = $9
    } else {
        name = $2
        type = $3
        address = hex($4)
        section_size = hex($6)
        flags = $8
    }

    if (type == "REL" || type == "RELA" || type == "RELR")
        report("relocation:" name, "relocation section is not allowed: " name)

    if (index(flags, "A") != 0) {
        section_end = address + section_size
        if (address < slot || address > slot + size || section_end > slot + size)
            report("slot:" name, "allocated section outside module slot: " name)
        if (type != "NOBITS" && section_size > 0 && entry >= address && entry < section_end)
            entry_in_image = 1
    }
}
END {
    if (!entry_in_image)
        report("entry-image", "ELF entry is not in an initialized allocated section")
    exit failed
}' "$sections" || {
    [ -s "$errors" ] || die "cannot inspect ELF sections: $elf"
}

if [ -s "$errors" ]; then
    cat "$errors" >&2
    exit 1
fi

printf '%s: module %s (%s) conforms\n' "$0" "$module_name" "$module_id"
