/**
 * @file    oled_hardware_i2c.c
 * @brief   SSD1306 OLED 驱动实现（硬件 I2C，含 SDA 卡死自恢复）
 * @note    I2C 传输带超时保护：超时后自动 bit-bang 解锁 SDA 并复位 I2C，
 *          依赖 system_ms（10ms 定时器中断累加）做超时基准
 */

#include "oled_hardware_i2c.h"
#include "oledfont.h"
#include "system/board.h"

/* ★必须 ≥ 20：system_ms 每 10ms 才跳一次、一跳 +10，写 10 的话传输途中
 * 碰上一次定时器中断就被判超时，白白复位 I2C（≥2ms）还打断正在写的字节 */
#define I2C_TIMEOUT_MS  (20)

extern volatile uint32_t system_ms;

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
    do
    {
        DL_GPIO_clearPins(GPIO_I2C_0_SCL_PORT, GPIO_I2C_0_SCL_PIN);
        delay_ms(1);
        DL_GPIO_setPins(GPIO_I2C_0_SCL_PORT, GPIO_I2C_0_SCL_PIN);
        delay_ms(1);

        if(DL_GPIO_readPins(GPIO_I2C_0_SDA_PORT, GPIO_I2C_0_SDA_PIN))
            break;
    }while(++cycleCnt < 100);
    mspm0_i2c_enable();
}

void OLED_ColorTurn(uint8_t i)
{
    if(i==0)
        OLED_WR_Byte(0xA6, OLED_CMD);
    if(i==1)
        OLED_WR_Byte(0xA7, OLED_CMD);
}

void OLED_DisplayTurn(uint8_t i)
{
    if(i==0)
    {
        OLED_WR_Byte(0xC8, OLED_CMD);
        OLED_WR_Byte(0xA1, OLED_CMD);
    }
    if(i==1)
    {
        OLED_WR_Byte(0xC0, OLED_CMD);
        OLED_WR_Byte(0xA0, OLED_CMD);
    }
}

/* ── 一次 I2C 事务 ──
 * SSD1306 在页地址模式下每收一个数据字节列地址就自动 +1，所以同一页里连续的几个字节
 * 能并进一次事务发。原来一个字节一趟 I2C：一趟 = 起始 + 地址 + ACK + 2 字节 + 停止 ≈ 29 bit
 * ≈ 73us（400kHz），其中 20 bit 是纯粹的趟次开销。8x16 的一个字原来 22 趟 ≈ 1.6ms，
 * 并起来以后是 4 趟数据 + 2 趟定位 ≈ 0.8ms —— 再往下就只剩位时间了，省不掉。
 *
 * ★页边界硬件不会替你跨：调用方要自己保证一段数据不跨页（列地址不用管，硬件会 +1）。
 */
#define OLED_I2C_ADDR   0x3C
#define OLED_TX_CHUNK   8    // 一次事务最多准备这么多字节（控制字节 + 数据）

/* 把 tx[0..tx_len) 交给一次 I2C 事务，返回真正进了 FIFO 的字节数。
 * 装多少发多少：MSPM0 各型号的 TX FIFO 深度不一样（4 或 8），拿返回的字节数当传输长度，
 * 就与 FIFO 深度无关，也不会 underrun */
static uint16_t OLED_I2C_Tx(uint8_t *tx, uint16_t tx_len)
{
    uint32_t start = system_ms;
    uint32_t idle_timeout = 0;
    uint16_t queued;

    while (!(DL_I2C_getControllerStatus(I2C_0_INST) & DL_I2C_CONTROLLER_STATUS_IDLE)) {
        if (++idle_timeout > 100000) break;   // 防死等：控制器一直不空闲则放弃
    }

    queued = DL_I2C_fillControllerTXFIFO(I2C_0_INST, tx, (uint8_t)tx_len);
    if (queued == 0)
        return 0;

    DL_I2C_clearInterruptStatus(I2C_0_INST, DL_I2C_INTERRUPT_CONTROLLER_TX_DONE);
    DL_I2C_startControllerTransfer(I2C_0_INST, OLED_I2C_ADDR,
                                   DL_I2C_CONTROLLER_DIRECTION_TX, queued);

    while (!DL_I2C_getRawInterruptStatus(I2C_0_INST, DL_I2C_INTERRUPT_CONTROLLER_TX_DONE))
    {
        if (system_ms - start >= I2C_TIMEOUT_MS)
        {
            oled_i2c_sda_unlock();
            break;
        }
    }

    return queued;
}

