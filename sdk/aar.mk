# Locates the AAR (Amiga ARM Runtime) checkout the ARM app SDK headers and
# sources come from. Include it from a Makefile, then use $(AAR_DIR).
#
# The default is sdk/aar, filled by sdk/scripts/fetch-aar.sh at the commit
# pinned in sdk/AAR_REF. To work against your own aar checkout, pass AAR_DIR=.
#
# SPDX-License-Identifier: GPL-3.0-or-later

AAR_MK_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
AAR_DIR ?= $(AAR_MK_DIR)/aar

ifneq ($(MAKECMDGOALS),clean)
ifeq ($(wildcard $(AAR_DIR)/include/zzapp.h),)
$(error AAR not found: $(AAR_DIR)/include/zzapp.h is missing. Run `sh sdk/scripts/fetch-aar.sh` from the firmware root, or set AAR_DIR= to your own aar checkout)
endif
endif
