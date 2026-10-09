/**
 * @file modbus.h
 * @brief Modbus RTU communication interface.
 *
 * This module provides low-level RS-485 GPIO control, UART communication,
 * and Modbus RTU frame handling functions. It is responsible for managing
 * transmission direction, serial communication, CRC calculation, and
 * Modbus transactions.
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
    /** Holding Register (Function Code 03). */
    REG_HOLDING,

    /** Input Register (Function Code 04). */
    REG_INPUT,

    /** Coil Status (Function Code 01). */
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
#define RS485_TX_GUARD_US 3500

/**
 * @brief Maximum time to wait for a slave response (milliseconds).
 */
#define RX_TIMEOUT_MS     1000

/**
 * @brief Maximum retry attempts for each Modbus request.
 */
#define MAX_RETRIES       3

/**
 * @brief Delay between consecutive Modbus requests (microseconds).
 */
#define POINT_DELAY_US    15000

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
 * @brief Returns the expected response length for a function code.
 *
 * @param fc Modbus function code.
 *
 * @return Expected response length in bytes.
 */
int mb_reply_len(uint8_t fc);

/**
 * @brief Executes a Modbus RTU transaction.
 *
 * Builds a Modbus request frame, transmits it over RS-485, waits for
 * the slave response, validates the CRC, and extracts the returned value.
 *
 * @param slave Modbus slave address.
 * @param fc Modbus function code.
 * @param addr Register or coil address.
 * @param value Pointer to store the received value.
 *
 * @return
 * - 1 if the transaction succeeds.
 * - 0 if the transaction fails.
 */
int mb_transaction(uint8_t slave,
                   uint8_t fc,
                   uint16_t addr,
                   uint16_t *value);

/**
 * @brief Executes a Modbus RTU write-single transaction.
 *
 * Builds and sends a Modbus write request for FC05 (Write Single Coil)
 * or FC06 (Write Single Register). The slave echoes the request frame
 * on success.
 *
 * @param slave Modbus slave address.
 * @param fc    Function code (0x05 or 0x06).
 * @param addr  Register or coil address.
 * @param value Value to write (0xFF00/0x0000 for coils, raw for registers).
 *
 * @return 1 on success, 0 on failure.
 */
int mb_write_single(uint8_t slave,
                    uint8_t fc,
                    uint16_t addr,
                    uint16_t value);

/**
 * @brief Executes a Modbus RTU write-multiple transaction.
 *
 * Builds and sends a Modbus write request for FC15 (Write Multiple Coils)
 * or FC16 (Write Multiple Registers).
 *
 * @param slave  Modbus slave address.
 * @param fc     Function code (0x0F or 0x10).
 * @param addr   Starting address.
 * @param count  Number of registers or coils to write.
 * @param values Array of values to write.
 *
 * @return 1 on success, 0 on failure.
 */
int mb_write_multiple(uint8_t slave,
                      uint8_t fc,
                      uint16_t addr,
                      uint16_t count,
                      const uint16_t *values);

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_H */