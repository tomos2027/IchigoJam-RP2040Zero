/*----------------------------------------------------------------------------

ntscv — NTSC composite video output library for RP2040 (PWM + DMA)

Copyright (c) 2026 the IchigoJam-RP2040Zero authors. MIT license.

PWM+DMAによるNTSC信号生成の着想は @lovyan03 氏による (手法・アイデアは著作権の
保護対象外)。本ライブラリは公開規格 (NTSC / SMPTE 170M) と pico-sdk (BSD-3-Clause)
の情報のみから仕様書 (docs/ntsc_video_spec.md) に基づいて新規に記述した独自実装
であり、既存のNTSCドライバ実装からのコード流用はない。

用途・条件はリポジトリの LICENSE および README のライセンス節を参照。

----------------------------------------------------------------------------*/

#ifndef NTSCV_H
#define NTSCV_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*==== 信号プリセット (コンパイル時切替、docs/ntsc_video_spec.md §2 参照) ====

   0 (既定 / S_D): 525ライン真インターレース @157.5MHz, 60.0716Hz (現行。
                   157.333333MHzは色ロックしないTVがあるため廃止)
   1 (S_A):        262ライン プログレッシブ @157.5MHz, 60.19Hz (元ドライバ再現)
   2 (S_B):        262ライン プログレッシブ @157.5MHz, 60.19Hz + 規格EQ/SERR同期
   3 (S_C):        525ライン真インターレース @157.5MHz, 60.07Hz (現行構造、旧クロック)

   既定値(S_D)を指定しないビルドでは、既存動作を変えない。
   S_A/S_B/S_Cは「線の原因が信号構造のどこか」という切り分け用。
*/
#ifndef NTSCV_SIG_PRESET
#define NTSCV_SIG_PRESET 0
#endif

#if NTSCV_SIG_PRESET == 0
/*--- S_D: 525ライン真インターレース (現行) ---*/
/* システムクロック157.5MHz (set_sys_clock_khz(157500)。元ドライバと同一)、
   PWM周期11サイクル → サンプルレート14.31818MHz (色信号は1周期4サンプル構成)。
   1走査線908サンプル (9988サイクル = 63.4286µs)、1フレーム525ライン
   (262.5ライン×2フィールド = 真のNTSCインターレース: フィールド2の同期系は
   半ライン (454サンプル) 遅れ。デインターレーサがフィールドパリティを
   判定でき、液晶TVの1ライン上下揺れ/ボブ反転がなくなる)。
   フィールド周期 = 262.5×9988/157500000 = 16646.8µs = 60.0716Hz。
     固定60Hzパネルの液晶TV/キャプチャは入力に同期しないため、フィールドレートが
     60Hzからずれるとリフレッシュ境界が「薄い線」として画面内を滑る (うなり)。
     60.0716Hzのうなりは0.0716Hz=約14秒周期。
   ※サブキャリア相当周波数 = サンプルレート/4 = 3.579545MHz (NTSC規格値ときっかり
     一致。元ドライバ「実機カラー実績あり」と同一クロック)。
     ※過去に157.333333MHz (60.0085Hz、うなり約117秒) を試したが、サブキャリアが
     規格より-0.11% (−3788Hz) ずれTVの色復調PLLがロックせず白黒表示になる機種が
     あったため157.5MHzへ変更 (色優先。色ロックしない機種が出た場合は
     本定数群とLINES_PER_FRAMEを調整してフィールドレートを緩める)。
   ※910サンプル (227.5周期・ライン毎にバースト位相180°交互) は過去に試したが、
     実機では色表示の改善が得られず、むしろ起動画面の問題と相関したため廃止。 */
