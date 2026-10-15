#ifndef __SOUND_H__
#define __SOUND_H__
#include "all_includes.h"

//Waveshare RP2040-Zero: 側面パッドGP14 (IchigoJam_PのGP20から変更: 裏面パッド不使用のため)
#define SOUND_PIN 14
// PWMキャリア設定: システムクロック(157.5MHz) / 1024 = ~154kHz (可聴域外)
#define SOUND_PWM_WRAP 1023

static uint slice_num;
static uint16 prev_tone = 0;
static struct repeating_timer audio_timer;
static bool audio_timer_ok = false;

// オーディオタイマーコールバック (~16kHz)
// PSGエミュレーションを1ステップ進め、混合値をPWMデューティへ出力
static bool audio_timer_callback(repeating_timer_t* rt) {
    uint8_t mixed = psg_emu_tick();  // 0-45
    // 混合値をPWMデューティへ変換 (0-45 → 0-1023)
    uint16_t duty = ((uint16_t)mixed * (SOUND_PWM_WRAP + 1)) / 46;
    if (duty > SOUND_PWM_WRAP) duty = SOUND_PWM_WRAP;
    pwm_set_gpio_level(SOUND_PIN, duty);
    return true;
}

void sound_init() {
    _g.psgratio = 1;
    // PWMキャリア設定 (フルシステムクロック、高周波キャリア+デューティ変調)
    // clkdiv=1.0 → PWMカウンタはシステムクロック(157.5MHz)で動作
    // wrap=1023 → キャリア周波数 = 157.5MHz/1024 ≈ 154kHz (完全に可聴域外)
    // スピーカーのインダクタンスが自然なローパスフィルタとなり、オーディオ信号のみが到達する
    gpio_set_function(SOUND_PIN, GPIO_FUNC_PWM);
    slice_num = pwm_gpio_to_slice_num(SOUND_PIN);
    pwm_set_clkdiv(slice_num, 1.0f);              // フルシステムクロック
    pwm_set_wrap(slice_num, SOUND_PWM_WRAP);      // 1024ステップ → ~154kHzキャリア
    pwm_set_gpio_level(SOUND_PIN, 0);
    pwm_set_enabled(slice_num, true);

    // PSG初期化 (全チャンネル消音)
    psg_emu_init();

    // オーディオタイマー (~16kHz) 開始
    // 63µs周期 → 15873Hz (PSGクロック1789772/112.3に近似)
    audio_timer_ok = add_repeating_timer_us(-63, audio_timer_callback, NULL, &audio_timer);

    prev_tone = 0;
}

void set_tone() {
    // 60FPSタイマーから呼ばれる: MMLパーサーが _g.psgtone を設定した後
    // _g.psgtone → PSG period変換 → レジスタ設定
    if (prev_tone != _g.psgtone) {
        if (_g.psgtone) {
            // IchigoJam psgtone値 → 周波数 → PSG period
            uint16_t period = psg_emu_tone_to_period(_g.psgtone);
            psg_emu_set_period(0, period);   // chA周波数
            psg_emu_set_volume(0, 12);        // 適度な音量
            psg_regs[7] &= ~0x01;             // chAトーン有効
        } else {
            // 消音
            psg_emu_set_volume(0, 0);
        }
        prev_tone = _g.psgtone;
    }
}

//クロックを変更したときにPWMの分周比を再設定する
//注意: PSGオーディオはフルシステムクロック (clkdiv=1.0) を使うため、PWM_CLKDIV_MHZ()は使わない
void sound_reconfig() {
    pwm_set_clkdiv(slice_num, 1.0f);
    prev_tone = 0;//次のset_toneで再設定させる
}

static void sound_switch(int on) {
    if (on) {
        psg_regs[7] &= ~0x01;  // chAトーン有効
    } else {
        psg_regs[7] |= 0x3F;   // 全チャンネル無効
    }
}

#endif
