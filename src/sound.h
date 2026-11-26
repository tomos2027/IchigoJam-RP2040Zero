#ifndef __SOUND_H__
#define __SOUND_H__
#include "all_includes.h"
#include "pico/multicore.h"
#include "hardware/structs/nvic.h"
#include "hardware/structs/watchdog.h"
#include "hardware/structs/scb.h"
#include "hardware/structs/systick.h"
#include "hardware/flash.h"

//Waveshare RP2040-Zero: 側面パッドGP14 (IchigoJam_PのGP20から変更: 裏面パッド不使用のため)
#define SOUND_PIN 14

// 多声PSGモード: PWMデューティ解像度 (キャリア = sysclk/1024 ≈ 154kHz、可聴域外)
#define SOUND_PWM_WRAP 1023
// オーディオ割り込み周期はpsg_emu.hのPSG_EMU_TIMER_US (18µs ≈ 55.6kHz) を使用

static uint slice_num;
static uint16 prev_tone = 0;
static repeating_timer_t psg_emu_timer;
// sound_multi_mode / sound_multi_play は psg.hで定義 (psg.hが先にincludeされるため)
// core0 (60FPS/USB/映像) が書き、core1 (オーディオ) が読むため volatile

// ===== オーディオ専用core1 =====
// PSGエンジンはcore1で動かす。core0の映像DMA割込み (優先度0) は毎チャンク
// ~15-20µs CPUを占有するため、同じcore0/同優先度だと音声ISRが遅延し
// リピートタイマーがまとめて追いついて音が荒れる。core1なら完全に分離される。
static bool __not_in_flash_func(psg_emu_timer_callback)(repeating_timer_t* rt);

// ===== core1パーク機構 (フラッシュ書込SAVE/KBD用) =====
// フラッシュ書込中はXIPが停止する。core1はXIPフラッシュ上のコードを
// 実行しているため、停止中のXIPアクセスがXIP復帰処理と競合しシステムが
// 停止する (実機確認: SAVE→ウォッチドッグリセット)。パーク中はcore1を
// RAM上のループで待機させ、PSGタイマー割込みも停止してXIPアクセスを完全に止める。
#define PSG_CORE1_CMD_PARK   0x5041524Bu  // 'PARK'
#define PSG_CORE1_CMD_RESUME 0x52455355u  // 'RESU'
#define PSG_CORE1_CMD_FLASH  0x464C5348u  // 'FLSH': core1にフラッシュ消去/書込みを委譲

// core1フラッシュ書込み用パラメータ (core0が書き、core1が読む)
volatile uint32_t flash_core1_offset = 0;
volatile const uint8_t *flash_core1_data = NULL;
volatile uint8_t flash_core1_done = 0;
// 診断: wait_readyの最終ステータス値と異常アボート指示 (core0⇔core1共有)
volatile uint8_t flash_core1_status_last = 0xFF;
volatile uint8_t flash_core1_abort = 0;

static volatile uint psg_core1_alarm_irq = 0;
static volatile bool psg_core1_parked = false;
// 診断: core1のパーク進行トレース (core0がタイムアウト時にダンプする)
// 1=park_and_wait開始 2=ACK送信済 3=RESUME受信済 4=アラーム再有効化済 5=完了ACK送信済
static volatile uint psg_core1_trace = 0;
// 診断: park_and_waitスピンループの実況 (core1が書き、core0がタイムアウト時にダンプ)
// spin_st=ループ内で読んだfifo_st値 spin_cnt=反復数(0=ループ突入前に凍結) got=脱出時に読んだFIFO値
static volatile uint32_t psg_core1_spin_st = 0;
static volatile uint32_t psg_core1_spin_cnt = 0;
static volatile uint32_t psg_core1_got = 0;

