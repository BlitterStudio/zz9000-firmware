#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/../.." && pwd -W)
docker run --rm --entrypoint //bin/bash -v "${repo_root}:/src:ro" verilator/verilator@sha256:a5b73e2fce0b2c483396f3800940f33b6faa802331a061c21694dd27b7352120 -lc '
  set -euo pipefail
  timeout 60 verilator --binary --timing -Wall -Wno-fatal \
    --top-module tb_zz9k_cmd_fetcher \
    -Mdir /tmp/zz9k_cmd_fetcher \
    /src/rtl/zz9k_rix/zz9k_cmd_fetcher.v \
    /src/test/rix_glue/tb_zz9k_cmd_fetcher.sv
  timeout 60 /tmp/zz9k_cmd_fetcher/Vtb_zz9k_cmd_fetcher
'
