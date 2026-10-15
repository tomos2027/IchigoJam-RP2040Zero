#ifndef __SYSTEM_H__
#define __SYSTEM_H__
#include "all_includes.h"

//ウォッチドッグ: core0が固まったらこの秒数後に自動再起動する (電源抜き差しでハングした場合の自動復帰)
//timer()内でwatchdog_update()を呼んで生きていることを伝える
#define WATCHDOG_TIMEOUT_MS 8000

static uint scb_orig;
static bool awake;
bool is_deep_sleeping = false;

int getSleepFlag() {
    return IJB_btn(0);
}

void record_clocks() {
    scb_orig = scb_hw->scr;
}

void recover_from_sleep() {
    scb_hw->scr = scb_orig;//スリープ時に立ったSLEEPDEEPビットを解除

    //rosc再開、クロック復元(clocks_init相当)、UART再設定
    sleep_power_up();
    set_sys_clock_khz(157500, true);//NTSC駆動クロック
    set_uart_bps();
    io_reconfig_pwm();//クロックが変わったのでPWMの分周比を再設定
    //スリープ中はtimer()が止まるためウォッチドッグを切ってあるので、ここで再有効化する
    watchdog_enable(WATCHDOG_TIMEOUT_MS, true);
}

static void sleep_callback(uint alarm_num) {
    awake = true;
}

//指定した秒数だけディープスリープする
static void rtc_sleep(int sec) {
    sleep_goto_sleep_for(sec * 1000, sleep_callback);
}

//RTCの仕様上、1秒刻みでしかディープスリープの秒数を指定できない
static inline void enterDeepSleep(int sec) {
    if (sec == 0) {
        return;
    }
    bool active = video_active();
    video_off(0);
    watchdog_disable();//スリープ中はtimer()が動かないのでウォッチドッグを止める (recover_from_sleepで再有効化)
    //pico-playground/sleep/hello_sleep/hello_sleep.c　10秒スリープするプログラムと書かれているが、実際は9秒
    //https://ghubcoder.github.io/posts/awaking-the-pico/ 参照
    record_clocks();
    sleep_run_from_xosc();
    awake = false;
    rtc_sleep(sec);
    while (!awake) {
        __wfi();//入れないとスリープから復帰できない時がある、謎
        // printf("Should be sleeping\n");
    }
    recover_from_sleep();
    if (active) {
        sleep_ms(1);//入れないとおかしくなる時がある
        video_on();
    }
}

//SLEEPコマンドで再起動をかけると、ボタンを押した状態で起動するのでLRUN0が実行されるのが想定の挙動だが、ボタンを一瞬だけ押すと普通に起動してしまう
//これを解消するために起動時にwatchdog_enable_caused_reboot()でSLEEPによる再起動か判別する
static void IJB_sleep() {
    while (IJB_btn(0)) {
        //ボタンを押している間はスリープに入らない
    }
    led_builtin(false);
    gpio_init(BTN);//ADCのままだと正しく反応しない
    gpio_pull_up(BTN);
    video_off(0);
    watchdog_disable();//スリープ中はtimer()が動かないのでウォッチドッグを止める (この関数は最終的にウォッチドッグで意図的に再起動する)
    //pico-playground/sleep/hello_dormant/hello_dormant.c 参照
    record_clocks();
    sleep_run_from_xosc();
    sleep_goto_dormant_until_pin(BTN, false, false);//ボタンを押すまでスリープし続ける
    recover_from_sleep();//クロックを戻さないとwatchdogが動かない？

    //watchdog_reboot(0, SRAM_END, 0)では、watchdog_hw->scratch[4] = 0になるが、
    //watchdog_hw->scratch[4] = WATCHDOG_NON_REBOOT_MAGICにしたい(watchdog_enable(0, 0)を呼び出すと可能)
    check_hw_layout(watchdog_hw_t, scratch[7], WATCHDOG_SCRATCH7_OFFSET);
    hw_clear_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS);
    watchdog_enable(0, 0);
    for (;;) {
        __wfi();
    }
}

INLINE void IJB_reset() {
    //https://forums.raspberrypi.com/viewtopic.php?t=308166 参照
    watchdog_reboot(0, SRAM_END, 0);
    for (;;) {
        __wfi();
    }
}

static int IJB_wait(int n, int active) {
    if (n < 0 && !video_active()) {//映像が止まっているときは負のwaitは使えないので早期リターン
        return 0;
    }

    if (active) {
        if (n < 0) { // ver 1.2b10
            _g.linecnt = 0;
            n = -n;
            while (_g.linecnt < n) {
                if (stopExecute())
                    return 1;
            }
        } else {
            uint64 start = time_us_64();
            while (time_us_64() - start < n * 16666) { //16666 ≒ 1000000 / 60
                if (stopExecute())
                    return 1;
            }
        }
        return 0;
    } else if (n > 0) {
        //ディープスリープ中はキーボード入力を無効にする
        is_deep_sleeping = true;
        lp.last_char = 0;
        led_builtin(false);
        enterDeepSleep(n / 60);
        //1秒未満は普通のsleepで誤魔化す
        sleep_us((n % 60) / 60.0 * 1000000);
        led_builtin(true);
        is_deep_sleeping = false;
    }
    return 0;
}

#endif
