#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Generate the EF scenes and replay them only through the real command
# ingress (GP1 -> ps7_0_axi_periph -> axi_fifo_mm_s -> RasterIX EF) over the
# mmio, pl330, burst16 and pl330-early transports; run_scenes.sh runs the
# same matrix after the stream-level transports.
#
# usage: run_fifo_path.sh [output-dir] [latency]
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec sh "$HERE/run_scenes.sh" "${1:-$HERE/out/fifo_path}" "${2:-2}" fifo