/* 连续写 len 个字节（同一页内）。返回值截断了就接着发剩下那部分 */
static void OLED_Write_Buf(const uint8_t *data, uint16_t len, uint8_t mode)
{
    uint8_t  tx[OLED_TX_CHUNK];
    uint16_t n, queued;

    tx[0] = (mode == OLED_DATA) ? 0x40 : 0x00;   // 控制字节：0x40 数据 / 0x00 命令

    while (len != 0) {
        n = (len < OLED_TX_CHUNK - 1) ? len : (OLED_TX_CHUNK - 1);
        for (uint16_t i = 0; i < n; i++)
            tx[i + 1] = data[i];

        queued = OLED_I2C_Tx(tx, (uint16_t)(n + 1));
        if (queued <= 1)
            return;                              // 一个字节都没发出去（I2C 卡了），别再空转

        n = (uint16_t)(queued - 1);              // 扣掉控制字节
        data += n;
        len  -= n;
    }
}

/* 同一个字节重复写 count 次（清屏用，免得为 128 个 0 单独准备一段缓冲） */
static void OLED_Write_Fill(uint8_t value, uint16_t count, uint8_t mode)
{
    uint8_t  tx[OLED_TX_CHUNK];
    uint16_t n, queued;

    tx[0] = (mode == OLED_DATA) ? 0x40 : 0x00;

    while (count != 0) {
        n = (count < OLED_TX_CHUNK - 1) ? count : (OLED_TX_CHUNK - 1);
        for (uint16_t i = 0; i < n; i++)
            tx[i + 1] = value;

        queued = OLED_I2C_Tx(tx, (uint16_t)(n + 1));
        if (queued <= 1)
            return;

        count -= (uint16_t)(queued - 1);
    }
}

void OLED_WR_Byte(uint8_t dat, uint8_t mode)
{
    OLED_Write_Buf(&dat, 1, mode);
}

void OLED_Set_Pos(uint8_t x, uint8_t y)
{
    /* 页地址 + 列地址高 4 位 + 列地址低 4 位，3 个命令字节并成一次事务 */
    uint8_t cmd[3];

    cmd[0] = (uint8_t)(0xB0 + y);
    cmd[1] = (uint8_t)(((x & 0xF0) >> 4) | 0x10);
    cmd[2] = (uint8_t)(x & 0x0F);

    OLED_Write_Buf(cmd, sizeof(cmd), OLED_CMD);
}

void OLED_Display_On(void)
{
    OLED_WR_Byte(0X8D, OLED_CMD);
    OLED_WR_Byte(0X14, OLED_CMD);
    OLED_WR_Byte(0XAF, OLED_CMD);
}

void OLED_Display_Off(void)
{
    OLED_WR_Byte(0X8D, OLED_CMD);
    OLED_WR_Byte(0X10, OLED_CMD);
    OLED_WR_Byte(0XAE, OLED_CMD);
}

void OLED_Clear(void)
{
    uint8_t page;

    for(page=0;page<8;page++)
    {
        OLED_Set_Pos(0, page);
        OLED_Write_Fill(0, 128, OLED_DATA);
    }
}

/* 字模按列排：8x8 是 6 个字节（1 页 × 6 列）；8x16 是 16 个字节，
 * 前 8 个在上半页（列 x..x+7）、后 8 个在下半页，正好一段一页 */
void OLED_ShowChar(uint8_t x, uint8_t y, uint8_t chr, uint8_t sizey)
{
    uint8_t c = (uint8_t)(chr - ' ');

    if(sizey==8)
    {
        OLED_Set_Pos(x, y);
        OLED_Write_Buf(&asc2_0806[c][0], 6, OLED_DATA);
    }
    else if(sizey==16)
    {
        OLED_Set_Pos(x, y);
        OLED_Write_Buf(&asc2_1608[c][0], 8, OLED_DATA);
        OLED_Set_Pos(x, (uint8_t)(y + 1));
        OLED_Write_Buf(&asc2_1608[c][8], 8, OLED_DATA);
    }
}

uint32_t oled_pow(uint8_t m, uint8_t n)
{
    uint32_t result=1;
    while(n--) result*=m;
    return result;
}

void OLED_ShowNum(uint8_t x, uint8_t y, uint32_t num, uint8_t len, uint8_t sizey)
{
    uint8_t t,temp,m=0;
    uint8_t enshow=0;
    if(sizey==8) m=2;
    for(t=0;t<len;t++)
    {
        temp=(num/oled_pow(10,len-t-1))%10;
        if(enshow==0 && t<(len-1))
        {
            if(temp==0)
            {
                OLED_ShowChar(x+(sizey/2+m)*t,y,' ',sizey);
                continue;
            }else enshow=1;
        }
        OLED_ShowChar(x+(sizey/2+m)*t,y,temp+'0',sizey);
    }
}

