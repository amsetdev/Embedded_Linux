#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include "touch.h"
#include "config.h"

/* ============================================================================
 * TOUCH MODULE
 * ========================================================================== */

int touch_x     = -1;
int touch_y     = -1;
int touch_down  = 0;

static int touch_fd     = -1;
static int touch_tapped = 0;   /* single-tap flag for this frame */

int touch_init(void)
{
    const char *devs[] = {
        "/dev/input/event0", "/dev/input/event1",
        "/dev/input/event2", "/dev/input/event3", NULL
    };

    for (int i = 0; devs[i]; i++) {
        int fd = open(devs[i], O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        uint8_t bits[KEY_MAX / 8 + 1] = {0};
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(bits)), bits);

        if (bits[ABS_X / 8] & (1 << (ABS_X % 8))) {
            touch_fd = fd;
            LOG_INFO("Touch device: %s", devs[i]);
            return 0;
        }
        close(fd);
    }

    LOG_WARN("No touch device found — touch disabled");
    return -1;
}

void touch_close(void)
{
    if (touch_fd >= 0) {
        close(touch_fd);
        touch_fd = -1;
    }
}

void touch_poll(void)
{
    if (touch_fd < 0) return;

    touch_tapped = 0;
    struct input_event ev;
    int prev_down = touch_down;

    while (read(touch_fd, &ev, sizeof(ev)) == sizeof(ev)) {
        if (ev.type == EV_ABS) {
            /* Support both single-touch (ABS_X/Y) and MT (slot 0x35/0x36) */
            if (ev.code == ABS_X  || ev.code == 0x35) touch_x = ev.value;
            if (ev.code == ABS_Y  || ev.code == 0x36) touch_y = ev.value;
        } else if (ev.type == EV_KEY && ev.code == 0x14a) { /* BTN_TOUCH */
            touch_down = ev.value;
        }
    }

    if (!prev_down && touch_down) touch_tapped = 1;
}

int touch_in_rect(int x, int y, int w, int h)
{
    return touch_tapped
        && touch_x >= x && touch_x < x + w
        && touch_y >= y && touch_y < y + h;
}