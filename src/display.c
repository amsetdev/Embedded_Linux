#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include "display.h"
#include "touch.h"
#include "config.h"

/* ============================================================================
 * DRM userspace API (avoids linux/drm.h dependency)
 * ========================================================================== */

#define DRM_IOCTL_BASE          'd'
#define DRM_IOWR(nr,t)          _IOWR(DRM_IOCTL_BASE,(nr),t)
#define DRM_IO(nr)              _IO(DRM_IOCTL_BASE,(nr))

#define DRM_CAP_DUMB_BUFFER     0x1
#define DRM_MODE_CONNECTED      1
#define DRM_DISPLAY_MODE_LEN    32

struct drm_get_cap      { uint64_t capability; uint64_t value; };
struct drm_mode_modeinfo {
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh, flags, type;
    char     name[DRM_DISPLAY_MODE_LEN];
};
struct drm_mode_card_res {
    uint64_t fb_id_ptr, crtc_id_ptr, connector_id_ptr, encoder_id_ptr;
    uint32_t count_fbs, count_crtcs, count_connectors, count_encoders;
    uint32_t min_width, max_width, min_height, max_height;
};
struct drm_mode_get_connector {
    uint64_t encoders_ptr, modes_ptr, props_ptr, prop_values_ptr;
    uint32_t count_modes, count_props, count_encoders;
    uint32_t encoder_id, connector_id, connector_type, connector_type_id;
    uint32_t connection, mm_width, mm_height, subpixel, pad;
};
struct drm_mode_get_encoder {
    uint32_t encoder_id, encoder_type, crtc_id, possible_crtcs, possible_clones;
};
struct drm_mode_crtc {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors, crtc_id, fb_id, x, y, gamma_size, mode_valid;
    struct drm_mode_modeinfo mode;
};
struct drm_mode_fb_cmd {
    uint32_t fb_id, width, height, pitch, bpp, depth, handle;
};
struct drm_mode_create_dumb {
    uint32_t height, width, bpp, flags, handle, pitch; uint64_t size;
};
struct drm_mode_map_dumb { uint32_t handle, pad; uint64_t offset; };

#define DRM_IOCTL_SET_MASTER        DRM_IO(0x1e)
#define DRM_IOCTL_DROP_MASTER       DRM_IO(0x1f)
#define DRM_IOCTL_GET_CAP           DRM_IOWR(0x0c, struct drm_get_cap)
#define DRM_IOCTL_MODE_GETRESOURCES DRM_IOWR(0xA0, struct drm_mode_card_res)
#define DRM_IOCTL_MODE_SETCRTC      DRM_IOWR(0xA2, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_GETENCODER   DRM_IOWR(0xA6, struct drm_mode_get_encoder)
#define DRM_IOCTL_MODE_GETCONNECTOR DRM_IOWR(0xA7, struct drm_mode_get_connector)
#define DRM_IOCTL_MODE_ADDFB        DRM_IOWR(0xAE, struct drm_mode_fb_cmd)
#define DRM_IOCTL_MODE_CREATE_DUMB  DRM_IOWR(0xB2, struct drm_mode_create_dumb)
#define DRM_IOCTL_MODE_MAP_DUMB     DRM_IOWR(0xB3, struct drm_mode_map_dumb)

/* ============================================================================
 * 5×7 FONT
 * ========================================================================== */

