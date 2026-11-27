# ビルド手順 (初心者向けガイド)

この文書は、IchigoJam RP2040-Zero版 (NeoJam) のファームウェアを
**ソースコードからコンパイルして、ボードに書き込むまで**を
初めての方でもできるように、順を追って説明します。

> **Dockerを使えばコンパイラ等のインストールを省略できます**:
> 詳しくは [docs/BUILD_DOCKER_JA.md](BUILD_DOCKER_JA.md) (Dockerビルドガイド) を参照してください。

ユーザーマニュアル (MANUAL_JA.md)・NEO/NEOSCROLL リファレンス (docs/NEO_JA.md)・
PSG サウンドリファレンス (docs/PSG_JA.md) も併せてご利用ください。

---

## 目次

1. [全体の流れ](#1-全体の流れ)
2. [必要なもの](#2-必要なもの)
3. [ステップ1: ツールのインストール](#3-ステップ1-ツールのインストール)
4. [ステップ2: ソースコードの取得](#4-ステップ2-ソースコードの取得)
5. [ステップ3: 依存ライブラリの準備](#5-ステップ3-依存ライブラリの準備)
6. [ステップ4: コンパイル (ビルド)](#6-ステップ4-コンパイル-ビルド)
7. [ステップ5: ボードへの書き込み](#7-ステップ5-ボードへの書き込み)
8. [ステップ6: 動作確認](#8-ステップ6-動作確認)
9. [うまくいかないとき (トラブルシューティング)](#9-うまくいかないとき-トラブルシューティング)
10. [上級編: 信号プリセットの切り替え](#10-上級編-信号プリセットの切り替え)

---

## 1. 全体の流れ

```
PCでコンパイル ──→ UF2ファイルができる ──→ ボードにコピー (書き込み) ──→ テレビ+キーボードでBASIC起動
     (10分程度)         IchigoJam_RP2040Zero.uf2         (ドラッグ&ドロップ)
```

- コンパイルとは、人間が読めるC言語のソースコードを、
  RP2040チップが実行できる機械語 (ファームウェア) に変換することです
- 出来上がる `IchigoJam_RP2040Zero.uf2` をボードにコピーするだけで
  ファームウェアの書き込みが完了します (専用の書き込み器は不要です)

---

## 2. 必要なもの

### ハードウェア

| 品物 | 備考 |
|---|---|
| Waveshare RP2040-Zero | 本体 (RP2040、2MBフラッシュ、Type-C、内蔵WS2812) |
| USB Type-C ケーブル | **データ通信対応**のもの (充電専用ケーブルではPCに認識されません) |
| テレビ (コンポジット映像入力あり) またはキャプチャ | 映像は GP3 + 75Ω + 560pF の外部回路経由 (MANUAL_JA.md §2.2) |
| USBキーボード | Type-C — **USBハブを介さず直接接続**してください |
| ブレッドボード・ジャンパー線等 | 上記の映像回路を組む場合 |

### PCとソフトウェア

| ソフト | 用途 | 入手先 |
|---|---|---|
| **Git** | ソースコードの取得 | https://git-scm.com/ |
| **CMake 3.13 以降** | ビルドの自動化 | https://cmake.org/download/ |
| **Arm GNU Toolchain** (arm-none-eabi-gcc) | RP2040用のコンパイラ | https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads |

- 動作確認済みの組合せの例: Arm GNU Toolchain 14.2.rel1 / CMake 3.4以降 / git 2.x
- Windowsの場合は「x86_64 Arm hosted cross toolchain」の
  `arm-none-eabi` 版 (exeインストーラ) を使用します

---

## 3. ステップ1: ツールのインストール

### Windows の場合

1. **Git for Windows** をインストール (https://git-scm.com/download/win)
2. **CMake** をインストール (インストーラで「Add CMake to the system PATH」にチェック)
3. **Arm GNU Toolchain** をインストール
   - ダウンロードページから `arm-gnu-toolchain-...-mingw-w64-i686-arm-none-eabi.exe` を実行
   - インストール完了時に「Add path to environment variable」にチェックを入れると楽です
4. インストール後、**コマンドプロンプト (またはPowerShell) を開き直して**確認:

```
git --version
cmake --version
arm-none-eabi-gcc --version
```

3つともバージョンが表示されればOKです。

### Linux (Ubuntu / Debian) の場合

```sh
sudo apt update
sudo apt install -y git cmake gcc-arm-none-eabi libnewlib-arm-none-eabi \
     libstdc++-arm-none-eabi-newlib
```

### macOS (Homebrew) の場合

```sh
brew install cmake armmbed/formbus/arm-gnu-toolchain
```

---

## 4. ステップ2: ソースコードの取得

ターミナル (WindowsはコマンドプロンプトやGit Bash) で:

```sh
git clone https://github.com/tomos2027/IchigoJam-RP2040Zero.git
cd IchigoJam-RP2040Zero
```

これで `IchigoJam-RP2040Zero` フォルダにソースコード一式が入ります。

---

## 5. ステップ3: 依存ライブラリの準備

本体のファームウェアは **pico-sdk** (RP2040用の開発キット) と
**pico-extras** (追加機能) を使ってビルドします。
リポジトリ直下に決まったバージョンで取得し、TinyUSB にパッチを当てます。

**リポジトリのフォルダ内で、以下のコマンドを順に実行**してください
(1行ずつコピー&ペースト):

```sh
# 1) pico-sdk (バージョン 2.3.1 / コミット 079c6f3)
git clone https://github.com/raspberrypi/pico-sdk.git
cd pico-sdk
git checkout 079c6f3
git submodule update --init --recursive

# 2) TinyUSB パッチ適用 (USBキーボード利用に必須)
cd lib/tinyusb
git apply ../../../patches/tinyusb_osal_task_delay.patch
cd ../../..

# 3) pico-extras (コミット 52fd7a7)
git clone https://github.com/raspberrypi/pico-extras.git
cd pico-extras
git checkout 52fd7a7
cd ..
```

実行後、リポジトリ直下が次のようになっていればOKです:

```
IchigoJam-RP2040Zero/
├── pico-sdk/      ← いま取得 (lib/tinyusb にパッチ済み)
├── pico-extras/   ← いま取得
├── patches/       ← TinyUSBパッチ (元からある)
├── src/           ← ファームウェアのソースコード
├── CMakeLists.txt
└── ...
```

> **ポイント**:
> - `pico-extras` は `pico-sdk` の**隣 (兄弟フォルダ)** に置くことで自動的に認識されます
>   (環境変数 `PICO_EXTRAS_PATH` の設定は不要です)
> - TinyUSBパッチが未適用の場合、ビルド時に
>   「TinyUSBのosal_pico.hにパッチが適用されていません」というエラーで
>   **意図的に停止するようになっています** (誤ったファームウェアの作成を防止)

---

## 6. ステップ4: コンパイル (ビルド)

リポジトリ直下で、**pico-sdkの場所を教えてからビルド**します:

### Linux / macOS の場合

```sh
export PICO_SDK_PATH=$PWD/pico-sdk
cmake -B build
cmake --build build
```

### Windows (コマンドプロンプト) の場合

```bat
set PICO_SDK_PATH=%CD%\pico-sdk
cmake -B build
cmake --build build
```

### Windows (PowerShell) の場合

```powershell
$env:PICO_SDK_PATH="$PWD\pico-sdk"
cmake -B build
cmake --build build
```

- 1回目の `cmake -B build` でビルドの準備をし (数秒〜数十秒)、
  2回目の `cmake --build build` でコンパイルします (**数分かかります**)
- `export PICO_SDK_PATH=...` は**ターミナルを開き直すたびに**再実行が必要です
  (設定を省略したい場合は `cmake -B build -DPICO_SDK_PATH=$PWD/pico-sdk` のように
  1行で指定することもできます)

### 成功の確認

ビルドが成功すると、最後に次のような表示が出ます:

```
[100%] Built target IchigoJam_RP2040Zero
```

そして **`build` フォルダの中に `IchigoJam_RP2040Zero.uf2`** が
作成されていれば成功です:

```
build/IchigoJam_RP2040Zero.uf2   ← これが書き込むファイル
```

> 2回目以降のビルドは変更された部分だけコンパイルされるため速いです。
> (`build` フォルダを削除すると最初からやり直しになります)

---

## 7. ステップ5: ボードへの書き込み

1. ボードの **BOOT ボタンを押しながら、RESET ボタンを押して離します**
   (RESET のみ搭載の個体・パッドの場合は、BOOTを押したままUSBケーブルを挿してもOK)
2. PCに **`RPI-RP2`** という名前のUSBドライブ (リムーバブルディスク) として
   認識されます
3. `build/IchigoJam_RP2040Zero.uf2` をそのドライブへ
   **ドラッグ&ドロップ (コピー)**
4. コピーが終わるとドライブは自動的に消え、**ボードが自動的に再起動して
   ファームウェアが起動します** (特別な操作は不要です)

---

## 8. ステップ6: 動作確認

1. **起動確認**: ボード上の内蔵LEDが **緑色に1回点滅** して消灯すれば
   ファームウェアは正常起動しています
2. **画面**: テレビの映像入力 (コンポジット) を選択すると、
   IchigoJamの起動画面 (タイトル + `OK`) が表示されます
3. **キーボード**: USBキーボードをType-Cへ接続して入力できます:

```bas
10 'TEST
20 LED 1:WAIT 30:LED 0
30 GOTO 20
RUN
```

内蔵LEDが赤く点滅すればBASICが動作しています。
`ESC` キーで停止します。詳しい使い方は `MANUAL_JA.md` を参照してください。

---

## 9. うまくいかないとき (トラブルシューティング)

| 症状 / エラーメッセージ | 原因 | 対処 |
|---|---|---|
| `arm-none-eabi-gcc: command not found` (コマンドが見つからない) | Armツールチェーンが未インストール、またはPATH未設定 | §3をやり直す。ターミナルを開き直して `arm-none-eabi-gcc --version` で確認 |
| `SDK location was not specified. Please set PICO_SDK_PATH...` | pico-sdkの場所が伝わっていない | §6の `PICO_SDK_PATH` 設定を確認 (ターミナルを開き直した場合は再実行) |
| `TinyUSBのosal_pico.hにパッチが適用されていません` | TinyUSBパッチ未適用 | §5の2) のパッチ適用コマンドを実行 (`git apply` は **リポジトリ直下で** 実行すると失敗します — 記載どおり `lib/tinyusb` フォルダ内で実行) |
| `PICO EXTRAS location was not specified...` | pico-extrasが見つからない | `pico-extras` を `pico-sdk` と同じフォルダに clone してください (§5の3)) |
| cmakeのバージョンが古いというエラー | CMake 3.13未満 | 新しいCMakeをインストール |
| コンパイル中に大量のエラー | pico-sdkのバージョンが違う / サブモジュール未取得 | §5の `git checkout 079c6f3` と `git submodule update --init --recursive` を確認 |
| `RPI-RP2` ドライブが現れない | ケーブルが充電専用 / BOOT+RESETの手順未達 | データ対応ケーブルに交換、BOOTを押したままRESET→離す手順をやり直す |
| 書き込んだが画面が出ない | 映像回路・テレビ入力の問題 | MANUAL_JA.md §2.2 (GP3+75Ω+560pF) と §11 トラブルシューティングを参照 |
| ビルドは成功するがBASICが起動しない | 古いUF2をコピーした可能性 | `build/IchigoJam_RP2040Zero.uf2` の日時を確認して再書き込み |

---

## 10. 上級編: 信号プリセットの切り替え

NTSC映像信号の構造は、コンパイル時オプション `NTSCV_SIG_PRESET` で
4種類から選べます (通常は既定のままで問題ありません):

| 値 | 信号 | 説明 |
|---|---|---|
| 0 (既定) | S_D | 525ライン真インターレース・規格EQ/SERR同期 @157.5MHz (60.0716Hz) |
| 1 | S_A | 262ライン プログレッシブ・元ドライバ再現 (60.19Hz) |
| 2 | S_B | S_A + 規格EQ/SERR同期 |
| 3 | S_C | S_D構造の旧クロック版 (60.07Hz) |

切り替えは cmake の設定時にオプションを追加します:

```sh
cmake -B build -DNTSCV_SIG_PRESET=1
cmake --build build
```

詳細は `docs/ntsc_video_spec.md` を参照してください。

---

## 依存ライブラリ・バージョン一覧

| ライブラリ | バージョン / コミット | 備考 |
|---|---|---|
| pico-sdk | 2.3.1 (コミット `079c6f3`) | サブモジュール取得必須 (lib/tinyusb ほか) |
| TinyUSB | pico-sdk同梱 | **`patches/tinyusb_osal_task_delay.patch` 適用必須** |
| pico-extras | コミット `52fd7a7` | pico-sdkの隣に配置 |
| Arm GNU Toolchain | 10.3以降 (14.2で動作確認) | arm-none-eabi |

---

## 参照

- ユーザーマニュアル: [MANUAL_JA.md](../MANUAL_JA.md)
- README (環境構築): [README_JA.md](../README_JA.md)
- NEO.*/NEOSCROLL リファレンス: [NEO_JA.md](NEO_JA.md)
- PSG サウンドリファレンス: [PSG_JA.md](PSG_JA.md)
- NTSC映像信号仕様: [ntsc_video_spec.md](ntsc_video_spec.md)
