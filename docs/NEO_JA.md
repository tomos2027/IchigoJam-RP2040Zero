# NEO.* / NEOSCROLL.* 詳細マニュアル

Waveshare RP2040-Zero版 IchigoJam (`IchigoJam-RP2040Zero-NTSC`) の
NeoPixel (WS2812B) 制御コマンド群 **NEO.\*** / **NEOSCROLL.\*** の詳細リファレンスです。
[Annex WiFi Basic](https://github.com/cicciocb/annex-wifi-basic) 互換の文法に準拠していますが、
本機固有の制限があります (本書の「共通仕様」を必ずお読みください)。

概要はユーザーマニュアル `MANUAL_JA.md` の 7.11 節も参照してください。

---

## 目次

1. [共通仕様](#1-共通仕様)
2. [NEO.* (ストリップ制御)](#2-neo-ストリップ制御)
3. [NEO.* 色関数](#3-neo-色関数)
4. [NEOSCROLL.* (ドットマトリクス制御)](#4-neoscroll-ドットマトリクス制御)
5. [NEOSCROLL スクロール動作の詳細](#5-neoscroll-スクロール動作の詳細)
6. [サンプルプログラム](#6-サンプルプログラム)
7. [制限事項・注意](#7-制限事項注意)
8. [エラー一覧](#8-エラー一覧)

---

## 1. 共通仕様

### 1.1 バッファと送信経路

- NEO.\* / NEOSCROLL.\* は `WS.PUT` / `WS.MATRIX` (WS.MODE 3) と **同じ8KB専用バッファ** (GRB 3バイト/px、最大 **2730px**) を共用します。
- 送信は `WS.LED` と同じPIO送信経路 (`ws_out`) を使うため、以下がそのまま有効です:
  - **`WS.SETUP` の出力信号反転** (逆相ドライバ/トランジスタ経由駆動用)
  - **SDON中の共有ピン保護** (ポート1/3/4/10/11 はブロック)
- バッファを共有するため、NEO.\* と NEOSCROLL.\* を混在させると最後に送信した内容で上書きされます (§7.2)。

### 1.2 色の形式 (RGB555パック色)

BASICの式が16bit整数のため、色は **16bit RGB555パック** で表現します
(Annexの24bit色相当は使用できません)。

```
COLOR = (R << 10) | (G << 5) | B     ※各成分 0〜31、COLOR全体で 0〜32767
```

- 8bit値からパック色へ変換するには `NEO.RGB(r,g,b)` を使います (各成分0-255、下位5bitに丸められます)。
- パック色から8bitへ展開する場合は `5bit値 → (v << 3) | (v >> 2)` で行われます (31→255)。
- 定数で直接指定する場合は `NEO.RGB(255,0,0)` = 1024 (赤)、`NEO.RGB(0,31,0)` = 992 (緑) のようになります。

### 1.3 gain / progress (1/256固定小数)

明るさ・ブレンド系の `gain` / `progress` / `p` / `x` / `y` は **1/256固定小数** です。

| 値 | 意味 |
|---|---|
| 0 | 0.0倍 |
| 128 | 0.5倍 |
| **256** | **1.0倍 (等倍)** |
| 512 | 2.0倍 (NEO.DIMのみ) |

### 1.4 ポート番号

ポートは `WS.LED` と同じ 0〜11 です。

| ポート | GPIO | 場所 |
|---|---|---|
| 0 | GP16 | 基板内蔵WS2812 (起動表示・エラー診断と共用) |
| 1〜6 | GP8〜GP13 | OUT1〜OUT6 (側面パッド) |
| 7 | GP15 | 側面パッド (外部WS2812標準) |
| 8 | GP2 | 側面パッド |
| 9〜11 | GP26/6/7 | IN2/IN3/IN4 (=OUT9〜11) |

※ SDON中はポート 1/3/4/10/11 (SD共有ピン) への送信はブロックされます。

### 1.5 状態の保持

- `NEO.SETUP` / `NEOSCROLL.SETUP` の設定 (ポート・LED数・キャンバス等) は **`CLV` でもリセットされません**。電源投入時の初期値に戻すには再起動するか、`NEOSCROLL.DELETE` を実行します。
- 電源投入時の初期値:

| 項目 | 初期値 |
|---|---|
| NEO ポート | 7 (GP15) |
| NEO LED数 | 8 |
| NEOSCROLL | 未使用 (`NEOSCROLL.SETUP` が必要) |

### 1.6 自動送信と disable 引数

NEO.\* の描画コマンドは既定では **書換のたびに即座にLEDへ送信** します。
最後尾の引数 `disable` に 1 を指定すると書込みのみ行い送信を省略できます。
連続書換した後に `NEO.REFRESH` を1回呼ぶことで、ちらつきや送信時間を削減できます。

---

## 2. NEO.* (ストリップ制御)

テープ状に接続したWS2812Bを位置 (0〜nb_led-1) 指定で制御します。

### 2.1 NEO.SETUP

```
NEO.SETUP port{,nb_led}
```

- `port`: ポート番号 0〜11 (§1.4)。
- `nb_led`: LED数 1〜2730。**省略時は現状維持** (初期値8)。
- 専用バッファ (WS.MODE 3) モード・オフセット0 に設定します (`WS.MODE` の設定を上書きします)。
- 例: `NEO.SETUP 7,16` — GP15に16個のテープ。

### 2.2 NEO.PIXEL

```
NEO.PIXEL pos,R,G,B{,disable}
NEO.PIXEL pos,COLOR{,disable}
NEO.PIXEL pos,"記号"{,disable}
```

- 位置 `pos` (0〜nb_led-1) の1pxに色を設定します。
- 引数の個数で自動判別します: **文字列 = 色記号**、1〜2個 = パック色指定、3〜4個 = R,G,B個別指定。
- R,G,B は各 0〜255 (範囲外はクランプ)。
- `"記号"` は色記号文字列 (§4.9 の表、NEOSCROLL.PRINT と共通)。**先頭1文字**を適用します。
  未定義の記号/空文字列は Illegal argument。
- ※ **裸の英字 (`B` 等) は色記号ではなく BASIC変数** です (変数名と衝突するため文字列のみ対応)。
  誤って `NEO.PIXEL 1,B` と書くと変数B (通常0=黒) が指定されます。
- 例:
  - `NEO.PIXEL 0,"R"` — 先頭を赤に (色記号)。
  - `NEO.PIXEL 0,255,0,0` — 同じく赤 (R,G,B指定)。
  - `NEO.PIXEL 0,NEO.RGB(255,0,0)` — 同じく赤 (パック色指定)。

### 2.3 NEO.STRIP

```
NEO.STRIP s,e,R,G,B{,disable}
NEO.STRIP s,e,COLOR{,disable}
NEO.STRIP s,e,"記号"{,disable}
```

- `s`〜`e` (**両端含む**) の連続領域を一括設定します。
- `"記号"` は色記号文字列 (§2.2と同じ。先頭1文字を適用)。
- エラー条件: `s < 0`、`e < s`、`e >= nb_led` → Illegal argument。
- 例: `NEO.STRIP 0,7,0,40,0` — 全体を緑に。 `NEO.STRIP 0,7,"R"` — 全体を赤に。

### 2.4 NEO.ROTATELEFT / NEO.ROTATERIGHT

```
NEO.ROTATELEFT n{,s,e,disable}
NEO.ROTATERIGHT n{,s,e,disable}
```

- 範囲 `[s,e]` (省略時は全体 0〜nb_led-1) 内で内容を回転します。**端の画素は反対側へ周回**します (消えません)。
- `n` は移動量 (0以上)。範囲長で剰余を取るため大きな値も指定できます。
- `ROTATELEFT` は内容が先頭 (s) 方向へ、`ROTATERIGHT` は終端 (e) 方向へ移動します。
- 追加メモリを使わず1ステップずつ回転するため、大きな `n` では時間がかかります。
- 例 (8個テープ全体を左へ1個回転):
  ```bas
  NEO.ROTATELEFT 1
  ```
- 部分範囲の例 (2〜5のみ回転): `NEO.ROTATELEFT 1,2,5`

### 2.5 NEO.SHIFTLEFT / NEO.SHIFTRIGHT

```
NEO.SHIFTLEFT n{,s,e,disable}
NEO.SHIFTRIGHT n{,s,e,disable}
```

- 範囲 `[s,e]` 内で内容をシフトします。**空いた画素は黒 (消灯)** になります (回転と違い周回しません)。
- `n` >= 範囲長のときは範囲全体が黒になります。
- 例: `NEO.SHIFTLEFT 1` — 全体を左へ1個ずらし、末尾が消灯。

### 2.6 NEO.REFRESH

```
NEO.REFRESH
```

- 専用バッファ全体 (0〜nb_led-1) を `NEO.SETUP` のポートへ送信します。
- `disable=1` 付きで書換した後の明示送信に使用します。

---

## 3. NEO.* 色関数

すべて戻り値は **RGB555パック色** (§1.2) です。引数の括弧は省略できません。

### 3.1 NEO.RGB

```
NEO.RGB(r,g,b)
```

- 8bit (0-255) のR,G,Bからパック色を作ります。各成分は0-255にクランプ後、下位5bitへ丸められます。
- 例: `NEO.RGB(255,128,0)` — オレンジ。

### 3.2 NEO.GETPIXEL

```
NEO.GETPIXEL(pos)
```

- バッファから位置 `pos` の現在色を読み出してパック色で返します。
- `pos` が 0〜nb_led-1 以外は Illegal argument。

### 3.3 NEO.DIM

```
NEO.DIM(COLOR,gain)
```

- 明るさを `gain` 倍します。`gain` 0-512 (1/256倍率、**256=等倍**、512=2倍)。
- 計算後、各成分は255を超えないようクランプされます (2倍ではほぼ飽和)。
- 例: `NEO.DIM(NEO.RGB(255,0,0),128)` — 半分の明るさの赤。

### 3.4 NEO.LIGHTEN

```
NEO.LIGHTEN(COLOR,gain)
```

- 白へブレンドします。`gain` 0-256 (0=原色、**256=白**)。
- 例: `NEO.LIGHTEN(NEO.RGB(0,0,255),128)` — 中間の明るい青。

### 3.5 NEO.DARKEN

```
NEO.DARKEN(COLOR,gain)
```

- 黒へブレンドします。`gain` 0-256 (0=原色、**256=黒**)。
- 例: `NEO.DARKEN(NEO.RGB(255,255,0),64)` — 4分の1暗い黄。

### 3.6 NEO.LINEARBLEND

```
NEO.LINEARBLEND(C1,C2,p)
```

- 2色 `C1`,`C2` を比率 `p` (0-256、**0=C1、256=C2**) で線形補間します。
- 例: 0→255のグラデーション:
  ```bas
  FOR I=0 TO 16
    NEO.PIXEL I,NEO.LINEARBLEND(NEO.RGB(0,0,255),NEO.RGB(255,0,0),I*16),1
  NEXT
  NEO.REFRESH
  ```

### 3.7 NEO.BILINEARBLEND

```
NEO.BILINEARBLEND(UL,UR,LL,LR,x,y)
```

- 4隅の色 `UL`(左上) `UR`(右上) `LL`(左下) `LR`(右下) を2次元補間します。
- `x`,`y` は 0-256。`(0,0)`=UL、`(256,0)`=UR、`(0,256)`=LL、`(256,256)`=LR。
- 例: `NEO.BILINEARBLEND(C1,C2,C3,C4,128,128)` — 4色の中間色。

---

## 4. NEOSCROLL.* (ドットマトリクス制御)

WS2812Bドットマトリクス (AnnexのNeoPixel Dot Matrix互換) にテキスト表示・スクロールを行います。
8x8モジュールのほか、**5x5・4x4など縦横独立のカスタム寸法モジュール** (`mw,mh` 引数) にも対応しています。

### 4.1 動作モデル (キャンバスとウィンドウ)

描画は **キャンバス** (仮想画面) に行い、表示は **ウィンドウ** (実LED寸法) を通して行います。

```
キャンバス (w x h px、最大8192px)
 ┌──────────────────────┐
 │  A ← NEOSCROLL.PRINT等で描画   │
 │   ┌───────┐            │
 │   │ ウィンドウ │───────────→ 実LED (dev*8 x lines*8)
 │   │(8x8など) │  SHOW x,y / SCROLL で位置が動く
 │   └───────┘            │
 └──────────────────────┘
```

- ウィンドウ位置 `(x,y)` は **キャンバス左上からのオフセット** です。
- `NEOSCROLL.SHOW x,y` で指定位置へ移動して表示、`NEOSCROLL.SCROLL` / `NEOSCROLL.OSCILLATE` で1pxずつ自動移動します。
- キャンバス寸法 `w,h` を省略するとウィンドウと同じ寸法になり、スクロール窓の可動域は0になります (静止表示)。

### 4.2 NEOSCROLL.SETUP

```
NEOSCROLL.SETUP devices,lines,port{,layout}{,width,height}{,flip}{,mw,mh}
```

- `devices`: 横方向のモジュール数 1-255 → 表示幅 = devices×mw px。
- `lines`: 縦方向のモジュール数 1-255 → 表示高 = lines×mh px。
- `port`: 出力ポート 0-11 (§1.4)。
- `layout`: 配線レイアウト 0-5 (下表)。省略時0。
- `width,height`: キャンバス寸法 (px)。省略時は表示寸法と同じ。合計8192px以内。
- `flip`: 1 = 表示を上下反転 (180度回転: 左右も同時に反転します)。省略時0。
- `mw,mh`: 1モジュールの寸法 (px、1-64、縦横独立)。省略時は **8x8** (Annex互換)。
  5x5パネルなら `5,5`、横長の自作モジュールなら `8,4` のように指定します。
  省略引数チェーンのため `mw,mh` を指定する場合は `layout,width,height,flip` も記述が必要です。
- 表示px合計 (devices×lines×mw×mh) は **2730px以内**。
- 実行すると全状態 (明るさ・フォント・カーソル・スクロール位置等) が初期化され、キャンバスは黒でクリアされます。

カスタムモジュールの例:

```bas
NEOSCROLL.SETUP 2,1,7,1,40,5,0,5,5  ' 5x5モジュール2個横 (10x5px表示) + キャンバス40x5
NEOSCROLL.SETUP 1,1,7,0,0,0,0,4,4   ' 4x4モジュール1個 (4x4px表示)
```

`layout` (配線の種類。Annexの公式割当が確認できないため一般的な6構成の割当です):

| 値 | 配線 | 説明 |
|---|---|---|
| 0 | 行優先 | 上の行から順に、各行は左→右 |
| 1 | 行優先ジグザグ | 行ごとに方向が折り返す (ヘビ配線) |
| 2 | 列優先 | 左の列から順に、各列は上→下 |
| 3 | 列優先ジグザグ | 列ごとに方向が折り返す |
| 4 | モジュール単位・行方向 | 各モジュール (mw×mh) は正方向、モジュールは行内で左→右 |
| 5 | モジュール単位・行方向ジグザグ | モジュール行ごとにモジュールの並びが折り返す |

※ layout 0-3 はモジュール内の配線を考慮しないためモジュール寸法に依存しません
(表示寸法だけが devices×mw / lines×mh に変わります)。モジュール単位の配線は layout 4/5 を使用してください。

### 4.3 NEOSCROLL.DELETE

```
NEOSCROLL.DELETE
```

- NEOSCROLLの全状態をリセットし、未使用状態に戻します (以降のNEOSCROLLコマンドはIllegal argument)。

### 4.4 NEOSCROLL.FILL

```
NEOSCROLL.FILL COLOR{,x,y,w,h}
```

- キャンバスの領域 `(x,y)` から幅 `w`、高さ `h` を `COLOR` で塗り潰します。
- `x` 以降を省略すると **キャンバス全面** を塗り潰します。
- 例: `NEOSCROLL.FILL NEO.RGB(0,0,64)` — 全面を暗い青に。

### 4.5 NEOSCROLL.TEXT.POS

```
NEOSCROLL.TEXT.POS x,y
```

- テキスト描画カーソルを設定します。**左上原点・y下向き** です
  (※Annexは左下原点のため、既存のAnnex用プログラムを流用する場合は注意)。

### 4.6 NEOSCROLL.TEXT.FONT

```
NEOSCROLL.TEXT.FONT n
```

- 内蔵フォントを選択します (1-5)。省略時は1。

| n | フォント | 高さ |
|---|---|---|
| 1 | 5x7 プロポーショナル (既定) | 7px |
| 2 | 6x12 | 12px |
| 3 | 8x13 | 13px |
| 4 | 9x15 | 15px |
| 5 | 10x20 | 20px |

- 対応するのは半角ASCII (0x20-0x7E) のみです。範囲外の文字はスキップされます。
- フォントはEJコマンド (OLED表示) と同じビットマップです。

### 4.7 NEOSCROLL.TEXT.BRIGHTNESS

```
NEOSCROLL.TEXT.BRIGHTNESS b
```

- テキストの明るさ 0-255 (既定255=等倍)。`PRINT` 実行時に色へ適用されます。

### 4.8 NEOSCROLL.BRIGHTNESS

```
NEOSCROLL.BRIGHTNESS b
```

- 表示全体の明るさ 0-255 (既定 **255=等倍**)。`NEOSCROLL.SHOW` の転送時に全画素へ適用されます。
- 夜間の眩しさ軽減などに使用します (例: `NEOSCROLL.BRIGHTNESS 128` で半分の明るさ)。バッファの色自体は変わりません。

### 4.9 NEOSCROLL.PRINT

```
NEOSCROLL.PRINT "text"{,"colors"}
```

- 現在のカーソル位置へテキストを描画します。
- 引数は **文字列リテラルのみ** (式・変数は指定できません)。
- `colors` は1文字が `text` の1文字に対応する **色記号** (下表、Annex互換)。省略可能。
- `colors` が文字列より短い、または省略した部分は **前回の色を引き継ぎ** ます (SETUP直後は黒)。
- 色記号に未定義の文字があった場合、その文字の色は変更されません。
- 改行 `\n` で次行の行頭へ移動します (行送りはフォント高さ+1px)。
- 描画は **OR書込み** (オン画素のみ書く) のため、既存の描画は消えません。上書きしたい場合は先に `FILL` で消してください。

色記号一覧 (大文字=標準色、小文字=暗色)。出力値はRGB555量子化後の実際の表示値 (R,G,B) です。

**使用できる場所**:
- `NEOSCROLL.PRINT "text","colors"` の色文字列 (1文字=1文字に対応)
- `NEO.PIXEL pos,"記号"` / `NEO.STRIP s,e,"記号"` の文字列引数 (先頭1文字を適用)

※ **裸の英字 (`B` 等) は色記号ではなく BASIC変数** です。`NEO.PIXEL 1,B` は変数B (通常0=黒) の指定になります。

| 記号 | 色 | 出力値 | 記号 | 色 | 出力値 |
|---|---|---|---|---|---|
| `B` | 青 | (0,0,255) | `b` | 紺 | (0,0,132) |
| `C` | シアン | (0,255,255) | `c` | 濃シアン | (0,140,140) |
| `G` | 緑 | (0,255,0) | `g` | 濃緑 | (0,99,0) |
| `K` | 黒 (消灯) | (0,0,0) | `k` | 濃灰 | (173,173,173) |
| `M` | マゼンタ | (255,0,255) | `m` | マルーン (暗赤に近い色) | (132,0,0) |
| `R` | 赤 | (255,0,0) | `r` | 暗赤 | (140,0,0) |
| `Y` | 黄 | (255,255,0) | `y` | 黄緑 | (173,255,41) |
| `O` | オレンジ | (255,165,0) | `o` | オリーブ | (132,132,0) |
| `P` | 紫 | (132,0,132) | `p` | ピンク | (255,198,206) |
| `W` | 白 | (255,255,255) | `w` | 明灰 | (214,214,214) |
| `S` | シルバー | (198,198,198) | | | |

※ 色値はAnnex (HTML色名) 準拠です。`K` は消灯、`m` と `r` は近い色ですが区別できます。
   既定輝度は255のため全色が設計通りの明るさで表示されます (暗くしたい場合は `NEOSCROLL.BRIGHTNESS` を使用)。

例:

```bas
NEOSCROLL.PRINT "STOP","RRRGGG"    ' STOPを R,R,R,G,G,G の6色で描画
NEOSCROLL.PRINT "OK!"              ' 色省略 → 緑 (前回色) のまま
```

### 4.10 NEOSCROLL.SHOW

```
NEOSCROLL.SHOW x,y
```

- キャンバスの `(x,y)` をウィンドウ左上として表示範囲をバッファへ展開し、`BRIGHTNESS` を適用してLEDへ転送します。
- キャンバスの範囲外は黒として扱われます。
- 明示的に表示位置を指定する場合に使用します (`SCROLL` 併用時は通常 `SHOW NEOSCROLL.X(),NEOSCROLL.Y()` を使います)。

### 4.11 NEOSCROLL.LIMITS

```
NEOSCROLL.LIMITS x1{,x2}{,y1}{,y2}
```

- スクロール位置 (`X()`/`Y()`) の可動範囲を設定します (窓オフセットの範囲)。
- 省略した引数は現状維持。`SETUP` 直後はキャンバス全域が可動域です
  (x方向: 0〜キャンバス幅-表示幅、y方向: 0〜キャンバス高-表示高)。
- 順序は大小逆でも構いません (内部で正規化されます)。
- 例 (横方向のみ0〜24で往復): `NEOSCROLL.LIMITS 0,24`

### 4.12 NEOSCROLL.MODE

```
NEOSCROLL.MODE m
```

- スクロール方向を設定します。

| m | 方向 |
|---|---|
| 0 | 右→左 (既定) |
| 1 | 下→上 |
| 2 | 左→右 |
| 3 | 上→下 |

### 4.13 NEOSCROLL.SYNC

```
NEOSCROLL.SYNC
```

- スクロール位置を現在の `MODE` の **始端** へ移動し、往復方向を順方向に戻します。
  - MODE 0 (右→左): 右端
  - MODE 1 (下→上): 下端
  - MODE 2/3: 左端/上端
- スクロールを最初からやり直すときに使用します。

### 4.14 NEOSCROLL.SCROLL / NEOSCROLL.OSCILLATE

```
NEOSCROLL.SCROLL            (コマンド形式)
A = NEOSCROLL.SCROLL        (関数形式)
NEOSCROLL.OSCILLATE         (コマンド形式)
A = NEOSCROLL.OSCILLATE     (関数形式)
```

- 呼び出しごとにスクロール位置を現在の `MODE` 方向へ1px進めます。
- **コマンド形式**: 進めるだけです。
- **関数形式**: 進めて、周期完了時に1を返します。
  - `SCROLL`: 可動範囲の端に達して反対端へ周回したとき1 (永久ループ用)。
  - `OSCILLATE`: 端で反転し、**始点へ戻ってきたとき** に1 (1周期で停止する演出用)。
- 戻り値を使わず `NEOSCROLL.SCROLL` と書いた場合はコマンドとして動作します。

### 4.15 NEOSCROLL.X() / NEOSCROLL.Y()

```
NEOSCROLL.X()
NEOSCROLL.Y()
```

- 現在のスクロール位置 (ウィンドウオフセット) を返します。括弧は必須です。

---

## 5. NEOSCROLL スクロール動作の詳細

### 5.1 典型的な永久スクロールの構成

```bas
10 NEOSCROLL.SETUP 2,1,7,1        ' 16x8表示、行ジグザグ
20 NEOSCROLL.MODE 0               ' 右→左
30 NEOSCROLL.TEXT.POS 0,1
40 NEOSCROLL.PRINT "HELLO!","Y"
50 NEOSCROLL.SYNC                 ' 右端から開始
60 A=NEOSCROLL.SCROLL             ' 1px進める (周回時A=1)
70 NEOSCROLL.SHOW NEOSCROLL.X(),NEOSCROLL.Y()
80 WAIT 6                         ' スクロール速度調整 (約30ms/px)
90 GOTO 60
```

- `WAIT` の値で速度を調整します (1ループ≒5ms×WAIT+処理時間)。
- テキストはキャンバスに書かれたままなので、`SCROLL` はウィンドウを動かすだけです。
- 表示位置が移動すると文字は左から順に入ってきますが、キャンバス=表示寸法 (可動域0) の場合は
  `SCROLL` しても位置が変わらないため `SHOW` の呼び方が同じでも問題ありません。

### 5.2 往復 (OSCILLATE) の例

```bas
10 NEOSCROLL.SETUP 1,1,7,1,0,32,8  ' 8x8表示 + 幅32pxのキャンバス
20 NEOSCROLL.MODE 0
30 NEOSCROLL.TEXT.POS 0,1
40 NEOSCROLL.PRINT "WAVE","C"
50 NEOSCROLL.SYNC
60 A=NEOSCROLL.OSCILLATE           ' 端で反転、始点復帰でA=1
70 NEOSCROLL.SHOW NEOSCROLL.X(),0
80 WAIT 6
90 IF A=0 GOTO 60                  ' 1周期で終了
```

---

## 6. サンプルプログラム

### 6.1 ストリップ: 色グラデーション+回転

```bas
10 NEO.SETUP 7,16
20 FOR I=0 TO 15
30   NEO.PIXEL I,NEO.LINEARBLEND(NEO.RGB(255,0,0),NEO.RGB(0,0,255),I*17),1
40 NEXT
50 NEO.REFRESH
60 WAIT 30
70 NEO.ROTATELEFT 1
80 GOTO 60
```

### 6.2 マトリクス: 静止テキスト表示

```bas
10 NEOSCROLL.SETUP 1,1,7,1
20 NEOSCROLL.FILL 0
30 NEOSCROLL.TEXT.POS 0,1
40 NEOSCROLL.PRINT "HI","G"
50 NEOSCROLL.BRIGHTNESS 64
60 NEOSCROLL.SHOW 0,0
```

### 6.3 マトリクス: 2モジュール横スクロール

§5.1 のプログラムを参照してください。

---

## 7. 制限事項・注意

### 7.1 Annexとの相違点

| 項目 | Annex | 本機 |
|---|---|---|
| 色 | 24bit (0-16777215) | **16bit RGB555 (0-32767)** |
| SPRITESHEET / SPRITE (BMP描画) | 対応 | **非対応** |
| フォント | BMPで追加可能 | **内蔵5種のみ** |
| TEXT.POS の原点 | 左下 | **左上** (y下向き) |
| モジュール寸法 | 8x8固定 | **縦横自由 (1-64px、SETUP引数 mw,mh)** |

### 7.2 バッファ共有

- NEO.\* と NEOSCROLL.\*、および `WS.PUT` / `WS.MATRIX` (mode3) は **同じバッファを共有** します。
- `NEOSCROLL.SHOW` は表示寸法分のバッファを全上書きするため、NEO.\* で描いた内容は消えます。逆に `NEO.PIXEL` 等でバッファを書換すると、次の `NEOSCROLL.SHOW` で再び上書きされます。
- 混在させる場合は「片方ずつ描いてすぐ送信」するか、送信ポートを分けて運用してください。

### 7.3 その他

- `NEOSCROLL.PRINT` は文字列リテラルのみ対応です (変数・式は不可)。
- 描画可能文字は半角ASCII (0x20-0x7E) のみです。それ以外 (かな・漢字等) は読み飛ばされます。
- 設定は `CLV` でリセットされません (§1.5)。
- NEO.\* の実行は `WS.MODE` の設定を mode3/オフセット0 に変更します。NEO使用後に `WS.LED` を配列モード (mode0) 等で使う場合は `WS.MODE` で戻してください。

---

## 8. エラー一覧

| 条件 | エラー |
|---|---|
| `NEO.SETUP` の port 0-11 / nb_led 1-2730 外 | Illegal argument |
| `NEO.PIXEL` / `NEO.GETPIXEL` の pos が nb_led 外 | Illegal argument |
| `NEO.STRIP` / `ROTATE*` / `SHIFT*` の範囲が不正 (s<0、e<s、e>=nb_led、n<0) | Illegal argument |
| `NEO.PIXEL` / `NEO.STRIP` の色記号文字列が未定義記号/空文字列 | Illegal argument |
| `NEOSCROLL.SETUP` の寸法・layout・flip 等が範囲外、合計2730px超、キャンバス8192px超 | Illegal argument |
| NEOSCROLL.SETUP 前に NEOSCROLL.* コマンドを実行 | Illegal argument |
| `NEOSCROLL.TEXT.FONT` の n が 1-5 外 | Illegal argument |
| `NEOSCROLL.BRIGHTNESS` / `TEXT.BRIGHTNESS` の b が 0-255 外 | Illegal argument |
| `NEOSCROLL.MODE` の m が 0-3 外 | Illegal argument |
| `NEOSCROLL.PRINT` の引数が文字列でない | Syntax error |
| カンマ/括弧の対応が不正 | Syntax error |

エラー発生時はメッセージが表示され、エラーの起きた行 (プログラム実行時) で実行が停止します。