static const uint8_t FONT5X7[][5] = {
  {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},
  {0x14,0x7F,0x14,0x7F,0x14},{0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},
  {0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},{0x00,0x1C,0x22,0x41,0x00},
  {0x00,0x41,0x22,0x1C,0x00},{0x14,0x08,0x3E,0x08,0x14},{0x08,0x08,0x3E,0x08,0x08},
  {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},
  {0x20,0x10,0x08,0x04,0x02},{0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},
  {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},{0x18,0x14,0x12,0x7F,0x10},
  {0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
  {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},
  {0x00,0x56,0x36,0x00,0x00},{0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},
  {0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},{0x32,0x49,0x79,0x41,0x3E},
  {0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
  {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},
  {0x3E,0x41,0x49,0x49,0x7A},{0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},
  {0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},
  {0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
  {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},
  {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},
  {0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},{0x63,0x14,0x08,0x14,0x63},
  {0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},{0x00,0x7F,0x41,0x41,0x00},
  {0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x7F,0x00},{0x04,0x02,0x01,0x02,0x04},
  {0x40,0x40,0x40,0x40,0x40},{0x00,0x01,0x02,0x04,0x00},{0x20,0x54,0x54,0x54,0x78},
  {0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},{0x38,0x44,0x44,0x48,0x7F},
  {0x38,0x54,0x54,0x54,0x18},{0x08,0x7E,0x09,0x01,0x02},{0x0C,0x52,0x52,0x52,0x3E},
  {0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},{0x20,0x40,0x44,0x3D,0x00},
  {0x7F,0x10,0x28,0x44,0x00},{0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x18,0x04,0x78},
  {0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},{0x7C,0x14,0x14,0x14,0x08},
  {0x08,0x14,0x14,0x18,0x7C},{0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},
  {0x04,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},{0x1C,0x20,0x40,0x20,0x1C},
  {0x3C,0x40,0x30,0x40,0x3C},{0x44,0x28,0x10,0x28,0x44},{0x0C,0x50,0x50,0x50,0x3C},
  {0x44,0x64,0x54,0x4C,0x44},{0x00,0x08,0x36,0x41,0x00},{0x00,0x00,0x7F,0x00,0x00},
  {0x00,0x41,0x36,0x08,0x00},{0x10,0x08,0x08,0x10,0x08},
};

/* ============================================================================
 * MODULE STATE
 * ========================================================================== */

int      disp_ok      = 0;
Screen   cur_screen   = SCREEN_STATUS;

static int      drm_fd     = -1;
static uint32_t drm_fb_id, drm_crtc_id, drm_conn_id;
static uint32_t *drm_map   = NULL;
static size_t    drm_size  = 0;
static uint32_t  drm_pitch_px = DISP_W;

static uint16_t  fb_buf[DISP_H][DISP_W];

/* ============================================================================
 * DRM INIT / FLUSH / CLEANUP
 * ========================================================================== */

int drm_init(void)
{
    drm_fd = open(DRM_DEVICE, O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        LOG_ERROR("DRM open %s: %s", DRM_DEVICE, strerror(errno));
        return -1;
    }

    if (ioctl(drm_fd, DRM_IOCTL_SET_MASTER, 0) < 0)
        LOG_WARN("DRM set master: %s", strerror(errno));

    struct drm_get_cap gcap = { .capability = DRM_CAP_DUMB_BUFFER };
    if (ioctl(drm_fd, DRM_IOCTL_GET_CAP, &gcap) < 0 || !gcap.value) {
        LOG_ERROR("DRM dumb buffer not supported");
        return -1;
    }

    uint32_t conn_ids[4], crtc_ids[4];
    struct drm_mode_card_res res = {0};
    res.connector_id_ptr = (uint64_t)(uintptr_t)conn_ids;
    res.crtc_id_ptr      = (uint64_t)(uintptr_t)crtc_ids;
    res.count_connectors = 4;
    res.count_crtcs      = 4;
    if (ioctl(drm_fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        LOG_ERROR("DRM getresources: %s", strerror(errno));
        return -1;
    }

    struct drm_mode_modeinfo mode = {0};
    int found = 0;
    for (uint32_t i = 0; i < res.count_connectors && !found; i++) {
        struct drm_mode_modeinfo modes[32];
        struct drm_mode_get_connector conn = {0};
        conn.connector_id = conn_ids[i];
        conn.modes_ptr    = (uint64_t)(uintptr_t)modes;
        conn.count_modes  = 32;
        if (ioctl(drm_fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0) continue;
        if (conn.connection != 1 || conn.count_modes == 0) continue;

        mode = modes[0];
        drm_conn_id = conn_ids[i];

        if (conn.encoder_id) {
            struct drm_mode_get_encoder enc = { .encoder_id = conn.encoder_id };
            if (ioctl(drm_fd, DRM_IOCTL_MODE_GETENCODER, &enc) == 0 && enc.crtc_id)
                drm_crtc_id = enc.crtc_id;
        }
        if (!drm_crtc_id) drm_crtc_id = crtc_ids[0];
        found = 1;
    }
    if (!found) { LOG_ERROR("No connected DRM display found"); return -1; }

    uint32_t W = mode.hdisplay;
    uint32_t H = mode.vdisplay;

    struct drm_mode_create_dumb cr = { .width = W, .height = H, .bpp = 32 };
    if (ioctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &cr) < 0) {
        LOG_ERROR("DRM create dumb: %s", strerror(errno)); return -1;
    }
    drm_size     = cr.size;
    drm_pitch_px = cr.pitch / 4;

    struct drm_mode_fb_cmd fb_cmd = {
        .width = W, .height = H, .pitch = cr.pitch,
        .bpp = 32, .depth = 24, .handle = cr.handle
    };
    if (ioctl(drm_fd, DRM_IOCTL_MODE_ADDFB, &fb_cmd) < 0) {
        LOG_ERROR("DRM addfb: %s", strerror(errno)); return -1;
    }
    drm_fb_id = fb_cmd.fb_id;

    struct drm_mode_map_dumb mp = { .handle = cr.handle };
    if (ioctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &mp) < 0) {
        LOG_ERROR("DRM map dumb: %s", strerror(errno)); return -1;
    }
    drm_map = mmap(0, drm_size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd, mp.offset);
    if (drm_map == MAP_FAILED) {
        LOG_ERROR("DRM mmap: %s", strerror(errno)); return -1;
    }
    memset(drm_map, 0, drm_size);

    struct drm_mode_crtc crtc = {0};
    crtc.crtc_id            = drm_crtc_id;
    crtc.fb_id              = drm_fb_id;
    crtc.set_connectors_ptr = (uint64_t)(uintptr_t)&drm_conn_id;
    crtc.count_connectors   = 1;
    crtc.mode               = mode;
    crtc.mode_valid         = 1;
    if (ioctl(drm_fd, DRM_IOCTL_MODE_SETCRTC, &crtc) < 0) {
        LOG_ERROR("DRM setcrtc: %s", strerror(errno)); return -1;
    }

    LOG_INFO("DRM display: %dx%d (our fb: %dx%d)", W, H, DISP_W, DISP_H);
    return 0;
}

void drm_flush(void)
{
    if (!drm_map) return;
    for (int y = 0; y < DISP_H; y++) {
        for (int x = 0; x < DISP_W; x++) {
            uint16_t p = fb_buf[y][x];
            uint8_t r = ((p >> 11) & 0x1F) * 255 / 31;
            uint8_t g = ((p >>  5) & 0x3F) * 255 / 63;
            uint8_t b = ( p        & 0x1F) * 255 / 31;
            drm_map[y * drm_pitch_px + x] =
                ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    }
}

void drm_cleanup(void)
{
    if (drm_map) { munmap(drm_map, drm_size); drm_map = NULL; }
    if (drm_fd >= 0) { close(drm_fd); drm_fd = -1; }
}

/* ============================================================================
 * DRAW PRIMITIVES
 * ========================================================================== */

void fb_fill(uint16_t col)
{
    for (int y = 0; y < DISP_H; y++)
        for (int x = 0; x < DISP_W; x++)
            fb_buf[y][x] = col;
}

void fb_pixel(int x, int y, uint16_t col)
{
    if (x >= 0 && x < DISP_W && y >= 0 && y < DISP_H)
        fb_buf[y][x] = col;
}

void fb_rect(int x, int y, int w, int h, uint16_t col)
{
    for (int dy = 0; dy < h; dy++)
        for (int dx = 0; dx < w; dx++)
            fb_pixel(x + dx, y + dy, col);
}

void fb_hline(int x, int y, int len, uint16_t col)
{
    for (int i = 0; i < len; i++) fb_pixel(x + i, y, col);
}

void fb_border(int x, int y, int w, int h, uint16_t col)
{
    fb_hline(x, y, w, col);
    fb_hline(x, y + h - 1, w, col);
    for (int i = 0; i < h; i++) {
        fb_pixel(x, y + i, col);
        fb_pixel(x + w - 1, y + i, col);
    }
}

int fb_char(int x, int y, char c, uint16_t fg, uint16_t bg, int scale)
{
    if (c < 0x20 || c > 0x7E) c = '?';
    const uint8_t *g = FONT5X7[(uint8_t)c - 0x20];
    for (int col = 0; col < 5; col++) {
        uint8_t line = g[col];
        for (int row = 0; row < 7; row++)
            fb_rect(x + col * scale, y + row * scale, scale, scale,
                    (line >> row) & 1 ? fg : bg);
    }
    return x + (5 + 1) * scale;
}

int fb_str(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale)
{
    while (*s) x = fb_char(x, y, *s++, fg, bg, scale);
    return x;
}

int fb_strw(const char *s, int scale)
{
    return (int)strlen(s) * 6 * scale;
}

void fb_str_c(int y, const char *s, uint16_t fg, uint16_t bg, int scale)
{
    int x = (DISP_W - fb_strw(s, scale)) / 2;
    fb_str(x, y, s, fg, bg, scale);
}

int ui_button(int x, int y, int w, int h, const char *label,
              uint16_t bg, uint16_t fg)
{
    fb_rect(x, y, w, h, bg);
    fb_border(x, y, w, h, fg);
    int tx = x + (w - fb_strw(label, 1)) / 2;
    int ty = y + (h - 7) / 2;
    fb_str(tx, ty, label, fg, bg, 1);
    return touch_in_rect(x, y, w, h);
}

/* ============================================================================
 * STATUS SCREEN
 * ========================================================================== */

void disp_status(int cycle, int mb_ok, int mq_ok, int success, int total)
{
    if (!disp_ok) return;
    fb_fill(COL_BLUE);

    /* Header */
    fb_rect(0, 0, DISP_W, 36, COL_HDRBLUE);
    fb_str_c(6,  "AMSET AUTOMATION PVT LTD", COL_WHITE, COL_HDRBLUE, 2);
    fb_str_c(24, "STM32MP157F-DK2",          COL_GOLD,  COL_HDRBLUE, 1);

    /* Time + cycle */
    char buf[64];
    time_t now = time(NULL);
    char hms[12]; strftime(hms, sizeof(hms), "%H:%M:%S", localtime(&now));
    char dts[16]; strftime(dts, sizeof(dts), "%Y-%m-%d", localtime(&now));
    snprintf(buf, sizeof(buf), "Cycle %d   %s  %s", cycle, dts, hms);
    fb_str_c(40, buf, COL_GRAY, COL_BLUE, 1);

    /* Status indicators */
    fb_rect(8, 58, 14, 14, mb_ok ? COL_GREEN : COL_RED);
    fb_str(28, 60,
           mb_ok ? "Modbus  : Connected" : "Modbus  : ERROR",
           mb_ok ? COL_GREEN : COL_RED, COL_BLUE, 1);

    fb_rect(8, 78, 14, 14, mq_ok ? COL_GREEN : COL_ORANGE);
    fb_str(28, 80,
           mq_ok ? "MQTT    : Online   " : "MQTT    : Offline  ",
           mq_ok ? COL_GREEN : COL_ORANGE, COL_BLUE, 1);

    /* Port + broker info */
    fb_str(8, 100, cfg.modbus_port, COL_DKGRAY, COL_BLUE, 1);
    char bshort[32];
    strncpy(bshort, cfg.mqtt_broker, 31);
    bshort[20] = 0;
    strcat(bshort, "...");
    fb_str(8, 114, bshort, COL_DKGRAY, COL_BLUE, 1);

    /* Progress bar */
    float pct = total > 0 ? (float)success / total * 100.0f : 0.0f;
    snprintf(buf, sizeof(buf), "Points: %d / %d  (%.0f%%)", success, total, pct);
    fb_str_c(134, buf, COL_GRAY, COL_BLUE, 1);
    fb_rect(8, 148, DISP_W - 16, 14, COL_DKGRAY);
    int filled = (int)((DISP_W - 16) * pct / 100.0f);
    if (filled > 0) fb_rect(8, 148, filled, 14, COL_GREEN);

    /* Divider */
    fb_hline(0, 168, DISP_W, COL_DKGRAY);
    fb_str_c(172, "Live Register Values", COL_PURPLE, COL_BLUE, 1);
    fb_hline(0, 184, DISP_W, COL_DKGRAY);

    /* Live values — two columns */
    int y = 190, col_side = 0, shown = 0;
    for (int i = 0; i < point_count && shown < 40 && y < DISP_H - 36; i++) {
        if (!points[i].valid) continue;
        int cx = col_side == 0 ? 4 : DISP_W / 2 + 4;
        char lbl[18];
        strncpy(lbl, points[i].label, 17); lbl[17] = 0;
        fb_str(cx, y, lbl, COL_GRAY, COL_BLUE, 1);
        snprintf(buf, sizeof(buf), "%d%s", points[i].value, points[i].unit);
        fb_str(cx + 108, y, buf, COL_CYAN, COL_BLUE, 1);
        col_side ^= 1;
        if (col_side == 0) { fb_hline(4, y + 11, DISP_W - 8, COL_DKGRAY); y += 14; }
        shown++;
    }

    /* Settings button */
    if (ui_button(DISP_W / 2 - 50, DISP_H - 32, 100, 28, "SETTINGS", COL_HDRBLUE, COL_GOLD))
        cur_screen = SCREEN_SETTINGS;

    drm_flush();
}

/* ============================================================================
 * SETTINGS SCREEN
 * ========================================================================== */

typedef struct {
    const char *label;
    int        *val;
    int         step;
    int         lo;
    int         hi;
} NumSetting;

void disp_settings(void)
{
    if (!disp_ok) return;

    static NumSetting nums[] = {
        { "Interval (s)",  &cfg.interval,     10,   5,    3600  },
        { "Modbus Baud",   &cfg.modbus_baud,  2400, 1200, 115200},
        { "Modbus Slave",  &cfg.modbus_slave, 1,    1,    247   },
        { "MQTT Port",     &cfg.mqtt_port,    1,    1,    65535 },
    };
    static const int NNUM = 4;

    fb_fill(COL_BLUE);
    fb_rect(0, 0, DISP_W, 24, COL_HDRBLUE);
    fb_str_c(4, "SETTINGS", COL_GOLD, COL_HDRBLUE, 2);

    for (int i = 0; i < NNUM; i++) {
        int ry = 50 + i * 60;
        fb_str_c(ry, nums[i].label, COL_GRAY, COL_BLUE, 1);
        char vbuf[24];
        snprintf(vbuf, sizeof(vbuf), "%d", *nums[i].val);

        if (ui_button(60, ry + 14, 60, 32, "  -  ", COL_DKBLUE, COL_WHITE)) {
            *nums[i].val -= nums[i].step;
            if (*nums[i].val < nums[i].lo) *nums[i].val = nums[i].lo;
        }

        fb_rect(130, ry + 14, 220, 32, COL_DKGRAY);
        fb_border(130, ry + 14, 220, 32, COL_GRAY);
        int tx = 130 + (220 - fb_strw(vbuf, 2)) / 2;
        fb_str(tx, ry + 22, vbuf, COL_WHITE, COL_DKGRAY, 2);

        if (ui_button(360, ry + 14, 60, 32, "  +  ", COL_DKBLUE, COL_WHITE)) {
            *nums[i].val += nums[i].step;
            if (*nums[i].val > nums[i].hi) *nums[i].val = nums[i].hi;
        }
    }

    int sy = 50 + NNUM * 60 + 20;
    fb_hline(0, sy, DISP_W, COL_DKGRAY); sy += 8;
    fb_str_c(sy, "Current Configuration", COL_PURPLE, COL_BLUE, 1); sy += 16;
    fb_hline(0, sy, DISP_W, COL_DKGRAY); sy += 8;

    fb_str(8, sy, "MQTT Broker:", COL_GRAY, COL_BLUE, 1); sy += 14;
    char bshort[48];
    strncpy(bshort, cfg.mqtt_broker, 47); bshort[47] = 0;
    fb_str(16, sy, bshort, COL_CYAN, COL_BLUE, 1); sy += 16;

    fb_str(8, sy, "MQTT User:", COL_GRAY, COL_BLUE, 1);
    fb_str(80, sy, cfg.mqtt_user, COL_CYAN, COL_BLUE, 1); sy += 14;

    fb_str(8, sy, "Modbus Port:", COL_GRAY, COL_BLUE, 1);
    fb_str(84, sy, cfg.modbus_port, COL_CYAN, COL_BLUE, 1); sy += 16;

    fb_hline(0, sy, DISP_W, COL_DKGRAY); sy += 8;
    fb_str_c(sy, "Edit broker/user/pass via SSH:", COL_DKGRAY, COL_BLUE, 1); sy += 12;
    fb_str_c(sy, "  settings.conf", COL_ORANGE, COL_BLUE, 1);

    if (ui_button(40, DISP_H - 50, 160, 38, "  SAVE  ", COL_GREEN, COL_WHITE)) {
        settings_save();
        LOG_INFO("Settings saved");
        cur_screen = SCREEN_STATUS;
    }
    if (ui_button(DISP_W - 200, DISP_H - 50, 160, 38, "  BACK  ", COL_RED, COL_WHITE)) {
        settings_load();
        cur_screen = SCREEN_STATUS;
    }

    drm_flush();
}