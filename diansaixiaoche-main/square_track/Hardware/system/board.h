#ifndef __BOARD_H__
#define __BOARD_H__

#include "ti_msp_dl_config.h"

void delay_us(unsigned long __us);
void delay_ms(unsigned long ms);

void uart0_send_char(char ch);
void uart0_send_string(const char *str);

// 非阻塞发送（环形缓冲 + TX 中断，缓冲满就整行丢掉，绝不在发送上等）
void uart0_send_string_nb(const char *str);           // 内部数长度
void uart0_send_nb(const char *data, uint16_t len);   // 长度已知时用，省一趟 strlen

#endif
