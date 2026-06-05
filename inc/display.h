#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>

/* ---- Display resolution ----------------------------------------------- */
#define DISP_W  480
#define DISP_H  800

/* ---- Screen identifiers ----------------------------------------------- */
typedef enum { SCREEN_STATUS, SCREEN_SETTINGS } Screen;
extern Screen cur_screen;

/* ---- Display availability flag ---------------------------------------- */
extern int disp_ok;

/* ---- DRM ---------------------------------------------------------------- */
int  drm_init(void);
void drm_flush(void);
void drm_cleanup(void);

/* ---- Touch -------------------------------------------------------------- */
int  touch_init(void);
void touch_poll(void);
int  touch_in_rect(int x, int y, int w, int h);

/* ---- Framebuffer drawing helpers --------------------------------------- */
void fb_fill(uint16_t c);
void fb_rect(int x, int y, int w, int h, uint16_t c);
void fb_hline(int x, int y, int len, uint16_t c);
void fb_border(int x, int y, int w, int h, uint16_t c);
int  fb_char(int x, int y, char c, uint16_t fg, uint16_t bg, int s);
int  fb_str(int x, int y, const char *s, uint16_t fg, uint16_t bg, int sc);
int  fb_strw(const char *s, int sc);
void fb_str_c(int y, const char *s, uint16_t fg, uint16_t bg, int sc);
int  ui_button(int x, int y, int w, int h, const char *lbl,
               uint16_t bg, uint16_t fg);

/* ---- Colour helpers ----------------------------------------------------- */
#define RGB(r,g,b) ((uint16_t)((((r)&0xF8u)<<8)|(((g)&0xFCu)<<3)|((b)>>3)))
#define COL_BLACK   RGB(  0,  0,  0)
#define COL_WHITE   RGB(255,255,255)
#define COL_GOLD    RGB(240,165,  0)
#define COL_BLUE    RGB(  5, 10, 40)
#define COL_HDRBLUE RGB(  0, 70,150)
#define COL_GREEN   RGB(  0,210, 80)
#define COL_RED     RGB(255, 60, 60)
#define COL_ORANGE  RGB(255,180,  0)
#define COL_CYAN    RGB(  0,210,240)
#define COL_GRAY    RGB(160,160,160)
#define COL_DKGRAY  RGB( 40, 40, 80)
#define COL_PURPLE  RGB(120,140,255)
#define COL_DKBLUE  RGB( 20, 40, 90)

/* ---- Screen renderers -------------------------------------------------- */
/**
 * disp_status() — render the live register status screen.
 * @cycle    current Modbus poll cycle number
 * @mb_ok    1 = Modbus OK, 0 = error
 * @mq_ok    1 = MQTT online, 0 = offline
 * @success  number of points successfully read
 * @total    total number of configured points
 */
void disp_status(int cycle, int mb_ok, int mq_ok, int success, int total);

/**
 * disp_settings() — render the interactive settings screen.
 * Reads/writes cfg (AppSettings) and calls settings_save()/settings_load()
 * on SAVE/BACK.
 */
void disp_settings(void);

#endif /* DISPLAY_H */