#define NTSCV_SYS_CLOCK_HZ    157500000u
#define NTSCV_SYS_PLL_VCO_HZ  0u        /* set_sys_clock_khz経由: core.cでは未使用 */
#define NTSCV_FIELDS_PER_FRAME 2
#define NTSCV_LINES_PER_FRAME 525       /* 262.5ライン×2フィールド (真インターレース) */
#define NTSCV_F1_VSYNC_END     9        /* ライン0-9  : f1垂直同期                     */
#define NTSCV_F1_ANCHOR_END    17       /* ライン10-17: ブランキング全書込アンカー     */
#define NTSCV_F1_BLANK1_END    35       /* ライン18-35: ブランキング(前) — 再生        */
#define NTSCV_F1_ACTIVE_START  36
#define NTSCV_F1_ACTIVE_END    251
#define NTSCV_F1_MARK          252      /* フィールド境界マーク (drawcount/表示更新)   */
#define NTSCV_F1_ERASE_END     260      /* ライン252-260: 内容部黒消し (261は再生)     */
#define NTSCV_F2_VSYNC_START   262      /* ライン262-272: f2垂直同期 (半ライン遅れ)    */
#define NTSCV_F2_VSYNC_END     272
#define NTSCV_F2_ANCHOR_END    280      /* ライン273-280: アンカー (281-297は再生)     */
#define NTSCV_F2_BLANK1_END    297
#define NTSCV_F2_ACTIVE_START  298
#define NTSCV_F2_ACTIVE_END    513
#define NTSCV_F2_MARK          514
#define NTSCV_F2_ERASE_END     522      /* ライン514-522: 内容部黒消し (523/524は再生) */

#elif NTSCV_SIG_PRESET == 1 || NTSCV_SIG_PRESET == 2
/*--- S_A / S_B: 262ライン プログレッシブ (元ドライバのフレーム構成) ---*/
/* システムクロック157.5MHz (set_sys_clock_khz(157500) 使用。PLL VCO 1050MHz/7=150MHz
   ではなく、fbdiv=175, postdiv=2,1? いずれにせよSDK経由で整数kHz設定可能)。
   1走査線は同じ908サンプル/9988サイクル=63.416µs。
   フレーム262ライン=16615.4µs → 60.185Hz (元ドライバ実績ありのVレート)。
   フィールド=フレーム (NTSCV_FIELDS_PER_FRAME=1)。 */
#define NTSCV_SYS_CLOCK_HZ    157500000u
#define NTSCV_SYS_PLL_VCO_HZ  0u        /* set_sys_clock_khz経由: core.cでは未使用 */
#define NTSCV_FIELDS_PER_FRAME 1
#define NTSCV_LINES_PER_FRAME 262
#define NTSCV_F1_VSYNC_END     9        /* ライン0-9  : 垂直同期                     */
#define NTSCV_F1_ANCHOR_END    17       /* ライン10-17: ブランキング全書込アンカー   */
#define NTSCV_F1_BLANK1_END    35       /* ライン18-35: ブランキング(前) — 再生      */
#define NTSCV_F1_ACTIVE_START  36
#define NTSCV_F1_ACTIVE_END    251
#define NTSCV_F1_MARK          252      /* フレームマーク (drawcount/表示更新)       */
#define NTSCV_F1_ERASE_END     260      /* ライン252-260: 内容部黒消し (261は再生)   */
/* プログレッシブ: フィールド2は無し (範囲を空にして判定に引っかからない) */
#define NTSCV_F2_VSYNC_START   NTSCV_LINES_PER_FRAME
#define NTSCV_F2_VSYNC_END     (NTSCV_LINES_PER_FRAME - 1)
#define NTSCV_F2_ANCHOR_END    (NTSCV_LINES_PER_FRAME - 1)
#define NTSCV_F2_BLANK1_END    (NTSCV_LINES_PER_FRAME - 1)
#define NTSCV_F2_ACTIVE_START  NTSCV_LINES_PER_FRAME
#define NTSCV_F2_ACTIVE_END    (NTSCV_LINES_PER_FRAME - 1)
#define NTSCV_F2_MARK          NTSCV_LINES_PER_FRAME
#define NTSCV_F2_ERASE_END     (NTSCV_LINES_PER_FRAME - 1)

