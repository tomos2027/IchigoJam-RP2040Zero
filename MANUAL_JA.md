# IchigoJam RP2040Zero ユーザーマニュアル

Waveshare RP2040-Zero 用 IchigoJam BASIC 1.6.1 (4K版) — **NTSCコンポジット出力専用**の使い方 handbookです。

- 対応基板: [Waveshare RP2040-Zero](https://www.waveshare.com/rp2040-zero.htm) (RP2040、2MBフラッシュ、内蔵WS2812=GP16)
- `VER()` = 16130 / `VER(1)` = 8 (RP2040)

---

## 1. はじめに

IchigoJam RP2040Zero は、教育用こどもパソコン **IchigoJam** をRP2040-Zeroに移植したものです。テレビ（NTSCコンポジット入力）とUSBキーボードを繋ぐだけでBASICのプログラミングが始められます。

**裏面パッド (GP17-25) は一切使用しません。** すべての機能が側面エッジパッドと基板実装部品 (WS2812) で完結します。

### 必要なもの

| 必須 | 品目 | 備考 |
|---|---|---|
| ○ | Waveshare RP2040-Zero | 本体 |
| ○ | **NTSCコンポジット対応テレビ** | 映像入力端子 (黄色) があるテレビ。HDMIモニタは本機では使えません |
| ○ | 抵抗75Ω、コンデンサ560pF | NTSC映像出力回路 (GP3から映像入力へ) |
| ○ | USBキーボード | Type-Cポートに直接接続 (変換アダプタ可) |
| ○ | 電源 | Type-C 5V (PCのUSBポートで可) |
| △ | SDカード + SDカードモジュール | SDカード機能を使う場合 (SPI対応品をGP6/7/8/10/11に配線) |
| △ | PC | ファームウェア書き込み時のみ |
| - | タクトボタン、スピーカー/アンプ、WS2812 LEDテープ、I2C EEPROM/センサー | BTN/SOUND/WS.LED/ENV.IN用 |

## 2. インストール

### 2.1 ファームウェアの書き込み (初回のみ)

1. Type-Cケーブルの**反対側**の準備: **BOOTボタンを押しながらRESETボタンを押して離し、BOOTを離す**
2. PCにType-C接続 → `RPI-RP2` というUSBドライブとして認識される
3. `IchigoJam_RP2040Zero.uf2` をそのドライブへドラッグ＆ドロップ
4. 自動的に再起動し、書き込み完了

### 2.2 NTSC映像の配線

GP3（側面パッド）から以下の回路を介してテレビの映像入力（黄色端子）に接続します:

```
GP3 ── 75Ω ──┬── 映像入力 (黄色端子)
             │
           === 560pF
             │
            GND ── テレビのGND
```

### 2.3 起動と接続

1. 電源投入 → **内蔵WS2812が点灯 (50%輝度)**すれば正常起動しています
2. テレビ画面に以下が表示されれば準備完了です:

```
IchigoJam BASIC 1.6.1 by jig.jp forked by na-s.jp
OK
```

3. Type-CにUSBキーボードを接続 (PCとは差し替え)
4. `PRINT "HELLO"` + Enter → `HELLO` と表示されれば完成です

> **起動時の自動実行**: スロット0 (`SAVE 0`) の先頭行を `@ARUN` にしておくと、電源投入時に自動実行されます。

### 2.4 動作診断 (内蔵WS2812)

基板上のWS2812 RGB LED (GP16に接続済み) が診断表示を兼ねます:

| WS2812の状態 | 意味 |
|---|---|
| 点滅 (約1Hz)…正常起動中 (ハートビート) (`LED 0`で消灯可) |
| 高速点滅 (約8Hz)…ハードフォルト発生 (約8秒後に自動再起動) |
| 消灯 | 起動に失敗 (BOOT+RESETで再書き込み) |
| 高速点滅+画面にE番号表示 | NTSCメモリ破壊検知 (診断用) |

## 3. ピンマップ (Waveshare RP2040-Zero)

### 3.1 IchigoJamのI/Oピン (すべて側面エッジパッド)

| 機能 | GP | SD共有 | 説明 |
|---|---|---|---|
| **NTSC映像** | **GP3** | | 75Ω+560pF回路→テレビ映像入力 |
| **LED (WS2812)** | **GP16** | 基板実装 | `LED 1`=点灯(50%)/`LED 0`=消灯、診断表示兼用、`WS.LED 7`でも駆動可 |
| **BTN** | GP28 | | ボタン→GND (内部プルアップ)。SLEEP復帰兼用。**ADC2** |
| **IN1** | GP27 | | 入力 (プルアップ)。**ADC1** アナログ可。IR.INポート1 |
| **IN2** | GP26 | | 入力 (プルアップ)。**ADC0** アナログ可。IR.INポート2 |
| **IN3** | GP6 | **SD CS** | 入力 (プルアップ)。SDON中はSD CSとして使用。IR.INポート3 |
| **IN4** | GP7 | **SD検出** | 入力 (プルアップ)。SDON中はカード検出として使用。IR.INポート4 |
| **OUT1** | GP8 | **SD MISO** | 出力。PWM対応。SDON中はSD MISOとして使用 |
| **OUT2** | GP9 | | 出力。PWM対応 |
| **OUT3** | GP10 | **SD CLK** | 出力。PWM対応。SDON中はSD CLKとして使用 |
| **OUT4** | GP11 | **SD MOSI** | 出力。PWM対応。SDON中はSD MOSIとして使用 |
| **OUT5** | GP12 | | 出力。PWM対応 (**IchigoJam_PのGP22から変更**) |
| **OUT6** | GP13 | | 出力 (**IchigoJam_PのGP21から変更**) |
| **SOUND** | GP14 | | BEEP/SOUND/PLAY (PWM方形波) (**IchigoJam_PのGP20から変更**) |
| **I2C0 SDA/SCL** | GP4/5 | | I2CR/I2CW、外部EEPROM、ENV.IN |
| **UART TX/RX** | GP0/1 | | UART命令 (115200bps)、シリアル出力 |

- **WS.LED** はポート1〜7を指定 (OUT1-6＋LED=GP16)。**SDON中はポート1/3/4 (SD共有ピン) がブロックされます**
- **PWM (サーボ)** はOUT1〜OUT5。**SDON中はポート1/3/4がブロックされます**
- **ANA()** はIN1 (ADC1) / IN2 (ADC0) / BTN (ADC2) の**アナログ値 (0-1023)** を返します

### 3.2 パッド配置

RP2040-Zeroの**側面エッジパッド**に以下のピンが出ています:

| 側面パッド (表側) | 機能 | 側面パッド (裏側) | 機能 |
|---|---|---|---|
| 1 | 3V3 | 11 | VBUS (5V) |
| 2 | GP0 (UART TX) | 12 | GP8 (OUT1 / **SD MISO**) |
| 3 | GP1 (UART RX) | 13 | GP9 (OUT2) |
| 4 | GP2 (I2C SDA) | 14 | GP10 (OUT3 / **SD CLK**) |
| 5 | GP3 (**NTSC映像**) | 15 | GP11 (OUT4 / **SD MOSI**) |
| 6 | GP4 (I2C SDA) | 16 | GP12 (OUT5) |
| 7 | GP5 (I2C SCL) | 17 | GP13 (OUT6) |
| 8 | GP6 (IN3 / **SD CS**) | 18 | GP14 (SOUND) |
| 9 | GP7 (IN4 / **SD検出**) | 19 | GP15 (予備) |
| 10 | GND | 20 | GND |

**追加の側面パッド** (基板の短辺側):
GP16 (**基板内蔵WS2812**)、GP26 (IN2)、GP27 (IN1)、GP28 (BTN)

**裏面パッド (GP17-25)**: **本機では未使用**です。

### 3.3 SDON中のポートブロック (重要)

`SDON` 実行中は、SDカードと共有している以下のポートで `OUT`/`PWM`/`WS.LED`/`IR.IN` コマンドがブロックされます:

| ブロックされるポート | GP | SD機能 |
|---|---|---|
| OUT1 (=WS.LED 1) | GP8 | SD MISO |
| OUT3 (=WS.LED 3) | GP10 | SD CLK |
| OUT4 (=WS.LED 4) | GP11 | SD MOSI |
| IN3 (IR.IN 3) | GP6 | SD CS |
| IN4 (IR.IN 4) | GP7 | SD検出 |

`OUT 2`、`OUT 5`、`OUT 6`、`IN1`、`IN2`、`BTN`、`SOUND`、`LED`、I2Cは**SDON中も通常どおり使用できます**。

`SDOFF` を実行すると、すべての共有ピンがIchigoJamのI/O状態 (IN=プルアップ入力、OUT=LOW出力) に復帰します。

## 4. BASICの基本操作

### 4.1 画面

32×24文字のNTSCテキスト画面です。`VIDEO`命令で拡大・反転、`CLS`/`CLP`/`CLV`等でクリアできます。

### 4.2 ファンクションキー

| キー | 動作 | キー | 動作 |
|---|---|---|---|
| F1 | CLP + CLS | F7 | `OUT0` (全出力OFF) |
| F2 | `LOAD` | F8 | `VIDEO1` |
| F3 | `SAVE` | F9 | `FILES` |
| F4 | `LIST` | F10 | `SWITCH` (本機では無効) |
| F5 | `RUN` | F11 | CLS |
| F6 | `?FREE()` | F12 | 行取消 (CAN) |

### 4.3 キーボード / 日本語入力

- `KBD 1` … JIS(日本語)配列 / `KBD 0` … US配列 (設定は保存される)
- **ローマ字かな入力**: `KBD 1`の状態で「変換」キーを押すとかなモードに入り、ローマ字入力がかなに変換されます
- `Ctrl`+`Space` でかなモードON/OFF、`Esc`で実行中断

### 4.4 メモリマップ (4K版)

```
#000〜 CHAR (フォントROM)
#700〜 PCG (フォント書き換えRAM、POKE #700-#7FF)
#800〜 VAR ([0]〜[101]とA-Z)
#900〜 VRAM (画面)
#C00〜 VAR2 ([102]〜[357])
#E00〜 LIST (プログラム領域 4096バイト)
```

## 5. プログラムの保存先

| 番号 | 保存先 | 容量 | 備考 |
|---|---|---|---|
| **0-24** | 内蔵フラッシュ (25本) | 1本4096バイト | 電源を切っても保持 |
| **100-131** | 外部EEPROM (24LC64/24LC256/24FC1025をI2C=GP4/5に接続) | 1本4096バイト | `FILES0`で有無を自動判定 |
| **200-299** | **SDカード** | 1本4096バイト | `P200.IJB`/`P200.BAS`形式、次章参照 |

## 6. SDカード (GivetakeJam_P準拠)

### 6.1 配線

RP2040-ZeroにはSDスロットがないため、**SDカードモジュール (SPI対応品)** を以下の側面パッドに配線します:

| 本機パッド | GP | SDカード側 | SD共有ポート |
|---|---|---|---|
| 側面パッド8 | GP6 | **DAT3 / CS** | IN3 |
| 側面パッド9 | GP7 | **CD (カード検出)** ※ | IN4 |
| 側面パッド12 | GP8 | **DAT0 / DO (MISO)** | OUT1 |
| 側面パッド14 | GP10 | **CLK / SCK** | OUT3 |
| 側面パッド15 | GP11 | **CMD / DI (MOSI)** | OUT4 |
| 電源 | 3V3 | VCC | 3.3V (5Vを繋がないこと) |
| GND | GND | GND | 共通GND |

※ **カード検出 (CD)**: SDカードモジュールのCard Detect端子をGP7に接続すると、カード挿入時にLOWになります。CD端子がないモジュールの場合は **GP7をGNDに直結**してください (常時挿入扱いになります)。

SDカードモジュール (SPI変換基板付き) を使うのが最も簡単です。CSと3.3Vの間に10kΩのプルアップ抵抗を入れると安定します (必須ではない)。

### 6.2 準備

1. **FAT32でフォーマットしたSDカード**をスロットに挿入
   - SD/SDHC (32GB以下) はそのままFAT32で使えます
   - **SDXC (64GB以上) は初期状態がexFATのため、FAT32で再フォーマットしてください**
2. `SDON` と入力 → `SD ready (FAT32, 1MHz)` と表示されれば準備完了

`SDON` 中は、SDと共有している OUT1/OUT3/OUT4/IN3/IN4 がSDカードに占有されます (§3.3参照)。

### 6.3 保存・読み込み

```
10 PRINT "HELLO"
SAVE 200          … P200.IJB として保存 (バイナリ形式)
SAVE 200,1        … P200.BAS として保存 (アスキー形式、UTF-8)
LOAD 200          … 読み込み (.IJB優先、なければ.BASを自動判別)
LRUN 200          … 読み込んで即実行
FILES             … SD一覧 (SDON中。各行に先頭行プレビュー)
SDOFF             … SD終了 (共有ピンがI/Oに復帰)
```

### 6.4 アスキー形式 (.BAS) — PCとの連携

`SAVE n,1` で保存した `P200.BAS`〜`P299.BAS` は**そのまま読めるテキストファイル (UTF-8)** です。

- 形式: 1行が「`行番号` + スペース + プログラム本文」、改行はCRLF
- PCでメモ帳 (Windows 10以降) やVS Code等でそのまま作成・閲覧・編集できます
- **文字コード**: LOADはUTF-8 (半角カナ、全角カナ→半角カナ自動変換、全角英数→半角ASCII、BOM対応) と旧形式の生バイトを自動判別。**漢字は非対応** (かなで記述)
- PCで作ったプログラムも `P210.BAS` の名前でカードの**ルート直下**に置けば `LOAD 210` で読み込めます

### 6.5 SDエラー一覧

| 表示 | 意味と対処 |
|---|---|
| `SD init error 1` | カード検出ピン (GP7) がHIGH → カード未挿入、またはGP7をGNDに直結する |
| `SD init error 2` | カードがCMD0に応答しない → 配線・電源・カードを確認 |
| `SD init error 3〜6` | カードの初期化プロトコルに失敗 → カード不良や相性 |
| `SD read error 10〜15` | 読み取り失敗 → 再SDON、カード交換 |
| `SD FS error 20〜26` | FAT32検査失敗 (23=FAT12/16) → **FAT32で再フォーマット** |
| `SD file error 30` | ファイル未発見 → SAVE済み番号を確認 |
| `SD file error 36` | **SDONしていない** → `SDON`を先に実行 |
| `SD file error 38` | アスキー形式への変換で4096バイト超 |
| `SD write error 40〜45` | 書き込み失敗 → 再SDON、カード交換 |

## 7. トラブルシューティング

| 症状 | 原因と対処 |
|---|---|
| **テレビに何も映らない** | ①GP3の75Ω+560pF回路とテレビの映像入力の接続を確認。②内蔵WS2812が点滅していればファームは動作中 → 配線を確認。③高速点滅ならハードフォルト → 再書き込み |
| キーボードが効かない | Type-Cに直接接続。`KBD 1`でJIS配列に |
| ANA()が常に0または1023 | `ANA(1)`=IN1(GP27)、`ANA(2)`=IN2(GP26)、`ANA(9)`=BTN(GP28)。配線とアナログ入力の電圧 (0-3.3V) を確認 |
| SDONで`SD init error 1` | GP7 (カード検出) がHIGHのまま → SDモジュールのCD端子をGP7に接続、またはGP7をGNDに直結 |
| SDONで`SD init error 2` | カードが見つからない → 配線 (GP6/8/10/11) と3.3V電源を確認 |
| `OUT 1,1` が効かない | **SDON中はOUT1がSDに占有されています**。`SDOFF`後に実行するか、`OUT 2`/`OUT 5`/`OUT 6`を使用 |
| `SAVE 0`等がエラーになる | フラッシュの保存領域 (25本) は0-24です。SDカードは200-299 |
| 基板のLEDが点かない | `LED 1`で点灯。`LED 0`で消灯。点滅 (ハートビート) が正常動作状態 |
| 高負荷時に固まる | 電源の電流不足 → より強力なUSB電源で試す |
| `SWITCH`が効かない | 本機はNTSC専用のため`SWITCH`コマンドは無効 (予約語として残している) |

## 8. 開発者向け: ビルド方法

依存ライブラリの取得とパッチ適用手順は [README_JA.md](README_JA.md#環境構築) を参照してください。

```sh
git clone https://github.com/tomos2027/IchigoJam-RP2040Zero.git
cd IchigoJam-RP2040Zero
# pico-sdk (2.3.1) + TinyUSBパッチ
# pico-extras
cmake -B build
cmake --build build
```

## 9. 仕様

| 項目 | 値 |
|---|---|
| BASIC | IchigoJam BASIC 1.6.1 (4K版、配列[0..357]、`WS.LED`/`IR.IN`/`ENV.IN`) |
| `VER()` / `VER(1)` | 16130 / 8 |
| CPUクロック | 157.5MHz (NTSC、VREG 1.20V) |
| 映像 | NTSCコンポジット、32×24文字 |
| 入力 | USBキーボード (Type-C)、UART、IN1-4/BTN |
| 音声 | PWM 1ch (BEEP/SOUND/PLAY) |
| 保存 | 内蔵25本 + EEPROM 32本 + SD 100本 |
| SD方式 | GivetakeJam_P準拠の共有ピン方式 (GP6/7/8/10/11) |

## 10. ライセンス・商標

- Copyright 2014-2026 the IchigoJam authors. All rights reserved. MIT license. https://ichigojam.net/license
- 4K版拡張は [GivetakeJam_P](https://github.com/Canon-boy4/ichigojam-firm) (Canon-boy4氏) 準拠 (MIT license)
- SDカードドライバ (`src/sd_card.h`) は Canon-boy4氏 GivetakeJam_P (rp2350-16130) の改変版 (MIT license)
- NTSC出力 (`src/ntsc/`) は KenKen氏による著作物 (営利目的でなければ自由に利用可)
- **IchigoJamは株式会社jig.jpの登録商標**です。Raspberry Pi はRaspberry Pi財団の登録商標です
- 本ファームウェアの利用には[IchigoJamロイヤリティフリープログラム利用規約](https://ichigojam.net/ichigojam-license.pdf)に同意の上で行ってください。第三者への配布にはライセンスが必要です: https://pcn.club/sp/ijp/
