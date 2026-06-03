#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>
#include "config.h"

/* ============================================================================
 * DISPLAY MODULE
 *
 * DRM/KMS framebuffer initialisation, software framebuffer drawing,
 * 5×7 bitmap font rendering, and the two application screens
 * (STATUS and SETTINGS).
 * ========================================================================== */

/* ---- Colour helpers (RGB888 → RGB565) ---- */
#define RGB(r,g,b)  ((uint16_t)((((r)&0xF8u)<<8)|(((g)&0xFCu)<<3)|((b)>>3)))

#define COL_BLACK    RGB(  0,  0,  0)
#define COL_WHITE    RGB(255,255,255)
#define COL_GOLD     RGB(240,165,  0)
#define COL_BLUE     RGB(  5, 10, 40)
#define COL_HDRBLUE  RGB(  0, 70,150)
#define COL_GREEN    RGB(  0,210, 80)
#define COL_RED      RGB(255, 60, 60)
#define COL_ORANGE   RGB(255,180,  0)
#define COL_CYAN     RGB(  0,210,240)
#define COL_GRAY     RGB(160,160,160)
#define COL_DKGRAY   RGB( 40, 40, 80)
#define COL_PURPLE   RGB(120,140,255)
#define COL_DKBLUE   RGB( 20, 40, 90)

/* ---- UI Screen enum ---- */
typedef enum { SCREEN_STATUS, SCREEN_SETTINGS } Screen;
extern Screen cur_screen;

/* ---- DRM init/cleanup ---- */
int  drm_init(void);
void drm_flush(void);
void drm_cleanup(void);

/* ---- Low-level draw primitives ---- */
void fb_fill(uint16_t col);
void fb_pixel(int x, int y, uint16_t col);
void fb_rect(int x, int y, int w, int h, uint16_t col);
void fb_hline(int x, int y, int len, uint16_t col);
void fb_border(int x, int y, int w, int h, uint16_t col);
int  fb_char(int x, int y, char c, uint16_t fg, uint16_t bg, int scale);
int  fb_str(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale);
int  fb_strw(const char *s, int scale);
void fb_str_c(int y, const char *s, uint16_t fg, uint16_t bg, int scale);
int  ui_button(int x, int y, int w, int h, const char *label,
               uint16_t bg, uint16_t fg);

/* ---- Application screens ---- */
void disp_status(int cycle, int mb_ok, int mq_ok, int success, int total);
void disp_settings(void);

/* ---- Flag — set to 1 by drm_init() on success ---- */
extern int disp_ok;

#endif /* DISPLAY_H */