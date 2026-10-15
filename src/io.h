#ifndef __IO_H__
#define __IO_H__
#include "all_includes.h"

//Waveshare RP2040-Zero パッド割り当て
//側面エッジパッド: GP0-15 + GP26-28 (+電源/GND)
//裏面パッド: GP17-25 (本機では未使用)
//GP6/7/8/10/11はSDカード (SPI1) と共有 (SDON中はOUT/PWM/WS.LED/IR.INがブロックされる)
#define LED 16  // 基板内蔵WS2812 (led_builtinドライバで駆動、ext_ws.hで定義、WS.LED 7でも使用可)
#define BTN 28  // 側面パッド (ADC2)  GNDとの間にボタンを接続 (内部プルアップ)
#define IN1 27  // 側面パッド (ADC1)
#define IN2 26  // 側面パッド (ADC0)
#define IN3 6   // 側面パッド (SD CSと共有)
#define IN4 7   // 側面パッド (SDカード検出と共有)
#define OUT1 8  // 側面パッド (SD MISOと共有)
#define OUT2 9  // 側面パッド
#define OUT3 10 // 側面パッド (SD CLKと共有)
#define OUT4 11 // 側面パッド (SD MOSIと共有)
#define OUT5 12 // 側面パッド (IchigoJam_PのGP22から変更: 裏面パッド不使用のため)
#define OUT6 13 // 側面パッド (IchigoJam_PのGP21から変更: 裏面パッド不使用のため)

#define IO_PIN_NUM 11

#define PLEN_MAX 2000
#define ANA_THRESHOLD (1024 / 4)

//ラズパイの動作クロックはNTSC駆動の157.5MHz
//PWMのカウントクロックを常に1MHzにする (clkdiv = sysクロック/1MHz)
//IchigoJamのPWMは1周期20msなので50Hz
//https://rikei-tawamure.com/entry/2021/02/08/213335#PWM%E7%94%A8%E3%82%AB%E3%82%A6%E3%83%B3%E3%82%BF PWMの計算方法は左記参照
#define PWM_CLKDIV_MHZ() (clock_get_hz(clk_sys) / (1000.0f * 1000.0f))
#define PWM_WRAP 19999 // 1MHz / 50Hz - 1

void sound_reconfig();//sound.hで定義 (io.hより後ろにincludeされるため前方宣言)
void led_builtin(bool on);//ext_ws.hで定義 (基板内蔵WS2812=GP16をLEDコマンド/診断表示に使う)

static uint8 in_pins[] = { IN1, IN2, IN3, IN4, OUT1, OUT2, OUT3, OUT4, BTN, OUT5, OUT6 };
static uint8 out_pins[] = { OUT1, OUT2, OUT3, OUT4, OUT5, OUT6, LED, IN1, IN2, IN3, IN4 };

void IJB_pwm(int port, int plen, int len) {
    if (!(1 <= port && port <= 5)) {
        return;
    }

    if (plen < 0) {
        plen = 0;
    } else if (plen > PLEN_MAX) {
        plen = PLEN_MAX;
    }
    uint8 pin = out_pins[port - 1];
    if (sd_pin_reserved(pin)) {
        return;//SD動作中の共有ピンはPWM化禁止
    }
    gpio_set_function(pin, GPIO_FUNC_PWM);
    uint slice_num = pwm_gpio_to_slice_num(pin);
    pwm_set_clkdiv(slice_num, PWM_CLKDIV_MHZ());
    pwm_set_wrap(slice_num, PWM_WRAP);
    pwm_set_gpio_level(pin, (PWM_WRAP + 1) * plen / PLEN_MAX);
    pwm_set_enabled(slice_num, true);
}

//クロックを変更したときに、PWM出力中のOUTポートとサウンドの分周比を再設定する
void io_reconfig_pwm() {
    float div = PWM_CLKDIV_MHZ();
    for (int i = 0; i < 6; i++) {//OUT1-6
        pwm_set_clkdiv(pwm_gpio_to_slice_num(out_pins[i]), div);
    }
    sound_reconfig();
}

bool is_adc_pin(uint pin) {
    return 26 <= pin && pin <= 28;//BTN || IN1 || IN2
}

//電圧を0-1023の範囲で返す
int get_adc_volt(uint pin) {
    adc_select_input(pin - 26);
    return adc_read() >> 2;//12ビットから10ビットに
}

bool is_adc_high(uint pin) {
    return get_adc_volt(pin) > ANA_THRESHOLD;
}

/*プルの指定
IN プルアップ(デフォルト)　フロートにも変更可能
OUT 指定しない
ANA 指定しない
*/

