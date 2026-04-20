# ZZ9000 ZYNQ FPGA + ZZ9000OS ARM Code — BlitterStudio fork

> **Fork notice.** This repository is an independent fork and continued
> development of the original MNT ZZ9000 FPGA/firmware sources. It is
> maintained by Dimitris Panokostas / **BlitterStudio** and is **not
> affiliated with, endorsed by, or supported by MNT Research GmbH**.
> The ZZ9000 hardware itself is designed and manufactured by MNT
> Research GmbH — hardware questions belong with them; firmware issues
> and fork-specific discussion belong here.
>
> Upstream (pre-fork): https://source.mnt.re/amiga/zz9000-firmware

The ZZ9000 is a graphics and ARM coprocessor card for Amiga computers equipped with Zorro slots. It is based on a Xilinx ZYNQ Z-7020 chip that combines 7-series FPGA fabric with dual ARM Cortex-A9 CPUs clocked at 666MHz. The current version has 1GB of DDR3 RAM and no eMMC soldered.

This repository contains the sources for the FPGA logic as well as the miniature "operating system" that runs on the ARM core 0 and instruments core 1 for user applications.

Warning: this is still in the process of cleanup. Many hacks have been made.

# Requirements

Currently requires Xilinx Vivado Webpack version.

# Getting around the Code

The interesting bits:

- `mntzorro.v` is the Zorro 2/3 interface and 24-bit video capture engine, AXI4-Lite interface
- `video_formatter.v` is the AXI video stream formatter that reinterprets and controls an incoming 32-bit word stream as 8-bit palette indexed, 16-bit 565 or 24-bit RGBX pixels and outputs a 24-bit true color parallel RGB stream with horizontal and vertical sync
- `zz9000-fw/ZZ9000_proto.sdk/ZZ9000OS/src`
  - `main.c` is the main entrypoint of ZZ9000OS.elf which runs on the ARM core 0 and talks to `MNTZorro` and `video_formatter` in the FPGA fabric
  - `gfx.c` graphics acceleration routines, currently mainly rect fill and copy
  - `ethernet.c` low-level ethernet driver/framer
- `ZZ9000_proto.srcs/constrs_1/new/zz9000.xdc` XDC constraints file that contains the pin/ball mappings and some timing voodoo

![ZZ9000 Block Design](gfx/zz9000-bd.png?raw=true "ZZ9000 Block Design")

# Set up Vivado Project

As Vivado projects are not suitable for version control, the Vivado project / block design is exported as a TCL script `zz9000_project.tcl`.

Start Vivado from your terminal in tcl mode:

```
source /path/to/Xilinx/Vivado/2018.3/settings64.sh
cd /place/where/you/checked/out/zz9000-firmware
vivado -mode tcl
```

Then, execute in the Vivado TCL shell:

```
source zz9000_project.tcl
exit
```

A folder `ZZ9000_proto` should have been created. Start Vivado normally (GUI) and navigate in the Open Project dialog to `zz9000-firmware/ZZ9000_proto` and open `ZZ9000_proto.xpr`. 

After a while you should be able to select "Run Synthesis" in Flow Navigator, and after that completes, "Run Implementation" and finally "Generate Bitstream". Finally, select "Export Hardware" from the "File" menu and check "Include Bitstream". Export to "Local to Project". Then, open the SDK by selecting "Launch SDK" from the "File" menu. Leave everything at "Local to Project" and click OK.

Your SDK workspace will start with the `zz9000_ps_wrapper_hw_platform_0` project.

To recreate a project for ZZ9000OS, go to "File" / "New" / "Application Project". Enter "ZZ9000OS" as the Project name and click "Next". Select "Empty Application". Click "Finish". Right click "ZZ9000OS" in your Project Explorer and select "Import". Select "General" / "File System", click "Next". Select the `ZZ9000_proto.sdk` / `ZZ9000OS` subfolder in your `zz9000-firmware` folder. Check the checkmark next to "ZZ9000OS". Click Finish. Select "Yes to all" in the overwrite dialog. Now you will be able to build ZZ9000OS.

Before the next step, configure your BSP Project. Expand the ZZ9000OS_bsp node and double click `system.mss`. Click "Modify this BSP's Settings". Check `xilffs` in the Supported Libraries.

To recreate the bootloader project, go to "File" / "New" / "Application Project". Enter "ZZ9000FSBL" as the Project name, select "Use existing" under Target Software and click "Next". Select "Zynq FSBL". Click "Finish".

# Building BOOT.bin

