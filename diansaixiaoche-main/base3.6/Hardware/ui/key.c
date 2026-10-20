/**
 * @file    key.c
 * @brief   三颗按键驱动：电平读取 + 单击检测（非阻塞状态机，50ms 消抖）
 * @note    key*_click() 需每 10ms 调用一次（跟随主循环节拍）
 */

#include "key.h"

/* 三颗按键的 GPIO 映射：集中成表，读电平与消抖状态机共用一份定义 */
typedef struct {
    GPIO_Regs *port;
    uint32_t   pin;
} KeyLine;

static const KeyLine key_lines[3] = {
    { KEY_button1_PORT, KEY_button1_PIN },
    { KEY_button2_PORT, KEY_button2_PIN },
    { KEY_button3_PORT, KEY_button3_PIN },
};

#define KEY_COUNT            3u
#define KEY_DEBOUNCE_TICKS   5u    /* 5 × 10ms = 50ms */

/* ── 电平读取 ── */
static inline uint8_t key_pressed_by_index(uint8_t idx)
{
    /* 与原来的 switch 默认分支一致：越界索引按最后一颗键处理 */
    if (idx >= KEY_COUNT)
        idx = KEY_COUNT - 1u;

    return (DL_GPIO_readPins(key_lines[idx].port, key_lines[idx].pin) == 0);
}

uint8_t key1_pressed(void) { return key_pressed_by_index(0); }
uint8_t key2_pressed(void) { return key_pressed_by_index(1); }
uint8_t key3_pressed(void) { return key_pressed_by_index(2); }

/* ── 单击检测：非阻塞状态机，每 10ms 调用一次，带 50ms 消抖 ──
 * 三颗按键共用同一套状态机（行为与原三份复制粘贴代码完全一致）：
 *   按下消抖 50ms → 确认按下 → 等待松开消抖 50ms → 返回单击 */
typedef enum {
    KEY_IDLE = 0,
    KEY_DEBOUNCE_DOWN,
    KEY_DOWN,
    KEY_DEBOUNCE_UP
} KeyState_t;

static uint8_t key_click_by_index(uint8_t idx)
{
    static KeyState_t state[KEY_COUNT] = {KEY_IDLE, KEY_IDLE, KEY_IDLE};
    static uint8_t    cnt[KEY_COUNT]   = {0, 0, 0};
    uint8_t pressed = key_pressed_by_index(idx);

    switch (state[idx]) {
        case KEY_IDLE:
            if (pressed) { cnt[idx] = KEY_DEBOUNCE_TICKS; state[idx] = KEY_DEBOUNCE_DOWN; }
            break;
        case KEY_DEBOUNCE_DOWN:
            if (!pressed) { state[idx] = KEY_IDLE; }
            else if (--cnt[idx] == 0) { state[idx] = KEY_DOWN; }
            break;
        case KEY_DOWN:
            if (!pressed) { cnt[idx] = KEY_DEBOUNCE_TICKS; state[idx] = KEY_DEBOUNCE_UP; }
            break;
        case KEY_DEBOUNCE_UP:
            if (pressed) { state[idx] = KEY_DOWN; }
            else if (--cnt[idx] == 0) { state[idx] = KEY_IDLE; return 1; }
            break;
    }
    return 0;
}

uint8_t key1_click(void) { return key_click_by_index(0); }
uint8_t key2_click(void) { return key_click_by_index(1); }
uint8_t key3_click(void) { return key_click_by_index(2); }
