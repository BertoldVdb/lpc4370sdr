lpc4370sdr host software
========================

Host library (`liblpc4370sdr`) and command-line tools for SDR receivers built
around the NXP LPC4370:

| Device         | Firmware                                  | USB ID      | Serial number        |
|----------------|-------------------------------------------|-------------|----------------------|
| Airspy R2      | lpc4370sdr, or the Airspy firmware        | `1d50:60a1` | `AIRSPY SN:<hex>`    |
| Airspy Mini    | lpc4370sdr, or the Airspy firmware        | `1d50:60a1` | `AIRSPY SN:<hex>`    |
| HydraSDR RFOne | lpc4370sdr                                | `1d50:60a1` | `HYDRASDR SN:<hex>`  |
| HydraSDR RFOne | the HydraSDR firmware                     | `38af:0001` | `HYDRASDR SN:<hex>`  |

The library is derived from libairspy (`airspyone_host`); its API is libairspy's
with the `airspy_` prefix changed to `lpc4370sdr_` (and `AIRSPY_` to
`LPC4370SDR_`). On top of it:

* **Devices**: both USB IDs and both serial number forms above. The frequency is
  sent as 64 bits to firmware that reports a version string starting with
  "HydraSDR" (the HydraSDR firmware needs it; the Airspy firmware stops on it).
* **Packing** (`lpc4370sdr_set_packing()`): 0 = 16-bit, 1 = 12-bit packed,
  2 = 12-bit packed with a chunk counter and the ADC status per chunk, 3 = 8-bit.