// ===== HardFault常駐フック (RAMベクタ表へ登録、両コア共通) =====
//フラッシュ操作中 (XIP停止) のフォault (残存XIPフェッチによるゴミ実行/不正アクセス) を
//シリアルへ通知する。例外フレームのPC/LRとcore1トレースを報告して停止する。
//メッセージは即値構築・16進のみ (除算/フラッシュリテラル不使用でXIP停止中も安全)
static void __attribute__((naked)) __not_in_flash_func(ram_hardfault_entry)(void) {
    __asm volatile (
        "mov r0, sp\n"
        "bl ram_hardfault_hook\n"
    );
}
static void __attribute__((noinline, used)) __not_in_flash_func(ram_hardfault_hook)(uint32_t *frame) {
    // 例外フレーム: [0]=r0 [1]=r1 [2]=r2 [3]=r3 [4]=r12 [5]=lr [6]=pc [7]=xpsr
    ram_diag_clear();
    ram_diag_addc('[');
    ram_diag_addc('H');
    ram_diag_addc('F');
    ram_diag_addc('!');
    ram_diag_addc(']');
    ram_diag_addc(' ');
    ram_diag_addc('p');
    ram_diag_addc('c');
    ram_diag_addc('=');
    ram_diag_addhex(frame[6] & ~1u);
    ram_diag_addc(' ');
    ram_diag_addc('l');
    ram_diag_addc('r');
    ram_diag_addc('=');
    ram_diag_addhex(frame[5]);
    ram_diag_addc(' ');
    ram_diag_addc('c');
    ram_diag_addc('1');
    ram_diag_addc('t');
    ram_diag_addc('r');
    ram_diag_addc('=');
    ram_diag_addhex((uint32_t)psg_core1_trace);
    ram_diag_send();
    for (;;) {
        tight_loop_contents();
    }
}

// パーク待機 (RAM実行): ACK送信→RESUME受信待ちの間にXIP命令フェッチを一切行わない。
// ACKはレジスタ直叩き1命令 (sio_hw->fifo_wr = 値) で送り、以降このRAM関数内で完結する。
// この関数が呼ばれる時点ではcore0はACK待ちでブロック中 (フラッシュ操作未開始) なので、
// 呼び出しまでの経路 (XIP) の実行は安全。ACK後はcore0が即フラッシュ操作を始めるため、
// ACK→待機ループ間でXIP命令を実行すると消去/書込とXIPフェッチが競合しcore1が暴走する
// (実機確認: [S8!] RESUME ACK TIMEOUT + 後続のntsc_init固着)
// __attribute__((noinline))必須: 単一呼び出しの小関数だとGCCがcore1メインループ(XIP)
// へ完全インライン化し、RAM配置が失われてスピンループがXIP実行になる。
// するとフラッシュ消去/書込中のXIPフェッチ停止でcore1がtrace=2で永久凍結する
// (実機+逆アセンブルで確認済み: [S8!] RESUME ACK TIMEOUTの原因)
static void __attribute__((noinline)) __not_in_flash_func(psg_core1_park_and_wait)(void) {
    psg_core1_trace = 1;                    // park_and_wait開始
    sio_hw->fifo_wr = PSG_CORE1_CMD_RESUME; // ACK送信 (RAM内で完結)
    psg_core1_trace = 2;                    // ACK送信済
    while (!(sio_hw->fifo_st & SIO_FIFO_ST_VLD_BITS)) {
        psg_core1_spin_st = sio_hw->fifo_st;
        psg_core1_spin_cnt++;
        tight_loop_contents();
    }
    psg_core1_got = sio_hw->fifo_rd; // RESUME受信 (値を診断記録)
    psg_core1_trace = 3;                    // RESUME受信済
}

// core1でフラッシュ消去/書込みを行い、完了後にACKを送信してRESUME待機 (RAM実行)
// core0はこの間RAMループで待機し、DMA合成割込みを継続させる
// core1 から watchdog を feed するヘルパ (APB アクセス可能なため)
static void __not_in_flash_func(core1_watchdog_feed)(void) {
    watchdog_hw->load = WATCHDOG_TIMEOUT_MS * 2000;
}

// core1 で実際のフラッシュ消去/書込みのみ行い、完了フラグを立てて復帰する。
// RESUME 待ちはメインループで行うため、フラッシュビジー中に SIO/APB を触らない。
// ROM の flash_range_program は core1 実行でハングする場合があるため、
// flash_do_cmd を使って 256B ページ単位で書き込む (core1 RAM 配置)。
static void __not_in_flash_func(core1_flash_wait_ready)(void) {
    uint8_t tx[2] = {0x05, 0x00};
    uint8_t rx[2];
    uint32_t guard = 0;
    do {
        core1_watchdog_feed();
        flash_do_cmd(tx, rx, 2);
        flash_core1_status_last = rx[1]; // 診断: 最終ステータス値 (core0のタイムアウト表示用)
        if (++guard > 1000000u) { // 異常: ビジーが解けない (ゴミ値等) はアボートしてcore0へ完了を報告
            flash_core1_abort = 1;
            break;
        }
    } while (rx[1] & 0x01);
}

