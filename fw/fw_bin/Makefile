# Hey Emacs, this is a -*- makefile -*-
#
# Copyright 2013-2024 Benjamin Vernoux <bvernoux@hydrasdr.com>
# Copyright (c) 2026 Bertold Van den Bergh <vandenbergh@bertold.org> (modifications for lpc4370sdr)
#
# This file is part of lpc4370sdr (derived from HydraSDR).
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2, or (at your option)
# any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; see the file COPYING.  If not, write to
# the Free Software Foundation, Inc., 51 Franklin Street,
# Boston, MA 02110-1301, USA.
#

BINARY = lpc4370sdr_fw

SRC_DIR = m4

SRC = m4.c adchs.c timing.c

OBJ = m0_bin.o m0s_bin.o

# Source from ../common dir
VPATH += ../common
SRC += \
	fault_handler.c \
	core.c \
	si5351c.c \
	w25q80bv.c \
	rom_iap.c \
	signal_mcu.c \
	lpc4370sdr_rfone_conf.c \
	lpc4370sdr_mini_conf.c \
	watchdog.c

LDSCRIPT = ../common/LPC4370_M4_rom_to_ram.ld

# The build options of the M0 image (m0_bin.s), which the M4's must match
M0_OPTS = ../m0/.opts

%.o: ../$(SRC_DIR)/%.c  Makefile
	@printf "  CC      $(subst $(shell pwd)/,,$(@))\n"
	$(Q)$(CC) $(CFLAGS) -o $@ -c $<

include ../common/Makefile_inc.mk

# The M0 and M0s images are pulled in with .incbin
m0_bin.o: ../m0/m0.bin
m0s_bin.o: ../m0s/m0s.bin
../m0/m0.bin: ;
../m0s/m0s.bin: ;

# RAM-only image for the LPC4370 ROM DFU mode, from the same objects (the build options go along)
dfu:
	$(Q)$(MAKE) BINARY=lpc4370sdr_dfu LDSCRIPT=../common/LPC4370_M4_dfu.ld lpc4370sdr_dfu.dfu

.PHONY: dfu
