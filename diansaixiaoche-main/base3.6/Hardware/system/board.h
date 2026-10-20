#ifndef BOARD_H
#define BOARD_H

#include "ti_msp_dl_config.h"

void delay_us(unsigned long __us);
void delay_ms(unsigned long ms);

void uart0_send_char(char ch);
void uart0_send_string(const char *str);
void uart0_send_string_nb(const char *str);

#endif /* BOARD_H */
