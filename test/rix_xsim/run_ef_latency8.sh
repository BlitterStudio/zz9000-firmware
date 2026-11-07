#!/usr/bin/env bash
# Run all EF scenes at the pessimistic LATENCY=8 DDR model.
set -euo pipefail
export RASTERIX_VARIANT=ef
for scene in s1 s2 s3 s4; do
  test/rix_xsim/run_xsim.sh "$scene" 8
done