static void __not_in_flash_func(core1_flash_program_sector)(uint32_t offs, const uint8_t *data) {
    uint8_t we[1] = {0x06};
    uint8_t dummy1[1];
    uint8_t tx[4 + 256];
    uint8_t rx[4 + 256];
    for (int page = 0; page < FLASH_SECTOR_SIZE / 256; page++) {
        if (flash_core1_abort) break; // 異常アボート (SAVE内容は無効、エラー報告へ)
        uint32_t addr = offs + (uint32_t)page * 256;
        tx[0] = 0x02;
        tx[1] = (uint8_t)(addr >> 16);
        tx[2] = (uint8_t)(addr >> 8);
        tx[3] = (uint8_t)addr;
        for (int i = 0; i < 256; i++) tx[4 + i] = data[page * 256 + i];
        flash_do_cmd(we, dummy1, 1);
        flash_do_cmd(tx, rx, sizeof(tx));
        core1_flash_wait_ready();
    }
}

static void __attribute__((noinline)) __not_in_flash_func(psg_core1_flash_only)(void) {
    psg_core1_trace = 1;
    uint32_t s = save_and_disable_interrupts(); // core1の割込みを止める
    flash_range_erase(flash_core1_offset, FLASH_SECTOR_SIZE);
    core1_flash_program_sector(flash_core1_offset, (const uint8_t *)flash_core1_data);
    restore_interrupts(s);
    flash_core1_done = 1;
    __compiler_memory_barrier();
    psg_core1_trace = 2;
}

// core1からのフラッシュ完了ACKを待つ (core0 RAM実行)。
// この間はDMA合成割込みが動作し続け、TVの同期を維持する。
static void __attribute__((noinline)) __not_in_flash_func(psg_core1_flash_wait_ack)(void) {
    while (!(sio_hw->fifo_st & SIO_FIFO_ST_VLD_BITS)) {
        watchdog_hw->load = WATCHDOG_TIMEOUT_MS * 2000;
        tight_loop_contents();
    }
    (void)sio_hw->fifo_rd;
}

void psg_core1_park(void) {
    if (!psg_core1_parked) {
        diag_log("[S2] park push");
        multicore_fifo_push_blocking(PSG_CORE1_CMD_PARK);
        uint32_t ack;
        if (multicore_fifo_pop_timeout_us(2000000, &ack)) {
            diag_log("[S3] park ack ok");
            diag_log_hex("  ack=", ack); // 期待値 0x52455355 ('RESU')
        } else {
            // ACK不可=core1がPARK要求を処理できていない。
            // alarm_irq==0ならPSGタイマー未初期化 (core1がWFIで永眠) が確定
            diag_log("[S3!] PARK ACK TIMEOUT");
            diag_log_hex("  fifo_st=", sio_hw->fifo_st);
            diag_log_hex("  alarm_irq=", (uint32_t)psg_core1_alarm_irq);
            diag_log_hex("  multi_mode=", (uint32_t)sound_multi_mode);
        }
        psg_core1_parked = true;
    }
}

