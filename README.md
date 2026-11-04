# IchigoJam RP2040Zero

[English](README.md) | [日本語](README_JA.md) | [マニュアル (Manual)](MANUAL_JA.md)

A port of IchigoJam BASIC 1.6.1 (4K version) for the **Waveshare RP2040-Zero** board, with **NTSC composite video output only**.

- Video: NTSC composite (GP3 + 75Ω + 560pF external circuit)
- Keyboard: USB keyboard on the Type-C port (native USB host)
- **Multi-voice PLAY (up to 3 voices)** via AY-3-8910 (MSX-compatible) PSG emulation, with per-voice `V0-15` volume and `SOUND reg,val` register access (single-voice BEEP/PLAY stays clean hardware PWM)
- **NTSC color text**: `VIDEO 9` (32×24) / `VIDEO 10` (42×27) color modes with MSX-style 16-color `COLOR f[,b[,c]]` / `COL f[,b[,c]]`, `ATTR x,y,a`, and `PAL n,v` / `PAL(n)` / `PAL RESET`
- SD card: [GivetakeJam_P](https://github.com/Canon-boy4/ichigojam-firm/tree/rp2350-16130) shared-pin design (GP6/7/8/10/11)
- **LED / `WS.LED 8,n` on GP2** (WS2812) and **`WS.LED 7,n` on GP15** (external LED strip); the onboard WS2812 (GP16) is used for the heartbeat and error diagnostics only
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
| **LED / WS.LED 8** | **GP2** | side pad | `LED 1`/`LED 0` (plain GPIO), `WS.LED 8,n` (WS2812) |
| **WS.LED 7** | **GP15** | side pad | external WS2812 data out |
| (onboard WS2812) | GP16 | onboard | heartbeat (green, ~1Hz) + error diagnostics only |
| **IN2** | GP26 | | **ADC0** analog |
| **IN1** | GP27 | | **ADC1** analog |
| **BTN** | GP28 | | **ADC2** |

> **WS2812 note**: `WS.LED 7,n` outputs on **GP15**, `WS.LED 8,n` on **GP2** (3.3V logic). A 5V WS2812 strip requires a level shifter (e.g. 74AHCT125) and a common GND with a separate 5V supply. Data order is **GRB**.
>
> Quick test (1 LED, red): `CLV:LET[0],0,40,0:WS.LED 7,1`

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
- NTSC output (`src/video/ntscv`) is an **original clean-room implementation (MIT, commercial use OK)**, written from the public NTSC (SMPTE 170M) standard and the pico-sdk only. Credits: PWM+DMA composite generation idea by @lovyan03; functional reference (no code reuse) is KenKen's `rp2040_pwm_ntsc_textgraph`. See `docs/ntsc_video_spec.md` and the legacy API shim `src/ntsc/ntsc_compat.h`

## Trademark
- IchigoJam is a registered trademark of jig.jp co., ltd.
- Raspberry Pi is a registered trademark of the Raspberry Pi Foundation
