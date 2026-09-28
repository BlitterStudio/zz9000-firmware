#!/bin/sh
# Cross-repo drift guard for the hand-mirrored mailbox ABI constants.
#
# The SDK (include/zz9k/abi.h) defines ZZ9K_OP_* / ZZ9K_SERVICE_* /
# ZZ9K_SERVICE_FLAG_* / ZZ9K_CAP_* and the firmware (ZZ9000OS/src/sdk_mailbox.h)
# mirrors them as SDK_OP_* / SDK_SERVICE_* / SDK_SERVICE_FLAG_* / SDK_CAP_* BY
# HAND across two repos. A silent divergence (same name, different opcode, flag
# bit, or capability bit) misdispatches or mis-advertises capabilities on
# hardware and is invisible to the host suite. This script compares every
# op/service/flag/cap NAME present in BOTH headers and fails on any numeric
# mismatch. Names present in only one side are reported but allowed: the firmware
# legitimately implements a subset (no CANCEL/GFX/MODULE/VENDOR ops, and fewer
# capability flags and cap bits than the SDK enumerates).
#
# Usage:
#   scripts/check-abi-mirror.sh [SDK_ABI_H] [FIRMWARE_SDK_MAILBOX_H]
# Defaults assume the firmware repo is a sibling checkout of the SDK repo, or
# is pointed at by $ZZ9K_FIRMWARE_HEADER.
#
# Exit: 0 = in sync, 1 = mismatch(es), 2 = a header could not be read.
set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
SDK_ABI=${1:-"$SCRIPT_DIR/../include/zz9k/abi.h"}
FW_HDR=${2:-${ZZ9K_FIRMWARE_HEADER:-"$SCRIPT_DIR/../../zz9000-firmware/ZZ9000_proto.sdk/ZZ9000OS/src/sdk_mailbox.h"}}

for f in "$SDK_ABI" "$FW_HDR"; do
  if [ ! -r "$f" ]; then
    echo "check-abi-mirror: cannot read header: $f" >&2
    echo "  pass paths explicitly, or set \$ZZ9K_FIRMWARE_HEADER" >&2
    exit 2
  fi
done

echo "check-abi-mirror: SDK  = $SDK_ABI"
echo "check-abi-mirror: FW   = $FW_HDR"

