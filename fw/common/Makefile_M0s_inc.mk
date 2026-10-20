# Hey Emacs, this is a -*- makefile -*-
#
# Copyright 2009 Uwe Hermann <uwe@hermann-uwe.de>
# Copyright 2010 Piotr Esden-Tempski <piotr@esden.net>
# Copyright 2012 Michael Ossmann <mike@ossmann.com>
# Copyright 2012 Jared Boone <jared@sharebrained.com>
# Copyright 2013-2024 Benjamin Vernoux <bvernoux@hydrasdr.com>
# Copyright (c) 2026 Bertold Van den Bergh <vandenbergh@bertold.org> (modifications for lpc4370sdr)
#
# This file is part of lpc4370sdr (derived from HydraSDR, based on the HackRF project).
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
# derived primarily from Makefiles in libopencm3

LPC4370SDR_OPTS = -DLPC43XX -DLPC43XX_M0S -DCORE_M0 -D__CORTEX_M=0

LDSCRIPT ?= ../common/LPC4370_M0s_ram_only.ld

LIBOPENCM3 ?= ../libopencm3

PREFIX		?= arm-none-eabi
CC = $(PREFIX)-gcc
LD = $(PREFIX)-gcc
OBJCOPY		= $(PREFIX)-objcopy
OBJDUMP		= $(PREFIX)-objdump
GDB		= $(PREFIX)-gdb

CFLAGS += -std=gnu99 -Os -g2 -Wall -Wextra -I$(LIBOPENCM3)/include -I../common \
		 -mthumb -DTHUMB -mcpu=cortex-m0 -MD -flto -ffunction-sections -fdata-sections \
        $(LPC4370SDR_OPTS)

LDFLAGS += -mcpu=cortex-m0 -mthumb -DTHUMB \
        -L../common \
		-L$(LIBOPENCM3)/lib -L$(LIBOPENCM3)/lib/lpc43xx_m0s \
		-T$(LDSCRIPT) -nostartfiles -lc -lnosys \
		-Wl,--gc-sections -Xlinker -Map=$(BINARY).map

OBJ += $(SRC:.c=.o)

# make DEBUG_REQUESTS=0, see Makefile_M0_inc.mk
DEBUG_REQUESTS ?= 1
ifeq ($(DEBUG_REQUESTS),0)
CFLAGS += -DLPC4370SDR_NO_DEBUG_REQUESTS
endif
# The build options in effect: .opts changes (and every object rebuilds) when they do
BUILD_OPTS := d$(DEBUG_REQUESTS)
OPTS_STAMP = .opts
ifneq ($(file < .opts),$(BUILD_OPTS))
$(file > .opts,$(BUILD_OPTS))
endif

# Be silent per default, but 'make V=1' will show all compiler calls.
ifneq ($(V),1)
Q := @
NULL := 2>/dev/null
else
LDFLAGS += -Wl,--print-gc-sections
endif

.SUFFIXES: .elf .bin .hex .srec .list .images
.SECONDEXPANSION:
.SECONDARY:

.DEFAULT_GOAL := all
all: images

images: $(BINARY).images
flash: $(BINARY).flash

%.images: %.bin %.hex %.srec %.list
	@#echo "*** $* images generated ***"

%.bin: %.elf
	@#printf "  OBJCOPY $(*).bin\n"
	$(Q)$(OBJCOPY) -Obinary $(*).elf $(*).bin

%.hex: %.elf
	@#printf "  OBJCOPY $(*).hex\n"
	$(Q)$(OBJCOPY) -Oihex $(*).elf $(*).hex

%.srec: %.elf
	@#printf "  OBJCOPY $(*).srec\n"
	$(Q)$(OBJCOPY) -Osrec $(*).elf $(*).srec

%.list: %.elf
	@#printf "  OBJDUMP $(*).list\n"
	$(Q)$(OBJDUMP) -S $(*).elf > $(*).list

%.elf: $(OBJ) $(LDSCRIPT)
	@#printf "  LD      $(subst $(shell pwd)/,,$(@))\n"
	$(Q)$(LD) $(LDFLAGS) -o $(*).elf $(OBJ) -lopencm3_lpc43xx_m0s

%.o: %.c Makefile
	@#printf "  CC      $(subst $(shell pwd)/,,$(@))\n"
	$(Q)$(CC) $(CFLAGS) -o $@ -c $<

%.o: %.s Makefile
	@#printf "  CC      $(subst $(shell pwd)/,,$(@))\n"
	$(Q)$(CC) $(CFLAGS) -o $@ -c $<

clean:
	$(Q)rm -f *.o
	$(Q)rm -f .opts
	$(Q)rm -f *.d
	$(Q)rm -f *.elf
	$(Q)rm -f *.bin
	$(Q)rm -f *.dfu
	$(Q)rm -f _tmp.dfu _header.bin
	$(Q)rm -f *.hex
	$(Q)rm -f *.srec
	$(Q)rm -f *.list
	$(Q)rm -f *.map
	$(Q)rm -f *.lst
	$(Q)rm -f ../common/*.o
	$(Q)rm -f ../common/*.d
	$(Q)rm -f ../common/*.lst

MAKE_ALL_RULE_HOOK:

FORCE:
.PHONY: FORCE images clean

# Anything that depends on FORCE will be considered out-of-date

-include $(OBJ:.o=.d)

# Recreates it after a clean in the same make run
$(OPTS_STAMP):
	$(file > $@,$(BUILD_OPTS))
$(OBJ): $(OPTS_STAMP)
