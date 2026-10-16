#ifndef __SOUND_H__
#define __SOUND_H__
#include "all_includes.h"

//Waveshare RP2040-Zero: 側面パッドGP14 (IchigoJam_PのGP20から変更: 裏面パッド不使用のため)
#define SOUND_PIN 14

static uint slice_num;
static uint16 prev_tone = 0;

void sound_init() {
    _g.psgratio = 1;
    gpio_set_function(SOUND_PIN, GPIO_FUNC_PWM);
    slice_num = pwm_gpio_to_slice_num(SOUND_PIN);
    pwm_set_clkdiv(slice_num, PWM_CLKDIV_MHZ());  // 1MHzカウンタ
    pwm_set_wrap(slice_num, 0);
    pwm_set_gpio_level(SOUND_PIN, 0);
    pwm_set_enabled(slice_num, true);
    prev_tone = 0;
}

// 60FPSタイマーから呼ばれる: _g.psgtone → PWM周波数設定 (旧IchigoJam_P方式)
// PWMハードウェアが正確な方形波を生成するため、タイマージッタがなくクリアな音が出る
void set_tone() {
    if (prev_tone != _g.psgtone) {
        if (_g.psgtone) {
            int freq = 60 * 261 / 2 / _g.psgtone;
            int wrap = 1000000 / freq - 1;
            pwm_set_wrap(slice_num, wrap);
            pwm_set_gpio_level(SOUND_PIN, wrap / 2);  // 50%デューティ = 方形波
        } else {
            pwm_set_counter(slice_num, 0);
            pwm_set_gpio_level(SOUND_PIN, 0);  // 消音
        }
        prev_tone = _g.psgtone;
    }
}

//クロックを変更したときにPWMの分周比を再設定する
void sound_reconfig() {
    pwm_set_clkdiv(slice_num, PWM_CLKDIV_MHZ());
    prev_tone = 0;//次のset_toneで再設定させる
}

static void sound_switch(int on) {
    if (on) {
        // set_tone()がPWM周波数を設定する (ここではPWMを有効化のみ)
        // PWMは常に有効だが、wrap=0/level=0で出力がLOWになっている
    } else {
        pwm_set_counter(slice_num, 0);
        pwm_set_gpio_level(SOUND_PIN, 0);  // 消音
    }
}

#endif
