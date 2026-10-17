# IchigoJam RP2040Zero

[English](README.md) | [日本語](README_JA.md) | [マニュアル (Manual)](MANUAL_JA.md)

A port of IchigoJam BASIC 1.6.1 (4K version) for the **Waveshare RP2040-Zero** board, with **NTSC composite video output only**.

- Video: NTSC composite (GP3 + 75Ω + 560pF external circuit)
- Keyboard: USB keyboard on the Type-C port (native USB host)
- **Multi-voice PLAY (up to 3 voices)** via AY-3-8910 (MSX-compatible) PSG emulation, with per-voice `V0-15` volume and `SOUND reg,val` register access (single-voice BEEP/PLAY stays clean hardware PWM)
- SD card: [GivetakeJam_P](https://github.com/Canon-boy4/ichigojam-firm/tree/rp2350-16130) shared-pin design (GP6/7/8/10/11)
- Onboard WS2812 (GP16) used as the LED command / diagnostic display
- **No back-pad pins (GP17-25) are used** — all functions on side edge pads

## Supported board
- [Waveshare RP2040-Zero](https://www.waveshare.com/rp2040-zero.htm) (RP2040, 2MB flash, Type-C, onboard WS2812)

## Quick start
1. Build or obtain `IchigoJam_RP2040Zero.uf2`
2. Hold **BOOT**, press and release **RESET** → the board enumerates as a `RPI-RP2` drive
3. Drag & drop the UF2 file
4. Connect NTSC video output (GP3 + 75Ω + 560pF) to a TV, USB keyboard to Type-C

## Pin assignment (all side edge pads, GP17-25 unused)

| Function | GP | SD shared | Notes |
|---|---|---|---|
| **NTSC video** | **GP3** | | 75Ω+560pF → video in |
| **SD CS** | **GP6** | **IN3** | SD card DAT3/CS |
| **SD detect** | **GP7** | **IN4** | Card detect (CD→LOW=present) |
| **SD MISO** | **GP8** | **OUT1** | SD card DAT0 |
| OUT2 | GP9 | | |
| **SD CLK** | **GP10** | **OUT3** | SD card CLK |
| **SD MOSI** | **GP11** | **OUT4** | SD card CMD |
| **OUT5** | **GP12** | | (changed from GP22) |
| **OUT6** | **GP13** | | (changed from GP21) |
| **SOUND** | **GP14** | | BEEP/PLAY/SOUND (changed from GP20) |
| **LED (WS2812)** | **GP16** | onboard | `LED 1`=white, `WS.LED 7` |
| **IN2** | GP26 | | **ADC0** analog |
| **IN1** | GP27 | | **ADC1** analog |
| **BTN** | GP28 | | **ADC2** |

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
