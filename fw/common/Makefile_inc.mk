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

LPC4370SDR_OPTS = -DLPC43XX -DLPC43XX_M4 -DCORE_M4

LDSCRIPT ?= ../common/LPC4370_M4.ld

LIBOPENCM3 ?= ../libopencm3

PREFIX ?= arm-none-eabi
CC = $(PREFIX)-gcc
LD = $(PREFIX)-gcc
OBJCOPY = $(PREFIX)-objcopy
OBJDUMP = $(PREFIX)-objdump
GDB = $(PREFIX)-gdb

#CFLAGS += -std=gnu99 -O3 -g3 -Wall -Wextra -I$(LIBOPENCM3)/include -I../common \
#		-fno-common -mcpu=cortex-m4 -mthumb -MD \
#		-mfloat-abi=hard -mfpu=fpv4-sp-d16 \
#		$(LPC4370SDR_OPTS)
CFLAGS += -std=gnu99 -O2 -g2 -Wall -Wextra -I$(LIBOPENCM3)/include -I../common \
		-fno-common -mcpu=cortex-m4 -mthumb -DTHUMB -MD -flto -ffunction-sections -fdata-sections \
		-mfloat-abi=hard -mfpu=fpv4-sp-d16 \
		$(LPC4370SDR_OPTS)
LDFLAGS += -mcpu=cortex-m4 -mthumb -DTHUMB -mfloat-abi=hard -mfpu=fpv4-sp-d16 \
		-L../common \
		-L$(LIBOPENCM3)/lib -L$(LIBOPENCM3)/lib/lpc43xx \
		-T$(LDSCRIPT) -nostartfiles --specs=nano.specs -lc -lnosys \
		-Wl,--gc-sections -Xlinker -Map=$(BINARY).map
OBJ += $(SRC:.c=.o)

# make NOFLASH=1: board without the SPI flash
ifeq ($(NOFLASH),1)
CFLAGS += -DLPC4370SDR_NO_FLASH=1
endif
# The build options in effect: .opts changes (and every object rebuilds) when they do. A file
# that always exists, since .SECONDARY would let a missing stamp go unnoticed.
BUILD_OPTS := f$(NOFLASH)_x$(EXTRA_CFLAGS)
# make EXTRA_CFLAGS=...: test builds, e.g. -DLPC4370SDR_STREAM_COUNTER_START=0xFFFFF000u
CFLAGS += $(EXTRA_CFLAGS)
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

program: $(BINARY).dfu
	$(Q)dfu-util -d 1fc9:000c -t 2048 -R -D $(BINARY).dfu

%.images: %.bin %.hex %.srec %.list
	@#echo "*** $* images generated ***"

# LPC43xx ROM DFU image: 16-byte boot header
%.dfu: %.bin
	$(Q)rm -f _header.bin
	$(Q)python -c "import os,struct,sys; d=struct.pack('<BBH8sI',0x1a,0x3f,(os.path.getsize('$(*).bin')+511)//512,b'',0xffffffff); getattr(sys.stdout,'buffer',sys.stdout).write(d)" > _header.bin
	$(Q)cat _header.bin $(*).bin >$(*).dfu
	$(Q)rm -f _header.bin

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
	$(Q)$(LD) $(LDFLAGS) -o $(*).elf $(OBJ) -lopencm3_lpc43xx

%.o: %.c Makefile
	@#printf "  CC      $(subst $(shell pwd)/,,$(@))\n"
	$(Q)$(CC) $(CFLAGS) -o $@ -c $<

%.o: %.s Makefile
	@#printf "  CC      $(subst $(shell pwd)/,,$(@))\n"
	$(Q)$(CC) $(CFLAGS) -o $@ -c $<

clean:
	$(Q)rm -f *.o
	$(Q)rm -f .opts .opts_*
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

.PHONY: images clean

-include $(OBJ:.o=.d)

# Recreates it after a clean in the same make run
$(OPTS_STAMP):
	$(file > $@,$(BUILD_OPTS))
$(OBJ): $(OPTS_STAMP)
