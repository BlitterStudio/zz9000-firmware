#!/bin/sh
# Self-test for scripts/check-abi-mirror.sh: prove it passes on a clean mirror,
# fails (exit 1) on a planted opcode / service-flag / capability-bit drift, and
# errors (exit 2) on a missing header. Uses tiny fixture headers so it needs
# neither repo checked out.
#
# Usage: check-abi-mirror-selftest.sh [path/to/check-abi-mirror.sh]
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
CHECK=${1:-"$HERE/../scripts/check-abi-mirror.sh"}
if [ ! -r "$CHECK" ]; then
  echo "selftest: cannot find checker at $CHECK" >&2
  exit 2
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

cat > "$WORK/abi.h" <<'EOF'
enum ZZ9KService {
  ZZ9K_SERVICE_CORE = 0x0000,
  ZZ9K_SERVICE_IMAGE = 0x0400,
  ZZ9K_SERVICE_VENDOR = 0x8000
};
enum ZZ9KOpcode {
  ZZ9K_OP_NOP = ZZ9K_SERVICE_CORE + 0x00,
  ZZ9K_OP_PING = ZZ9K_SERVICE_CORE + 0x02,
  ZZ9K_OP_DECODE_JPEG = ZZ9K_SERVICE_IMAGE + 0x01
};
enum ZZ9KServiceFlags {
  ZZ9K_SERVICE_FLAG_FIRMWARE = 1U << 0,
  ZZ9K_SERVICE_FLAG_IMAGE_JPEG = 1U << 16,
  ZZ9K_SERVICE_FLAG_IMAGE_PNG = 1U << 17
};
enum ZZ9KCapability {
  ZZ9K_CAP_MAILBOX = 1U << 0,
  ZZ9K_CAP_DOORBELL = 1U << 12,
  ZZ9K_CAP_GFX_OPS = 1U << 17
};
EOF

# Clean firmware mirror (all common names match). Firmware implements only a
# subset of the SDK flags (no IMAGE_PNG) and caps (no GFX_OPS) — subset is
# allowed, not a drift.
cat > "$WORK/fw_ok.h" <<'EOF'
#define SDK_SERVICE_CORE   0x0000U
#define SDK_SERVICE_IMAGE  0x0400U
#define SDK_OP_NOP         0x0000U
#define SDK_OP_PING        0x0002U
#define SDK_OP_DECODE_JPEG 0x0401U
#define SDK_SERVICE_FLAG_FIRMWARE   (1U << 0)
#define SDK_SERVICE_FLAG_IMAGE_JPEG (1U << 16)
#define SDK_CAP_MAILBOX  (1U << 0)
#define SDK_CAP_DOORBELL (1U << 12)
EOF

# Drifted firmware mirror (DECODE_JPEG opcode wrong; flags all match).
cat > "$WORK/fw_drift.h" <<'EOF'
#define SDK_SERVICE_CORE   0x0000U
#define SDK_SERVICE_IMAGE  0x0400U
#define SDK_OP_NOP         0x0000U
#define SDK_OP_PING        0x0002U
#define SDK_OP_DECODE_JPEG 0x0499U
#define SDK_SERVICE_FLAG_FIRMWARE   (1U << 0)
#define SDK_SERVICE_FLAG_IMAGE_JPEG (1U << 16)
EOF

# Flag-drifted firmware mirror (ops match; IMAGE_JPEG flag at wrong bit).
cat > "$WORK/fw_flagdrift.h" <<'EOF'
#define SDK_SERVICE_CORE   0x0000U
#define SDK_SERVICE_IMAGE  0x0400U
#define SDK_OP_NOP         0x0000U
#define SDK_OP_PING        0x0002U
#define SDK_OP_DECODE_JPEG 0x0401U
#define SDK_SERVICE_FLAG_FIRMWARE   (1U << 0)
#define SDK_SERVICE_FLAG_IMAGE_JPEG (1U << 18)
EOF

