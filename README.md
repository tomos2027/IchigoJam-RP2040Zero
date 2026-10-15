# IchigoJam RP2040Zero

[English](README.md) | [日本語](README_JA.md) | [マニュアル (Manual)](MANUAL_JA.md)

Waveshare RP2040-Zero用ユーザーマニュアルは [MANUAL_JA.md](MANUAL_JA.md) を参照してください。

概要・ビルド方法は [README_JA.md](README_JA.md) を参照してください。

English README is a brief summary. See [README_JA.md](README_JA.md) (Japanese) for details.

## Overview

A port of IchigoJam BASIC 1.6.1 (4K version) for the **Waveshare RP2040-Zero** board, with **NTSC composite video output only** (no DVI/HDMI).

- Video: NTSC composite (GP3 + 75Ω + 560pF external circuit)
- Keyboard: USB keyboard on the Type-C port (native USB host)
- SD card: external wiring on GP12-15 (SPI1)
- Onboard WS2812 (GP16) used as the LED command / diagnostic display

## Building

```sh
git clone https://github.com/tomos2027/IchigoJam-RP2040Zero.git
cd IchigoJam-RP2040Zero
# Obtain pico-sdk (2.3.1) and pico-extras, apply the TinyUSB patch (see README_JA.md)
cmake -B build
cmake --build build
```

## License
- Copyright 2014-2026 the IchigoJam authors. All rights reserved. MIT license. https://ichigojam.net/license
- SD card driver based on Canon-boy4's GivetakeJam_P (rp2350-16130, MIT license)
- NTSC output based on KenKen's `rp2040_pwm_ntsc_textgraph` (free for non-commercial use)

## Trademark
- IchigoJam is a registered trademark of jig.jp co., ltd.
- Raspberry Pi is a registered trademark of the Raspberry Pi Foundation