# The SDK file is read first (FNR==NR) so all ZZ9K_SERVICE_* bases are known
# before the firmware file is parsed and the ops are resolved in END. Pure
# POSIX awk (no strtonum / 3-arg match) so it runs under mawk and gawk alike.
awk '
function hex2dec(s,   n,i,c,d) {
  sub(/^0[xX]/, "", s); sub(/[uUlL]+$/, "", s); n = 0
  if (s == "") return -1
  for (i = 1; i <= length(s); i++) {
    c = tolower(substr(s, i, 1)); d = index("0123456789abcdef", c) - 1
    if (d < 0) return -1
    n = n * 16 + d
  }
  return n
}
# ---- SDK header (first file) ----
FNR == NR {
  s = $0; gsub(/[,;].*$/, "", s); gsub(/[ \t]+/, " ", s)
  n = split(s, a, " ")
  # ZZ9K_SERVICE_<NAME> = 0x<hex>   (flags use "1U << N" -> no 0x token, skipped)
  if (s ~ /ZZ9K_SERVICE_[A-Z0-9_]+ = 0x/) {
    name = ""; val = ""
    for (i = 1; i <= n; i++) {
      if (a[i] ~ /^ZZ9K_SERVICE_/) name = substr(a[i], 14)
      else if (a[i] ~ /^0x/) val = a[i]
    }
    if (name != "" && name !~ /^FLAG_/ && val != "") sdksvc[name] = hex2dec(val)
  }
  # ZZ9K_OP_<NAME> = ZZ9K_SERVICE_<BASE> + 0x<off>
  if (s ~ /ZZ9K_OP_[A-Z0-9_]+ = ZZ9K_SERVICE_[A-Z0-9_]+ \+ 0x/) {
    op = ""; base = ""; off = ""
    for (i = 1; i <= n; i++) {
      if (a[i] ~ /^ZZ9K_OP_/) op = substr(a[i], 9)
      else if (a[i] ~ /^ZZ9K_SERVICE_/) base = substr(a[i], 14)
      else if (a[i] ~ /^0x/) off = a[i]
    }
    if (op != "" && base != "" && off != "") { opbase[op] = base; opoff[op] = off }
  }
  # ZZ9K_SERVICE_FLAG_<NAME> = 1U << <bit>   (name at 19: "ZZ9K_SERVICE_FLAG_")
  if (s ~ /ZZ9K_SERVICE_FLAG_[A-Z0-9_]+ = 1U? << [0-9]+/) {
    name = ""; bit = ""
    for (i = 1; i <= n; i++) {
      if (a[i] ~ /^ZZ9K_SERVICE_FLAG_/) name = substr(a[i], 19)
      else if (a[i] == "<<" && (i + 1) <= n) bit = a[i + 1]
    }
    if (name != "" && bit ~ /^[0-9]+$/) sdkflag[name] = bit + 0
  }
  # ZZ9K_CAP_<NAME> = 1U << <bit>   (name at 10: "ZZ9K_CAP_"; global caps board
  # advertises in QUERY_CAPS, mirrored firmware-side as SDK_CAP_<NAME>)
  if (s ~ /ZZ9K_CAP_[A-Z0-9_]+ = 1U? << [0-9]+/) {
    name = ""; bit = ""
    for (i = 1; i <= n; i++) {
      if (a[i] ~ /^ZZ9K_CAP_/) name = substr(a[i], 10)
      else if (a[i] == "<<" && (i + 1) <= n) bit = a[i + 1]
    }
    if (name != "" && bit ~ /^[0-9]+$/) sdkcap[name] = bit + 0
  }
  # ZZ9K_MEDIA_STATUS_<NAME> = <dec>U   (paged MEDIA_SESSION STATUS selectors;
  # name at 19: "ZZ9K_MEDIA_STATUS_"). A page number that disagrees across the
  # repos silently returns the wrong counters instead of failing.
  if (s ~ /ZZ9K_MEDIA_STATUS_[A-Z0-9_]+ = [0-9]+/) {
    name = ""; val = ""
    for (i = 1; i <= n; i++) {
      if (a[i] ~ /^ZZ9K_MEDIA_STATUS_/) name = substr(a[i], 19)
      else if (a[i] ~ /^[0-9]+[uUlL]*$/) val = a[i]
    }
    if (name != "" && val != "") {
      sub(/[uUlL]+$/, "", val); sdkpage[name] = val + 0
    }
  }
  # ZZ9K_MEDIA_PRESENT_<NAME> = 1U << <bit>   (name at 20)
  if (s ~ /ZZ9K_MEDIA_PRESENT_[A-Z0-9_]+ = 1U? << [0-9]+/) {
    name = ""; bit = ""
    for (i = 1; i <= n; i++) {
      if (a[i] ~ /^ZZ9K_MEDIA_PRESENT_/) name = substr(a[i], 20)
      else if (a[i] == "<<" && (i + 1) <= n) bit = a[i + 1]
    }
    if (name != "" && bit ~ /^[0-9]+$/) sdkpresent[name] = bit + 0
  }
  next
}
# ---- firmware header (second file) ----
/^[ \t]*#[ \t]*define[ \t]+SDK_OP_[A-Z0-9_]+[ \t]+0x/ {
  s = $0; gsub(/[ \t]+/, " ", s); n = split(s, a, " ")
  for (i = 1; i <= n; i++) if (a[i] ~ /^SDK_OP_/) { name = substr(a[i], 8); break }
  for (i = 1; i <= n; i++) if (a[i] ~ /^0x/) { fwop[name] = hex2dec(a[i]); break }
}
/^[ \t]*#[ \t]*define[ \t]+SDK_SERVICE_[A-Z0-9_]+[ \t]+0x/ {
  s = $0; gsub(/[ \t]+/, " ", s); n = split(s, a, " ")
  for (i = 1; i <= n; i++) if (a[i] ~ /^SDK_SERVICE_/) { name = substr(a[i], 13); break }
  if (name ~ /^FLAG_/) next
  for (i = 1; i <= n; i++) if (a[i] ~ /^0x/) { fwsvc[name] = hex2dec(a[i]); break }
}
# #define SDK_SERVICE_FLAG_<NAME> (1U << <bit>)   (name at 18: "SDK_SERVICE_FLAG_")
/^[ \t]*#[ \t]*define[ \t]+SDK_SERVICE_FLAG_[A-Z0-9_]+[ \t]+.*<</ {
  s = $0; gsub(/[()]/, " ", s); gsub(/[ \t]+/, " ", s); n = split(s, a, " ")
  name = ""; bit = ""
  for (i = 1; i <= n; i++) {
    if (a[i] ~ /^SDK_SERVICE_FLAG_/) name = substr(a[i], 18)
    else if (a[i] == "<<" && (i + 1) <= n) bit = a[i + 1]
  }
  if (name != "" && bit ~ /^[0-9]+$/) fwflag[name] = bit + 0
}
# #define SDK_CAP_<NAME> (1U << <bit>)   (name at 9: "SDK_CAP_"). Composite
# aliases like SDK_*_CAPABILITY_BITS do not start with SDK_CAP_ and carry no
# "<<", so this pattern skips them and only captures the single-bit defines.
/^[ \t]*#[ \t]*define[ \t]+SDK_CAP_[A-Z0-9_]+[ \t]+.*<</ {
  s = $0; gsub(/[()]/, " ", s); gsub(/[ \t]+/, " ", s); n = split(s, a, " ")
  name = ""; bit = ""
  for (i = 1; i <= n; i++) {
    if (a[i] ~ /^SDK_CAP_/) name = substr(a[i], 9)
    else if (a[i] == "<<" && (i + 1) <= n) bit = a[i + 1]
  }
  if (name != "" && bit ~ /^[0-9]+$/) fwcap[name] = bit + 0
}
# #define SDK_MEDIA_STATUS_<NAME> <dec>U   (name at 18: "SDK_MEDIA_STATUS_")
/^[ \t]*#[ \t]*define[ \t]+SDK_MEDIA_STATUS_[A-Z0-9_]+[ \t]+[0-9]/ {
  s = $0; gsub(/[ \t]+/, " ", s); n = split(s, a, " ")
  name = ""; val = ""
  for (i = 1; i <= n; i++) {
    if (a[i] ~ /^SDK_MEDIA_STATUS_/) name = substr(a[i], 18)
    else if (a[i] ~ /^[0-9]+[uUlL]*$/) val = a[i]
  }
  if (name != "" && val != "") { sub(/[uUlL]+$/, "", val); fwpage[name] = val + 0 }
}
# #define SDK_MEDIA_PRESENT_<NAME> (1U << <bit>)   (name at 19)
/^[ \t]*#[ \t]*define[ \t]+SDK_MEDIA_PRESENT_[A-Z0-9_]+[ \t]+.*<</ {
  s = $0; gsub(/[()]/, " ", s); gsub(/[ \t]+/, " ", s); n = split(s, a, " ")
  name = ""; bit = ""
  for (i = 1; i <= n; i++) {
    if (a[i] ~ /^SDK_MEDIA_PRESENT_/) name = substr(a[i], 19)
    else if (a[i] == "<<" && (i + 1) <= n) bit = a[i + 1]
  }
  if (name != "" && bit ~ /^[0-9]+$/) fwpresent[name] = bit + 0
}
END {
  mism = 0; common = 0; sdkonly = 0; fwonly = 0
  # Resolve SDK ops to absolute values now that all bases are known.
  for (op in opbase) {
    b = opbase[op]
    if (!(b in sdksvc)) { printf("  ! SDK op ZZ9K_OP_%s references unknown base ZZ9K_SERVICE_%s\n", op, b); mism++; continue }
    sdkop[op] = sdksvc[b] + hex2dec(opoff[op])
  }
  # Compare ops.
  for (op in sdkop) {
    if (op in fwop) {
      common++
      if (sdkop[op] != fwop[op]) {
        printf("  MISMATCH  op %-26s SDK ZZ9K_OP_%s=0x%04x  FW SDK_OP_%s=0x%04x\n",
               op, op, sdkop[op], op, fwop[op]); mism++
      }
    } else { sdkonly++; sdkonly_names = sdkonly_names " " op }
  }
  for (op in fwop) if (!(op in sdkop)) { fwonly++; fwonly_names = fwonly_names " " op }
  # Compare service bases.
  for (sv in sdksvc) {
    if (sv in fwsvc) {
      common++
      if (sdksvc[sv] != fwsvc[sv]) {
        printf("  MISMATCH  service %-20s SDK ZZ9K_SERVICE_%s=0x%04x  FW SDK_SERVICE_%s=0x%04x\n",
               sv, sv, sdksvc[sv], sv, fwsvc[sv]); mism++
      }
    }
  }
  # Compare service-flag bit positions (per-name; the same bit is legitimately
  # reused across differently-named flags, so comparison is by NAME only).
  flagcommon = 0
  for (fl in sdkflag) {
    if (fl in fwflag) {
      common++; flagcommon++
      if (sdkflag[fl] != fwflag[fl]) {
        printf("  MISMATCH  flag %-24s SDK ...FLAG_%s=1<<%d  FW ...FLAG_%s=1<<%d\n",
               fl, fl, sdkflag[fl], fl, fwflag[fl]); mism++
      }
    }
  }
  # Compare global capability bits (ZZ9K_CAP_* vs SDK_CAP_*), by NAME. Firmware
  # advertises a subset (no MODULES/GFX_OPS/STORAGE_OPS) -> SDK-only caps allowed.
  capcommon = 0
  for (cp in sdkcap) {
    if (cp in fwcap) {
      common++; capcommon++
      if (sdkcap[cp] != fwcap[cp]) {
        printf("  MISMATCH  cap %-25s SDK ZZ9K_CAP_%s=1<<%d  FW SDK_CAP_%s=1<<%d\n",
               cp, cp, sdkcap[cp], cp, fwcap[cp]); mism++
      }
    }
  }
  # Compare paged MEDIA_SESSION STATUS selectors and presentation flag bits.
  pagecommon = 0
  for (pg in sdkpage) {
    if (pg in fwpage) {
      common++; pagecommon++
      if (sdkpage[pg] != fwpage[pg]) {
        printf("  MISMATCH  status page %-14s SDK ZZ9K_MEDIA_STATUS_%s=%d  FW SDK_MEDIA_STATUS_%s=%d\n",
               pg, pg, sdkpage[pg], pg, fwpage[pg]); mism++
      }
    }
  }
  presentcommon = 0
  for (pf in sdkpresent) {
    if (pf in fwpresent) {
      common++; presentcommon++
      if (sdkpresent[pf] != fwpresent[pf]) {
        printf("  MISMATCH  present flag %-13s SDK ZZ9K_MEDIA_PRESENT_%s=1<<%d  FW SDK_MEDIA_PRESENT_%s=1<<%d\n",
               pf, pf, sdkpresent[pf], pf, fwpresent[pf]); mism++
      }
    }
  }
  printf("check-abi-mirror: %d common name(s) compared (incl. %d service flag[s], %d cap bit[s], %d status page[s], %d present flag[s]), %d SDK-only, %d firmware-only op(s)\n",
         common, flagcommon, capcommon, pagecommon, presentcommon, sdkonly, fwonly)
  if (sdkonly > 0) printf("  SDK-only ops (firmware does not implement):%s\n", sdkonly_names)
  if (fwonly  > 0) printf("  firmware-only ops (not in SDK abi.h):%s\n", fwonly_names)
  if (mism > 0) { printf("check-abi-mirror: FAIL — %d mismatch(es)\n", mism); exit 1 }
  printf("check-abi-mirror: OK — no drift\n")
}
' "$SDK_ABI" "$FW_HDR"
