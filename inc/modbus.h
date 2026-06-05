#ifndef MODBUS_H
#define MODBUS_H

#include <stdint.h>

/* ---- Register type enum ------------------------------------------------ */
typedef enum { REG_HOLDING, REG_INPUT, REG_COIL, REG_DISCRETE } RegType;

/* ---- Timing constants -------------------------------------------------- */
#define RS485_PRE_TX_US   200    /* DE HIGH → first bit settling time      */
#define RS485_TX_GUARD_US 1100   /* ~1 byte time at 9600 baud              */
#define RX_TIMEOUT_MS     1000   /* max wait for slave reply               */
#define MAX_RETRIES       3      /* per-point retry count                  */
#define POINT_DELAY_US    15000  /* inter-point gap                        */

/* ---- GPIO / UART state ------------------------------------------------- */
extern int uart_fd;   /* raw serial fd — public so modbus thread can check */

/* ---- RS485 DE GPIO ---------------------------------------------------- */
int  rs485_gpio_init(void);
void rs485_tx(void);
void rs485_rx(void);
void rs485_gpio_close(void);

/* ---- Raw UART ---------------------------------------------------------- */
int  uart_open(const char *port, int baud);
void uart_close(void);
void uart_drain_tx(void);
void uart_flush_rx(void);
int  uart_read_timeout(uint8_t *buf, int want, int timeout_ms);

/* ---- Modbus RTU frame layer ------------------------------------------- */
uint16_t mb_crc16(const uint8_t *buf, int len);
int      mb_reply_len(uint8_t fc);
int      mb_transaction(uint8_t slave, uint8_t fc,
                        uint16_t addr, uint16_t *value);

#endif /* MODBUS_H */
