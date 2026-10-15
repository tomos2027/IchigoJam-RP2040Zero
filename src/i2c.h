#ifndef __I2C_H__
#define __I2C_H__
#include "all_includes.h"

//Waveshare RP2040-Zeroの基板既定I2C (I2C1: GP6/7) はIchigoJamのIN3/IN4と衝突するため、
//IchigoJam_P (Pico) と同じ I2C0 (GP4=SDA, GP5=SCL) を使用する
#define I2C0_SDA_PIN 4
#define I2C0_SCL_PIN 5

#define I2C_DEFAULT_BPS 400000
#define TIMEOUT_US 1000000 //TODO タイムアウト入れるべきか、入れるなら何秒にすべきか検討する

static int i2c_baudrate = I2C_DEFAULT_BPS;

/* Initialize I2C module  0:ok 1:_g.err */
int i2c0_init() {
    i2c_init(i2c0, I2C_DEFAULT_BPS);
    gpio_set_function(I2C0_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C0_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C0_SDA_PIN);
    gpio_pull_up(I2C0_SCL_PIN);
    return 0;
}

// 0:success 1:io error 2:parm error
//readのときwritemode=1
INLINE int IJB_i2c(uint8 writemode, uint16* parameter) {
    int client_address = parameter[0];
    uint8_t* src1 = (uint8_t*)(parameter[1] + (uint)ram - OFFSET_RAMROM);
    int len1 = parameter[2];
    uint8_t* src2 = (uint8_t*)(parameter[3] + (uint)ram - OFFSET_RAMROM);
    int len2 = parameter[4];
    int res;
    res = i2c_write_timeout_us(i2c0, client_address, src1, len1, false, TIMEOUT_US);//nostop=trueにすると、sakura.ioが正しく動かない
    if (!writemode) {
        res += i2c_write_timeout_us(i2c0, client_address, src2, len2, false, TIMEOUT_US);
    } else {
        res += i2c_read_timeout_us(i2c0, client_address, src2, len2, false, TIMEOUT_US);
    }
    return res == len1 + len2 ? 0 : 1;
}

void set_i2c_bps() {
    i2c_set_baudrate(i2c0, i2c_baudrate);
}

void i2c0_bps(int n) {
    i2c_baudrate = n * 1000;
    set_i2c_bps();
}

#endif
