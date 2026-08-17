# Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

# Vivado 2018.3 does not propagate a project SCOPED_TO_REF constraint into a
# block-design module-reference OOC run. This tracked pre-hook is attached to
# that discovered run before launch, avoiding generated-XCI edits.
set hook_dir [file dirname [file normalize [info script]]]
read_xdc -unmanaged [file join $hook_dir ZZ9000_proto.srcs constrs_1 new video_formatter_ooc_timing.xdc]
