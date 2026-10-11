lpc4370sdr firmware directory structure and software architecture
===================================================================

* `m0` contains the code for the M0 core, which manages USB (file `m0.c`):
  * Boot initialization (function `main`): board detection, USB init, M0 core
    IRQs (USB0 and M4 core)
  * The R82x tuner (R828D or R820T2), driven by the USB requests in `usb_req.c`
  * The USB requests: `usb_vendor_request()` dispatches the requests set up in
    `usb_req_init()`; requests 0x80 and up have their own table
  * Streaming: the main loop and the bulk IN completion hand the chunks the M4
    finished to the USB controller (`stream_queue_chunks()`), writing the frame
    header of each chunk when framing is on
  * Communication with the M4 core through shared memory and the M4 core IRQ
    (`m4core_isr`)

* `m0s` contains the code for the M0s core (file `m0s.c`); it is free for
  custom use and disabled, see `main` in `m4/m4.c`.

* `m4` contains the code for the M4F core (file `m4.c`):
  * Boot initialization (function `main`): pin setup, system clocks, M4 core
    IRQs (DMA and M0 core), ADC init, booting the M0 core
  * ADC capture (`adchs.c`): the GPDMA fills a ring of slots in local and AHB
    SRAM (`lpc4370sdr_stream.h`); the DMA interrupt counts finished chunks and
    overruns
  * Packing: the main loop packs finished chunks in place to 12 or 8 bits and
    writes the frame tail marker
  * Timing (`timing.c`): PPS and USB SOF capture against the sample count
  * Commands from the M0 core (start/stop, sample rate, packing, framing):
    the M0 core IRQ (`m0core_isr`) only wakes the main loop, which runs them
    between two packed chunks (`m0_commands`)

* `common` contains code for both cores:
  * The board configurations: `lpc4370sdr_rfone_conf.c` (HydraSDR RFOne and
    Airspy R2: SI5351C clocking, sample rate table with the tuner IF and
    filter settings) and `lpc4370sdr_mini_conf.c` (Airspy Mini: 24 MHz clock)
  * `lpc4370sdr_commands.h`: the host protocol (vendor requests, packing
    modes, frame header)
  * Drivers: LPC4370 core and clocks, R82x tuner, SI5351C, USB, SPI flash
    (W25Q80BV/S25FL032P), watchdog

* `fw_bin` links the m0, m0s and m4 images into `lpc4370sdr_fw.bin` (SPI flash)
  and `lpc4370sdr_dfu.dfu` (RAM). `dfuprog_hydrasdr` and `usbprog_hydrasdr`
  hold the Windows flashing tools from the HydraSDR project.

* `libopencm3` contains the peripheral library for the M0, M0s and M4 cores.

* `scripts` contains a Python script that writes the version header from git
  (not used by the build: `m0/m0.hdr` sets the version).

The M0 core is the main manager: it answers USB, so the M4F core stays free for
the signal path.
