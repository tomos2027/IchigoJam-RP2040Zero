# Docker ビルドガイド (初心者向け)

**Docker** を使うと、コンパイラや開発キットをPCにインストールすることなく、
このリポジトリのファームウェア (UF2ファイル) をコンパイルできます。

従来の方法 (ツールをPCにインストールしてビルド) は
[docs/BUILD_JA.md](BUILD_JA.md) を参照してください。

---

## 目次

1. [はじめに: Docker方式の特徴](#1-はじめに-docker方式の特徴)
2. [必要なもの](#2-必要なもの)
3. [ステップ1: ソースコードの取得](#3-ステップ1-ソースコードの取得)
4. [ステップ2: ビルド環境イメージの作成 (初回のみ)](#4-ステップ2-ビルド環境イメージの作成-初回のみ)
5. [ステップ3: コンパイル](#5-ステップ3-コンパイル)
6. [ステップ4: ボードへの書き込み](#6-ステップ4-ボードへの書き込み)
7. [信号プリセットの切り替え](#7-信号プリセットの切り替え)
8. [うまくいかないとき (トラブルシューティング)](#8-うまくいかないとき-トラブルシューティング)
9. [Docker方式と従来方式の比較](#9-docker方式と従来方式の比較)

---

## 1. はじめに: Docker方式の特徴

| 項目 | 内容 |
|---|---|
| インストールするもの | **Dockerだけ** (コンパイラ・CMake・開発キットは不要) |
| 依存ライブラリ | **自動で準備されます** (pico-sdk 2.3.1 + TinyUSBパッチ + pico-extras がイメージに同梱) |
| コンパイル方法 | スクリプト1つ (`build-docker.sh` / `build-docker.bat`) |
| 出力 | `build-docker/IchigoJam_RP2040Zero.uf2` (ホスト側フォルダに生成) |
| 環境の再現性 | イメージに依存ライブラリのバージョンが固定されるため、誰がビルドしても同じ結果になります |

```
docker build (初回のみ) ──→ ビルド環境イメージ完成
docker run (スクリプト) ──→ コンパイル ──→ build-docker/IchigoJam_RP2040Zero.uf2
ボードへコピー ──→ 完了
```

---

## 2. 必要なもの

| 品物 | 備考 |
|---|---|
| **Docker** | Windows/macOS: [Docker Desktop](https://www.docker.com/products/docker-desktop/)、Linux: Docker Engine。事前に起動しておいてください |
| ソースコード | GitHubから取得 (ステップ1) |
| Waveshare RP2040-Zero + Type-Cケーブル | 書き込み用 (ケーブルは**データ通信対応**のもの) |
| テレビ・USBキーボード等 | 動作確認用 (書き込みだけであれば不要) |

※ PCの性能によりますが、イメージ作成は初回のみ約3〜10分、
  以後のコンパイルは約1〜2分です。

---

## 3. ステップ1: ソースコードの取得

ターミナル (Windowsはコマンドプロンプト/PowerShell) で:

```sh
git clone https://github.com/tomos2027/IchigoJam-RP2040Zero.git
cd IchigoJam-RP2040Zero
```

**pico-sdk / pico-extras の取得は不要です** (イメージに含まれます)。

---

## 4. ステップ2: ビルド環境イメージの作成 (初回のみ)

リポジトリ直下で:

```sh
docker build -t neojam-build .
```

- イメージの中に、次のものが自動で準備されます:
  - ARMコンパイラ (arm-none-eabi-gcc)・CMake・make
  - **pico-sdk 2.3.1** (コミット `079c6f3`) + サブモジュール
  - **TinyUSBパッチ** (USBキーボード利用に必須)
  - **pico-extras** (コミット `52fd7a7`)
- 完了すると `neojam-build` という名前のイメージができます
- 依存ライブラリのバージョンを更新した場合は、同じコマンドを
  再実行してください (変更のあった部分だけ再構築されます)

---

## 5. ステップ3: コンパイル

付属のスクリプトを1つ実行するだけです:

### Linux / macOS の場合

```sh
./build-docker.sh
```

### Windows の場合

```bat
build-docker.bat
```

### 成功の確認

最後に次の表示が出て、**`build-docker` フォルダの中に
`IchigoJam_RP2040Zero.uf2`** が作成されていれば成功です:

```
[100%] Built target IchigoJam_RP2040Zero
```

> - 生成されるUF2はホスト側フォルダ (`build-docker/`) に作られるため、
>   そのままドラッグ&ドロップでボードに書き込めます
> - コンパイル結果 (build-dockerフォルダ) は通常のビルド (BUILD_JA.md の
>   `build/`) とは分離されているため、両方を混在させて使っても干渉しません

---

## 6. ステップ4: ボードへの書き込み

1. ボードの **BOOT ボタンを押しながら、RESET ボタンを押して離します**
2. PCに **`RPI-RP2`** ドライブが現れます
3. `build-docker/IchigoJam_RP2040Zero.uf2` をそのドライブへコピー
4. 自動的に再起動してファームウェアが起動します
   (内蔵LEDが緑色に1回点滅 → テレビにIchigoJam画面 → USBキーボードで入力可)

詳しくは [docs/BUILD_JA.md](BUILD_JA.md) の「ステップ5」「ステップ6」を参照してください。

---

## 7. 信号プリセットの切り替え

NTSC映像信号の構造は、環境変数 `NTSCV_SIG_PRESET` で切り替えられます
(通常は既定のままで問題ありません):

### Linux / macOS の場合

```sh
NTSCV_SIG_PRESET=1 ./build-docker.sh
```

### Windows (コマンドプロンプト) の場合

```bat
set NTSCV_SIG_PRESET=1
build-docker.bat
```

| 値 | 信号 |
|---|---|
| 0 (既定) | S_D: 525ライン真インターレース・規格EQ/SERR同期 @157.5MHz |
| 1 | S_A: 262ライン プログレッシブ・元ドライバ再現 |
| 2 | S_B: S_A + 規格EQ/SERR同期 |
| 3 | S_C: S_D構造の旧クロック版 |

詳細は `docs/ntsc_video_spec.md` を参照してください。

---

## 8. うまくいかないとき (トラブルシューティング)

| 症状 / エラーメッセージ | 原因 | 対処 |
|---|---|---|
| `docker: command not found` | Dockerが未インストール | §2からDockerをインストール |
| `Cannot connect to the Docker daemon` | Docker Desktop/Engineが起動していない | Docker Desktopを起動 (Windows/macOS)、`sudo systemctl start docker` (Linux) |
| `permission denied while trying to connect...` (Linux) | dockerグループに未参加 | `sudo usermod -aG docker $USER` 実行後、ログインし直す (或いはコマンドの頭にsudo) |
| イメージ作成時に `server certificate verification failed` 等 | ネットワーク/プロキシの証明書問題 | 会社・学校のプロキシ環境ではDocker Desktopのプロキシ設定や社内CAの登録が必要な場合があります |
| コンパイル中に「パッチが適用されていません」系のエラー | イメージが古い | `docker build -t neojam-build .` を再実行してイメージを更新 |
| `build-docker/IchigoJam_RP2040Zero.uf2` ができない | スクリプトの途中でエラー | 直前のエラーメッセージを確認 (上記の各項目も参照) |
| 生成ファイルがroot所有になる (Linux) | `-u` オプション無しで実行した | 付属の `build-docker.sh` を使用 (ホストユーザで実行します) |
| `RPI-RP2` ドライブが現れない | ケーブルが充電専用 / 手順未達 | [docs/BUILD_JA.md](BUILD_JA.md) §9 を参照 |
| 磁気ディスク容量不足 | イメージは約1〜2GB使用します | `docker system df` で確認、不要なイメージは `docker image prune` で削除 |

---

## 9. Docker方式と従来方式の比較

| 項目 | Docker方式 (本書) | 従来方式 ([BUILD_JA.md](BUILD_JA.md)) |
|---|---|---|
| PCへのインストール | Dockerのみ | Git + CMake + Arm GNU Toolchain |
| 依存ライブラリの準備 | 自動 (イメージに同梱) | 手動でclone・バージョン固定・パッチ適用 |
| 初回所要時間 | 約10〜15分 (イメージ作成含む) | 約30〜60分 |
| 2回目以降のコンパイル | 約1〜2分 (スクリプト1つ) | 約1〜2分 |
| 環境の再現性 | 高い (イメージで固定) | ツールバージョンに依存 |
| 向いている人 | 手早くファームを作りたい人・Linux未経験者 | 開発 (ソース編集+デバッグ) を継続する人 |

どちらの方式でも生成されるUF2は同じです。

---

## 参照

- 従来のビルド手順 (ツールをPCにインストール): [BUILD_JA.md](BUILD_JA.md)
- ユーザーマニュアル: [MANUAL_JA.md](../MANUAL_JA.md)
- NEO.*/NEOSCROLL リファレンス: [NEO_JA.md](NEO_JA.md)
- NTSC映像信号仕様: [ntsc_video_spec.md](ntsc_video_spec.md)
