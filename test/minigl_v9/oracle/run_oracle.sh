#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Helper script to build and run the differential oracle against
# the host front end in Docker (gcc:13).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKTREE_DIR="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
BUILD_DIR="${WORKTREE_DIR}/build"
CORPUS_DIR="${BUILD_DIR}/minigl_v9_corpus"
DIFF_DIR="${BUILD_DIR}/oracle_diffs"

mkdir -p "${BUILD_DIR}" "${DIFF_DIR}"

echo "================================================================================"
echo "ZZ9000 MiniGL Wire v9 Differential Oracle"
echo "Worktree: ${WORKTREE_DIR}"
echo "Corpus:   ${CORPUS_DIR}"
echo "Diffs:    ${DIFF_DIR}"
echo "================================================================================"

docker run --rm \
    -v "${WORKTREE_DIR}:/src:ro" \
    -v "${BUILD_DIR}:/build" \
    -w /tmp \
    gcc:13 bash -c "
        set -euo pipefail
        mkdir -p /tmp/work/ZZ9000_proto.sdk/ZZ9000OS
        cp -r /src/ZZ9000_proto.sdk/ZZ9000OS/src /tmp/work/ZZ9000_proto.sdk/ZZ9000OS/
        cp -r /src/sdk /tmp/work/sdk
        mkdir -p /tmp/work/test/minigl_v9/oracle
        cp /src/test/minigl_v9/*.* /tmp/work/test/minigl_v9/ 2>/dev/null || true
        cp /src/test/minigl_v9/Makefile /tmp/work/test/minigl_v9/ 2>/dev/null || true
        cp /src/test/minigl_v9/oracle/* /tmp/work/test/minigl_v9/oracle/ 2>/dev/null || true
        cd /tmp/work/test/minigl_v9
        make libmw_host.a || true
        cd /tmp/work/test/minigl_v9/oracle
        make
        ./oracle_runner /build/minigl_v9_corpus /build/oracle_diffs
    "