void io_init() {
    adc_init();
    for (int i = 0; i < 4; i++) {
        uint8 pin = in_pins[i];
        if (sd_pin_reserved(pin)) {
            continue;//SD動作中の共有ピン (IN3/IN4) はスキップ
        }
        gpio_init(pin);
        gpio_pull_up(pin);
    }
    for (int i = 1; i <= 6; i++) {
        IJB_out(i, 0);//IJB_out内でsd_pin_reservedチェック
    }
    //LED (GP16) は基板内蔵WS2812のためGPIO直接制御せずled_builtinで駆動する (pico_initで点灯)
    gpio_init(BTN);
    gpio_pull_up(BTN);
}

//keycodeが0でないキーは全て反応する
int IJB_btn(int n) {
    if (n < 0) {
        int res = 0;
        for (uint8_t i = 0; i < 6; i++) {
            uint8_t keycode = prev_report.keycode[i];
            uint8_t ascii = keycode < 128 ? keycode2ascii[keycode][0] : 0;
            switch (ascii) {
            case LEFT:
                res |= 1 << 0;
                break;
            case RIGHT:
                res |= 1 << 1;
                break;
            case UP:
                res |= 1 << 2;
                break;
            case DOWN:
                res |= 1 << 3;
                break;
            case ' ':
                res |= 1 << 4;
                break;
            case 'x'://keycode2ascii[][0]は小文字を取得する
                res |= 1 << 5;
                break;
            }
        }
        return res;
    } else if (n == 0) {
        return !is_adc_high(BTN);
    } else {
        for (uint8_t i = 0; i < 6; i++) {
            uint8_t keycode = prev_report.keycode[i];
            if (0 < keycode && keycode < 128) {
                if (n == keycode2ascii[keycode][0] || n == keycode2ascii[keycode][1]) {
                    return true;
                }
            }
        }
        return false;
    }
}

int IJB_in() {
    int res = 0;
    for (int i = 0; i < IO_PIN_NUM; i++) {
        int pin = in_pins[i];
        if (sd_pin_reserved(pin) || pin == SD_DETECT_PIN) {
            continue;//SD動作中の共有ピンは読まない (意味のない値を返さない)
        }
        bool bit = gpio_get(pin);
        res |= bit << i;
    }
    return res;
}

void IJB_out(int port, int st) {
    if (!(0 <= port && port <= IO_PIN_NUM)) {
        return;
    }
    if (port == 0) {
        for (int i = 0; i < IO_PIN_NUM; i++) {
            if (i == 6) {//LEDポート(7)は基板内蔵WS2812のためgpio_putしない
                continue;
            }
            if (sd_pin_reserved(out_pins[i])) {
                continue;//SD動作中の共有ピンはブロック
            }
            gpio_put(out_pins[i], st & (1 << i));
        }
        led_builtin((st & (1 << 6)) != 0);
    } else if (port == 7) {//LED: 基板内蔵WS2812 (GP16)
        led_builtin(st != 0);
    } else {
        uint8 pin = out_pins[port - 1];
        if (sd_pin_reserved(pin)) {
            return;//SD動作中の共有ピン (OUT1/3/4) はブロック
        }
        gpio_init(pin);
        if (st >= 0) {//OUT
            gpio_set_dir(pin, GPIO_OUT);
            gpio_put(pin, st);
        } else if (st == -1) {//IN(フロート)
            gpio_disable_pulls(pin);
        } else if (st == -2) {//IN(プルアップ)
            gpio_pull_up(pin);
        }
    }
}

INLINE void IJB_led(int st) {
    IJB_out(7, st != 0);
}

INLINE int IJB_ana(int n) {
    if ((0 <= n && n <= 2) || n == 9) {
        if (n == 0) {
            n = 9;
        }
        int pin = in_pins[n - 1];
        bool is_pull_up = gpio_is_pulled_up(pin);
        adc_gpio_init(pin);
        int volt = get_adc_volt(pin);
        is_pull_up ? gpio_pull_up(pin) : gpio_pull_down(pin);
        return volt;
    }
    return 0;
}

INLINE void IJB_clo() {
    io_init();
}

void IJB_dac(int port, int val) {
    //何もしない
}

//BASICのINポート番号→GPIO番号 (4K版、IR.INコマンド用、GivetakeJam_P準拠)
static int ir_port_to_gpio(int port) {
    switch (port) {
        case 1: return IN1;
        case 2: return IN2;
        case 3: return sd_pin_reserved(IN3) ? -1 : IN3;//SD動作中は使用不可
        case 4: return sd_pin_reserved(IN4) ? -1 : IN4;//SD動作中は使用不可
    }
    return -1;
}

#endif