#elif NTSCV_SIG_PRESET == 3
/*--- S_C: 525ライン真インターレース (現行構造) @157.5MHz ---*/
/* フレーム構成はS_Dと同一。クロックのみ元ドライバと同じ157.5MHz。
   フィールドレート = 60.0707Hz。 */
#define NTSCV_SYS_CLOCK_HZ    157500000u
#define NTSCV_SYS_PLL_VCO_HZ  0u
#define NTSCV_FIELDS_PER_FRAME 2
#define NTSCV_LINES_PER_FRAME 525
#define NTSCV_F1_VSYNC_END     9
#define NTSCV_F1_ANCHOR_END    17
#define NTSCV_F1_BLANK1_END    35
#define NTSCV_F1_ACTIVE_START  36
#define NTSCV_F1_ACTIVE_END    251
#define NTSCV_F1_MARK          252
#define NTSCV_F1_ERASE_END     260
#define NTSCV_F2_VSYNC_START   262
#define NTSCV_F2_VSYNC_END     272
#define NTSCV_F2_ANCHOR_END    280
#define NTSCV_F2_BLANK1_END    297
#define NTSCV_F2_ACTIVE_START  298
#define NTSCV_F2_ACTIVE_END    513
#define NTSCV_F2_MARK          514
#define NTSCV_F2_ERASE_END     522

#else
#error "Unknown NTSCV_SIG_PRESET (0-3)"
#endif

/* 1フレーム内の区分サイズ (プリセット共通で同一値) */
#define NTSCV_FIELD_VSYNC_LINES 10      /* 垂直同期 = ライン0-9 (10ライン)          */
#define NTSCV_FIELD_BLANK1_LINES 26     /* ブランキング(前) = 26ライン              */
#define NTSCV_FIELD_ACTIVE_LINES 216   /* 有効映像 = 216ライン                     */

#define NTSCV_PWM_WRAP        10        /* wrap+1 = 11サイクル = 1サンプル */
#define NTSCV_SAMPLES_PER_LINE 908      /* 908サンプル = 9988サイクル = 63.483us/line */
#define NTSCV_CHUNK_LINES 2             /* 1DMA転送=2走査線 (再arm期限127µs) */
#define NTSCV_CHUNK_SAMPLES (NTSCV_CHUNK_LINES * NTSCV_SAMPLES_PER_LINE) /* 1816 */

/* ロング同期用 (元ドライバのvsync): 同期[0,840) + 黒[840,908) */
#define NTSCV_LONG_SYNC_SAMPLES (NTSCV_SAMPLES_PER_LINE - NTSCV_H_SYNC_SAMPLES)

/* 垂直同期スタイル: S_Aのみ元ドライバのロング同期、それ以外は規格EQ/SERR */
#if NTSCV_SIG_PRESET == 1
#define NTSCV_LONG_SYNC_VSYNC 1
#else
#define NTSCV_LONG_SYNC_VSYNC 0
#endif

/* ライン内のサンプル区分 (旧ドライバと同一幾何) */
#define NTSCV_H_SYNC_SAMPLES 68        /* H同期幅 68/14.303MHz ≈ 4.75us     */
/* 均等化パルス幅: 規格 (SMPTE 170M) どおり H同期の半分幅 (2.335µs→34サンプル)。
   同期分離器はパルス幅でH同期とEQを区別するため、EQをH同期幅 (68) にすると
   TVがvsync領域の半ライン中央パルスをH同期として余分に数え、ラインカウンタが
   1フレーム+10ずれる → TV内部の垂直タイミングがH÷535≒58.9Hzでフリーランし、
   入力60.0716Hzとの差約1.2Hzでフレーム境界のシームが約0.9秒/周でスクロール
   (実測「1〜2秒」) し、ラップ毎に瞬間的な上下揺れが出るため半幅必須 */
