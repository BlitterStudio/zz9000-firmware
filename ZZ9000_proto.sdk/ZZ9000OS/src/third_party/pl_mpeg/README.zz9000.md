# pl_mpeg vendoring note

This directory contains `pl_mpeg.h` from
<https://github.com/phoboslab/pl_mpeg> at commit
`c871f2be022ece7ef4f64230b4fb8e1fb9eb6023` (2025-12-30).

Upstream identifies the single-header library as MIT licensed and carries
`SPDX-License-Identifier: MIT` in the source header. ZZ9000 carries one local
robustness patch in `plm_buffer_write()`: a failed `PLM_REALLOC` returns zero
without discarding the original allocation or dereferencing NULL. The adapter
turns that short write into a clean session I/O error. Adapter and
output-conversion code lives outside this directory under GPL-3.0-or-later.

The initial firmware backend enables only the MPEG-PS demuxer and MPEG-1 video
decoder. Audio remains on the existing ZZ9000 audio-session path.
