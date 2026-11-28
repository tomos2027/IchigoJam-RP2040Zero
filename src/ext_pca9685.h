//PCA9685 (I2C 16ch PWM/サーボドライバ) — PCA9685.SETUP/SETFREQ/PWM コマンド
//I2C0 (GP4=SDA/GP5=SCL、起動時にi2c0_init()済み) を使用する。
//ENV.IN・EJ (OLED)・外部EEPROMとバスを共有する (アドレスが異なるため共存可)。
//
//仕様:
//  PCA9685.SETUP <address>   I2Cアドレス 7bit 0x40-0x7F (既定0x40、A0-A5ピンで変更)
//                            チップを初期化 (スリープ解除+自動インクリメント+トーテンポール)
//  PCA9685.SETFREQ <freq>    PWM周波数 24-1526Hz (サーボは50)
//                            prescale = round(25MHz/(4096*freq))-1 を
//                            スリープ→設定→復帰(RESTART)のデータシート手順で設定
//  PCA9685.PWM <pin>,<value> pin 0-15、value 0-4095 (12bitデューティ)
//                            LEDn_ON=0、LEDn_OFF=value を4バイト連続書込み
//                            (SETUPの自動インクリメント有効が必要)
//
//SETUP必須: 未実行でSETFREQ/PWMするとIllegal argument (CLVでも未実行へ戻る)。
//I2C転送の失敗 (NACK等) は無視する (IJB_i2cと同じサイレント方式)。
//チップの電源再投入後は SETUP を再実行すること (MCU側は状態を保持しない)。
#ifndef __EXT_PCA9685_H__
#define __EXT_PCA9685_H__
#include "all_includes.h"

//===== レジスタ (PCA9685データシート準拠) =====
#define PCA9685_REG_MODE1      0x00
#define PCA9685_REG_MODE2      0x01
#define PCA9685_REG_LED0_ON_L  0x06
#define PCA9685_REG_PRE_SCALE  0xFE
#define PCA9685_MODE1_AI       0x20  // 自動インクリメント
#define PCA9685_MODE1_SLEEP    0x10  // スリープ (オシレータ停止)
#define PCA9685_MODE1_RESTART  0x80  // 再始動 (自己クリア)
#define PCA9685_MODE2_OUTDRV   0x04  // トーテンポール出力
#define PCA9685_OSC_CLK        25000000 // 内部オシレータ 25MHz

static int pca_addr = 0x40;  // I2Cアドレス 7bit (SETUPで設定、既定0x40)
static int pca_ready = 0;    // SETUP済みか (CLV/起動でクリア)

// リセット (basic_clearVarsから呼ばれる): 既定アドレスへ戻しSETUP必須状態へ
void pca9685_reset(void) {
    pca_addr = 0x40;
    pca_ready = 0;
}

// 1レジスタ書込み (reg+data の2バイト転送)
static void pca_write_reg(int reg, int val) {
    uint8_t buf[2] = { (uint8_t)reg, (uint8_t)val };
    i2c_write_timeout_us(i2c0, pca_addr, buf, 2, false, TIMEOUT_US);
}

//PCA9685.SETUP <address> — I2Cアドレス (7bit 0x40-0x7F) 設定+チップ初期化
//  MODE1=0x20 (スリープ解除+自動インクリメント)、MODE2=0x04 (トーテンポール)。
//  周波数はチップの現在値を維持 (変更はPCA9685.SETFREQ。パワーオン既定は200Hz)
S_INLINE void command_pca9685_setup() {
    int addr = token_expression();
    IJB_ERR_CHK();
    token_end();
    IJB_ERR_CHK();
    if (addr < 0x40 || addr > 0x7F) {
        command_error(ERR_ILLEGAL_ARGUMENT);
        return;
    }
    pca_addr = addr;
    pca_ready = 1;
    pca_write_reg(PCA9685_REG_MODE1, PCA9685_MODE1_AI);      // スリープ解除+AI
    pca_write_reg(PCA9685_REG_MODE2, PCA9685_MODE2_OUTDRV);  // トーテンポール
}

//PCA9685.SETFREQ <freq> — PWM周波数 24-1526Hz
//  prescale = round(25MHz/(4096*freq))-1 を整数演算で計算し、
//  スリープ→プリスケール設定→復帰(RESTART)のデータシート手順で反映
S_INLINE void command_pca9685_setfreq() {
    int freq = token_expression();
    IJB_ERR_CHK();
    token_end();
    IJB_ERR_CHK();
    if (!pca_ready || freq < 24 || freq > 1526) {
        command_error(ERR_ILLEGAL_ARGUMENT);
        return;
    }
    int prescale = (PCA9685_OSC_CLK + 2048 * freq) / (4096 * freq) - 1; // round(x)-1
    if (prescale < 3) prescale = 3;
    if (prescale > 255) prescale = 255;
    pca_write_reg(PCA9685_REG_MODE1, PCA9685_MODE1_AI | PCA9685_MODE1_SLEEP); // スリープ
    pca_write_reg(PCA9685_REG_PRE_SCALE, prescale);
    pca_write_reg(PCA9685_REG_MODE1, PCA9685_MODE1_AI | PCA9685_MODE1_RESTART); // 復帰+再始動
}

//PCA9685.PWM <pin>,<value> — pin 0-15、value 0-4095 (12bitデューティ)
//  LEDn_ON=0、LEDn_OFF=value を4バイト連続書込み (SETUPの自動インクリメント有効が必要)
S_INLINE void command_pca9685_pwm() {
    int pin = token_expression();
    IJB_ERR_CHK();
    if (token_getCode() != TOKEN_COMMA) {
        command_error(ERR_SYNTAX_ERROR);
        return;
    }
    int value = token_expression();
    IJB_ERR_CHK();
    token_end();
    IJB_ERR_CHK();
    if (!pca_ready || pin < 0 || pin > 15) {
        command_error(ERR_ILLEGAL_ARGUMENT);
        return;
    }
    if (value < 0) value = 0;
    if (value > 4095) value = 4095;
    uint8_t buf[5] = { (uint8_t)(PCA9685_REG_LED0_ON_L + pin * 4), 0, 0,
                       (uint8_t)(value & 0xFF), (uint8_t)(value >> 8) };
    i2c_write_timeout_us(i2c0, pca_addr, buf, 5, false, TIMEOUT_US);
}

#endif