#define NTSCV_EQ_PULSE_SAMPLES 34      /* 均等化パルス幅 34/14.303MHz ≈ 2.38us */
#define NTSCV_LINE_HEAD_SAMPLES 172    /* 有効画素の開始位置 (同期+ポーチ類) */
#define NTSCV_ACTIVE_SAMPLES 672       /* 有効画素幅 (IJ 32×21、42×16と共通) */
#define NTSCV_MONO80_ACTIVE_SAMPLES 640 /* モノクロ80文字モードのみ          */
#define NTSCV_BURST_CYCLES 9            /* カラーバースト 9周期 (36サンプル)  */
#define NTSCV_BREEZEWAY_SAMPLES 8       /* 同期末尾からバーストまでの間       */

/* モード別の有効画素 (旧ドライバ: 全モード H_PICTURE=172 から) */
#define NTSCV_IJ_COLOR_START NTSCV_IJ_TEXT_START
#define NTSCV_IJ_COLOR_SAMPLES NTSCV_ACTIVE_SAMPLES
#define NTSCV_IJ_COLOR_CHAR_SAMPLES NTSCV_IJ_CHAR_SAMPLES
#define NTSCV_IJ_TEXT_START 172
#define NTSCV_COLOR_START 172
#define NTSCV_COLOR_ACTIVE_SAMPLES 672
#define NTSCV_MONO80_START 172

/* フレームシーケンスを進めるライン (有効映像の直後。フィールドごとに2か所) */

/* PWM出力レベル (10段D/A、外部回路は75Ω+560pF) */
#define NTSCV_LEVEL_SYNC  0   /* 同期チップ  (-40 IRE) */
#define NTSCV_LEVEL_BLACK 2   /* ブランキング/黒 (0 IRE) */
#define NTSCV_LEVEL_WHITE 9   /* 白 (+100 IRE) */

/*==== 表示モード (値は旧VMODE互換) ====
  uint8_t変数への代入はアトミックなので、映像出力中の切替も1ライン内で完結する。
*/
#define NTSCV_MODE_COLOR_TEXT 3   /* 42×27文字 256色+背景色           */
#define NTSCV_MODE_MONO_TEXT  5   /* 80×27文字 白黒                   */
#define NTSCV_MODE_IJ_TEXT    6   /* 32×24文字 白黒 (IchigoJam互換)    */
#define NTSCV_MODE_COLOR_GRAPH 18 /* 42×27文字 + 336×216 グラフィック */
#define NTSCV_MODE_IJ_COLOR    7  /* 32×24文字 16色 (COLOR: 前景/背景を属性VRAMで指定) */

/* IJTEXTモードのVRAM書式 */
#define NTSCV_IJ_COLS 32
#define NTSCV_IJ_ROWS 24
#define NTSCV_IJ_LINES_PER_ROW 9  /* 8ラインフォントを9走査線に引き伸ばし */
#define NTSCV_IJ_CHAR_SAMPLES 21 /* 1文字のサンプル幅 (単色VIDEO1: 32×21 = 672) */
#define NTSCV_IJ_ATTR_INVERT 0x80
#define NTSCV_IJ_ATTR_CURSOR 0x40

/* カラーテキスト/グラフィックモードの寸法 */
#define NTSCV_COLOR_COLS 42
#define NTSCV_COLOR_ROWS 27
#define NTSCV_GRAPH_COLS 336 /* X解像度: 42文字×8ドット */
#define NTSCV_GRAPH_ROWS 216

/* モノクロテキストモードの寸法 */
#define NTSCV_MONO80_COLS 80
#define NTSCV_MONO80_ROWS 27

/* 診断用ガード値 (DMAバッファ前後の整合性確認) */
#define NTSCV_GUARD_A_VALUE 0x5CA10001u
#define NTSCV_GUARD_B_VALUE 0x5CA10002u

