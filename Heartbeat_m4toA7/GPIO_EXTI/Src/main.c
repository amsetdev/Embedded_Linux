/* =============================================================
 * File    : main.c
 * Core    : STM32MP1 — Cortex-M4
 * Brief   : LED blink every 1s + shared memory heartbeat to A7
 *  LED blinking on M4 + watching counter on A7 with devmem2.  If it increments, M4 is alive.
 *  watch -n 1 'devmem2 0x38000014 w'(to see counter in hex)
 *  watch -n 1 'devmem2 0x38000014 w | grep -o "0x[0-9A-Fa-f]*$" | xargs printf "Heartbeat Counter: %d\n"' ( to see counter in decimal)
 * ============================================================= */

#include "main.h"
#include <stdint.h>

/* ------------------------------------------------------------------ */
/*  SHARED MEMORY                                                       */
/*  RETRAM map:                                                         */
/*  0x38000000 — Your existing interface (untouched)                   */
/*  0x38000010 — Heartbeat block (this module, 16 bytes)               */
/* ------------------------------------------------------------------ */

#define HB_SHARED_BASE_M4       (0x00000010UL)
#define HB_MAGIC_WORD           (0xBEEF5A5AUL)
#define HB_MAX_COUNTER          (0xFFFFFFFEUL)

typedef struct __attribute__((packed))
{
    volatile uint32_t magic;         /* HB_MAGIC_WORD when M4 alive    */
    volatile uint32_t counter;       /* Increments every 1 second      */
    volatile uint32_t timestamp_ms;  /* HAL_GetTick() snapshot         */
    volatile uint32_t reserved;
} SharedHeartbeat_t;

/* ------------------------------------------------------------------ */
/*  CONFIGURATION                                                       */
/* ------------------------------------------------------------------ */

#define HB_PERIOD_MS            1000U
#define LED_TOGGLE_MS           1000U
#define LED_GPIO_PORT           GPIOA
#define LED_GPIO_PIN            GPIO_PIN_13

/* ------------------------------------------------------------------ */
/*  GLOBALS                                                             */
/* ------------------------------------------------------------------ */

static SharedHeartbeat_t * const p_shared =
    (SharedHeartbeat_t *)HB_SHARED_BASE_M4;

static uint32_t s_last_hb_tick  = 0U;
static uint32_t s_last_led_tick = 0U;

EXTI_HandleTypeDef hexti;   /* Fixes: undefined reference to 'hexti'  */

/* ------------------------------------------------------------------ */
/*  FIX 1 — assert_failed (called by HAL when USE_FULL_ASSERT is on)   */
/* ------------------------------------------------------------------ */

void assert_failed(uint8_t *file, uint32_t line)
{
    (void)file;
    (void)line;
    /* Trap here so debugger catches assertion failures */
    while (1) { ; }
}

/* ------------------------------------------------------------------ */
/*  FIX 2 — SystemClock_Config                                         */
/*  STM32MP1 M4: A7 sets up clocks before releasing M4.               */
/*  M4 only needs to call SystemCoreClockUpdate().                     */
/* ------------------------------------------------------------------ */

static void SystemClock_Config(void)
{
    /* A7 (TF-A / U-Boot) already configured clocks before booting M4.
     * Just update the CMSIS variable so HAL_GetTick() is accurate.   */
    SystemCoreClockUpdate();
}

/* ------------------------------------------------------------------ */
/*  FIX 3 — MX_GPIO_Init                                               */
/*  Configure GPIOA PIN 13 as push-pull output for LED.               */
/* ------------------------------------------------------------------ */

static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    /* Enable GPIOA clock */
    __HAL_RCC_GPIOA_CLK_ENABLE();

    /* Set pin LOW before enabling output (no glitch) */
    HAL_GPIO_WritePin(LED_GPIO_PORT, LED_GPIO_PIN, GPIO_PIN_RESET);

    /* Configure PA13 as output push-pull, no pull, low speed */
    GPIO_InitStruct.Pin   = LED_GPIO_PIN;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(LED_GPIO_PORT, &GPIO_InitStruct);
}

/* ------------------------------------------------------------------ */
/*  HELPERS                                                             */
/* ------------------------------------------------------------------ */

static inline uint8_t tick_elapsed(uint32_t last_tick, uint32_t period_ms)
{
    return ((HAL_GetTick() - last_tick) >= period_ms) ? 1U : 0U;
}

static void heartbeat_init(void)
{
    p_shared->magic        = 0U;
    p_shared->counter      = 0U;
    p_shared->timestamp_ms = 0U;
    p_shared->reserved     = 0U;
    __DSB();
}

static void heartbeat_update(void)
{
    p_shared->counter = (p_shared->counter >= HB_MAX_COUNTER)
                        ? 0U
                        : (p_shared->counter + 1U);

    p_shared->timestamp_ms = HAL_GetTick();
    __DMB();
    p_shared->magic = HB_MAGIC_WORD;
}

/* ------------------------------------------------------------------ */
/*  MAIN                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();

    heartbeat_init();

    s_last_hb_tick  = HAL_GetTick();
    s_last_led_tick = HAL_GetTick();

    while (1)
    {
        /* Heartbeat — write counter to shared RETRAM every 1s */
        if (tick_elapsed(s_last_hb_tick, HB_PERIOD_MS))
        {
            heartbeat_update();
            s_last_hb_tick = HAL_GetTick();
        }

        /* LED — toggle every 1s, non-blocking */
        if (tick_elapsed(s_last_led_tick, LED_TOGGLE_MS))
        {
            HAL_GPIO_TogglePin(LED_GPIO_PORT, LED_GPIO_PIN);
            s_last_led_tick = HAL_GetTick();
        }
    }
}
