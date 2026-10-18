# tbs5530-macos

A user-space driver for the **TBS5530** USB TV tuner box on **macOS**, written
in C on top of libusb. No kernel extension, no DriverKit, no virtual machine.
It covers the satellite side (DVB-S/S2), streams one channel over HTTP to VLC
and decodes teletext.

## Please read this first

- **This was vibecoded.** It was written with an AI assistant in a single
  session on 18 October 2026, from about 04:21 to 05:10 in the morning
  (roughly 50 minutes, including the first questions about whether this is
  possible at all). It works on one machine with one dish. Keep
  expectations low.
- **The author does not understand what the code does and cannot explain
  it.** Please do not expect answers to issues, questions or requests for
  help. Pull requests may or may not be looked at.
- No warranty of any kind (see `LICENSE`). Not affiliated with TBS
  (TurboSight), Montage or anyone else.

If it does not work for you, the sources and the notes below are all there is.

## What works

Tested on one Apple Silicon Mac with a universal LNB behind a 4-port DiSEqC
switch.

- Loading the FX2 firmware and waking the box (cold and warm start)
- The Montage M88RS6060 satellite demodulator/tuner: DVB-S and DVB-S2
  (QPSK/8PSK), universal LNB, 13/18 V, 22 kHz tone, DiSEqC 1.0 committed
  switch and tone burst
- Streaming the whole transponder over USB (about 65 Mbit/s without errors)
- Picking one service (PAT/PMT) and serving it over HTTP, plus an `.m3u`
  playlist, for VLC or anything else that plays MPEG-TS
- Teletext decoding (pages as plain text or HTML), with the Latin national
  character sets

## What does not

- The terrestrial/cable side (Sony CXD2878: DVB-T/T2/C, ISDB-T, ATSC)
- The CI slot, DiSEqC 1.1/1.2/USALS, unicable, blind scan, channel scan
- Anything not listed under "What works"

## Requirements

- macOS with the Xcode command line tools (`xcode-select --install`)
- [Homebrew](https://brew.sh): `brew install libusb pkg-config`
- The two firmware files, see [firmware/README.md](firmware/README.md)

## Build

    make

There are no prebuilt binaries. Building it yourself avoids Gatekeeper
warnings, because a binary you compile locally is not quarantined.

## Use

Nothing else may be using the box (no other driver, no VM with USB
passthrough). The examples use a public Astra 19.2E transponder; replace the
numbers with your own.

    ./tbs5530 probe                                   # firmware + demod check
    ./tbs5530 tune 11494 H 22000                      # lock and signal report
    ./tbs5530 tune 11494 H 22000 --port B             # through a DiSEqC switch
    ./tbs5530 record 11494 H 22000 10 mux.ts          # whole transponder, 10 s
    ./tbs5530 serve 11494 H 22000 --sid 1234 --vlc    # one service to VLC

`FREQ_MHZ` is the transponder frequency, `H`/`V` the polarisation and the
last number the symbol rate in kS/s. Add `--dvbs` for DVB-S (the default is
DVB-S2). To find service IDs, record a few seconds and run
`ffprobe -show_programs mux.ts`.

While `serve` runs:

- `http://<host>:5530/stream.ts`: the service as MPEG-TS
- `http://<host>:5530/playlist.m3u`: a playlist pointing at it
- `http://<host>:5530/teletext`: teletext index, `/teletext/100` for a page

Teletext from a recording, without the tuner:

    ./tbs5530 ttx mux.ts 100

Teletext national option `110` means Czech/Slovak in the default character
set group and Turkish in the Western European one. The program uses the
default; `--ttx-turkish` switches to Turkish.

`./tbs5530` without arguments lists all commands and options. `-v`, `-vv`
and `-vvv` print progressively more, up to every USB transfer.

## How it works

- **USB bridge.** The box is a Cypress FX2 that boots without code. The
  program uploads `dvb-usb-id5530.fw` through vendor request `0xa0`, waits
  for the box to re-enumerate and then talks to it with the vendor requests
  of the TBS Linux driver (I2C read/write, board control).
- **The packet-size patch.** The TBS firmware's SET_CONFIGURATION handler
  sets the transport stream endpoint to 1024-byte packets, although the
  endpoint is a 512-byte high-speed bulk endpoint. macOS refuses those
  packets (`LIBUSB_ERROR_OVERFLOW`). Before uploading, the program changes
  one byte in its in-memory copy of the firmware so the endpoint stays at
  512. The firmware file is not modified, and the box forgets everything at
  power-off.
- **Demodulator.** `src/m88rs6060.c` is the TBS Linux driver with the parts
  this box does not use removed, otherwise unchanged. `src/linux_shim.*`
  provides the small part of the kernel API it needs (regmap, I2C, mutexes,
  firmware loading, DVB frontend structures) on top of libusb.
- **Streaming.** 32 queued asynchronous bulk transfers keep the FX2 FIFO
  from overflowing.
- **Service filter.** PAT and PMT are parsed live; only the chosen service's
  PIDs are passed, with a generated single-program PAT.
- **Teletext.** PES (ETSI EN 300 472), bit-reversed bytes, Hamming 8/4,
  odd parity, pages and subpages, mosaics as Unicode sextants (ETSI EN 300
  706).

## Credits

- The TBS Linux drivers in [tbsdtv/linux_media](https://github.com/tbsdtv/linux_media),
  in particular `tbs5530.c` and `m88rs6060.c` by Davin Zhang. Everything this
  program knows about the hardware comes from there.
- [matrog/tbs5580-macos](https://github.com/matrog/tbs5580-macos), a
  user-space macOS driver for the sibling TBS5580. It was the inspiration
  for this project and showed the approach: libusb, the TBS Linux code as
  the reference, and a thin replacement for the kernel API.
- [libusb](https://libusb.info).
- The ETSI standards EN 300 468, EN 300 472, EN 300 706 and EN 302 307,
  freely available from [etsi.org](https://www.etsi.org).

## License

GPL-2.0, see `LICENSE`. Parts are derived from the TBS Linux drivers
(GPL-2.0 and GPL-2.0-or-later); their copyright notices are kept in the
source files. The firmware files are proprietary and not included.