/*==== 状態 (volatile: 映像割込みと別コンテキストから読み書きする) ===*/
extern volatile uint16_t ntscv_frame_seq;  /* フィールド終端ごとに+1 (旧drawcount) */
extern volatile uint8_t ntscv_mode;        /* 現在の表示モード                   */
extern volatile uint8_t ntscv_ij_zoom;     /* IJ拡大倍率 1/2/4/8 (VRAMは複製格子) */
extern volatile uint8_t ntscv_ij_cursor_xor; /* IJカーソルのXORマスク (0x00/0xF0/0xFF) */
extern volatile int16_t ntscv_ij_cursor_cell; /* カラーモード用カーソルセル位置 (32x24インデックス、-1=なし) */

/*==== API ===*/

/* NTSC映像出力を初期化して開始する。
   gpio: 信号出力ピン。クロックは NTSCV_SYS_CLOCK_HZ (157.5MHz、set_sys_clock_khz)
   が呼び出し前に設定されていること (未設定ならここで設定する)。
   VRAM/フォントはあらかじめ bind 系APIで登録しておくこと。 */
void ntscv_init(uint8_t gpio);

/* DMAチャネル・PWM・割込みを完全に解放する (再開は ntscv_init)。
   フラッシュ書込等で割込みを長時間止めたあとの位相再整合にも使う。 */
void ntscv_release(void);

/* PWM/DMAを止めずにDMAリングをフィールド先頭から再構成する。
   フラッシュ書込後の位相再整合用: TVの同期ロックを維持しつつ位相を戻す。 */
void ntscv_resync(void);

/* フラッシュ書込前に呼ぶ: 4チャンネルバッファを垂直同期領域（ライン0-7）で
   埋め、以降の合成を停止する。DMAハードウェアは自動連鎖を続けるため、
   CPUを長時間止めてもTVは同期を保つ。 */
void ntscv_freeze_vsync(void);

/* フラッシュ書込後に呼ぶ: 合成を再開する。 */
void ntscv_unfreeze(void);

/* 映像を一時停止/再開する (停止中は同期信号も出ない) */
void ntscv_stop(void);
void ntscv_resume(void);

/* 映像出力が動作中か */
bool ntscv_active(void);

/* 走査ラインごとに呼ばれるフック (IchigoJamのTICK相当) */
void ntscv_set_line_hook(void (*cb)(void));

/* テキスト層のVRAMを登録する: codes=文字コード、attrs=属性。
   モードごとの配列寸法は cols×rows (IJ: 32×24、カラー: 42×27、80桁: 80×27)。 */
void ntscv_bind_text(uint8_t *codes, uint8_t *attrs, int cols, int rows);

/* グラフィックVRAMを登録する (336×216バイト、1画素=パレット番号1バイト) */
void ntscv_bind_graph(uint8_t *vram);

/* フォントを登録する (256文字×8バイト、RAM上であること) */
void ntscv_bind_font(const uint8_t *font);

/* 登録済みテキスト層VRAM (コード+属性) をゼロクリアする */
void ntscv_clear_text(void);

/* パレット設定: idx 0-255=前景色、256=背景色 (旧set_paletteと同じ引数順) */
void ntscv_palette_set(uint16_t idx, uint8_t b, uint8_t r, uint8_t g);

/* 既定パレット (0-7: RGB、8-15: 半輝度RGB、16-255: 白、256: 黒) を設定する */
void ntscv_palette_default(void);

/* テストパターン選択: 0=通常、1=SMPTEカラーバー、2=グリッド。
   有効映像域を置き換える (パレット240-246はパターン1が使用する)。 */
void ntscv_test_pattern(int p);

/* 診断用: 現在の走査ライン (0..524、フレーム内ライン番号) */
uint16_t ntscv_debug_scanline(void);