// core1にフラッシュ消去/書込みを委譲する。
// 実行中、core0はRAMループで待機し、映像DMA割込みを継続させる。
// core1にフラッシュ消去/書込みを依頼し、完了まで core0 は RAM ループで待機する。
// フラッシュチップがビジーになると core0 の XIP フェッチが停止するため、
// この関数全体を RAM 配置にし、命令フェッチを一切行わないようにする。
// 診断: 待機中はRAM診断チャネル (storage.h) でハートビートを送信し、3秒でタイムアウト
// を記録する (待機は継続: core1のwait_ready反復上限が異常アボートする。戻り値1=エラー)
static int __attribute__((noinline)) __not_in_flash_func(psg_core1_flash_op_and_wait)(uint32_t offset, const uint8_t *data) {
    flash_core1_offset = offset;
    flash_core1_data = data;
    flash_core1_done = 0;
    flash_core1_abort = 0;
    flash_core1_status_last = 0xFF;
    psg_core1_parked = false; // 今回は RESUME 待ち不要
    __compiler_memory_barrier();

    // 診断: フラッシュ操作中の割込み継続確認 (XIP復帰後の呼出のみ安全)
    uint32_t diag_t0 = (uint32_t)time_us_64();
    uint32_t diag_irq0 = ntscv_debug_irq_count();
    uint32_t diag_mrg0 = ntscv_debug_events();
    diag_log("[S10] flash op start");

    // 完了フラグを RAM でポーリング。DMA 合成割込みのみ許可して TV 信号を維持。
    // SysTick はフラッシュビジー中にハンドラをフェッチしようとしてストールするため停止する。
    // ここで全割込みを先に止めてから FIFO へコマンドを送る。そうしないと、core1 が
    // フラッシュ操作を始めて XIP が停止した後に USB 等のフラッシュ上ハンドラが割り込み、
    // core0 が XIP 上の命令をフェッチできずにフリーズする恐れがある。
    uint32_t nvic_state = nvic_hw->iser;
    nvic_hw->icer = ~(1u << DMA_IRQ_1);
    uint32_t systick_ctrl = systick_hw->csr;
    systick_hw->csr = systick_ctrl & ~M0PLUS_SYST_CSR_ENABLE_BITS;

    // フラッシュ書込み中は core0 の APB アクセスが停止する可能性があるため、
    // その間 watchdog を一時的に無効化する。
    hw_clear_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS);

    // FIFO へ FLASH コマンドを送信
    while (!(sio_hw->fifo_st & SIO_FIFO_ST_RDY_BITS)) tight_loop_contents();
    sio_hw->fifo_wr = PSG_CORE1_CMD_FLASH;
    __sev();

    // 診断: ポーリング開始 (RAMチャネル: XIP停止中も送信可)
    ram_diag_clear();
    ram_diag_addc('[');
    ram_diag_addc('S');
    ram_diag_addc('1');
    ram_diag_addc('0');
    ram_diag_addc('b');
    ram_diag_addc(']');
    ram_diag_send();

    // 完了フラグが立つまで RAM でポーリング。
    // 約100-200ms毎にRAMチャネルへハートビート (core0の生存とDMA IRQ継続の実時間証明)。
    // 3秒経過したらタイムアウトを記録して待機継続 (IRQは維持: core1の異常は
    // wait_ready反復上限がアボートし、完了後に[S11]とステータスを報告する)
    uint32_t hb = 0;
    uint8_t timed_out = 0;
    while (!flash_core1_done) {
        tight_loop_contents();
        if (++hb >= 2000000u) {
            hb = 0;
            // 直接レジスタ読出し (time_us_64はFLASH配置のためXIP停止中の呼出は不可)
            uint32_t now = timer_hw->timerawl;
            ram_diag_clear();
            ram_diag_addc('[');
            ram_diag_addc('S');
            ram_diag_addc('1');
            ram_diag_addc('0');
            ram_diag_addc('p');
            ram_diag_addc(']');
            ram_diag_addc(' ');
            ram_diag_addc('t');
            ram_diag_addc('=');
            ram_diag_addhex(now - diag_t0);
            ram_diag_addc(' ');
            ram_diag_addc('i');
            ram_diag_addc('r');
            ram_diag_addc('q');
            ram_diag_addc('=');
            ram_diag_addhex(ntscv_debug_irq_count() - diag_irq0);
            ram_diag_addc(' ');
            ram_diag_addc('m');
            ram_diag_addc('r');
            ram_diag_addc('g');
            ram_diag_addc('=');
            ram_diag_addhex((uint32_t)(uint16_t)(ntscv_debug_events() - diag_mrg0));
            ram_diag_send();
            if (!timed_out && (uint32_t)(now - diag_t0) >= 3000000u) {
                timed_out = 1;
                ram_diag_clear();
                ram_diag_addc('[');
                ram_diag_addc('S');
                ram_diag_addc('1');
                ram_diag_addc('0');
                ram_diag_addc('!');
                ram_diag_addc(']');
                ram_diag_addc('T');
                ram_diag_addc('I');
                ram_diag_addc('M');
                ram_diag_addc('E');
                ram_diag_addc('O');
                ram_diag_addc('U');
                ram_diag_addc('T');
                ram_diag_send();
            }
        }
    }
    systick_hw->csr = systick_ctrl;
    nvic_hw->iser = nvic_state;

    // watchdog を再有効化
    watchdog_hw->load = WATCHDOG_TIMEOUT_MS * 2000;
    hw_set_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS);

    // 診断: XIP復帰後にのみ呼べる。irq=操作中のDMA IRQ呼出増分 (約508µs毎+1が正常継続)
    diag_log("[S11] flash op done");
    diag_log_hex("  ms=", ((uint32_t)time_us_64() - diag_t0) / 1000u);
    diag_log_hex("  irq=", ntscv_debug_irq_count() - diag_irq0);
    diag_log_hex("  mrg=", (uint32_t)(uint16_t)(ntscv_debug_events() - diag_mrg0));
    if (timed_out || flash_core1_abort) {
        diag_log("[S10x] FLASH OP ABNORMAL - core1 status/abort/trace:");
        diag_log_hex("  stat=", flash_core1_status_last);
        diag_log_hex("  abrt=", flash_core1_abort);
        diag_log_hex("  c1tr=", psg_core1_trace);
    }
    return (timed_out || flash_core1_abort) ? 1 : 0;
}

