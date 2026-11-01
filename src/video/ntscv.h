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

/*==== 信号タイミング (docs/ntsc_video_spec.md §2 参照) ====

  システムクロック157.5MHz、PWM周期11サイクル → サンプルレート14.318MHz
  = NTSCカラーサブキャリア3.579545MHzの4倍 (1周期4サンプル)。
  1走査線908サンプル (サブキャリア227周期分)、1フィールド262ライン。
  NTSCV_LINES_PER_FIELD (262) は旧ドライバ互換の走査線Tickカウントにも使う。
*/
#define NTSCV_SYS_CLOCK_KHZ   157500u
#define NTSCV_PWM_WRAP        10        /* wrap+1 = 11サイクル = 1サンプル */
#define NTSCV_SAMPLES_PER_LINE 908      /* 908 / 14.318MHz ≈ 63.42us/line   */
#define NTSCV_LINES_PER_FIELD 262
#define NTSCV_CHUNK_LINES 2             /* 1DMA転送=2走査線 (再arm期限127µs) */
#define NTSCV_CHUNK_SAMPLES (NTSCV_CHUNK_LINES * NTSCV_SAMPLES_PER_LINE) /* 1816 */

/* フィールド内のライン区分 */
#define NTSCV_FIELD_VSYNC_LINES 10      /* ライン0-9   : 垂直同期            */
#define NTSCV_FIELD_BLANK1_LINES 26     /* ライン10-35 : ブランキング(前)     */
#define NTSCV_FIELD_ACTIVE_LINES 216   /* ライン36-251 : 有効映像            */
#define NTSCV_FIELD_BLANK2_LINES 10    /* ライン252-261: ブランキング(後)     */
#define NTSCV_ACTIVE_START (NTSCV_FIELD_VSYNC_LINES + NTSCV_FIELD_BLANK1_LINES)

/* ライン内のサンプル区分 */
#define NTSCV_H_SYNC_SAMPLES 68        /* H同期幅 68/14.318MHz ≈ 4.75us     */
#define NTSCV_LINE_HEAD_SAMPLES 172    /* 有効画素の開始位置 (同期+ポーチ類) */
#define NTSCV_ACTIVE_SAMPLES 672       /* 有効画素幅 (IJ 32×21、42×16と共通) */
#define NTSCV_MONO80_ACTIVE_SAMPLES 640 /* モノクロ80文字モードのみ          */
#define NTSCV_ACTIVE_TAIL_SAMPLES (NTSCV_SAMPLES_PER_LINE - NTSCV_LINE_HEAD_SAMPLES - NTSCV_ACTIVE_SAMPLES)
#define NTSCV_BURST_CYCLES 9            /* カラーバースト 9周期 (36サンプル)  */
#define NTSCV_BREEZEWAY_SAMPLES 8       /* 同期末尾からバーストまでの間       */

/* フレームシーケンスを進めるライン (有効映像の直後) */
#define NTSCV_FRAME_MARK_LINE (NTSCV_FIELD_VSYNC_LINES + NTSCV_FIELD_BLANK1_LINES + NTSCV_FIELD_ACTIVE_LINES)

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

/* IJTEXTモードのVRAM書式 */
#define NTSCV_IJ_COLS 32
#define NTSCV_IJ_ROWS 24
#define NTSCV_IJ_LINES_PER_ROW 9  /* 8ラインフォントを9走査線に引き伸ばし */
#define NTSCV_IJ_CHAR_SAMPLES 21 /* 1文字のサンプル幅 (32×21 = 672)      */
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

/*==== API ===*/

/* NTSC映像出力を初期化して開始する。
   gpio: 信号出力ピン。クロックは NTSCV_SYS_CLOCK_KHZ (157.5MHz) が
   呼び出し前に設定されていること (未設定ならここで設定する)。
   VRAM/フォントはあらかじめ bind 系APIで登録しておくこと。 */
void ntscv_init(uint8_t gpio);

/* DMAチャネル・PWM・割込みを完全に解放する (再開は ntscv_init)。
   フラッシュ書込等で割込みを長時間止めたあとの位相再整合にも使う。 */
void ntscv_release(void);

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

/* 診断用: 現在の走査ライン (0..261) */
uint16_t ntscv_debug_scanline(void);

/* 診断用: 合流割込み (割込み長期遅延) の累積回数。正常運用では増えない
   (フラッシュ書込等でのみ増え、その後 ntscv_init で再位相する) */
uint16_t ntscv_debug_events(void);

/* 診断用: DMAバッファ前後のガード値 (NTSCV_GUARD_A/B_VALUEと比較して破壊検知) */
uint32_t ntscv_guard_a(void);
uint32_t ntscv_guard_b(void);

/*---- 内部 (ライブラリ内で共用) ----*/

/* render側の起業時初期化 (ライン変換表の作成)。ntscv_init から呼ばれる。
   割込み内で割り算 (libgcc: フラッシュ配置) を避けるためのもの */
void ntscv_render_boot_init(void);

/* 2走査線分 (1816サンプル) のチャンクを組み立てる。
   first_line: チャンク先頭のライン番号 (0..260、常に偶数)。
   バッファ永続化契約 (docs/ntsc_video_spec.md §5):
   - 有効映像ラインは内容部 (サンプル172..843) のみ書き換え、
     同期/ポーチ部と末尾はバッファに残った内容を使い回す
   - 垂直同期は先頭8ライン、ブランキングは各領域先頭8ラインのみ全書き込みし、
     残りは書き込まない (4チャネルリング: 各チャネルが4スロット前の
     自バッファ内容を再生するため、アンカーは4チャネル分必要)
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
