/**
 * @file display.h
 * @brief Display, framebuffer, and touch interface.
 *
 * This module provides the graphical user interface for the Smart RTU
 * application. It includes functions for initializing the display,
 * rendering screens, drawing graphics primitives, handling touch input,
 * and updating the framebuffer.
 */

#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Display Resolution                                                         */
/* -------------------------------------------------------------------------- */

/** @brief Display width in pixels. */
#define DISP_W 480

/** @brief Display height in pixels. */
#define DISP_H 800

/* -------------------------------------------------------------------------- */
/* Screen Types                                                               */
/* -------------------------------------------------------------------------- */

/**
 * @brief Application screen identifiers.
 */
typedef enum
{
    SCREEN_STATUS,     /**< Status screen. */
    SCREEN_SETTINGS    /**< Settings screen. */
} Screen;

/** @brief Currently displayed screen. */
extern Screen cur_screen;

/* -------------------------------------------------------------------------- */
/* Display State                                                              */
/* -------------------------------------------------------------------------- */

/** @brief Indicates whether the display has been initialized successfully. */
extern int disp_ok;

/* -------------------------------------------------------------------------- */
/* DRM Functions                                                              */
/* -------------------------------------------------------------------------- */

/**
 * @brief Initializes the DRM display subsystem.
 *
 * @return
 * - 0 on success.
 * - Non-zero on failure.
 */
int drm_init(void);

/**
 * @brief Flushes the framebuffer to the display.
 */
void drm_flush(void);

/**
 * @brief Releases display resources.
 */
void drm_cleanup(void);

/* -------------------------------------------------------------------------- */
/* Touch Functions                                                            */
/* -------------------------------------------------------------------------- */

/**
 * @brief Initializes the touch controller.
 *
 * @return
 * - 0 on success.
 * - Non-zero on failure.
 */
int touch_init(void);

/**
 * @brief Polls the touch controller for touch events.
 */
void touch_poll(void);

/**
 * @brief Checks whether the touch position lies within a rectangle.
 *
 * @param x Rectangle X coordinate.
 * @param y Rectangle Y coordinate.
 * @param w Rectangle width.
 * @param h Rectangle height.
 *
 * @return
 * - 1 if touched inside the rectangle.
 * - 0 otherwise.
 */
int touch_in_rect(int x, int y, int w, int h);

/* -------------------------------------------------------------------------- */
/* Framebuffer Drawing Functions                                              */
/* -------------------------------------------------------------------------- */

/**
 * @brief Fills the entire framebuffer with a color.
 *
 * @param c RGB565 color.
 */
void fb_fill(uint16_t c);

/**
 * @brief Draws a filled rectangle.
 *
 * @param x Left coordinate.
 * @param y Top coordinate.
 * @param w Width.
 * @param h Height.
 * @param c RGB565 color.
 */
void fb_rect(int x, int y, int w, int h, uint16_t c);

/**
 * @brief Draws a horizontal line.
 *
 * @param x Starting X coordinate.
 * @param y Y coordinate.
 * @param len Line length.
 * @param c RGB565 color.
 */
void fb_hline(int x, int y, int len, uint16_t c);

/**
 * @brief Draws a rectangular border.
 *
 * @param x Left coordinate.
 * @param y Top coordinate.
 * @param w Width.
 * @param h Height.
 * @param c RGB565 color.
 */
void fb_border(int x, int y, int w, int h, uint16_t c);

/**
 * @brief Draws a single character.
 *
 * @param x X coordinate.
 * @param y Y coordinate.
 * @param c Character.
 * @param fg Foreground color.
 * @param bg Background color.
 * @param s Scale factor.
 *
 * @return Character width in pixels.
 */
int fb_char(int x, int y, char c, uint16_t fg, uint16_t bg, int s);

/**
 * @brief Draws a string.
 *
 * @param x X coordinate.
 * @param y Y coordinate.
 * @param s String to draw.
 * @param fg Foreground color.
 * @param bg Background color.
 * @param sc Text scale factor.
 *
 * @return Rendered string width in pixels.
 */
int fb_str(int x, int y, const char *s, uint16_t fg, uint16_t bg, int sc);

/**
 * @brief Returns the rendered width of a string.
 *
 * @param s String.
 * @param sc Text scale factor.
 *
 * @return Width in pixels.
 */
int fb_strw(const char *s, int sc);

/**
 * @brief Draws a horizontally centered string.
 *
 * @param y Y coordinate.
 * @param s String.
 * @param fg Foreground color.
 * @param bg Background color.
 * @param sc Text scale factor.
 */
void fb_str_c(int y, const char *s, uint16_t fg, uint16_t bg, int sc);

/**
 * @brief Draws a button.
 *
 * @param x X coordinate.
 * @param y Y coordinate.
 * @param w Width.
 * @param h Height.
 * @param lbl Button label.
 * @param bg Background color.
 * @param fg Text color.
 *
 * @return Non-zero if the button is pressed.
 */
int ui_button(int x, int y, int w, int h, const char *lbl,
              uint16_t bg, uint16_t fg);

/* -------------------------------------------------------------------------- */
/* Color Definitions                                                          */
/* -------------------------------------------------------------------------- */

/** @brief Converts 8-bit RGB values to RGB565 format. */
#define RGB(r,g,b) ((uint16_t)((((r)&0xF8u)<<8)|(((g)&0xFCu)<<3)|((b)>>3)))

#define COL_BLACK   RGB(0,   0,   0)
#define COL_WHITE   RGB(255, 255, 255)
#define COL_GOLD    RGB(240, 165, 0)
#define COL_BLUE    RGB(5,   10,  40)
#define COL_HDRBLUE RGB(0,   70, 150)
#define COL_GREEN   RGB(0,  210,  80)
#define COL_RED     RGB(255, 60,  60)
#define COL_ORANGE  RGB(255,180,   0)
#define COL_CYAN    RGB(0,  210, 240)
#define COL_GRAY    RGB(160,160,160)
#define COL_DKGRAY  RGB(40,  40,  80)
#define COL_PURPLE  RGB(120,140,255)
#define COL_DKBLUE  RGB(20,  40,  90)

/* -------------------------------------------------------------------------- */
/* Screen Rendering                                                           */
/* -------------------------------------------------------------------------- */

/**
 * @brief Displays the system status screen.
 *
 * @param cycle Current Modbus polling cycle.
 * @param mb_ok Modbus communication status.
 * @param mq_ok MQTT connection status.
 * @param success Number of successfully read points.
 * @param total Total configured Modbus points.
 */
void disp_status(int cycle,
                 int mb_ok,
                 int mq_ok,
                 int success,
                 int total);

/**
 * @brief Displays the settings screen.
 *
 * Allows viewing and modifying application settings.
 */
void disp_settings(void);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_H */