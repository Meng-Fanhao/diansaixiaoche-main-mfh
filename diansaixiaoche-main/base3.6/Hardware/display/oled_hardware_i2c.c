/**
 * @file    oled_hardware_i2c.c
 * @brief   SSD1306 OLED 驱动实现（硬件 I2C，含 SDA 卡死自恢复）
 * @note    I2C 传输带超时保护：超时后自动 bit-bang 解锁 SDA 并复位 I2C，
 *          依赖 system_ms（10ms 定时器中断累加）做超时基准。
 *          刷新走"缓冲 + 批量写"：Show* 只改 1KB 显存，OLED_Refresh 一次性
 *          把整帧推过去（约 25ms），不再逐字节发 START/STOP。
 */

#include "oled_hardware_i2c.h"
#include "oledfont.h"
#include "system/board.h"
#include <string.h>

#define I2C_TIMEOUT_MS    (10u)
#define OLED_I2C_ADDR     0x3C
#define OLED_WIDTH        128u
#define OLED_PAGES        8u
#define OLED_FRAME_SIZE   (OLED_WIDTH * OLED_PAGES)
/* 1 字节控制字 + 整页数据，一次 transfer 发完一页 */
#define OLED_PAGE_CHUNK   (1u + OLED_WIDTH)

extern volatile uint32_t system_ms;

static uint8_t frame_buf[OLED_FRAME_SIZE];

static int mspm0_i2c_disable(void)
{
    DL_I2C_reset(I2C_0_INST);
    DL_GPIO_initDigitalOutput(GPIO_I2C_0_IOMUX_SCL);
    DL_GPIO_initDigitalInputFeatures(GPIO_I2C_0_IOMUX_SDA,
         DL_GPIO_INVERSION_DISABLE, DL_GPIO_RESISTOR_NONE,
         DL_GPIO_HYSTERESIS_DISABLE, DL_GPIO_WAKEUP_DISABLE);
    DL_GPIO_clearPins(GPIO_I2C_0_SCL_PORT, GPIO_I2C_0_SCL_PIN);
    DL_GPIO_enableOutput(GPIO_I2C_0_SCL_PORT, GPIO_I2C_0_SCL_PIN);
    return 0;
}

static int mspm0_i2c_enable(void)
{
    DL_I2C_reset(I2C_0_INST);
    DL_GPIO_initPeripheralInputFunctionFeatures(GPIO_I2C_0_IOMUX_SDA,
        GPIO_I2C_0_IOMUX_SDA_FUNC, DL_GPIO_INVERSION_DISABLE,
        DL_GPIO_RESISTOR_NONE, DL_GPIO_HYSTERESIS_DISABLE,
        DL_GPIO_WAKEUP_DISABLE);
    DL_GPIO_initPeripheralInputFunctionFeatures(GPIO_I2C_0_IOMUX_SCL,
        GPIO_I2C_0_IOMUX_SCL_FUNC, DL_GPIO_INVERSION_DISABLE,
        DL_GPIO_RESISTOR_NONE, DL_GPIO_HYSTERESIS_DISABLE,
        DL_GPIO_WAKEUP_DISABLE);
    DL_GPIO_enableHiZ(GPIO_I2C_0_IOMUX_SDA);
    DL_GPIO_enableHiZ(GPIO_I2C_0_IOMUX_SCL);
    DL_I2C_enablePower(I2C_0_INST);
    SYSCFG_DL_I2C_0_init();
    return 0;
}

void oled_i2c_sda_unlock(void)
{
    uint8_t cycleCnt = 0;

    mspm0_i2c_disable();
    do {
        DL_GPIO_clearPins(GPIO_I2C_0_SCL_PORT, GPIO_I2C_0_SCL_PIN);
        delay_ms(1);
        DL_GPIO_setPins(GPIO_I2C_0_SCL_PORT, GPIO_I2C_0_SCL_PIN);
        delay_ms(1);

        if (DL_GPIO_readPins(GPIO_I2C_0_SDA_PORT, GPIO_I2C_0_SDA_PIN))
            break;
    } while (++cycleCnt < 100);
    mspm0_i2c_enable();
}

/* 一次 I2C 事务的公共流程：填 FIFO → 等控制器空闲 → 启动 → 等 TX_DONE。
 * 超时就 bit-bang 解锁 SDA，避免显示异常时把调用者卡死 */
