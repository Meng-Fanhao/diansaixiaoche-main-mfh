#include "ti_msp_dl_config.h"
#include "board.h"
#include "uart_callback.h"

/* UART1 接收缓存：电机反馈帧固定为 9 字节。 */
u8 usart1_receive_data[10];

/* UART1 发送 1 字节，等待发送 FIFO 有空位后写入。 */
void usart1_send(u8 data)
{
    while (DL_UART_isTXFIFOFull(UART_1_INST) == true);
    DL_UART_Main_transmitData(UART_1_INST, data);
}

/* UART1 发送数组，保持参考例程 USARTx_SEND 的调用风格。 */
void USART1_SEND(u8 *data, u8 len)
{
    u8 i = 0;

    for (i = 0; i < len; i++)
    {
        usart1_send(data[i]);
    }
}

/* 兼容旧工程里已有的 uart1_send_data 名称，实际仍调用参考例程风格接口。 */
void uart1_send_data(uint8_t data)
{
    usart1_send(data);
}

/* 兼容旧工程里已有的 uart1_send_SendArray 名称。 */
void uart1_send_SendArray(uint8_t *data, uint8_t len)
{
    USART1_SEND(data, len);
}

/* BCC 异或校验：从帧头开始，对指定字节数逐字节异或。 */
u8 BCC_Sum1(u8 *usart_data, unsigned char Count_Number)
{
    unsigned char crc_sum = 0, k;

    for (k = 0; k < Count_Number; k++)
    {
        crc_sum = crc_sum ^ usart_data[k];
    }

    return crc_sum;
}

/* UART1 接收中断：逐字节拼接 9 字节反馈帧，校验通过后更新实时位置。 */
void UART_1_INST_IRQHandler(void)
{
    u8 Usart1_Receive;
    static u8 Count = 0;

    if (DL_UART_Main_getPendingInterrupt(UART_1_INST) == DL_UART_MAIN_IIDX_RX)
    {
        Usart1_Receive = DL_UART_Main_receiveData(UART_1_INST);

        /* 等待帧头 0x7A，避免错误字节导致接收错位。 */
        if (Count == 0 && Usart1_Receive != 0x7A)
        {
            return;
        }

        usart1_receive_data[Count++] = Usart1_Receive;

        if (Count >= 9)
        {
            Count = 0;

            /* 帧尾必须为 0x7B。 */
            if (usart1_receive_data[8] != 0x7B)
            {
                return;
            }

            /* 第 8 字节为前 7 字节 BCC 校验。 */
            if (usart1_receive_data[7] == BCC_Sum1(usart1_receive_data, 7))
            {
                /* 地址 0x01，反馈类型 0x01：1 号电机实时位置。 */
                if (usart1_receive_data[1] == 0x01)
                {
                    /* 0x01 = 多圈总角度反馈，0x02 = 机械角度（单圈绝对）反馈，都按位置处理。 */
                    if (usart1_receive_data[2] == 0x01 || usart1_receive_data[2] == 0x02)
                    {
                        Motor1_Current_Position = (usart1_receive_data[3] << 24) +
                                                  (usart1_receive_data[4] << 16) +
                                                  (usart1_receive_data[5] << 8) +
                                                   usart1_receive_data[6];
                    }
                    else if (usart1_receive_data[2] == 0x00)
                    {
                        motor1_Current_Speed = (usart1_receive_data[3] << 24) +
                                               (usart1_receive_data[4] << 16) +
                                               (usart1_receive_data[5] << 8) +
                                                usart1_receive_data[6];
                    }
                }

                /* 地址 0x02，反馈类型 0x01：2 号电机实时位置。 */
                if (usart1_receive_data[1] == 0x02)
                {
                    /* 0x01 = 多圈总角度反馈，0x02 = 机械角度（单圈绝对）反馈，都按位置处理。 */
                    if (usart1_receive_data[2] == 0x01 || usart1_receive_data[2] == 0x02)
                    {
                        Motor2_Current_Position = (usart1_receive_data[3] << 24) +
                                                  (usart1_receive_data[4] << 16) +
                                                  (usart1_receive_data[5] << 8) +
                                                   usart1_receive_data[6];
                    }
                    else if (usart1_receive_data[2] == 0x00)
                    {
                        motor2_Current_Speed = (usart1_receive_data[3] << 24) +
                                               (usart1_receive_data[4] << 16) +
                                               (usart1_receive_data[5] << 8) +
                                                usart1_receive_data[6];
                    }
                }
            }
        }
    }
}