# SPDX-License-Identifier: GPL-3.0-or-later
#
# Differential oracle runner build for ZZ9000 MiniGL wire v9.

CXX ?= g++
CC ?= gcc
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2
CFLAGS ?= -std=c99 -Wall -Wextra -O2

ROOT := ../../..
FIRMWARE := $(ROOT)/ZZ9000_proto.sdk/ZZ9000OS/src

CPPFLAGS := -iquote $(FIRMWARE) -I. -DMINIGL_HOST_TEST

MW_LIB ?= ../libmw_host.a

all: oracle_runner

$(MW_LIB):
	$(MAKE) -C .. libmw_host.a

oracle_runner: oracle_runner.cpp $(MW_LIB)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(MW_LIB) -lm -o $@

clean:
	rm -f oracle_runner

.PHONY: all clean