static void oled_write_buf(uint8_t *buf, uint16_t len)
{
    uint32_t start = system_ms;
    uint32_t idle_timeout = 0;

    DL_I2C_fillControllerTXFIFO(I2C_0_INST, buf, len);
    DL_I2C_clearInterruptStatus(I2C_0_INST, DL_I2C_INTERRUPT_CONTROLLER_TX_DONE);

    while (!(DL_I2C_getControllerStatus(I2C_0_INST) & DL_I2C_CONTROLLER_STATUS_IDLE)) {
        if (++idle_timeout > 100000u) break;   /* 防死等：控制器一直不空闲则放弃 */
    }

    DL_I2C_startControllerTransfer(I2C_0_INST, OLED_I2C_ADDR,
                                   DL_I2C_CONTROLLER_DIRECTION_TX, len);

    while (!DL_I2C_getRawInterruptStatus(I2C_0_INST, DL_I2C_INTERRUPT_CONTROLLER_TX_DONE)) {
        if (system_ms - start >= I2C_TIMEOUT_MS) {
            oled_i2c_sda_unlock();
            break;
        }
    }
}

/* 发一段命令序列（上电初始化、开关屏用，次数极少，逐字节发即可） */
static void oled_write_cmd(uint8_t dat)
{
    uint8_t buf[2] = {0x00, dat};
    oled_write_buf(buf, (uint16_t)sizeof(buf));
}

/* 兼容旧接口：立即写一字节。新代码应走缓冲 + OLED_Refresh */
void OLED_WR_Byte(uint8_t dat, uint8_t mode)
{
    uint8_t buf[2] = {(uint8_t)(mode ? 0x40 : 0x00), dat};
    oled_write_buf(buf, (uint16_t)sizeof(buf));
}

/* 把一页（控制字 + 128 字节数据）作为一次 transfer 发出 */
static void oled_write_page(uint8_t page)
{
    uint8_t chunk[OLED_PAGE_CHUNK];
    uint8_t i;

    chunk[0] = 0x40;
    for (i = 0; i < OLED_WIDTH; i++)
        chunk[1 + i] = frame_buf[page * OLED_WIDTH + i];

    oled_write_buf(chunk, OLED_PAGE_CHUNK);
}

/* 整帧推送：先回到页寻址模式 + (0,0)，再逐页批量写。
 * 8 次 transfer 代替原来的 1032 次，400kHz 下约 25ms */
void OLED_Refresh(void)
{
    uint8_t page;

    oled_write_cmd(0x20);   /* 页寻址模式 */
    oled_write_cmd(0x02);
    oled_write_cmd(0xB0);   /* page 0 */
    oled_write_cmd(0x00);   /* column low  */
    oled_write_cmd(0x10);   /* column high */

    for (page = 0; page < OLED_PAGES; page++) {
        oled_write_page(page);
    }
}

void OLED_ColorTurn(uint8_t i)
{
    if (i == 0)
        oled_write_cmd(0xA6);
    if (i == 1)
        oled_write_cmd(0xA7);
}

void OLED_DisplayTurn(uint8_t i)
{
    if (i == 0) {
        oled_write_cmd(0xC8);
        oled_write_cmd(0xA1);
    }
    if (i == 1) {
        oled_write_cmd(0xC0);
        oled_write_cmd(0xA0);
    }
}

void OLED_Display_On(void)
{
    oled_write_cmd(0x8D);
    oled_write_cmd(0x14);
    oled_write_cmd(0xAF);
}

void OLED_Display_Off(void)
{
    oled_write_cmd(0x8D);
    oled_write_cmd(0x10);
    oled_write_cmd(0xAE);
}

void OLED_Clear(void)
{
    memset(frame_buf, 0, sizeof(frame_buf));
}

void OLED_ShowChar(uint8_t x, uint8_t y, uint8_t chr, uint8_t sizey)
{
    const uint8_t *font;
    uint8_t c       = (uint8_t)(chr - ' ');
    uint8_t sizex   = (uint8_t)(sizey / 2);
    uint8_t n_pages, page, col;

    if (sizey == 8) {
        font    = asc2_0806[c];
        n_pages = 1;
    } else if (sizey == 16) {
        font    = asc2_1608[c];
        n_pages = 2;
    } else {
        return;
    }

    for (page = 0; page < n_pages; page++) {
        for (col = 0; col < sizex; col++) {
            frame_buf[(uint16_t)(y + page) * OLED_WIDTH + x + col] =
                font[page * sizex + col];
        }
    }
}

/* 10^n 查表：比循环累乘省一个函数调用和一串乘法 */
static uint32_t oled_pow(uint8_t m, uint8_t n)
{
    static const uint32_t pow10[] = {
        1u,        10u,        100u,       1000u,      10000u,
        100000u,   1000000u,   10000000u,  100000000u, 1000000000u
    };
    uint32_t result = 1;

    if (m == 10u && n < (uint8_t)(sizeof(pow10) / sizeof(pow10[0])))
        return pow10[n];

    while (n--)
        result *= m;

    return result;
}

