/**
 * @file modbus.h
 * @brief Modbus RTU SLAVE communication interface.
 *
 * This module provides low-level RS-485 GPIO control, UART communication,
 * and Modbus RTU frame handling for operation as a Modbus RTU SLAVE
 * (server). It listens on RS-485 for requests addressed to its own
 * slave ID and replies from the shared register map (mb_regmap.h).
 *
 * The physical layer (RS-485 direction GPIO + UART open/close/read)
 * is unchanged from the master version; only the transaction logic
 * is inverted (listen-and-reply instead of request-and-wait).
 */

#ifndef MODBUS_H
#define MODBUS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* -------------------------------------------------------------------------- */
/* Data Types                                                                 */
/* -------------------------------------------------------------------------- */

/**
 * @brief Supported Modbus register types.
 */
typedef enum
{
    /** Holding Register (Function Code 03/06/16). */
    REG_HOLDING,

    /** Input Register (Function Code 04). */
    REG_INPUT,

    /** Coil Status (Function Code 01/05/15). */
    REG_COIL,

    /** Discrete Input (Function Code 02). */
    REG_DISCRETE

} RegType;

/* -------------------------------------------------------------------------- */
/* Timing Configuration                                                       */
/* -------------------------------------------------------------------------- */

/**
 * @brief Delay before transmitting after enabling the RS-485 driver.
 */
#define RS485_PRE_TX_US   200

/**
 * @brief Guard time after transmission before returning to receive mode.
 */
#define RS485_TX_GUARD_US 1100

/**
 * @brief Maximum time to wait for the START of a new request while idle
 *        (milliseconds). The slave blocks here between requests.
 */
#define RX_IDLE_TIMEOUT_MS   1000

/**
 * @brief Maximum time to wait for the remainder of a frame once the
 *        first byte has arrived (milliseconds). Used to detect the
 *        Modbus RTU inter-frame silence / end of frame.
 */
#define RX_FRAME_TIMEOUT_MS  50

/**
 * @brief Maximum number of retry attempts (kept for API compatibility;
 *        unused by the slave, which does not retry).
 */
#define MAX_RETRIES       3

/**
 * @brief Delay between consecutive Modbus requests (microseconds).
 *        Kept for API compatibility with the rest of the application.
 */
#define POINT_DELAY_US    15000

/**
 * @brief Maximum size of a Modbus RTU ADU this slave will buffer.
 */
#define MB_RTU_MAX_ADU    256

/* -------------------------------------------------------------------------- */
/* Global Variables                                                           */
/* -------------------------------------------------------------------------- */

/**
 * @brief UART file descriptor.
 *
 * The descriptor is opened by uart_open() and can be used by other
 * modules to verify UART availability.
 */
extern int uart_fd;

/* -------------------------------------------------------------------------- */
/* RS-485 Driver Control                                                      */
/* -------------------------------------------------------------------------- */

/**
 * @brief Initializes the RS-485 Driver Enable (DE) GPIO.
 *
 * Configures the GPIO used to switch between transmit and receive modes.
 *
 * @return
 * - 0 on success.
 * - -1 on failure.
 */
int rs485_gpio_init(void);

/**
 * @brief Enables RS-485 transmit mode.
 *
 * Drives the DE pin HIGH and waits for the configured settling time.
 */
void rs485_tx(void);

/**
 * @brief Enables RS-485 receive mode.
 *
 * Drives the DE pin LOW.
 */
void rs485_rx(void);

/**
 * @brief Releases GPIO resources used for RS-485 control.
 */
void rs485_gpio_close(void);

/* -------------------------------------------------------------------------- */
/* UART Functions                                                             */
/* -------------------------------------------------------------------------- */

/**
 * @brief Opens and configures the UART port.
 *
 * @param port UART device path.
 * @param baud Communication baud rate.
 *
 * @return
 * - 0 on success.
 * - -1 on failure.
 */
int uart_open(const char *port, int baud);

/**
 * @brief Closes the UART port.
 */
void uart_close(void);

/**
 * @brief Waits until all transmit data has been sent.
 *
 * Also applies the configured transmit guard delay.
 */
void uart_drain_tx(void);

/**
 * @brief Clears any pending received UART data.
 */
void uart_flush_rx(void);

/**
 * @brief Reads data from the UART with a timeout.
 *
 * @param buf Destination buffer.
 * @param want Number of bytes to read.
 * @param timeout_ms Timeout in milliseconds.
 *
 * @return Number of bytes actually received.
 */
int uart_read_timeout(uint8_t *buf, int want, int timeout_ms);

/* -------------------------------------------------------------------------- */
/* Modbus RTU Functions                                                       */
/* -------------------------------------------------------------------------- */

/**
 * @brief Calculates the Modbus RTU CRC-16.
 *
 * @param buf Pointer to the data buffer.
 * @param len Number of bytes.
 *
 * @return Computed CRC-16 value.
 */
uint16_t mb_crc16(const uint8_t *buf, int len);

/**
 * @brief Runs the Modbus RTU slave loop: waits for a request on the
 *        UART addressed to @p slave_id (or the broadcast address 0),
 *        decodes it, serves it from the shared register map, and
 *        transmits the reply. Runs until *running becomes 0.
 *
 * @param slave_id  This device's Modbus RTU slave address (1-247).
 * @param running   Pointer to the application's run flag; the loop
 *                  exits promptly once *running == 0.
 */
void mb_rtu_slave_run(uint8_t slave_id, volatile int *running);

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_H */
