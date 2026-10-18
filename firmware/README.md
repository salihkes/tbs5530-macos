# Firmware

Two proprietary files are needed. They are not part of this repository.

| File | What it is | SHA-256 of the copy this was tested with |
|---|---|---|
| `dvb-usb-id5530.fw` | 8051 code for the Cypress FX2 USB bridge (8192 bytes) | `b97cf208bb586f706b26aa5450de6bb97375a3e3eabe833cddb0ae3a4f69d0dc` |
| `dvb-demod-m88rs6060.fw` | Montage M88RS6060 demodulator firmware (12436 bytes) | `20809969b4091c2c1f3167bd0f043a47720e162acd7d551f1bcf31c1b6a41848` |

They ship with TBS's open source Linux driver package (see the TBS download
page for the TBS5530, "Linux driver"), inside the tuner firmware archive. A
Linux machine with the TBS driver installed has them in `/lib/firmware/`.

Copy both files into this directory, or point the program at another one
with `--fw-dir DIR`.

Other versions of `dvb-usb-id5530.fw` may work, but the USB packet-size patch
described in the main README is only applied when the expected bytes are
found at the expected address. Without it, streaming fails on macOS with
`LIBUSB_ERROR_OVERFLOW`.
