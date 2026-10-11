lpc4370sdr
==========

Firmware and host software for SDR receivers built around the NXP LPC4370
(Cortex-M4 with two Cortex-M0 cores and the 80 MS/s ADCHS). Supported boards:

* HydraSDR RFOne
* Airspy R2
* Airspy Mini

Components
----------

* [`fw`](fw/): the firmware. One image runs on every supported board and
  detects the board at boot. See [`fw/README.md`](fw/README.md) for the host
  protocol, building and flashing.
* [`host`](host/): the host library and command-line tools. See
  [`host/README.md`](host/README.md) for the API, the libairspy compatibility
  library and building on Linux and Windows.
  * [`host/liblpc4370sdr`](host/liblpc4370sdr/): the host library, derived from
    libairspy.
  * [`host/lpc4370sdr-tools`](host/lpc4370sdr-tools/): the command-line tools
    (`lpc4370sdr_info`, `lpc4370sdr_rx` and others).

Getting started
---------------

1. Build the firmware and load it onto the board, following
   [`fw/README.md`](fw/README.md).
2. Build and install the host library and tools, following
   [`host/README.md`](host/README.md).
3. Run `lpc4370sdr_info` to check that the board is found.

Licenses
--------

The firmware is licensed under the GNU GPL version 2, see
[`fw/LICENSE`](fw/LICENSE). The host library is BSD 3-clause with MIT
components, see [`host/liblpc4370sdr/LICENSE.md`](host/liblpc4370sdr/LICENSE.md).
The host tools are licensed under the GNU GPL version 2, see
[`host/lpc4370sdr-tools/LICENSE.md`](host/lpc4370sdr-tools/LICENSE.md).

This project derives from the Airspy, HydraSDR and HackRF projects. It is not
endorsed by Airspy, HydraSDR or Great Scott Gadgets.