void psg_core1_resume(void) {
    if (psg_core1_parked) {
        multicore_fifo_push_blocking(PSG_CORE1_CMD_RESUME);
        uint32_t ack;
        if (multicore_fifo_pop_timeout_us(2000000, &ack)) {
            diag_log("[S8] resume ack ok");
        } else {
            diag_log("[S8!] RESUME ACK TIMEOUT");
            diag_log_hex("  fifo_st=", sio_hw->fifo_st);
            diag_log_hex("  c1trace=", (uint32_t)psg_core1_trace);
            diag_log_hex("  timer_intr=", timer_hw->intr);
            // NVIC ISPR直読み (このSDK版にはirq_get_pendingが無いため)
            diag_log_hex("  c1irq_pend=", (*(volatile uint32_t *)(0xE000E200u + (psg_core1_alarm_irq / 32u) * 4u) >> (psg_core1_alarm_irq % 32u)) & 1u);
            diag_log_hex("  spin_st=", psg_core1_spin_st);
            diag_log_hex("  spin_cnt=", psg_core1_spin_cnt);
            diag_log_hex("  got=", psg_core1_got);
            diag_log_hex("  park_fn=", (uint32_t)&psg_core1_park_and_wait); // 0x2xxxxxxx=RAM配置OK / 0x1xxxxxxx=XIP(バグ)
            // 再送プローブ: ACKが来ればcore1は後から回復した(一時凍結)、来なければ完全凍結のまま
            multicore_fifo_push_blocking(PSG_CORE1_CMD_RESUME);
            if (multicore_fifo_pop_timeout_us(200000, &ack)) {
                diag_log("[S8r] retry ack ok");
            } else {
                diag_log("[S8r] retry timeout");
            }
            diag_log_hex("  spin_cnt2=", psg_core1_spin_cnt);
            diag_log_hex("  c1trace2=", (uint32_t)psg_core1_trace);
        }
        psg_core1_parked = false;
    }
}

