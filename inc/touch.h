#ifndef TOUCH_H
#define TOUCH_H

/* ============================================================================
 * TOUCH MODULE
 *
 * Linux evdev single-touch / multi-touch input.
 * Call touch_init() once, then touch_poll() each frame before drawing.
 * Use touch_in_rect() to detect tap events on UI elements.
 * ========================================================================== */

/* Open touch device; returns 0 on success, -1 if no device found */
int  touch_init(void);

/* Close touch device */
void touch_close(void);

/* Poll pending events; updates internal state and sets tap flag */
void touch_poll(void);

/* Returns 1 if a tap occurred this frame inside the given rectangle */
int  touch_in_rect(int x, int y, int w, int h);

/* Current coordinates (valid while touch_down is set) */
extern int touch_x;
extern int touch_y;
extern int touch_down;

#endif /* TOUCH_H */