void OLED_ShowString(uint8_t x, uint8_t y, const uint8_t *chr, uint8_t sizey)
{
    uint8_t j=0;
    while (chr[j]!='\0')
    {
        OLED_ShowChar(x,y,chr[j++],sizey);
        if(sizey==8) x+=6;
        else x+=sizey/2;
    }
}

/* 字模 16 列 × 2 页 = 32 个字节，一段一页 */
void OLED_ShowChinese(uint8_t x, uint8_t y, uint8_t no, uint8_t sizey)
{
    if(sizey!=16) return;

    OLED_Set_Pos(x, y);
    OLED_Write_Buf(&Hzk[no][0], 16, OLED_DATA);
    OLED_Set_Pos(x, (uint8_t)(y + 1));
    OLED_Write_Buf(&Hzk[no][16], 16, OLED_DATA);
}

/* sizey 向上取整到页数，每页连续 sizex 列 */
void OLED_DrawBMP(uint8_t x, uint8_t y, uint8_t sizex, uint8_t sizey, const uint8_t *BMP)
{
    uint8_t  pages = (uint8_t)(sizey/8 + ((sizey%8)?1:0));
    uint16_t j = 0;
    uint8_t  i;

    for(i=0;i<pages;i++)
    {
        OLED_Set_Pos(x, (uint8_t)(i+y));
        OLED_Write_Buf(&BMP[j], sizex, OLED_DATA);
        j += sizex;
    }
}

void OLED_Init(void)
{
    if(DL_I2C_getSDAStatus(I2C_0_INST) == DL_I2C_CONTROLLER_SDA_LOW)
        oled_i2c_sda_unlock();

    delay_ms(200);

    OLED_WR_Byte(0xAE, OLED_CMD);//--turn off oled panel
    OLED_WR_Byte(0x00, OLED_CMD);//---set low column address
    OLED_WR_Byte(0x10, OLED_CMD);//---set high column address
    OLED_WR_Byte(0x40, OLED_CMD);//--set start line address
    OLED_WR_Byte(0x81, OLED_CMD);//--set contrast control register
    OLED_WR_Byte(0xCF, OLED_CMD); // Set SEG Output Current Brightness
    OLED_WR_Byte(0xA1, OLED_CMD);//--Set SEG/Column Mapping
    OLED_WR_Byte(0xC8, OLED_CMD);//Set COM/Row Scan Direction
    OLED_WR_Byte(0xA6, OLED_CMD);//--set normal display
    OLED_WR_Byte(0xA8, OLED_CMD);//--set multiplex ratio(1 to 64)
    OLED_WR_Byte(0x3f, OLED_CMD);//--1/64 duty
    OLED_WR_Byte(0xD3, OLED_CMD);//-set display offset
    OLED_WR_Byte(0x00, OLED_CMD);//-not offset
    OLED_WR_Byte(0xd5, OLED_CMD);//--set display clock divide ratio/oscillator frequency
    OLED_WR_Byte(0x80, OLED_CMD);//--set divide ratio
    OLED_WR_Byte(0xD9, OLED_CMD);//--set pre-charge period
    OLED_WR_Byte(0xF1, OLED_CMD);//Set Pre-Charge as 15 Clocks & Discharge as 1 Clock
    OLED_WR_Byte(0xDA, OLED_CMD);//--set com pins hardware configuration
    OLED_WR_Byte(0x12, OLED_CMD);
    OLED_WR_Byte(0xDB, OLED_CMD);//--set vcomh
    OLED_WR_Byte(0x40, OLED_CMD);//Set VCOM Deselect Level
    OLED_WR_Byte(0x20, OLED_CMD);//-Set Page Addressing Mode (0x00/0x01/0x02)
    OLED_WR_Byte(0x02, OLED_CMD);//
    OLED_WR_Byte(0x8D, OLED_CMD);//--set Charge Pump enable/disable
    OLED_WR_Byte(0x14, OLED_CMD);//--set(0x10) disable
    OLED_WR_Byte(0xA4, OLED_CMD);// Disable Entire Display On (0xa4/0xa5)
    OLED_WR_Byte(0xA6, OLED_CMD);// Disable Inverse Display On (0xa6/a7)
    OLED_Clear();
    OLED_WR_Byte(0xAF, OLED_CMD); /*display ON*/
}
