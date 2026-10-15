# IchigoJam RP2040Zero

[English](README.md) | [日本語](README_JA.md) | [マニュアル (Manual)](MANUAL_JA.md)

IchigoJam BASIC 1.6.1 (4K版) の **Waveshare RP2040-Zero** (RP2040) 移植・**NTSCコンポジット出力専用版**です。

- 映像出力: **NTSCコンポジット** (GP3 + 75Ω + 560pF の外部回路 → テレビの映像入力へ)
- キーボード: Type-CポートにUSBキーボードを直接接続 (ネイティブUSBホスト)
- **SDカード**: [GivetakeJam_P](https://github.com/Canon-boy4/ichigojam-firm/tree/rp2350-16130) 準拠の**共有ピン方式** (GP6/7/8/10/11、SDON中は該当OUT/INポートをブロック)
- **アスキー形式 `SAVE n,1` → `Pnnn.BAS` (UTF-8)** でPC連携可
- **ANA() アナログ入力対応** (IN1=GP27/ADC1、IN2=GP26/ADC0、BTN=GP28/ADC2)
- **基板内蔵WS2812 (GP16)** をLEDコマンド・診断表示・`WS.LED 7`に使用
- **裏面パッド (GP17-25) は未使用** — すべての機能が側面エッジパッドで完結

## 対応基板
- [Waveshare RP2040-Zero](https://www.waveshare.com/rp2040-zero.htm) (RP2040、2MBフラッシュ、Type-C、内蔵WS2812)

## クイックスタート
1. `build/IchigoJam_RP2040Zero.uf2` をビルドまたは入手
2. **BOOTボタンを押しながらRESET** → 離すと`RPI-RP2`ドライブとして認識
3. UF2をドラッグ＆ドロップ → 自動再起動
4. NTSC出力をGP3 + 75Ω + 560pF の回路でテレビの映像入力へ、Type-CにUSBキーボードを接続

## ピン割り当て (すべて側面エッジパッド、裏面GP17-25は未使用)

| 機能 | GP | SD共有 | 説明 |
|---|---|---|---|
| **NTSC映像** | **GP3** | | 75Ω+560pF回路→映像入力 |
| **SD CS** | **GP6** | **IN3と共有** | SDカードのDAT3/CS |
| **SD検出** | **GP7** | **IN4と共有** | カード検出 (CD端子→LOW=挿入) |
| **SD MISO** | **GP8** | **OUT1と共有** | SDカードのDAT0 |
| OUT2 | GP9 | | |
| **SD CLK** | **GP10** | **OUT3と共有** | SDカードのCLK |
| **SD MOSI** | **GP11** | **OUT4と共有** | SDカードのCMD |
| **OUT5** | **GP12** | | (IchigoJam_PのGP22から変更) |
| **OUT6** | **GP13** | | (IchigoJam_PのGP21から変更) |
| **SOUND** | **GP14** | | (IchigoJam_PのGP20から変更) |
| (予備) | GP15 | | |
| **LED** | **GP15** | 側面パッド | `LED 1`/`LED 0` (通常GPIO)、`WS.LED 7` (外部WS2812) |
| (内蔵WS2812) | GP16 | 基板実装 | ハートビート (緑・1Hz) + エラー診断専用 |
| **IN2** | GP26 | | **ADC0** アナログ入力可 |
| **IN1** | GP27 | | **ADC1** アナログ入力可 |
| **BTN** | GP28 | | **ADC2**、ボタン→GND (内部プルアップ) |
| I2C0 SDA/SCL | GP4/5 | | 外部EEPROM、ENV.IN |
| UART TX/RX | GP0/1 | | 115200bps |

> **SDON中の共有ピン**: OUT1(GP8)、OUT3(GP10)、OUT4(GP11)、IN3(GP6)、IN4(GP7) はSDカード通信に使用されます。`OUT`/`PWM`/`WS.LED`/`IR.IN` コマンドの該当ポートは自動的にブロックされます。`SDOFF` でピンがI/O状態に復帰します。

## SDカードの使い方 (GivetakeJam_P準拠)
```
SDON            … カード+FAT32初期化 → "SD ready (FAT32, 1MHz)"
SAVE 200        … P200.IJB (バイナリ形式) に保存
SAVE 200,1      … P200.BAS (アスキー形式、UTF-8) に保存 → PCで閲覧・編集可
LOAD 200        … .IJB優先、なければ.BASを自動判別 (テキスト取込み)
LRUN 200        … 読み込んで実行
FILES           … SDON中の省略形=SD一覧 (200-299、先頭行プレビュー)
SDOFF           … SD終了 (共有ピンがI/Oに復帰)
```

## 環境構築 (ビルド)

**CMake** と **GCC (arm-none-eabi)** をインストールし、リポジトリ直下に依存ライブラリをclone:

```sh
git clone https://github.com/tomos2027/IchigoJam-RP2040Zero.git
cd IchigoJam-RP2040Zero

# pico-sdk (2.3.1 / コミット 079c6f3)
git clone https://github.com/raspberrypi/pico-sdk.git
cd pico-sdk && git checkout 079c6f3 && git submodule update --init --recursive
# TinyUSBパッチ適用 (USBキーボード利用に必須)
cd lib/tinyusb && git apply ../../../patches/tinyusb_osal_task_delay.patch && cd ../../..

# pico-extras (コミット 52fd7a7)
git clone https://github.com/raspberrypi/pico-extras.git
```

## ビルド
```sh
cmake -B build
cmake --build build
```

## スペック
| 項目 | 値 |
|---|---|
| BASIC | IchigoJam BASIC 1.6.1 (4K版、配列[0..357]) |
| `VER()` / `VER(1)` | 16130 / 8 (RP2040) |
| クロック | 157.5MHz (NTSC、VREG 1.20V) |
| 内部保存 | フラッシュ末尾に25本 (1本4096バイト) |
| SD形式 | FAT32 (MBR/スーパーフロッピー)、8.3名、ルートのみ |

## 既知の注意点
- **SDカードスロットは基板にありません**。SDカードモジュールをGP6/7/8/10/11に配線してください (詳細はMANUAL_JA.md §6.1)
- **SDON中はOUT1/OUT3/OUT4/IN3/IN4がSDカードに占有されます** (GivetakeJam_Pと同じ仕様)。`OUT 2`や`OUT 5`/`OUT 6`、`IN1`/`IN2`はSDON中も使用できます
- TFカードは**FAT32フォーマット必須** (SDXCはexFATのため再フォーマット、32GB以下推奨)
- `.BAS` (アスキー形式) はUTF-8で保存。LOADはUTF-8と旧形式の生バイトを自動判別。**漢字は非対応** (かなで記述)
- 157.5MHzオーバークロックで動作 (IchigoJam_Pと同じ仕様)
- **OUT5=GP12、OUT6=GP13、SOUND=GP14** はIchigoJam_P (GP22/GP21/GP20) と異なります (裏面パッド不使用のため)

## ライセンス / License
- Copyright 2014-2026 the IchigoJam authors. All rights reserved. MIT license. https://ichigojam.net/license
- 4K版拡張 (プログラム領域4096バイト化、拡張配列、`IR.IN`、`ENV.IN`等) は [GivetakeJam_P](https://github.com/Canon-boy4/ichigojam-firm) 準拠 (MIT license)
- SDカードドライバ (`src/sd_card.h`) は Canon-boy4氏 GivetakeJam_P (rp2350-16130, MIT) の実装をUTF-8対応に改変
- NTSC出力 (`src/ntsc/`) は KenKen氏 `rp2040_pwm_ntsc_textgraph` による著作物 (営利目的でなければ自由に利用可、利用明記が推奨)

## 商標 / Trademark
IchigoJamは、株式会社jig.jpの登録商標です (IchigoJam is a registered trademark of jig.jp co., ltd.)
Raspberry Pi は、Raspberry Pi財団の登録商標です (Raspberry Pi is a registered trademark of the Raspberry Pi Foundation)

## 利用規約に同意の上、ダウンロードしてください。第三者への配布にはライセンスが必要になります。詳しくはこちら。https://pcn.club/sp/ijp/
- 利用規約（IchigoJamロイヤリティフリープログラム利用規約） https://ichigojam.net/ichigojam-license.pdf
