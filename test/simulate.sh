#!/bin/sh
set -e
iverilog -DSIMULATION=1 -otestbench testbench_video.v ../video_formatter.v
./testbench
