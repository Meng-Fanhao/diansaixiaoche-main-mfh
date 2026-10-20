#include "board.h"
#include <stdio.h>
#include <string.h>

#define RE_0_BUFF_LEN_MAX   128

volatile uint8_t  recv0_buff[RE_0_BUFF_LEN_MAX] = {0};
volatile uint16_t recv0_length = 0;
volatile uint8_t  recv0_flag = 0;

void delay_us(unsigned long __us)
{
    uint32_t ticks;
    uint32_t told, tnow, tcnt = 38;

    ticks = __us * 32;    // 32MHz 下 1μs = 32 tick
    told = SysTick->VAL;

    while (1)
    {
        tnow = SysTick->VAL;

        if (tnow != told)
        {
            if (tnow < told)
                tcnt += told - tnow;
            else
                tcnt += SysTick->LOAD - tnow + told;   // 重载，跨了一圈

            told = tnow;

            if (tcnt >= ticks)
                break;
        }
    }
}

void delay_ms(unsigned long ms)
{
    delay_us(ms * 1000);
}

void uart0_send_char(char ch)
{
    while (DL_UART_isBusy(UART_0_INST) == true);
    DL_UART_Main_transmitData(UART_0_INST, ch);
}

void uart0_send_string(const char *str)
{
    if (str == NULL) return;
    while (*str != '\0')
    {
        uart0_send_char(*str++);
    }
}

/* 非阻塞发送：环形缓冲 + TX 中断。缓冲满就整行丢掉，绝不在发送上等 ——
 * 遥测是诊断输出，不能反过来阻塞 10ms 控制周期 */
#define UART0_TX_BUFF_LEN   256u
#define UART0_TX_BUFF_MASK  (UART0_TX_BUFF_LEN - 1u)   /* 长度必须是 2 的幂 */

static volatile uint8_t  tx_buff[UART0_TX_BUFF_LEN];
static volatile uint16_t tx_head = 0;
static volatile uint16_t tx_tail = 0;
static volatile uint8_t  tx_running = 0;

static void uart0_tx_kick(void)
{
    if (tx_running || tx_tail == tx_head)
        return;

    tx_running = 1;
    DL_UART_Main_transmitData(UART_0_INST, tx_buff[tx_tail]);
    tx_tail = (uint16_t)((tx_tail + 1u) & UART0_TX_BUFF_MASK);
    DL_UART_enableInterrupt(UART_0_INST, DL_UART_INTERRUPT_TX);
}

void uart0_send_string_nb(const char *str)
{
    size_t len;
    uint16_t head, free_cnt, i;

    if (str == NULL)
        return;

    len = strlen(str);
    if (len == 0u || len >= UART0_TX_BUFF_LEN)
        return;

    __disable_irq();

    head = tx_head;
    free_cnt = (uint16_t)((uint16_t)(tx_tail - head - 1) & UART0_TX_BUFF_MASK);
    if (len > free_cnt)
    {
        __enable_irq();
        return;
    }

    for (i = 0; i < len; i++)
    {
        tx_buff[head] = (uint8_t)str[i];
        head = (uint16_t)((head + 1u) & UART0_TX_BUFF_MASK);
    }
    tx_head = head;

    uart0_tx_kick();

    __enable_irq();
}

#if !defined(__MICROLIB)
#if (__ARMCLIB_VERSION <= 6000000)
struct __FILE
{
    int handle;
};
#endif

FILE __stdout;

void _sys_exit(int x)
{
    (void)x;
}
#endif

int fputc(int ch, FILE *stream)
{
    (void)stream;
    DL_UART_Main_transmitDataBlocking(UART_0_INST, (uint8_t)ch);
    return ch;
}

int fputs(const char *restrict s, FILE *restrict stream)
{
    int count = 0;
    (void)stream;
    while (*s) {
        DL_UART_Main_transmitDataBlocking(UART_0_INST, (uint8_t)*s++);
        count++;
    }
    return count;
}

int puts(const char *_ptr)
{
    int count = fputs(_ptr, stdout);
    count += fputs("\n", stdout);
    return count;
}

void UART_0_INST_IRQHandler(void)
{
    uint8_t receivedData;

    switch (DL_UART_getPendingInterrupt(UART_0_INST))
    {
        case DL_UART_IIDX_RX:

            receivedData = DL_UART_Main_receiveData(UART_0_INST);

            if (recv0_length < RE_0_BUFF_LEN_MAX - 1)
            {
                recv0_buff[recv0_length++] = receivedData;
            }
            else
            {
                recv0_length = 0;
            }
            recv0_flag = 1;
            break;

        case DL_UART_IIDX_TX:

            if (tx_tail != tx_head)
            {
                DL_UART_Main_transmitData(UART_0_INST, tx_buff[tx_tail]);
                tx_tail = (uint16_t)((tx_tail + 1u) & UART0_TX_BUFF_MASK);
            }
            else
            {
                /* 必须等"真的空了"这一拍才关中断：写完最后一个字节就关，
                 * 下次 kick 会误判发送器空闲、覆写保持寄存器，字节就丢了 */
                DL_UART_disableInterrupt(UART_0_INST, DL_UART_INTERRUPT_TX);
                tx_running = 0;
            }
            break;

        default:
            break;
    }
}
