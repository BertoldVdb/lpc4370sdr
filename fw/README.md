lpc4370sdr firmware
===================

Firmware for SDR receivers built around the NXP LPC4370 (Cortex-M4 with two
Cortex-M0 cores and the 80 MS/s ADCHS). One image runs on:

| Board          | Tuner             | Clock                          | Sample rates (IQ, MSPS)            |
|----------------|-------------------|--------------------------------|------------------------------------|
| HydraSDR RFOne | R828D, 3 RF ports | SI5351C                        | 10, 5, 2.5, 15; alt 12, 8, 6, 4.096 |
| Airspy R2      | R820T2            | SI5351C                        | 10, 2.5, 5, 15; alt 12, 8, 6, 4.096 |
| Airspy Mini    | R820T2            | 24 MHz crystal, no SI5351C     | 6, 3, 12, 10; alt 4.096            |

The board is detected at boot from the tuner's I2C address and the presence of
the SI5351C. RF port selection is refused on the R820T2 boards (their tuner has
a single input).

The firmware derives from the HydraSDR RFOne firmware (rfone_fw, Benjamin
Vernoux), which itself derives from the Airspy and HackRF firmware. Its sources
are licensed under the GNU GPL version 2 or (at your option) any later version,
see `LICENSE`; libopencm3 and the files taken from it (some linker scripts and
headers) are under the GNU LGPL version 3 or later. The firmware binaries link
libopencm3 statically, and LGPLv3 code combines with GPLv2-or-later code only
under the GPL version 3: a distributed binary is effectively under the GNU GPL
version 3.

USB identity
------------

Every board enumerates as `1d50:60a1` (Airspy / OpenMoko). The serial number
says what the board is: `AIRSPY SN:<16 hex digits>` on the Airspy boards,
`HYDRASDR SN:<16 hex digits>` on the HydraSDR RFOne (the Airspy library only
lists and opens the former; the lpc4370sdr host library opens both). The version string names the board first, for host software
that tells boards apart by it:

    AirSpy NOS (lpc4370sdr v0.1.0 2026-09-28)
    AirSpy MINI (lpc4370sdr v0.1.0 2026-09-28)
    HydraSDR RFOne (lpc4370sdr v0.1.0 2026-09-28)

The Airspy boards report board ID 0, like the Airspy firmware. The version
itself comes from `m0/m0.hdr`.

Host protocol
-------------

The Airspy/HydraSDR vendor requests are kept (commands 0..28, with
`SET_RF_PORT` at 28; `SET_FREQ` takes a 4- or 8-byte frequency). Additions,
see `common/lpc4370sdr_commands.h`:

* Packing (`SET_PACKING`): 0 = 16-bit, 1 = 12-bit packed, 2 = 12-bit packed
  with a chunk counter and the ADC status at the start of every chunk,
  3 = 8-bit.
* Vendor requests 0x80..0x8A: stream status, framing, watchdog, auxiliary UART
  baud rate and write, crystal calibration, memory read/write, function call
  (memory write and function call only in a build with `DEBUG_REQUESTS=1`, the
  default) and USB SOF tagging.
* Framing: every USB chunk starts with a 96-byte header
  (`lpc4370sdr_frame_header_t`, version 2) carrying the chunk index, lost and
  overrun counts, the tuner frequency, the last PPS edge and USB SOF tags, and
  bytes received on the auxiliary UART, and a stream start counter that changes
  when a setting change (sample rate, packing, framing) restarted the stream.
  Counters that only grow go out modulo 2^16 and the edge's and SOF's sample
  indices as their low words; the host extends them, and takes the chunk's
  sample index as the chunk index times its samples. A check word covers the
  header. 31 bytes are extension items - type, length, data - that a host
  passes on without knowing them: new fields go there without a host change.
  `LPC4370SDR_EXT_HEALTH` (stream health) rides every 128th chunk. A test
  build, `EXTRA_CFLAGS=-DLPC4370SDR_EXT_TEST`, fills every chunk's items with
  a pattern the host can check byte for byte (`lpc4370sdr_extcheck -T` in
  `host/`) and starts the 16-bit counters just short of their wrap.
  The PPS edge comes with the auxiliary UART's bytes at the edge: the UART
  interrupt counts the bytes it takes from the FIFO and stamps them with the
  PPS timer, and the PPS capture takes both, so the header gives the edge's
  place among the bytes (`pps_uart_rel`) and how long the line had been quiet
  before it (`pps_uart_quiet_us`, 0 when a byte arrived at the edge) - what
  binds a GNSS module's announcement of the next pulse to the right pulse.
  The payload ends with a tail marker (`LPC4370SDR_FRAME_FLAG_TAIL`, every
  packing mode) that tells a host whether the chunk lost USB packets or was
  overwritten in the device while it was being sent. The sample index is 64-bit
  and keeps counting when the 32-bit chunk index wraps.