void OLED_ShowNum(uint8_t x, uint8_t y, uint32_t num, uint8_t len, uint8_t sizey)
{
    uint8_t t, temp, m = 0;
    uint8_t enshow = 0;
    uint8_t x_step;

    if (sizey == 8) m = 2;
    x_step = (uint8_t)(sizey / 2 + m);

    for (t = 0; t < len; t++) {
        temp = (uint8_t)((num / oled_pow(10, (uint8_t)(len - t - 1))) % 10);
        if (enshow == 0 && t < (len - 1)) {
            if (temp == 0) {
                OLED_ShowChar((uint8_t)(x + x_step * t), y, ' ', sizey);
                continue;
            }
            enshow = 1;
        }
        OLED_ShowChar((uint8_t)(x + x_step * t), y, (uint8_t)(temp + '0'), sizey);
    }
}

void OLED_ShowString(uint8_t x, uint8_t y, const char *chr, uint8_t sizey)
{
    uint8_t j = 0;

    while (chr[j] != '\0') {
        OLED_ShowChar(x, y, (uint8_t)chr[j++], sizey);
        x = (uint8_t)(x + ((sizey == 8) ? 6 : sizey / 2));
    }
}

void OLED_ShowChinese(uint8_t x, uint8_t y, uint8_t no, uint8_t sizey)
{
    uint8_t i;

    if (sizey != 16)
        return;

    for (i = 0; i < 16; i++) {
        frame_buf[(uint16_t)y * OLED_WIDTH + x + i]       = Hzk[no][i];
        frame_buf[(uint16_t)(y + 1) * OLED_WIDTH + x + i] = Hzk[no][16 + i];
    }
}

void OLED_DrawBMP(uint8_t x, uint8_t y, uint8_t sizex, uint8_t sizey, const uint8_t BMP[])
{
    uint16_t j = 0;
    uint8_t i, m;

    sizey = (uint8_t)(sizey / 8 + ((sizey % 8) ? 1 : 0));
    for (i = 0; i < sizey; i++) {
        for (m = 0; m < sizex; m++) {
            frame_buf[(uint16_t)(i + y) * OLED_WIDTH + x + m] = BMP[j++];
        }
    }
}

void OLED_Init(void)
{
    if (DL_I2C_getSDAStatus(I2C_0_INST) == DL_I2C_CONTROLLER_SDA_LOW)
        oled_i2c_sda_unlock();

    delay_ms(200);

    oled_write_cmd(0xAE); /*--turn off oled panel*/
    oled_write_cmd(0x00); /*---set low column address*/
    oled_write_cmd(0x10); /*---set high column address*/
    oled_write_cmd(0x40); /*--set start line address*/
    oled_write_cmd(0x81); /*--set contrast control register*/
    oled_write_cmd(0xCF); /* Set SEG Output Current Brightness */
    oled_write_cmd(0xA1); /*--Set SEG/Column Mapping*/
    oled_write_cmd(0xC8); /*Set COM/Row Scan Direction*/
    oled_write_cmd(0xA6); /*--set normal display*/
    oled_write_cmd(0xA8); /*--set multiplex ratio(1 to 64)*/
    oled_write_cmd(0x3F); /*--1/64 duty*/
    oled_write_cmd(0xD3); /*-set display offset*/
    oled_write_cmd(0x00); /*-not offset*/
    oled_write_cmd(0xD5); /*--set display clock divide ratio/oscillator frequency*/
    oled_write_cmd(0x80); /*--set divide ratio*/
    oled_write_cmd(0xD9); /*--set pre-charge period*/
    oled_write_cmd(0xF1); /*Set Pre-Charge as 15 Clocks & Discharge as 1 Clock*/
    oled_write_cmd(0xDA); /*--set com pins hardware configuration*/
    oled_write_cmd(0x12);
    oled_write_cmd(0xDB); /*--set vcomh*/
    oled_write_cmd(0x40); /*Set VCOM Deselect Level*/
    oled_write_cmd(0x20); /*-Set Page Addressing Mode (0x00/0x01/0x02)*/
    oled_write_cmd(0x02);
    oled_write_cmd(0x8D); /*--set Charge Pump enable/disable*/
    oled_write_cmd(0x14); /*--set(0x10) disable*/
    oled_write_cmd(0xA4); /* Disable Entire Display On (0xA4/0xA5)*/
    oled_write_cmd(0xA6); /* Disable Inverse Display On (0xA6/A7)*/
    OLED_Clear();
    OLED_Refresh();
    oled_write_cmd(0xAF); /*display ON*/
}