static void psg_audio_core1_entry(void) {
    // core1上でプールを作成 (このハードウェアアラームのIRQはcore1側で処理される)。
    // core0のデフォルトプールはアラーム3を使用中のため、未使用番号を自動取得する。
    // 最優先に設定: このcoreでは他に邪魔するものは無いが念のため。
    alarm_pool_t* pool = alarm_pool_create_with_unused_hardware_alarm(1);
    if (pool) {
        psg_core1_alarm_irq = TIMER_IRQ_0 + alarm_pool_hardware_alarm_num(pool);
        irq_set_priority(psg_core1_alarm_irq, 0);
        // 常時走らせる常駐タイマー (多声OFF中はコールバックが何もしない)
        alarm_pool_add_repeating_timer_us(pool, -PSG_EMU_TIMER_US,
            psg_emu_timer_callback, NULL, &psg_emu_timer);
    }
    for (;;) {
        // フラッシュ書込 (SAVE/KBD) のコマンドをポーリング。
        // multicore_fifo_pop_timeout_us(0,...) は内部で WFE/アラームプールを使い、
        // core0 からの SEV を見落として永久に眠ることがあるため、SIO FIFO レジスタを直接見る。
        if (sio_hw->fifo_st & SIO_FIFO_ST_VLD_BITS) {
            uint32_t cmd = sio_hw->fifo_rd;
            if (cmd == PSG_CORE1_CMD_PARK) {
                // 先にPSGタイマー割込みを停止してからACKする (この時点ではcore0が
                // ACK待ちでブロック中のためXIP上のコード実行は安全)。ACK後に
                // RAM待機関数へ入り、RESUME受信まで1命令もXIPを実行しない
                if (pool) irq_set_enabled(psg_core1_alarm_irq, false);
                psg_core1_park_and_wait(); // RAM内でACK送信+RESUME待機
                // ここからはフラッシュ操作完了後 (XIP復帰済み)
                if (pool) irq_set_enabled(psg_core1_alarm_irq, true);
                psg_core1_trace = 4; // アラーム再有効化済
                multicore_fifo_push_blocking(PSG_CORE1_CMD_RESUME); // 完了ACK
                psg_core1_trace = 5; // 完了ACK送信済
            } else if (cmd == PSG_CORE1_CMD_FLASH) {
                // core1でフラッシュ消去/書込みを実行。core0はRAMの完了フラグをポーリング。
                if (pool) irq_set_enabled(psg_core1_alarm_irq, false);
                psg_core1_flash_only(); // RAM内で消去/書込み、完了フラグをセット
                if (pool) irq_set_enabled(psg_core1_alarm_irq, true);
                psg_core1_trace = 4;
                // RESUME 待ちは不要: core0 は完了フラグを見て復帰し、core1 はそのままメインループへ
            }
        }
        // WFI/WFE せずに常にポーリング (消費電力は増えるが SAVE/KBD の信頼性を優先)
        tight_loop_contents();
    }
}

