/**
 * @file    oled_hardware_i2c.h
 * @brief   SSD1306 OLED（0.96"，I2C 硬件接口）显示驱动接口
 *
 * SysConfig 配置步骤：
 *   1. 添加 I2C 模块，命名为 "I2C_0"
 *   2. 勾选 "Enable Controller Mode"
 *   3. 总线速度选 Fast Mode (400kHz)
 *   4. 按接线配置 SDA/SCL 引脚
 */

/*
 * SysConfig Configuration Steps:
 *   I2C:
 *     1. Add an I2C module.
 *     2. Name it as "I2C_0".
 *     3. Check the box "Enable Controller Mode".
 *     4. Set "Standard Bus Speed" to "Fast Mode (400kHz)". (optional)
 *     5. Set the pins according to your needs.
 */

#ifndef OLED_HARDWARE_I2C_H
#define OLED_HARDWARE_I2C_H

#include "ti_msp_dl_config.h"

#define OLED_CMD  0
#define OLED_DATA 1

void OLED_ColorTurn(uint8_t i);
void OLED_DisplayTurn(uint8_t i);
void OLED_WR_Byte(uint8_t dat, uint8_t cmd);
void OLED_Refresh(void);
void OLED_Display_On(void);
void OLED_Display_Off(void);
void OLED_Clear(void);
void OLED_ShowChar(uint8_t x, uint8_t y, uint8_t chr, uint8_t sizey);
void OLED_ShowNum(uint8_t x, uint8_t y, uint32_t num, uint8_t len, uint8_t sizey);
void OLED_ShowString(uint8_t x, uint8_t y, const char *chr, uint8_t sizey);
void OLED_ShowChinese(uint8_t x, uint8_t y, uint8_t no, uint8_t sizey);
void OLED_DrawBMP(uint8_t x, uint8_t y, uint8_t sizex, uint8_t sizey, const uint8_t BMP[]);
void OLED_Init(void);
void oled_i2c_sda_unlock(void);

#endif /* OLED_HARDWARE_I2C_H */
