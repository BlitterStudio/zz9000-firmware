# Inspect the implemented design after build_bitstream.sh/.ps1. The report is
# intentionally complete so new crossings around the overlay are visible in
# their design-wide context; filter it for "overlay_linebuffer" when auditing
# the three bundled-data handshakes.
file mkdir test/video/build
open_project ZZ9000_proto/ZZ9000_proto.xpr
open_run impl_1
report_cdc -details -file test/video/build/overlay_cdc.rpt
close_project
