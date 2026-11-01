/*----------------------------------------------------------------------------

ntscv_core.c — PWM+DMA映像出力エンジン (docs/ntsc_video_spec.md §5)

Copyright (c) 2026 the IchigoJam-RP2040Zero authors. MIT license.

構成: 4つのDMAチャネルが2走査線分 (1816サンプル) ずつをPWMのCC比較
レジスタへ供給し、完了ごとに次のチャネルへ相互chainで即再始動する
(4チャネルリング)。各チャネルは4スロット (=508µs) ごとに1度だけ
ストリームするため、バッファ再構築の期限は508µsあり、WS2812送信等の
割込み禁止 (最大約31µs) や compose の遅れが重なっても大幅な余裕がある
(2チャネル+127µs期限では IJ合成~92µs+31µs が期限すれすれになり、
はずれた際にバッファ終端の外側 (ガード値・隣接RAM) が信号として
再生されて白いノイズラインが出ることがある → 4リング化で解消)。

期限を超えた場合に備え、リングの直後に「安全チャンク」(黒い有効な
2走査線分) を置く。万一バッファ終端からはみ出して再生しても白い
ゴミではなく黒い2ラインとして出る。

完了割込み (DMA_IRQ_1、最高優先度0) で「次に表示するチャンク」の
内容を完了した側のバッファへ書き込む (arm_next[] = 各チャネルが次に
表示するチャンクの先頭ライン。1チャンク=8ライン間隔で+8進行し、
262ライン (=131チャンク、4で割り切れない) のフィールド境界も
mod 262の算術が自動的に追跡する)。

割込み合流 (508µs以上の割込み停止) が起きた場合は、その時点で
ストリーミング中 (BUSY) のチャネルは内容の再構築を諦めて目標だけ
進め、アイドルのチャネルだけ再構築する → 垂直位相は保たれる。
通常運用では合流はフラッシュ書込時のみで、その後 ntscv_init() で
位相を合わせ直す。

----------------------------------------------------------------------------*/

#include "ntscv.h"
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/pwm.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include <assert.h>

#define NTSCV_DMA_CHANNELS 4

volatile uint16_t ntscv_frame_seq = 0;
volatile uint8_t ntscv_mode = NTSCV_MODE_COLOR_TEXT;
volatile uint8_t ntscv_ij_zoom = 1;
volatile uint8_t ntscv_ij_cursor_xor = 0;
volatile uint8_t ntscv_running = 0;

/* DMAチャンクバッファ (1チャネル分=2走査線) ×4 + 安全チャンク。
   前後のガード値でバッファ境界の破壊を検知する。
   安全チャンクは期限超過時のはみ出し再生が「黒い有効ライン」に
   なるよう黑色で埋めておく (起動時に1度だけ) */
typedef struct {
    volatile uint32_t guard_a;
    uint16_t chunk[NTSCV_DMA_CHANNELS][NTSCV_CHUNK_SAMPLES];
    uint16_t safe_chunk[NTSCV_CHUNK_SAMPLES];
    volatile uint32_t guard_b;
} ntscv_bufs_t;

static ntscv_bufs_t ntscv_dma_bufs __attribute__((aligned(4)));

static uint dma_ch[NTSCV_DMA_CHANNELS];
static uint pwm_slice_num;
static uint out_pin;
static bool engine_inited = false;
static void (*line_hook_cb)(void);

/* 次に各チャネルのバッファへ詰めるチャンクの先頭ライン番号。
   各チャネルは4チャンク (8ライン) 先を担当する */
static volatile uint16_t arm_next[NTSCV_DMA_CHANNELS];

/* 診断: 割込み合流 (長期割込み遅延) の累積回数 */
static volatile uint16_t merge_events = 0;

static inline uint16_t advance_chunk(uint16_t n)
{
    /* 1チャネルは4スロット (2ライン×4=8ライン) 先のチャンクを担当する */
    n = (uint16_t)(n + NTSCV_CHUNK_LINES * NTSCV_DMA_CHANNELS);
    return n < NTSCV_LINES_PER_FIELD ? n : (uint16_t)(n - NTSCV_LINES_PER_FIELD);
}