/* 診断用: 合流割込み (割込み長期遅延) の累積回数。正常運用では増えない
   (フラッシュ書込等でのみ増え、その後 ntscv_init で再位相する) */
uint16_t ntscv_debug_events(void);

/* 診断用: DMA IRQハンドラの呼出累計 (SAVE等のフラッシュ書込み中に割込みが
   継続しているかの確認用。約508µs毎に1増える) */
uint32_t ntscv_debug_irq_count(void);

/* フィールド境界 (フレームマークラインの合成時、DMA割込み内) に1回呼ぶコールバックを
   登録する。直後がVSYNC+ブランキングの非表示窓のため、TVRAMコピー等の表示更新を
   ティアリング無しで行える (init.hのfield_display_updateが登録する) */
void ntscv_set_field_hook(void (*cb)(void));

/* 診断用: DMAバッファ前後のガード値 (NTSCV_GUARD_A/B_VALUEと比較して破壊検知) */
uint32_t ntscv_guard_a(void);
uint32_t ntscv_guard_b(void);

/*---- 内部 (ライブラリ内で共用) ----*/

/* render側の起業時初期化 (ライン変換表の作成)。ntscv_init から呼ばれる。
   割込み内で割り算 (libgcc: フラッシュ配置) を避けるためのもの */
void ntscv_render_boot_init(void);

/* IJカラー描画表 (色×位相×21サンプル) を現在のパレットから再構築する。
   ntscv_render_boot_init と ntscv_palette_set (色0-15変更時) から呼ばれる */
void ntscv_render_palette_changed(void);

/* 2走査線分 (1816サンプル) のチャンクを組み立てる。
   first_line: チャンク先頭のライン番号 (0..524、チャンク列は+2ずつmod 525で巡回。
   525が奇数のためフレーム境界チャンク (524, 0) は折返し合成し、偶奇パリティは
   フレームごとに反転する)。
   バッファ永続化契約 (docs/ntsc_video_spec.md §5):
   - 有効映像ラインは内容部 (サンプル172..843) のみ書き換え、
     同期/ポーチ部と末尾はアンカー全書き込み (f1: ライン10-17、f2: 273-280)
     の内容をフィールド中ずっと使い回す (後退8ライン連鎖は必ずアンカーに到達:
     8ライン幅のアンカー窓はステップ8で跳び越せない)
   - 垂直同期 (f1: 0-9、f2: 262-272) とアンカー、消去域 (f1: 252-260、
     f2: 514-522) は全書き込みし、261/523/524は書き込まない
     (4チャネルリング: 4チャネル分のアンカーが必要)
   - 停止中は書き込まない (直前のフィールドが静止画として再生される) */
void ntscv_compose_chunk(uint16_t *dst, uint16_t first_line);

/* 安全チャンク (同期+黒の有効な2走査線分) を作る。
   DMAがバッファ終端からはみ出して再生した際の白いゴミ防止用
   (core.cが起動時に1度だけ呼ぶ) */
void ntscv_compose_safety_chunk(uint16_t *dst);

/* パレット本体: [色][0]=位相0-1ペア、[1]=位相2-3ペア (リトルエンディアン) */
extern uint32_t ntscv_palette[257][2];

/* RGB→4位相サンプル値の計算 (テストパターンが使用) */
void ntscv_palette_calc(uint16_t out4[4], uint8_t b, uint8_t r, uint8_t g);

/* 内部走査状態 (コアが管理) */
extern volatile uint8_t ntscv_running;

/* バインドされたVRAM/フォント (renderが使用) */
extern uint8_t *ntscv_vram_codes;
extern uint8_t *ntscv_vram_attrs;
extern const uint8_t *ntscv_font;
extern uint8_t *ntscv_vram_graph;

/* テストパターン状態 */
extern volatile uint8_t ntscv_test_mode;

#ifdef __cplusplus
}
#endif

#endif /* NTSCV_H */