The Eclipse-based Vivado SDK has a built-in tool (Menu "Xilinx" / "Create Boot Image") to generate the boot image `BOOT.bin`. To prepare, open `ZZ9000_proto.sdk/ZZ9000OS/bootimage/ZZ9000OS.bif` in a text editor and change the absolute paths to match the correct ELF files on your filesystem.

In the "Create Boot Image" dialog, select "Import from existing BIF file" and select the definition file `ZZ9000_proto.sdk/ZZ9000OS/bootimage/ZZ9000OS.bif` and click "Create Image".

`BOOT.bin` contains 3 files (“partitions”):

- `FSBL.elf` (first stage bootloader, written in C, Xilinx default)
- FPGA bitfile (synthesized by Vivado from Verilog sources and Block Design)
- `ZZ9000OS.elf` (ZZ9000OS ARM executable, written in C)

# Hardware Connectivity

Schematics are in the manual (PDF): https://mntre.com/media/ZZ9000_info_md/zz9000-manual.pdf

- DVI (via non-HDMI compliant HDMI connector) with SiliconImage 9022 encoder
- Gigabit Ethernet port with Micrel KSZ9031 PHY
- MicroSD slot (for firmware loading)
- USB 2.0 port (not yet used)

# License / Copyright

If not stated otherwise in specific source code files, everything here is:

Copyright (C) 2016-2026, Lucie L. Hartmann <lucie@mntre.com>
MNT Research GmbH, Berlin
https://mntre.com

SPDX-License-Identifier: GPL-3.0-or-later
https://spdx.org/licenses/GPL-3.0-or-later.html

Scanline bitstream patches (V1 intensity scanlines and V2 multi-mode
scanlines with parity control) by Xanxi, adapted for
firmware 1.13+ / 1.14 by Dimitris Panokostas (midwan). V2 adds three
patterns (classic / soft / gradient) with odd/even parity, gated to
AGA scandoubled modes and RTG resolutions below 350 lines; it leaves
the V1 intensity registers decoded but unused.

RTG performance optimizations (NEON intrinsics, compiler flags, batch
palette transfer, operation fusion) by Dimitris Panokostas.

USB 2.0 host stack on the ARM side (EHCI + USB command proxy, Zynq
AXI stability fixes — USBMODE_SDIS, ULPI dynamic XCVR switching,
BURSTSIZE, TXFIFO threshold, direct-DMA bulk transfers from the shared
mailbox) by Dimitris Panokostas.

Scanlines V2 block-design integration (the TCL wiring that connects
mntzorro's scanline outputs to video_formatter) and the split
firmware/bitstream/bootimage build scripts by Dimitris Panokostas.

# Making the Platform Cable Work

First, `apt install fxload`

Copy the `xilinx-xusb` folder to `/etc/xilinx-xusb`.

Edit `/etc/udev/rules.d/xusbdfwu.rules` with the following content:

```
# version 0003
ATTRS{idVendor}=="03fd", ATTRS{idProduct}=="0008", MODE="666"
SUBSYSTEM=="usb", ACTION=="add", ATTRS{idVendor}=="03fd", ATTRS{idProduct}=="0007", RUN+="/sbin/fxload -v -t fx2 -I /etc/xilinx-xusb/xusbdfwu.hex -D $tempnode"
SUBSYSTEM=="usb", ACTION=="add", ATTRS{idVendor}=="03fd", ATTRS{idProduct}=="0009", RUN+="/sbin/fxload -v -t fx2 -I /etc/xilinx-xusb/xusb_xup.hex -D $tempnode"
SUBSYSTEM=="usb", ACTION=="add", ATTRS{idVendor}=="03fd", ATTRS{idProduct}=="000d", RUN+="/sbin/fxload -v -t fx2 -I /etc/xilinx-xusb/xusb_emb.hex -D $tempnode"
SUBSYSTEM=="usb", ACTION=="add", ATTRS{idVendor}=="03fd", ATTRS{idProduct}=="000f", RUN+="/sbin/fxload -v -t fx2 -I /etc/xilinx-xusb/xusb_xlp.hex -D $tempnode"
SUBSYSTEM=="usb", ACTION=="add", ATTRS{idVendor}=="03fd", ATTRS{idProduct}=="0013", RUN+="/sbin/fxload -v -t fx2 -I /etc/xilinx-xusb/xusb_xp2.hex -D $tempnode"
SUBSYSTEM=="usb", ACTION=="add", ATTRS{idVendor}=="03fd", ATTRS{idProduct}=="0015", RUN+="/sbin/fxload -v -t fx2 -I /etc/xilinx-xusb/xusb_xse.hex -D $tempnode"
```

This will make udev launch fxload whenever the platform cable is plugged in. Fxload will in turn load those hex files (firmware) into the cable. The LED on the cable should then turn green.