static void __not_in_flash_func(arm_channel)(int idx)
{
    uint16_t first = arm_next[idx];
    ntscv_compose_chunk(ntscv_dma_bufs.chunk[idx], first);
    dma_channel_set_read_addr(dma_ch[idx], ntscv_dma_bufs.chunk[idx], false);
    arm_next[idx] = advance_chunk(first);
}

static void __not_in_flash_func(ntscv_dma_irq_handler)(void)
{
    uint32_t done = dma_hw->ints1;
    dma_hw->ints1 = done;

    for (int i = 0; i < NTSCV_DMA_CHANNELS; i++) {
        uint32_t m = 1u << dma_ch[i];
        if (!(done & m)) continue;
        if (dma_channel_is_busy(dma_ch[i])) {
            /* 割込みが合流してこのチャネルは既に次チャンクを再生中:
               内容の再構築は諦めて目標だけ進める (垂直位相は保たれる) */
            merge_events++;
            arm_next[i] = advance_chunk(arm_next[i]);
        } else {
            arm_channel(i);
        }
    }

    /* 走査ラインごとのフック (TICK相当)。1チャンク=2ラインなので2回呼ぶ */
    if (line_hook_cb) {
        line_hook_cb();
        line_hook_cb();
    }
}

void ntscv_init(uint8_t gpio)
{
    if (engine_inited) ntscv_release();

    /* クロック: 呼び出し側で157.5MHz設定済みのこと (未設定ならここで設定) */
    if (clock_get_hz(clk_sys) != (uint32_t)NTSCV_SYS_CLOCK_KHZ * 1000u) {
        set_sys_clock_khz(NTSCV_SYS_CLOCK_KHZ, true);
    }

    ntscv_dma_bufs.guard_a = NTSCV_GUARD_A_VALUE;
    ntscv_dma_bufs.guard_b = NTSCV_GUARD_B_VALUE;

    /* 安全チャンク: 同期信号+黒画面の有効な2走査線 (はみ出し再生用) */
    ntscv_compose_safety_chunk(ntscv_dma_bufs.safe_chunk);

    if (ntscv_mode == 0) ntscv_mode = NTSCV_MODE_COLOR_TEXT;

    ntscv_render_boot_init(); /* ライン変換表 (割込み内の割り算排除) */
    ntscv_palette_default(); /* 既定パレット (RGB+半輝度+白+背景黒) */

    out_pin = gpio;
    pwm_slice_num = pwm_gpio_to_slice_num(gpio);
    uint pwm_chan = pwm_gpio_to_channel(gpio); /* ピン偶奇 → スライスのA/B */

    gpio_set_function(gpio, GPIO_FUNC_PWM);
    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_clkdiv(&cfg, 1.f);
    pwm_init(pwm_slice_num, &cfg, false);
    pwm_set_wrap(pwm_slice_num, NTSCV_PWM_WRAP); /* 11サイクル = 1サンプル */

    /* DMAの書込先: CCレジスタの該当チャネル半分 (16ビット) */
    volatile void *cc_half = (pwm_chan == PWM_CHAN_A)
        ? (volatile void *)&pwm_hw->slice[pwm_slice_num].cc
        : (volatile void *)((uintptr_t)&pwm_hw->slice[pwm_slice_num].cc + 2);

    for (int i = 0; i < NTSCV_DMA_CHANNELS; i++) {
        dma_ch[i] = dma_claim_unused_channel(true);
    }
    for (int i = 0; i < NTSCV_DMA_CHANNELS; i++) {
        uint next = dma_ch[(i + 1) % NTSCV_DMA_CHANNELS];
        dma_channel_config c = dma_channel_get_default_config(dma_ch[i]);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
        channel_config_set_read_increment(&c, true);
        channel_config_set_write_increment(&c, false);
        channel_config_set_dreq(&c, DREQ_PWM_WRAP0 + pwm_slice_num);
        channel_config_set_chain_to(&c, next); /* 完了で次チャネルを即始動 */
        dma_channel_configure(dma_ch[i], &c,
                              cc_half,
                              ntscv_dma_bufs.chunk[i],
                              NTSCV_CHUNK_SAMPLES,
                              false);
    }

    /* 先頭4チャンク (ライン0-7) を用意し、各チャネルは4スロット先を作る */
    ntscv_running = 1;
    for (int i = 0; i < NTSCV_DMA_CHANNELS; i++) {
        ntscv_compose_chunk(ntscv_dma_bufs.chunk[i],
                           (uint16_t)(i * NTSCV_CHUNK_LINES));
        arm_next[i] = (uint16_t)((NTSCV_DMA_CHANNELS + i) * NTSCV_CHUNK_LINES);
    }

    uint32_t mask = 0;
    for (int i = 0; i < NTSCV_DMA_CHANNELS; i++) mask |= 1u << dma_ch[i];
    dma_set_irq1_channel_mask_enabled(mask, true);
    irq_set_exclusive_handler(DMA_IRQ_1, ntscv_dma_irq_handler);
    irq_set_enabled(DMA_IRQ_1, true);
    /* 最高優先度にする。USB等より優先しないとチャンクのバッファ更新が
       間に合わず走査位相がずれて画面が流れる */
    irq_set_priority(DMA_IRQ_1, 0);

    pwm_set_enabled(pwm_slice_num, true);
    dma_channel_start(dma_ch[0]);
    engine_inited = true;
}

