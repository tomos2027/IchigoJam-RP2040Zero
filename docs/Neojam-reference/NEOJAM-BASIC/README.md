## NEOJAM コマンド一覧
NEOJAM command reference (Japanese)

NEOJAM で使用できる命令を一覧しています。

Download ZIP でファイル一覧をダウンロードできます。
GitHub・Git を使っている場合は Clone を使っても良いでしょう。

RAW で参照した場合、またファイルをダウンロードした場合
.txt ファイルは文字コード UTF-8、改行コード CR+LF になります。
Windows ではメモ帳を使用する事が可能です。

- NEOJAM (対象リポジトリ) https://github.com/tomos2027/IchigoJam-RP2040Zero
- IchigoJam (公式) https://ichigojam.net/
- IchigoJam BASIC コマンド一覧 (元となったリファレンス) https://github.com/fu-sen/IchigoJam-BASIC/

### NEOJAM について

NEOJAM は Waveshare RP2040-Zero (RP2040) で動作する
IchigoJam BASIC 1.6.1 4K 版ベースの BASIC です。
テレビ (NTSC コンポジット入力) と USB キーボードを繋ぐだけで
BASIC のプログラミングが始められる、NTSC コンポジット出力
専用の 1 枚基板パソコンです。

映像出力は NTSC コンポジット (GP3、75Ω+560pF の外部回路経由)
専用で、VIDEO 9/10 による 16 色カラー表示に対応しています。
CPU は 157.5MHz で駆動します。

### バージョン表記の注意

NEOJAM は単一プラットフォームのため、
各ファイルの対応表記は「NEOJAM」です。
カラー表示 (VIDEO 9/10) 関連のコマンドは
「NEOJAM (カラー表示・テスト実装)」と表記しています。

`VER()` は 16130 (IchigoJam BASIC 1.6.1 4K 版ベース)、
`VER(1)` は 8 (RP2040) を返します。

### NEOJAM 固有コマンド・拡張

- `WS.LED` ポート選択式 (0=基板内蔵 WS2812 (GP16)・1-6=OUT1-6・
  7=外部 WS2812 (GP15)・8=LED (GP2)・9-11=OUT9-11)、PIO 駆動
- `WS.MODE` / `WS.PUT` / `RGB` WS2812 NeoMatrix 対応
  (ピクセル格納モード 0-3・オフセット、8KB 専用バッファ、RGB332 関数)
- `WS.SETUP` WS.LED の LED 数上限・オフセット・出力信号反転
- `WS.MATRIX` / `WS.XY` x,y 座標でマトリックス描画
  (8x8 / 5x5、ジグザグ配線・左右/上下反転、R,G,B 0-255)
- `NEO.*` / `NEOSCROLL.*` Annex WiFi 互換の WS2812 ストリップ・
  ドットマトリクス制御 (RGB555 色関数・カスタムモジュール寸法対応。
  詳細は docs/NEO_JA.md)
- `PCA9685.*` I2C 接続の 16 チャンネル PWM ドライバ制御
  (サーボ 16 個・LED 調光 16ch。詳細は docs/PCA9685_JA.md)
- `DISTANCE(trig,echo)` HC-SR04 超音波距離センサ (cm 単位)
- `IR.IN` / `IR.OUT` NEC 赤外線の受信・送信 (4K 版拡張)
- `ENV.IN` 環境センサー (AHT20+BMP280、4K 版拡張)
- `PLAY` 3 声同時演奏 (AY-3-8910 互換 PSG・MSX 互換 MML)
- `SOUND` PSG レジスタ (R0-R13) 直接操作 (GP14 出力)
- `COLOR` / `COL`・`PAL`・`ATTR` NTSC カラー表示 (VIDEO 9/10)
- `SDON` / `SDOFF` SD カード (FAT32、スロット 200-299、GP6/7/8/10/11 共有)
- `PWM` 高速モード (負の周期で 1/48 マイクロ秒単位)
- `EJ` OLED表示 (SSD1306 128x64・I2C 0x3C、GP4/5、Milkjam IOTPAD互換)
- `VIDEO` 9 (32x24 16 色) / 10 (42x27 16 色)

### 注意

- 本機は NTSC コンポジット出力専用のため `SWITCH` コマンドは
  無効です (予約語としてのみ残っています)。
- IchigoJam BASIC の `LANG` コマンドには対応していません。
- `DAC` コマンドは RP2040 に DAC がないため出力しません。
- プログラム領域は 4096 バイト、配列は [0]-[357] です (4K 版)。
- プログラム保存先は 内蔵フラッシュ 0-24 (25 本) /
  外部 EEPROM 100-131 / SD カード 200-299 です。
- 音声出力 (GP14) はアンプ+スピーカーの接続が必要です
  (圧電ブザー直結は実用音量になりません)。
- 保存スロットの詳細・配線の詳細は
  NEOJAM リポジトリの MANUAL_JA.md を参照して下さい。

### ライセンス

この文章は CC BY で公式に公開されている
「IchigoJam BASIC リファレンス」、および
ふうせん🎈 FU-SEN さんが公開している
「IchigoJam BASIC コマンド一覧」(CC BY 4.0)、
NEOJAM リポジトリ (README_JA.md・MANUAL_JA.md・ソースコード) の
情報を元に NEOJAM 向けに独自に文章化しているものです。

- IchigoJam BASIC リファレンス: CC BY https://ichigojam.net/
  オンライン https://fukuno.jig.jp/app/csv/ichigojam-cmd.html
- IchigoJam BASIC コマンド一覧 (CC BY 4.0)
  https://github.com/fu-sen/IchigoJam-BASIC/
- NEOJAM https://github.com/tomos2027/IchigoJam-RP2040Zero
  (IchigoJam コアは MIT license、
   Copyright 2014-2026 the IchigoJam authors)

NEOJAM は IchigoJam BASIC 1.6.1 4K 版をベースとした
オープンソースファームウェアです。
IchigoJam は株式会社 jig.jp の登録商標です。
Raspberry Pi は Raspberry Pi Foundation の登録商標です。