* **Framing** (`lpc4370sdr_set_framing()`, on by default when the firmware
  supports it): every USB chunk carries a header with its sample index, the
  device's lost and overrun counts, the tuner frequency, the last PPS edge, USB
  SOF tags and bytes from the auxiliary UART. `lpc4370sdr_transfer_get_metadata()`
  hands them to the sample callback. The library checks every header and, in
  packing 1..3, the chunk's tail marker: chunks the device overwrote while
  sending them are dropped (`corrupt_chunks`), losses between device and library
  are counted (`unaccounted_chunks`), and after a lost USB packet the library
  finds the chunk boundaries again (`resyncs`). Every gap is listed in the
  metadata with its position in the block and its length (`gaps`), so the
  device sample index of every sample is known.
  The header (version 2, the firmware's) sends its slowly growing counters
  modulo 2^16 and the chunk index in 32 bits; the library extends them, so the
  metadata's counters and sample indices are whole. Its extension items reach
  the application as `ext[]` areas with their chunk's sample index, read with
  `lpc4370sdr_ext_next()`; items the library does not know pass through, so new
  firmware fields need no library change. `lpc4370sdr_extcheck` streams from a
  board and checks all of it chunk by chunk (with `-T`, against a test build's
  pattern), across packing modes, losses (`-l`) and stream restarts (`-R`).
  The UART bytes are numbered from the start of the stream (`uart_offset`), and
  each PPS edge is placed among them (`pps_uart_count`, the bytes before it,
  counted by the device at the edge) with the time the line had been quiet
  before it (`pps_uart_quiet`): what binds a GNSS module's announcement of the
  next pulse to the right pulse, and shows the module had nothing queued.
* **Gap fill** (`lpc4370sdr_set_gap_fill()`, off by default): missing samples
  are replaced by zeros, up to one USB buffer's worth per block, so a sample's
  position in the stream is its time. `dropped_samples` counts the samples that
  are still missing.
* **Antenna input** (`lpc4370sdr_set_rf_port()`, `lpc4370sdr_rx -P`): selects
  the HydraSDR RFOne's input (0 = ANT, 1 = CABLE1, 2 = CABLE2), with the
  lpc4370sdr or the HydraSDR firmware; refused on the Airspy boards.
* **Device status and control**: stream counters, watchdog, auxiliary UART,
  crystal calibration, memory read/write, function calls and USB SOF tagging
  (vendor requests 0x80..0x8A of the lpc4370sdr firmware).
* **Uncached USB buffers**: where the zero-copy USB buffers are slow to read
  (USB controllers that are not IO coherent, such as the Raspberry Pi's
  dwc_otg), the stream uses ordinary buffers instead and lets the kernel copy
  into them - cheaper than reading uncached memory from user space. The
  library measures this when it allocates the buffers;
  `LPC4370SDR_NO_ZEROCOPY` skips zero-copy outright.

**libairspy compatibility**: `-DBUILD_AIRSPY_COMPAT=ON` also builds
`libairspy.so.0` (`airspy.dll` on Windows) with the libairspy 1.0.12 API on top
of this library, so programs built for libairspy (gqrx, SDR++, SDR#, the
`airspy_*` tools, ...) run unchanged and also open the HydraSDR RFOne. It is
installed to `lib/lpc4370sdr-airspy` (and the headers to
`include/lpc4370sdr-airspy/libairspy`), out of the way of a real libairspy:

```
LD_LIBRARY_PATH=/usr/local/lib/lpc4370sdr-airspy gqrx
```

The sample callback gets blocks of the size libairspy delivers. Gap fill is on;
what the library leaves out of a longer gap is filled with zeros up to the end
of the block being gathered, and the rest counts in the next block's
`dropped_samples`, so a sample's position in the stream plus the
`dropped_samples` before it is its time. `airspy_lib_version()` reports 1.0.12.
This is not Airspy's library.

Tools: `lpc4370sdr_info`, `lpc4370sdr_rx`, `lpc4370sdr_lib_version`,
`lpc4370sdr_calibrate`, `lpc4370sdr_debug`, `lpc4370sdr_spiflash`,
`lpc4370sdr_r820t`, `lpc4370sdr_si5351c`, `lpc4370sdr_gpio`,
`lpc4370sdr_gpiodir`.

## Building

### Linux

Prerequisites: a C compiler, CMake, pkg-config and libusb-1.0 (Debian/Ubuntu:
`build-essential cmake pkg-config libusb-1.0-0-dev`).

```
mkdir build
cd build
cmake .. -DINSTALL_UDEV_RULES=ON
make
sudo make install
sudo ldconfig
```

`-DINSTALL_UDEV_RULES=ON` installs `lpc4370sdr-tools/52-lpc4370sdr.rules`, which
gives the `plugdev` group access to the devices.

### Windows (MSYS2/mingw64)

```
pacman -S git mingw-w64-x86_64-make mingw-w64-x86_64-pkgconf mingw-w64-x86_64-gcc mingw-w64-x86_64-libusb mingw-w64-x86_64-cmake
mkdir build
cd build
cmake .. -G "MinGW Makefiles"
mingw32-make.exe
```

Copy `libusb-1.0.dll` and `libwinpthread-1.dll` from `/mingw64/bin` next to the
executables if they are not in the path. Run the tools from a Windows command
shell: in MSYS2 shells Ctrl-C does not stop `lpc4370sdr_rx` cleanly.

`liblpc4370sdr/vc` holds the Visual Studio projects inherited from libairspy.

## Origin and licenses

Derived from libairspy (https://github.com/airspy/airspyone_host) by Benjamin
Vernoux and Youssef Touil, itself based on the HackRF project. This project is
not endorsed by Airspy, HydraSDR or Great Scott Gadgets.

* `liblpc4370sdr/src/lpc4370sdr.c`, `lpc4370sdr.h`, `lpc4370sdr_commands.h` and
  the CMake files: BSD 3-clause, see `liblpc4370sdr/LICENSE.md`.
* `liblpc4370sdr/src/iqconverter_*` and `filters.h`: MIT. These are the
  versions from libairspy before June 2025 (commit `bd15be3`); the later
  versions carry different terms.
* `lpc4370sdr-tools`: GPL 2.0, see `lpc4370sdr-tools/LICENSE.md`.
* `getopt`: LGPL 2.1.