# Cap-drifted firmware mirror (ops + flags match; DOORBELL cap at wrong bit).
cat > "$WORK/fw_capdrift.h" <<'EOF'
#define SDK_SERVICE_CORE   0x0000U
#define SDK_SERVICE_IMAGE  0x0400U
#define SDK_OP_NOP         0x0000U
#define SDK_OP_PING        0x0002U
#define SDK_OP_DECODE_JPEG 0x0401U
#define SDK_SERVICE_FLAG_FIRMWARE   (1U << 0)
#define SDK_SERVICE_FLAG_IMAGE_JPEG (1U << 16)
#define SDK_CAP_MAILBOX  (1U << 0)
#define SDK_CAP_DOORBELL (1U << 13)
EOF

fail=0

# Case 1: clean mirror -> exit 0.
if sh "$CHECK" "$WORK/abi.h" "$WORK/fw_ok.h" >"$WORK/out1" 2>&1; then
  echo "selftest: case 1 (clean) PASS"
else
  echo "selftest: case 1 (clean) FAIL — expected exit 0, got $?"; cat "$WORK/out1"; fail=1
fi

# Case 2: planted drift -> exit 1 and the mismatch is reported.
if sh "$CHECK" "$WORK/abi.h" "$WORK/fw_drift.h" >"$WORK/out2" 2>&1; then
  echo "selftest: case 2 (drift) FAIL — expected non-zero exit, got 0"; cat "$WORK/out2"; fail=1
else
  rc=$?
  if [ "$rc" -eq 1 ] && grep -q "MISMATCH  op DECODE_JPEG" "$WORK/out2"; then
    echo "selftest: case 2 (drift) PASS"
  else
    echo "selftest: case 2 (drift) FAIL — exit $rc, expected 1 + DECODE_JPEG mismatch"; cat "$WORK/out2"; fail=1
  fi
fi

# Case 3: missing header -> exit 2.
if sh "$CHECK" "$WORK/abi.h" "$WORK/does-not-exist.h" >"$WORK/out3" 2>&1; then
  echo "selftest: case 3 (missing) FAIL — expected exit 2, got 0"; fail=1
else
  rc=$?
  if [ "$rc" -eq 2 ]; then
    echo "selftest: case 3 (missing) PASS"
  else
    echo "selftest: case 3 (missing) FAIL — exit $rc, expected 2"; cat "$WORK/out3"; fail=1
  fi
fi

# Case 4: planted service-flag drift -> exit 1 and the flag mismatch is reported.
if sh "$CHECK" "$WORK/abi.h" "$WORK/fw_flagdrift.h" >"$WORK/out4" 2>&1; then
  echo "selftest: case 4 (flag drift) FAIL — expected non-zero exit, got 0"; cat "$WORK/out4"; fail=1
else
  rc=$?
  if [ "$rc" -eq 1 ] && grep -q "MISMATCH  flag IMAGE_JPEG" "$WORK/out4"; then
    echo "selftest: case 4 (flag drift) PASS"
  else
    echo "selftest: case 4 (flag drift) FAIL — exit $rc, expected 1 + IMAGE_JPEG flag mismatch"; cat "$WORK/out4"; fail=1
  fi
fi

# Case 5: planted capability-bit drift -> exit 1 and the cap mismatch is reported.
if sh "$CHECK" "$WORK/abi.h" "$WORK/fw_capdrift.h" >"$WORK/out5" 2>&1; then
  echo "selftest: case 5 (cap drift) FAIL — expected non-zero exit, got 0"; cat "$WORK/out5"; fail=1
else
  rc=$?
  if [ "$rc" -eq 1 ] && grep -q "MISMATCH  cap DOORBELL" "$WORK/out5"; then
    echo "selftest: case 5 (cap drift) PASS"
  else
    echo "selftest: case 5 (cap drift) FAIL — exit $rc, expected 1 + DOORBELL cap mismatch"; cat "$WORK/out5"; fail=1
  fi
fi

if [ "$fail" -eq 0 ]; then
  echo "check-abi-mirror-selftest: passed"
else
  echo "check-abi-mirror-selftest: FAILED"
fi
exit "$fail"