* Framing and SOF tagging survive the restart a setting change performs; they
  are cleared when the receiver is switched off.
* A USB bus reset or `SET_CONFIGURATION(0)` switches the receiver off. The
  configuration has a single bulk IN endpoint (0x81).
* `SPIFLASH_READ` reads the flash only (it stalls past its end); `MEM_READ`
  reads memory.
* While a tuner request is still running in the main loop (a host gave up on
  it), the requests that restart the receiver (`RECEIVER_MODE` RX, and sample
  rate, packing and framing changes while streaming) stall.
* When USB cannot keep up, the device drops whole chunks (counted as lost, and
  visible as gaps in the chunk index) instead of sending chunks it is already
  overwriting.

Hosts must clear the bulk IN endpoint halt (`libusb_clear_halt`) before
starting a stream; the Airspy and HydraSDR host libraries do.

Directory structure
-------------------

See `lpc4370sdr-firmware-architecture.md`.

Building
--------

Prerequisites: `arm-none-eabi-gcc` and newlib, GNU make (4.2 or later),
Python 3 (`python3`).

    make -C libopencm3
    make
    make -C fw_bin dfu

`PREFIX=...` selects another toolchain (default `arm-none-eabi`). Options, given
to the second and the third command alike, e.g. `make NOFLASH=1` and
`make -C fw_bin dfu NOFLASH=1`:

* `NOFLASH=1`: for boards without SPI flash. The flash requests stall and the
  Airspy Mini gets its auxiliary UART on the flash pads (P3_5 RX, P3_4 TX).
* `WATCHDOG=1`: host watchdog, see the `WATCHDOG` vendor request.
* `DEBUG_REQUESTS=0`: leaves out the `MEM_WRITE` and `CALL` vendor requests
  (`MEM_READ` stays). They let anyone with access to the USB device write its
  memory and run code on it; the default (`DEBUG_REQUESTS=1`) keeps them, for
  development and for tools such as `lpc4370sdr_debug`.
* `EXTRA_CFLAGS=...`: test builds, e.g.
  `EXTRA_CFLAGS=-DLPC4370SDR_STREAM_COUNTER_START=0xFFFFF000u` starts the chunk
  counters just short of their 32-bit wrap.

Changing any option rebuilds everything (`.opts` in each build directory
records the options in effect). `fw_bin` links in the M0 image built by `make`:
it stops with an error when its options are not the ones `m0` was built with,
rather than build an M4 image that does not match it.

Outputs in `fw_bin`: `lpc4370sdr_fw.bin` (SPI flash image) and
`lpc4370sdr_dfu.dfu` (runs from RAM, loaded over USB DFU).

Loading and flashing
--------------------

Run from RAM, until the next power cycle: with the LPC4370 in its ROM USB DFU
mode (boards without SPI flash always boot into it; on the RFOne set `SW2` to
`1-2`):

    dfu-util -d 1fc9:000c -t 2048 -R -D fw_bin/lpc4370sdr_dfu.dfu

The "unable to read DFU status" message at the end is normal; the board comes
up as `1d50:60a1` a few seconds later.

Write to SPI flash, from a running firmware, with the lpc4370sdr host tools
(`host/lpc4370sdr-tools`; the Airspy library does not list the RFOne):

    lpc4370sdr_spiflash -w fw_bin/lpc4370sdr_fw.bin

The firmware erases only the flash's first 64 KB sector, so only images of up
to 64 KB can be written.

Checking the version
--------------------

    lpc4370sdr_info

prints the version string above for every connected board.
