#ifndef __SOUND_H__
#define __SOUND_H__
#include "all_includes.h"

//Waveshare RP2040-Zero: 側面パッドGP14 (IchigoJam_PのGP20から変更: 裏面パッド不使用のため)
#define SOUND_PIN 14

// 多声PSGモード: PWMデューティ解像度 (キャリア = sysclk/1024 ≈ 154kHz、可聴域外)
#define SOUND_PWM_WRAP 1023
// オーディオ割り込み周期 (~16kHz、psg_emu.hのPSG_EMU_RATEに対応)
#define SOUND_EMU_TIMER_US 63

static uint slice_num;
static uint16 prev_tone = 0;
static repeating_timer_t psg_emu_timer;
static alarm_pool_t* psg_emu_pool = NULL;
static bool psg_emu_timer_running = false;
// sound_multi_mode / sound_multi_play は psg.hで定義 (psg.hが先にincludeされるため)

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
    psg_emu_timer_running = false;
    // 16kHzオーディオ用の専用アラームプール (60FPSタイマーと別のハードウェアアラーム)
    // 最高優先割り込みにする: 60FPSコールバック (USBタスク含む) 実行中でも音が途切れない
    if (!psg_emu_pool) {
        psg_emu_pool = alarm_pool_create(3, 1);
        irq_set_priority(TIMER_IRQ_3, 0);
    }
}

// ~16kHzオーディオ割り込み: PSGエミュを1ステップ進め、3ch混合値をPWMデューティへ出力
// 多声モードOFFを検出したらタイマーは自ら停止する (return false)
bool psg_emu_timer_callback(repeating_timer_t* rt) {
    if (!sound_multi_mode) {
        psg_emu_timer_running = false;
        return false;
    }
    uint mixed = psg_emu_tick(); // 0-45 (3ch × 音量15)
    pwm_set_gpio_level(SOUND_PIN, mixed * 22); // 0-990 / wrap 1023
    if (!sound_multi_mode) {
        pwm_set_gpio_level(SOUND_PIN, 0); // モードOFFへの切替競合対策: 消音を保証
    }
    return true;
}

// 多声PSGモード切替 (psg.h / basic.hのSOUNDコマンドから呼ばれる)
// on=true:  PWMをデューティモード (wrap=1023) にし~16kHzタイマーを開始、PSGエミュ出力
// on=false: PWMをトーンモード (1MHzカウンタ) に戻す、16kHzタイマーはコールバックが自ら停止
// by_play=true: 多声PLAYによるモードON (psg_tickが全声終了を検出して自動OFF)
void sound_set_multi_mode(bool on, bool by_play) {
    if (on) {
        if (!sound_multi_mode) {
            // PWMをデューティモードへ (クロック無分周: キャリア = 157.5MHz/1024 ≈ 154kHz)
            pwm_set_enabled(slice_num, false);
            pwm_set_clkdiv(slice_num, 1);
            pwm_set_wrap(slice_num, SOUND_PWM_WRAP);
            pwm_set_counter(slice_num, 0);
            pwm_set_gpio_level(SOUND_PIN, 0);
            pwm_set_enabled(slice_num, true);
            psg_emu_init();
            sound_multi_mode = true;
            if (!psg_emu_timer_running) {
                psg_emu_timer_running = true;
                alarm_pool_add_repeating_timer_us(psg_emu_pool, -SOUND_EMU_TIMER_US,
                    psg_emu_timer_callback, NULL, &psg_emu_timer);
            }
        }
        if (by_play) {
            sound_multi_play = true;
        }
    } else {
        if (!sound_multi_mode && !sound_multi_play)
            return; // 既にPWMトーンモード
        sound_multi_mode = false; // 16kHzコールバックが次tickで自ら停止
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
// 多声モード中は16kHz割り込みがPWMデューティを制御するため何もしない
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
        return; // 多声モード中は16kHz割り込みが制御 (PWMトーンの開始/停止はしない)
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
