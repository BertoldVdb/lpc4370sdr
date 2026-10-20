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
  hands them to the sample callback. The library checks every header and the
  chunk's tail marker (the firmware sets it in every packing, older firmware in
  packing 1..3 only): chunks the device overwrote while sending them are
  dropped (`corrupt_chunks`), losses between device and library are counted
  (`unaccounted_chunks`), and after a lost USB packet the library finds the
  chunk boundaries again (`resyncs`). Every gap is listed in the
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
  before it (`pps_uart_quiet_us`): what binds a GNSS module's announcement of the
  next pulse to the right pulse, and shows the module had nothing queued.
  Bytes beyond the 1024 a block's metadata holds are counted in `uart_dropped`
  and skipped by the next block's `uart_offset`.
* **Sample rates**: `lpc4370sdr_get_samplerates()` lists the rates of the
  sample type in force: the real types run at twice the I/Q rates.
  `lpc4370sdr_set_samplerate()` takes an index into that list or a rate from
  it (any other rate from 1 MHz goes to the device, which may have it outside
  its list); while streaming the device restarts the stream
  (`stream_restarts`).
* **Gap fill** (`lpc4370sdr_set_gap_fill()`, off by default): missing samples
  are replaced by zeros, up to one USB buffer's worth per block, so a sample's
  position in the stream is its time. `dropped_samples` counts the samples that
  are still missing.
* **Antenna input** (`lpc4370sdr_set_rf_port()`, `lpc4370sdr_rx -P`): selects
  the HydraSDR RFOne's input (0 = ANT, 1 = CABLE1, 2 = CABLE2), with the
  lpc4370sdr or the HydraSDR firmware; refused on the Airspy boards.
* **Device status and control**: stream counters, watchdog, auxiliary UART,
  crystal calibration, memory read/write, function calls and USB SOF tagging
  (vendor requests 0x80..0x8A of the lpc4370sdr firmware), and
  `lpc4370sdr_reset()`, which reboots the device (vendor request 0): it
  enumerates again, so close the handle and open the device anew.
* **Uncached USB buffers**: where the zero-copy USB buffers are slow to read
  (USB controllers that are not IO coherent, such as the Raspberry Pi's
  dwc_otg), the stream uses ordinary buffers instead and lets the kernel copy
  into them - cheaper than reading uncached memory from user space. The
  library measures this when it allocates the buffers;
  `LPC4370SDR_NO_ZEROCOPY` skips zero-copy outright.

**libairspy compatibility**: `-DBUILD_AIRSPY_COMPAT=ON` also builds
`libairspy.so.0` (`airspy.dll` on Windows, MinGW builds included) with the libairspy 1.0.12 API on top
of this library, so programs built for libairspy (gqrx, SDR++, SDR#, the
`airspy_*` tools, ...) run unchanged and also open the HydraSDR RFOne. It is
installed to `lib/lpc4370sdr-airspy` (and the headers to
`include/lpc4370sdr-airspy/libairspy`), out of the way of a real libairspy:

```
LD_LIBRARY_PATH=/usr/local/lib/lpc4370sdr-airspy gqrx
```

The sample callback gets blocks of the size libairspy delivers. Gap fill is on;
what the library leaves out of a longer gap is filled with zeros up to the end
of the block being gathered (in the I/Q types from where the conversion
filter's output passes the gap: `lpc4370sdr_get_conversion_delay()` samples
after its place), and the rest counts in the next block's
`dropped_samples`, so a sample's position in the stream plus the
`dropped_samples` before it is its time. `airspy_lib_version()` reports 1.0.12.
This is not Airspy's library.

Where it differs from libairspy: `airspy_set_sample_type()` while streaming
restarts the stream with the new type (the samples of the restart are missing
and not counted in `dropped_samples`); from the sample callback it returns
`AIRSPY_ERROR_BUSY`. `airspy_close()` from the sample callback stops the stream
and returns success; the device is closed by the next `airspy_open*()` or
`airspy_exit()`. `airspy_set_receiver_mode()` (exported by libairspy though not
in `airspy.h`) with `RECEIVER_MODE_OFF` stops the stream. Like libairspy, the
library on Linux also exports the I/Q converters (`iqconverter_float_*`,
`iqconverter_int16_*`), `HB_KERNEL_FLOAT`/`HB_KERNEL_INT16` and the gain
tables; their headers are not installed, and the converter structs are not
libairspy's.

Tools: `lpc4370sdr_info`, `lpc4370sdr_rx`, `lpc4370sdr_lib_version`,
`lpc4370sdr_calibrate`, `lpc4370sdr_debug`, `lpc4370sdr_extcheck`,
`lpc4370sdr_spiflash`, `lpc4370sdr_r820t`, `lpc4370sdr_si5351c`,
`lpc4370sdr_gpio`, `lpc4370sdr_gpiodir`.

**Versions**: while the major version is 0, every minor version may break
binary compatibility (the notes in `lpc4370sdr.h` say which did), and the
shared library's SONAME carries both: `liblpc4370sdr.so.0.4`. Programs built
against another minor version need rebuilding; revisions stay compatible.

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
gives the user logged in at the seat (`TAG+="uaccess"`) and the `plugdev` group,
where it exists, access to the devices; it goes to `/etc/udev/rules.d`, or
`-DUDEV_RULES_PATH=...` (packages: `/usr/lib/udev/rules.d`).

Programs find the library with pkg-config (`liblpc4370sdr`); the static one
(`liblpc4370sdr.a`) links with `pkg-config --static --libs liblpc4370sdr`.

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

This is the supported Windows build. A program that links the static library
defines `LPC4370SDR_STATIC` (CMake does that for the `lpc4370sdr-static`
target); otherwise `lpc4370sdr.h` declares the API imported from the DLL.

`liblpc4370sdr/vc` holds the Visual Studio projects inherited from libairspy
(the library and most tools; not `lpc4370sdr_debug` or `lpc4370sdr_extcheck`),
untested. They bring no libusb or pthreads: put the libusb 1.0.20 (or later)
Windows binaries in `libusb-1.0.20` next to `liblpc4370sdr` (`include\libusb-1.0`,
`MS32\dll`, `MS64\dll`) and pthreads-win32 2.9.1 in `libpthread-2-9-1-win`
(`include`, `lib\x86`, `lib\x64`), and copy `libusb-1.0.dll` and
`pthreadVC2.dll` (`pthreadVCE2.dll` for Win32) next to the executables.

## Origin and licenses

Derived from libairspy (https://github.com/airspy/airspyone_host) by Benjamin
Vernoux and Youssef Touil, itself based on the HackRF project. This project is
not endorsed by Airspy, HydraSDR or Great Scott Gadgets.

* `liblpc4370sdr/src/lpc4370sdr.c`, `lpc4370sdr.h`, `lpc4370sdr_commands.h` and
  `liblpc4370sdr/src/CMakeLists.txt`: BSD 3-clause, see `liblpc4370sdr/LICENSE.md`.
* `liblpc4370sdr/CMakeLists.txt` keeps the GPL 2.0 (or later) header it had in
  libairspy: it is a build script, the library it builds is not under it. The
  modules in `cmake/modules` carry their own terms.
* `liblpc4370sdr/src/iqconverter_*` and `filters.h`: MIT. These are the
  versions from libairspy before June 2025 (commit `bd15be3`); the later
  versions carry different terms.
* `lpc4370sdr-tools`: GPL 2.0, see `lpc4370sdr-tools/LICENSE.md`.
* `getopt`: LGPL 2.1.