void ntscv_release(void)
{
    if (!engine_inited) return;
    ntscv_running = 0;
    uint32_t mask = 0;
    for (int i = 0; i < NTSCV_DMA_CHANNELS; i++) {
        dma_channel_abort(dma_ch[i]);
        dma_channel_unclaim(dma_ch[i]);
        mask |= 1u << dma_ch[i];
    }
    dma_set_irq1_channel_mask_enabled(mask, false);
    irq_set_enabled(DMA_IRQ_1, false);
    irq_remove_handler(DMA_IRQ_1, ntscv_dma_irq_handler);
    pwm_set_enabled(pwm_slice_num, false);
    gpio_set_function(out_pin, GPIO_FUNC_NULL);
    engine_inited = false;
}

void ntscv_stop(void)
{
    ntscv_running = 0;
}

void ntscv_resume(void)
{
    ntscv_running = 1;
}

bool ntscv_active(void)
{
    return ntscv_running != 0;
}

void ntscv_set_line_hook(void (*cb)(void))
{
    line_hook_cb = cb;
}

uint16_t ntscv_debug_scanline(void)
{
    /* 現在表示中の走査ライン ≈ ch0の次回表示チャンク先頭 - 8 */
    uint16_t n = arm_next[0];
    n = (uint16_t)(n + NTSCV_LINES_PER_FIELD
                     - NTSCV_CHUNK_LINES * NTSCV_DMA_CHANNELS);
    return n >= NTSCV_LINES_PER_FIELD ? (uint16_t)(n - NTSCV_LINES_PER_FIELD) : n;
}

uint16_t ntscv_debug_events(void)
{
    return merge_events;
}

uint32_t ntscv_guard_a(void)
{
    return ntscv_dma_bufs.guard_a;
}

uint32_t ntscv_guard_b(void)
{
    return ntscv_dma_bufs.guard_b;
}

/*---- 仕様の整合性をコンパイル時に確認 ----*/

static_assert(NTSCV_SYS_CLOCK_KHZ * 22 == 315 * (NTSCV_PWM_WRAP + 1) * 1000,
              "サンプルレート = 315/22 MHz = NTSCカラーサブキャリア (315/88 MHz) の4倍");
static_assert((NTSCV_PWM_WRAP + 1) * NTSCV_SAMPLES_PER_LINE == 9988,
              "1走査線 = 9988サイクル = 63.42us @157.5MHz");
static_assert(NTSCV_LINES_PER_FIELD % 2 == 0, "チャンクはライン境界で一致すること");
static_assert(NTSCV_LINES_PER_FIELD % NTSCV_CHUNK_LINES == 0,
              "チャンクはフィールド内で整数回転すること");
static_assert(NTSCV_CHUNK_SAMPLES <= 0xffff, "DMA転送カウントは16bit範囲内であること");