void sound_init() {
    _g.psgratio = 1;
    gpio_set_function(SOUND_PIN, GPIO_FUNC_PWM);
    // 音声出力パッドを最大駆動(12mA)+高速スルーレートに設定する (既定は4mA)。
    // 低インピーダンス負荷をわずかにドライブしやすくするためのもの。
    // 圧電ブザー直結はこれでも実用音量にならない (アンプ/ドライバ経由が必須、MANUAL §7.5)。
    // パッド設定はGPIO/PWMどちらの機能でも有効 (pwm_set_gpio_level等では消えない)
    gpio_set_drive_strength(SOUND_PIN, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_slew_rate(SOUND_PIN, GPIO_SLEW_RATE_FAST);
    slice_num = pwm_gpio_to_slice_num(SOUND_PIN);
    pwm_set_clkdiv(slice_num, PWM_CLKDIV_MHZ()); // 1MHzカウンタ (PWMトーンモード)
    pwm_set_wrap(slice_num, 0);
    pwm_set_gpio_level(SOUND_PIN, 0);
    pwm_set_enabled(slice_num, true);
    prev_tone = 0;
    psg_emu_init();
    sound_multi_mode = false;
    sound_multi_play = false;
    // PSGエンジンをcore1へ移設 (映像/USB/60FPSと分離して音の荒れを防ぐ)
    multicore_launch_core1(psg_audio_core1_entry);
}

// 約56kHzオーディオ割込み (core1): PSGエミュを1ステップ進め、3ch双極性混合値をPWMデューティへ出力
// 常駐タイマーなので常にtrueを返す。多声OFF時は何もしない (core0のPWMトーンモードを尊重)
// 高レート(18µs)でXIPフラッシュ待ちによるジッタを避けるためRAM実行にする
bool __not_in_flash_func(psg_emu_timer_callback)(repeating_timer_t* rt) {
    if (!sound_multi_mode) {
        return true;
    }
    int8_t mixed = psg_emu_tick(); // -45..+45 (3ch × 音量±15、双極性混合)
    // 双極性をデューティへ: ×16で1声±240 (47%) ≈ 単声PWMの振幅に相当
    // 2声までフルスケール、3声同時ピークはクランプ (ソフトリミット)
    int level = 512 + mixed * 16;
    if (level < 0)
        level = 0;
    if (level > SOUND_PWM_WRAP)
        level = SOUND_PWM_WRAP;
    pwm_set_gpio_level(SOUND_PIN, level);
    return true;
}

// 多声PSGモード切替 (psg.h / basic.hのSOUNDコマンドから呼ばれる)
// on=true:  PWMをデューティモード (wrap=1023) にしPSGエミュ出力を有効化
// on=false: PWMをトーンモード (1MHzカウンタ) に戻す
// オーディオタイマーはcore1で常駐しているため、ここのON/OFFはフラグとPWM設定のみ
// by_play=true: 多声PLAYによるモードON (psg_tickが全声終了を検出して自動OFF)
void sound_set_multi_mode(bool on, bool by_play) {
    if (on) {
        if (!sound_multi_mode) {
            // 先にPWMをデューティモードへ (クロック無分周: キャリア = 157.333333MHz/1024 ≈ 154kHz)。
            // sound_multi_modeは最後に立てる (core1が未設定のPWMへ書かないように)
            pwm_set_enabled(slice_num, false);
            pwm_set_clkdiv(slice_num, 1);
            pwm_set_wrap(slice_num, SOUND_PWM_WRAP);
            pwm_set_counter(slice_num, 0);
            pwm_set_gpio_level(SOUND_PIN, 0);
            pwm_set_enabled(slice_num, true);
            psg_emu_init();
            sound_multi_mode = true;
        }
        if (by_play) {
            sound_multi_play = true;
        }
    } else {
        if (!sound_multi_mode && !sound_multi_play)
            return; // 既にPWMトーンモード
        sound_multi_mode = false; // 先にOFF (core1のPWMデューティ書き込みを止める)
        sound_multi_play = false;
        _g.psgtone = 0; // 停止トーンの残存でPWMが再発音するのを防止
        prev_tone = 0;  // set_tone()に再設定させる
        // PWMをトーンモードへ戻す (1MHzカウンタ)
        pwm_set_enabled(slice_num, false);
        pwm_set_clkdiv(slice_num, PWM_CLKDIV_MHZ());
        pwm_set_wrap(slice_num, 0);
        pwm_set_counter(slice_num, 0);
        pwm_set_gpio_level(SOUND_PIN, 0);
        pwm_set_enabled(slice_num, true);
    }
}

// 60FPSタイマーから呼ばれる: _g.psgtone → PWM周波数設定 (旧IchigoJam_P方式)
// PWMハードウェアが正確な方形波を生成するため、タイマージッタがなくクリアな音が出る
// 多声モード中は56kHz割り込みがPWMデューティを制御するため何もしない
void set_tone() {
    if (sound_multi_mode) {
        return;
    }
    if (prev_tone != _g.psgtone) {
        if (_g.psgtone) {
            int freq = 60 * 261 / 2 / _g.psgtone;
            int wrap = 1000000 / freq - 1;
            pwm_set_wrap(slice_num, wrap);
            pwm_set_gpio_level(SOUND_PIN, wrap / 2); // 50%デューティ = 方形波
        } else {
            pwm_set_counter(slice_num, 0);
            pwm_set_gpio_level(SOUND_PIN, 0); // 消音
        }
        prev_tone = _g.psgtone;
    }
}

//クロックを変更したときにPWMの分周比を再設定する
void sound_reconfig() {
    // スリープ復帰/クロック変更後に音声出力パッド設定を再適用
    gpio_set_drive_strength(SOUND_PIN, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_slew_rate(SOUND_PIN, GPIO_SLEW_RATE_FAST);
    if (sound_multi_mode) {
        pwm_set_clkdiv(slice_num, 1); // デューティモード: キャリア = sysclk/1024
    } else {
        pwm_set_clkdiv(slice_num, PWM_CLKDIV_MHZ());
        prev_tone = 0; //次のset_toneで再設定させる
    }
}

static void sound_switch(int on) {
    if (sound_multi_mode) {
        return; // 多声モード中は56kHz割り込みが制御 (PWMトーンの開始/停止はしない)
    }
    if (on) {
        // set_tone()がPWM周波数を設定する (ここではPWMを有効化のみ)
        // PWMは常に有効だが、wrap=0/level=0で出力がLOWになっている
    } else {
        pwm_set_counter(slice_num, 0);
        pwm_set_gpio_level(SOUND_PIN, 0); // 消音
    }
}

#endif
