#!/usr/bin/env python3
"""Apply only the pre-73c309a portSelect implementation to a disposable RTL copy."""
import sys
from pathlib import Path

path = Path(sys.argv[1])
source = path.read_text()
source = source.replace(
    "reg  [ 3 : 0]               portSelect;",
    "reg [`MAX($clog2(NPRT), 1) - 1 : 0]  portSelect;",
    1,
)
old = """if ((portSelect + 1) < NPRT)
                    begin
                        portSelect <= portSelect + 1;
                    end
                    else
                    begin
                        portSelect <= 0;
                    end"""
if old not in source:
    raise SystemExit("expected post-73c309a portSelect wrap")
source = source.replace(old, "portSelect <= portSelect + 1;", 1)
path.write_text(source)
