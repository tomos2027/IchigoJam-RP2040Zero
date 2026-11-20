## Milkjam Basic コマンド一覧
Milkjam Basic command reference (Japanese)

Milkjam Basic で使用できる命令を一覧しています。

Download ZIP でファイル一覧をダウンロードできます。
GitHub・Git を使っている場合は Clone を使っても良いでしょう。

RAW で参照した場合、またファイルをダウンロードした場合
.txt ファイルは文字コード UTF-8、改行コード CR+LF になります。
Windows ではメモ帳を使用する事が可能です。

- Milkjam (対象リポジトリ) https://github.com/tomos2027/Milkjam/
- IchigoJam (公式) https://ichigojam.net/
- IchigoJam BASIC コマンド一覧 (元となったリファレンス) https://github.com/fu-sen/IchigoJam-BASIC/

### Milkjam Basic について

Milkjam は Raspberry Pi Pico 2 W (RP2350A + CYW43439 WiFi) で動作する
IchigoJam BASIC 1.6.1 4K 版ベースの BASIC です。
WiFi 接続・センサー・ThingsBoard テレメトリ送信・OLED 表示・
SD カードを 1 枚の Pico 2 W に統合した IoT 向け BASIC で、
`EJ` コマンド (IOTPAD 機能) による操作が特徴です。

映像出力は `SWITCH` コマンドで 3 モードから選択します。

- `SWITCH 0` DVI (HDMI) 出力 720x480p60 (TV 互換 480p)
- `SWITCH 1` NTSC コンポジット出力 (GP3、カラー対応)
- `SWITCH 2` TFT 液晶 ILI9341 出力 (GP12-16、白黒)

### バージョン表記の注意

Milkjam Basic は単一プラットフォームのため、
各ファイルの対応表記は「Milkjam Basic」です。
NTSC モードでのみ動作するコマンドは
「Milkjam Basic (NTSC モードのみ)」と表記しています。

`VER()` は 16130 (IchigoJam BASIC 1.6.1 4K 版ベース)、
`VER(1)` は 9 (RP2350) を返します。

### Milkjam 固有コマンド・拡張

- `EJ` IOTPAD 機能 (WiFi・時刻・センサー・ThingsBoard・OLED)
- `PAD.IN` センサー値 + RTC 時刻の一括取得 (16 値)
- `SDON` / `SDOFF` SD カード (FAT32、スロット 200-299)
- `LCDON` / `LCDOFF` TFT 液晶と HDMI sock のピン排他管理
- `COLOR` / `COL`・`PAL`・`ATTR` NTSC カラー表示 (VIDEO 9/10)
- `IR.IN` / `IR.OUT` NEC 赤外線の受信・送信 (4K 版拡張)
- `ENV.IN` 環境センサー (AHT20+BMP280、4K 版拡張)
- `PLAY` 3 声同時演奏 (AY-3-8910 互換 PSG・MSX 互換 MML)
- `SOUND` PSG レジスタ (R0-R13) 直接操作
- `SWITCH` 0=DVI / 1=NTSC / 2=TFT (TFT は 180° 回転指定可)
- `VIDEO` 9 (32x24 16 色) / 10 (42x27 16 色)
- `WS.LED` ポート選択式 (0=内蔵 LED・1-6=OUT1-6・7=LED)
- `PWM` 高速モード (負の周期で 1/48 マイクロ秒単位)

### 注意

- IchigoJam BASIC の `LANG` コマンドには対応していません。
- `DAC` コマンドは RP2350 に DAC がないため出力しません。
- プログラム領域は 4096 バイト、配列は [0]-[357] です (4K 版)。
- プログラム保存先は 内蔵フラッシュ 0-24 (25 本) /
  外部 EEPROM 100-131 / SD カード 200-299 です。
- 保存スロットの詳細・IOTPAD 機能の詳細は
  Milkjam リポジトリの MANUAL_JA.md を参照して下さい。

### ライセンス

この文章は CC BY で公式に公開されている
「IchigoJam BASIC リファレンス」、および
ふうせん🎈 FU-SEN さんが公開している
「IchigoJam BASIC コマンド一覧」(CC BY 4.0)、
Milkjam リポジトリ (README_JA.md・MANUAL_JA.md・ソースコード) の
情報を元に Milkjam Basic 向けに独自に文章化しているものです。

- IchigoJam BASIC リファレンス: CC BY https://ichigojam.net/
  オンライン https://fukuno.jig.jp/app/csv/ichigojam-cmd.html
- IchigoJam BASIC コマンド一覧 (CC BY 4.0)
  https://github.com/fu-sen/IchigoJam-BASIC/
- Milkjam https://github.com/tomos2027/Milkjam/
  (IchigoJam コアは MIT license、
   Copyright 2014-2025 the IchigoJam authors)

Milkjam は IchigoJam BASIC 1.6.1 4K 版をベースとした
オープンソースファームウェアです。
IchigoJam は株式会社 jig.jp の登録商標です。
Raspberry Pi は Raspberry Pi Foundation の登録商標です。
