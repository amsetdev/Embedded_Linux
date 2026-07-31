/**
 * @file mb_regmap.c
 * @brief Shared Modbus data model implementation.
 *
 * See mb_regmap.h. This is the single source of truth for coil,
 * discrete-input, holding-register, and input-register values that
 * BOTH the RTU slave (modbus.c) and TCP slave (mb_tcp.c) serve to
 * external masters.
 */

#include "mb_regmap.h"

#include <string.h>
#include <math.h>
#include <pthread.h>

static uint8_t  coils[MB_NUM_COILS];
static uint8_t  discrete_inputs[MB_NUM_DISCRETE_INPUTS];
static uint16_t holding_regs[MB_NUM_HOLDING_REGS];
static uint16_t input_regs[MB_NUM_INPUT_REGS];

static pthread_mutex_t regmap_mutex = PTHREAD_MUTEX_INITIALIZER;

/** @brief Simulation step counter, advanced by mb_regmap_tick(). */
static unsigned long sim_step = 0;

void mb_regmap_lock(void)   { pthread_mutex_lock(&regmap_mutex); }
void mb_regmap_unlock(void) { pthread_mutex_unlock(&regmap_mutex); }

void mb_regmap_init(void)
{
    pthread_mutex_lock(&regmap_mutex);

    memset(coils, 0, sizeof(coils));
    memset(discrete_inputs, 0, sizeof(discrete_inputs));
    memset(holding_regs, 0, sizeof(holding_regs));
    memset(input_regs, 0, sizeof(input_regs));

    /* Seed demo/simulated starting values so a master reading
     * immediately after startup already sees non-zero data. */
    for (int i = 0; i < MB_NUM_HOLDING_REGS; i++)
        holding_regs[i] = (uint16_t)(i * 10);

    for (int i = 0; i < MB_NUM_INPUT_REGS; i++)
        input_regs[i] = (uint16_t)(1000 + i);

    for (int i = 0; i < MB_NUM_DISCRETE_INPUTS; i++)
        discrete_inputs[i] = (uint8_t)(i % 2);

    for (int i = 0; i < MB_NUM_COILS; i++)
        coils[i] = 0;

    pthread_mutex_unlock(&regmap_mutex);
}

void mb_regmap_tick(void)
{
    pthread_mutex_lock(&regmap_mutex);

    sim_step++;

    /* input_regs[0]: free-running counter (e.g. "uptime ticks"). */
    input_regs[0] = (uint16_t)sim_step;

    /* input_regs[1]: simulated analog reading, sine wave 0-1000. */
    double angle = (double)(sim_step % 360) * (M_PI / 180.0);
    input_regs[1] = (uint16_t)(500.0 + 500.0 * sin(angle));

    /* input_regs[2]: simulated temperature x10, drifting 200-300 (20.0-30.0C). */
    input_regs[2] = (uint16_t)(250 + (int)(50.0 * sin(angle / 3.0)));

    /* discrete_inputs[0]: 1Hz heartbeat bit. */
    discrete_inputs[0] = (uint8_t)(sim_step % 2);

    pthread_mutex_unlock(&regmap_mutex);
}

uint8_t mb_regmap_get_coil(int addr)
{
    if (addr < 0 || addr >= MB_NUM_COILS) return 0;
    return coils[addr];
}

void mb_regmap_set_coil(int addr, uint8_t value)
{
    if (addr < 0 || addr >= MB_NUM_COILS) return;
    coils[addr] = value ? 1 : 0;
}

uint8_t mb_regmap_get_discrete(int addr)
{
    if (addr < 0 || addr >= MB_NUM_DISCRETE_INPUTS) return 0;
    return discrete_inputs[addr];
}

uint16_t mb_regmap_get_holding(int addr)
{
    if (addr < 0 || addr >= MB_NUM_HOLDING_REGS) return 0;
    return holding_regs[addr];
}

void mb_regmap_set_holding(int addr, uint16_t value)
{
    if (addr < 0 || addr >= MB_NUM_HOLDING_REGS) return;
    holding_regs[addr] = value;
}

uint16_t mb_regmap_get_input(int addr)
{
    if (addr < 0 || addr >= MB_NUM_INPUT_REGS) return 0;
    return input_regs[addr];
}

uint8_t  *mb_regmap_coils_ptr(void)    { return coils; }
uint8_t  *mb_regmap_discrete_ptr(void) { return discrete_inputs; }
uint16_t *mb_regmap_holding_ptr(void)  { return holding_regs; }
uint16_t *mb_regmap_input_ptr(void)    { return input_regs; }
