#!/bin/bash
# Verify that _vector_table is allocated, executable, inside a PT_LOAD segment,
# and placed at the expected high-image VBAR base (0x08200000).
set -euo pipefail

ELF="${1:-ZZ9000_proto.sdk/ZZ9000OS/build/ZZ9000OS.elf}"
EXPECTED_VBAR="${2:-0x08200000}"

if [ ! -f "$ELF" ]; then
    echo "ERROR: ELF file not found: $ELF" >&2
    exit 1
fi

READELF="${READELF:-arm-none-eabi-readelf}"
NM="${NM:-arm-none-eabi-nm}"
OBJDUMP="${OBJDUMP:-arm-none-eabi-objdump}"

if ! command -v "$READELF" >/dev/null 2>&1; then
    READELF=readelf
fi
if ! command -v "$NM" >/dev/null 2>&1; then
    NM=nm
fi
if ! command -v "$OBJDUMP" >/dev/null 2>&1; then
    OBJDUMP=objdump
fi

# 1. Symbol check via nm: _vector_table must exist and NOT be marked 'N'/'n' (non-allocated / discarded)
NM_LINE=$("$NM" -n "$ELF" | grep -w "_vector_table" | head -1 || true)
if [ -z "$NM_LINE" ]; then
    echo "ERROR: symbol '_vector_table' not found in $ELF" >&2
    exit 1
fi

VEC_TYPE=$(echo "$NM_LINE" | awk '{print $2}')
VEC_ADDR_HEX=$(echo "$NM_LINE" | awk '{print $1}')
VEC_ADDR=$((16#$VEC_ADDR_HEX))
EXP_ADDR=$((EXPECTED_VBAR))

if [ "$VEC_TYPE" = "N" ] || [ "$VEC_TYPE" = "n" ]; then
    echo "ERROR: _vector_table is marked '$VEC_TYPE' (non-allocated / discarded section). Exception vectors are NOT loaded into memory!" >&2
    exit 1
fi

if [ "$VEC_ADDR" -ne "$EXP_ADDR" ]; then
    printf "ERROR: _vector_table address 0x%08x != expected VBAR base 0x%08x\n" "$VEC_ADDR" "$EXP_ADDR" >&2
    exit 1
fi

# 2. Check readelf section headers: section containing _vector_table must have SHF_ALLOC ('A') and SHF_EXECINSTR ('X')
SYM_LINE=$("$READELF" -s -W "$ELF" | grep -w "_vector_table" | head -1 || true)
SEC_NDX=$(echo "$SYM_LINE" | awk '{print $7}')
if [ -z "$SEC_NDX" ] || [ "$SEC_NDX" = "UND" ] || [ "$SEC_NDX" = "ABS" ]; then
    echo "ERROR: invalid section index '$SEC_NDX' for _vector_table" >&2
    exit 1
fi

SEC_LINE=$("$READELF" -S -W "$ELF" | grep -E "\[\s*${SEC_NDX}\]" | head -1 || true)
if [[ "$SEC_LINE" != *"AX"* && ( "$SEC_LINE" != *"A"* || "$SEC_LINE" != *"X"* ) ]]; then
    echo "ERROR: section containing _vector_table lacks ALLOC (A) or EXEC (X) flags: $SEC_LINE" >&2
    exit 1
fi

# 3. Check readelf program headers: _vector_table must be inside a PT_LOAD segment with execute permission
LOAD_COVERED=0
while read -r pline; do
    if [[ "$pline" =~ LOAD[[:space:]]+(0x[0-9a-fA-F]+)[[:space:]]+(0x[0-9a-fA-F]+)[[:space:]]+(0x[0-9a-fA-F]+)[[:space:]]+(0x[0-9a-fA-F]+)[[:space:]]+(0x[0-9a-fA-F]+)[[:space:]]+([RWE]+) ]]; then
        seg_vaddr=$((${BASH_REMATCH[2]}))
        seg_memsiz=$((${BASH_REMATCH[5]}))
        seg_flags="${BASH_REMATCH[6]}"
        if [ "$VEC_ADDR" -ge "$seg_vaddr" ] && [ "$VEC_ADDR" -lt $((seg_vaddr + seg_memsiz)) ]; then
            if [[ "$seg_flags" == *"E"* ]]; then
                LOAD_COVERED=1
                break
            fi
        fi
    fi
done < <("$READELF" -l -W "$ELF")

if [ "$LOAD_COVERED" -ne 1 ]; then
    echo "ERROR: _vector_table (0x$VEC_ADDR_HEX) is NOT inside an executable PT_LOAD segment!" >&2
    exit 1
fi

# 4. Check disassembly: instruction at _vector_table must branch to _boot
DISASM=$("$OBJDUMP" -d --start-address="$VEC_ADDR" --stop-address=$((VEC_ADDR + 4)) "$ELF" || true)
if [[ "$DISASM" != *"_boot"* && "$DISASM" != *"b"* ]]; then
    echo "ERROR: reset vector does not branch to _boot: $DISASM" >&2
    exit 1
fi

echo "[check_vectors] PASS: _vector_table at 0x$VEC_ADDR_HEX (type $VEC_TYPE) is ALLOC+EXEC in PT_LOAD segment."
