lpc4370sdr-tools
================

The lpc4370sdr command-line tools. See the README.md in the directory above for
what they support and how to build them. Every tool prints its options when
given a wrong one; numbers may be given in decimal, `0x` hex or `0b` binary, and
a negative or too large number is refused.

## lpc4370sdr_rx

* `-r -` writes the samples to stdout (in binary mode on Windows too). When the
  reader goes away (`| head -c ...`) the stream stops as on a write error: the
  board is stopped and closed, and the exit status is 1.
* `-a` is the rate in samples per second of the sample type: the real types
  (`-t 1`, `3`, `4`) take the ADC rate, so `-t 3 -a 10000000` streams 10 MS/s
  real, `-t 2 -a 10000000` 10 MS/s IQ. Values up to 1000000 are an index into
  the list `lpc4370sdr_info` shows.
* `-n` counts samples of the sample type: IQ samples with the IQ types. With
  `-t 5` (RAW) it counts ADC samples; in packing 2 the chunk counter and the ADC
  status at the start of every chunk do not count, and in 12-bit packing (`-p 1`
  and `-p 2`) the file ends with the group of 8 values (three 32-bit words, 12
  bytes) the last sample is in: up to 7 samples more.
* `-f` is in MHz, rounded to the Hz, from 24 to 1900 MHz.
* `-B baud` sets the auxiliary UART's baud rate, `-N 1` copies its bytes to
  stderr, `-C ppb` sets the crystal correction for this session (the stored one
  is `lpc4370sdr_calibrate`'s).
* `-w` names the WAV file after the UTC time.
* SIGINT, SIGTERM and SIGHUP, and on Windows Ctrl-C, Ctrl-Break, closing the
  console window and logging off, stop the stream; the WAV header is completed
  and the file closed, also when the stream fails to start.

## lpc4370sdr_spiflash

`-w file` writes the file at the address `-a` (default 0), and it has to fit in
the first 64 KB of the flash: the device can only erase that sector. The tool
reads the whole sector first, puts the file at its address, erases the sector,
writes all 64 KB and verifies them, so the bytes the file does not cover keep
their content. The calibration (`lpc4370sdr_calibrate`, at 0x20000) lies outside
the sector. If writing fails after the erase, the 64 KB the sector was to hold
are saved to `lpc4370sdr_spiflash_sector0.bin`: write them back with
`lpc4370sdr_spiflash -w lpc4370sdr_spiflash_sector0.bin`.

## lpc4370sdr_calibrate

`-w ppb` takes a correction between -1000000 and 1000000 ppb, reads the flash
back to verify it, and `-s` opens a board by its serial number. If the write
fails after the erase, the board has no stored calibration until `-w` is run
again.

## lpc4370sdr_extcheck

Not built with MSVC (it uses POSIX calls MinGW has).

## udev rules

`-DINSTALL_UDEV_RULES=ON` installs `52-lpc4370sdr.rules` to
`/etc/udev/rules.d`, whatever `CMAKE_INSTALL_PREFIX` is; packages set
`-DUDEV_RULES_PATH=/usr/lib/udev/rules.d`. The rules give the user logged in at
the seat access (`TAG+="uaccess"`), and the `plugdev` group where it exists.

Derived from AirSpy host software by Benjamin Vernoux <bvernoux@airspy.com> and
Youssef Touil <youssef@airspy.com> (http://www.airspy.com), which is based on
the HackRF project (http://greatscottgadgets.com/hackrf/).
