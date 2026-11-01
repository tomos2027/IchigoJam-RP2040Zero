#ifndef __SOUND_H__
#define __SOUND_H__
#include "all_includes.h"
#include "pico/multicore.h"

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

static void psg_audio_core1_entry(void) {
    // core1上でプールを作成 (このハードウェアアラームのIRQはcore1側で処理される)。
    // core0のデフォルトプールはアラーム3を使用中のため、未使用番号を自動取得する。
    // 最優先に設定: このcoreでは他に邪魔するものは無いが念のため。
    alarm_pool_t* pool = alarm_pool_create_with_unused_hardware_alarm(1);
    if (pool) {
        irq_set_priority(TIMER_IRQ_0 + alarm_pool_hardware_alarm_num(pool), 0);
        // 常時走らせる常駐タイマー (多声OFF中はコールバックが何もしない)
        alarm_pool_add_repeating_timer_us(pool, -PSG_EMU_TIMER_US,
            psg_emu_timer_callback, NULL, &psg_emu_timer);
    }
    for (;;) {
        __wfi(); // タイマー割込みのみで駆動 (プール取得失敗時は無音で継続)
    }
}

void sound_init() {
    _g.psgratio = 1;
    gpio_set_function(SOUND_PIN, GPIO_FUNC_PWM);
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
            // 先にPWMをデューティモードへ (クロック無分周: キャリア = 157.5MHz/1024 ≈ 154kHz)。
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
