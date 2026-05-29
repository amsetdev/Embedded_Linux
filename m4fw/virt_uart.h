/*
 * Stub — our firmware uses a raw rpmsg_endpoint directly and does not
 * use the VirtUart abstraction layer.  This empty header satisfies the
 * #include in openamp_conf.h from the STM32CubeMP1 example.
 */
#ifndef VIRT_UART_H
#define VIRT_UART_H

#include <openamp/open_amp.h>

typedef enum {
    VIRT_UART_OK    = 0,
    VIRT_UART_ERROR = 1,
} VIRT_UART_StatusTypeDef;

typedef struct {
    struct rpmsg_endpoint ept;
    uint8_t               *pRxBuffPtr;
    uint16_t               RxXferSize;
    volatile uint16_t      RxXferCount;
    void (*RxCpltCallback)(struct __VIRT_UART_HandleTypeDef *huart);
} VIRT_UART_HandleTypeDef;

/* No-op stubs — never called by our code */
static inline VIRT_UART_StatusTypeDef
VIRT_UART_Init(VIRT_UART_HandleTypeDef *h) { (void)h; return VIRT_UART_OK; }

static inline VIRT_UART_StatusTypeDef
VIRT_UART_Transmit(VIRT_UART_HandleTypeDef *h, uint8_t *buf, uint16_t len)
{ (void)h; (void)buf; (void)len; return VIRT_UART_OK; }

#endif /* VIRT_UART_H */
