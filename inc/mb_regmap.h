/**
 * @file mb_regmap.h
 * @brief Shared Modbus data model for the RTU + TCP slave.
 *
 * A single in-memory register map is exposed to BOTH the Modbus RTU
 * slave (RS485) and the Modbus TCP slave. A write performed by a
 * master over one interface is immediately visible to a master
 * reading over the other interface, since both act on the same
 * backing arrays protected by one mutex.
 *
 * Values start out demo/simulated (a slowly ticking counter, a sine
 * wave, alternating bits, ...) so the slave can be exercised with
 * any standard Modbus master/test tool without extra wiring. Replace
 * mb_regmap_tick() with real sensor/IO updates when ready.
 */

#ifndef MB_REGMAP_H
#define MB_REGMAP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* -------------------------------------------------------------------------- */
/* Sizing                                                                     */
/* -------------------------------------------------------------------------- */

#define MB_NUM_COILS            64   /**< FC01/05/15 - read/write coils          */
#define MB_NUM_DISCRETE_INPUTS  64   /**< FC02       - read-only discrete inputs */
#define MB_NUM_HOLDING_REGS     128  /**< FC03/06/16 - read/write holding regs   */
#define MB_NUM_INPUT_REGS       128  /**< FC04       - read-only input regs      */

/* -------------------------------------------------------------------------- */
/* Lifecycle                                                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief Initializes the shared register map with demo values and
 *        creates its protecting mutex. Call once at startup before
 *        starting the RTU/TCP slave threads.
 */
void mb_regmap_init(void);

/**
 * @brief Advances the built-in demo/simulated data by one step.
 *
 * Call periodically (e.g. once per second) from a background thread.
 * Only touches registers a master hasn't just written, so writes
 * made by an external master remain visible/stable.
 */
void mb_regmap_tick(void);

/* -------------------------------------------------------------------------- */
/* Thread-safe accessors used by the RTU and TCP slave engines               */
/* -------------------------------------------------------------------------- */

void    mb_regmap_lock(void);
void    mb_regmap_unlock(void);

uint8_t  mb_regmap_get_coil(int addr);
void     mb_regmap_set_coil(int addr, uint8_t value);

uint8_t  mb_regmap_get_discrete(int addr);

uint16_t mb_regmap_get_holding(int addr);
void     mb_regmap_set_holding(int addr, uint16_t value);

uint16_t mb_regmap_get_input(int addr);

/**
 * @brief Direct pointers to the backing arrays.
 *
 * Exposed so the libmodbus-based TCP slave can build a
 * modbus_mapping_t that reads/writes these arrays directly.
 * Callers must hold the regmap lock while touching them from
 * outside mb_regmap.c.
 */
uint8_t  *mb_regmap_coils_ptr(void);
uint8_t  *mb_regmap_discrete_ptr(void);
uint16_t *mb_regmap_holding_ptr(void);
uint16_t *mb_regmap_input_ptr(void);

#ifdef __cplusplus
}
#endif

#endif /* MB_REGMAP_H */
