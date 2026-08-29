#!/bin/bash
# Local driver: run build_variant_bitstreams.sh with the Windows PowerShell
# bitstream builder. Invoke from Git Bash in the repo root.
export BITSTREAM_BUILDER="powershell -NoProfile -ExecutionPolicy Bypass -File ./build_bitstream.ps1"
exec ./build_variant_bitstreams.sh "$@"
