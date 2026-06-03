
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include "modbus.h"

/* ---- DRM userspace API (avoids linux/drm.h dependency) ---- */
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
/* ---- end DRM ---- */
#include <modbus/modbus.h>
#include <mosquitto.h>
#include <sqlite3.h>

/* ============================================================================
 * CONFIGURATION — edit or override via settings.conf at runtime
 * ========================================================================== */

#define MODBUS_PORT_DEF      "/dev/ttyACM0"
#define MODBUS_BAUD_DEF      9600
#define MODBUS_SLAVE_DEF     1
#define MODBUS_PARITY        'N'
#define MODBUS_DATA_BITS     8
#define MODBUS_STOP_BITS     1

#define CONFIG_FILE          "registers.csv"
#define SETTINGS_FILE        "settings.conf"

#define MQTT_BROKER_DEF      "3040e50ebdbb4f949b7ec9480b0a0326.s1.eu.hivemq.cloud"
#define MQTT_PORT_DEF        8883
#define MQTT_TOPIC           "modbus/data"
#define MQTT_USERNAME_DEF    "prasad"
#define MQTT_PASSWORD_DEF    "prasad#12$A"
#define MQTT_STORAGE_DB      "mqtt_storage.db"

#define INTERVAL_DEF         30
#define MAX_RETRIES          2
#define POINT_DELAY_US       10000
#define MAX_POINTS           2000
#define LABEL_MAX            64
#define UNIT_MAX             16
#define PAYLOAD_MAX          131072

/* Display */
#define DRM_DEVICE           "/dev/dri/card0"
#define DISP_W               480
#define DISP_H               800

/* ============================================================================
 * SETTINGS — loaded from settings.conf, overrides the DEF values above
 * ========================================================================== */

typedef struct {
    char modbus_port[64];
    int  modbus_baud;
    int  modbus_slave;
    char mqtt_broker[256];
    int  mqtt_port;
    char mqtt_user[64];
    char mqtt_pass[64];
    int  interval;
} AppSettings;

static AppSettings cfg;

static void settings_defaults(void)
{
    strncpy(cfg.modbus_port, MODBUS_PORT_DEF, sizeof(cfg.modbus_port)-1);
    cfg.modbus_baud  = MODBUS_BAUD_DEF;
    cfg.modbus_slave = MODBUS_SLAVE_DEF;
    strncpy(cfg.mqtt_broker, MQTT_BROKER_DEF, sizeof(cfg.mqtt_broker)-1);
    cfg.mqtt_port = MQTT_PORT_DEF;
    strncpy(cfg.mqtt_user, MQTT_USERNAME_DEF, sizeof(cfg.mqtt_user)-1);
    strncpy(cfg.mqtt_pass, MQTT_PASSWORD_DEF, sizeof(cfg.mqtt_pass)-1);
    cfg.interval = INTERVAL_DEF;
}

static void settings_load(void)
{
    settings_defaults();
    FILE *f = fopen(SETTINGS_FILE, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        while (*k == ' ') k++;
        while (*v == ' ') v++;
        if (!strcmp(k,"modbus_port"))  strncpy(cfg.modbus_port, v, sizeof(cfg.modbus_port)-1);
        else if (!strcmp(k,"modbus_baud"))  cfg.modbus_baud  = atoi(v);
        else if (!strcmp(k,"modbus_slave")) cfg.modbus_slave = atoi(v);
        else if (!strcmp(k,"mqtt_broker")) strncpy(cfg.mqtt_broker, v, sizeof(cfg.mqtt_broker)-1);
        else if (!strcmp(k,"mqtt_port"))   cfg.mqtt_port  = atoi(v);
        else if (!strcmp(k,"mqtt_user"))   strncpy(cfg.mqtt_user, v, sizeof(cfg.mqtt_user)-1);
        else if (!strcmp(k,"mqtt_pass"))   strncpy(cfg.mqtt_pass, v, sizeof(cfg.mqtt_pass)-1);
        else if (!strcmp(k,"interval"))    cfg.interval   = atoi(v);
    }
    fclose(f);
}

static void settings_save(void)
{
    FILE *f = fopen(SETTINGS_FILE, "w");
    if (!f) return;
    fprintf(f, "modbus_port=%s\n",  cfg.modbus_port);
    fprintf(f, "modbus_baud=%d\n",  cfg.modbus_baud);
    fprintf(f, "modbus_slave=%d\n", cfg.modbus_slave);
    fprintf(f, "mqtt_broker=%s\n",  cfg.mqtt_broker);
    fprintf(f, "mqtt_port=%d\n",    cfg.mqtt_port);
    fprintf(f, "mqtt_user=%s\n",    cfg.mqtt_user);
    fprintf(f, "mqtt_pass=%s\n",    cfg.mqtt_pass);
    fprintf(f, "interval=%d\n",     cfg.interval);
    fclose(f);
}

/* ============================================================================
 * COLOUR HELPERS  (RGB888 → RGB565)
 * ========================================================================== */

#define RGB(r,g,b) ((uint16_t)((((r)&0xF8u)<<8)|(((g)&0xFCu)<<3)|((b)>>3)))

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
 * DATA STRUCTURES
 * ========================================================================== */

typedef enum { REG_HOLDING, REG_INPUT, REG_COIL, REG_DISCRETE } RegType;

typedef struct {
    char    label[LABEL_MAX];
    int     address;
    RegType reg_type;
    char    data_type;
    char    unit[UNIT_MAX];
    int     value;
    int     valid;
} ModbusPoint;

/* ============================================================================
 * GLOBALS
 * ========================================================================== */

static ModbusPoint       points[MAX_POINTS];
static int               point_count    = 0;
static modbus_t         *mb_ctx         = NULL;
static struct mosquitto *mosq           = NULL;
static sqlite3          *db             = NULL;
static volatile int      mqtt_connected = 0;
static volatile int      running        = 1;
static pthread_mutex_t   db_mutex       = PTHREAD_MUTEX_INITIALIZER;

/* Modbus background thread */
static pthread_mutex_t   points_mutex   = PTHREAD_MUTEX_INITIALIZER;
static volatile int      mb_cycle       = 0;
static volatile int      mb_ok_flag     = 0;
static volatile int      mb_success_cnt = 0;
static pthread_t         mb_thread_id;

/* Display */
static int      disp_ok   = 0;
static int      drm_fd    = -1;
static uint32_t drm_fb_id, drm_crtc_id, drm_conn_id;
static uint32_t *drm_map  = NULL;
static size_t    drm_size = 0;
static uint32_t  drm_pitch_px = DISP_W;   /* pitch in pixels */
static uint16_t  fb[DISP_H][DISP_W];

/* Touch */
static int touch_fd      = -1;
static int touch_x       = -1;
static int touch_y       = -1;
static int touch_down    = 0;
static int touch_tapped  = 0;  /* single-tap event this frame */

/* UI */
typedef enum { SCREEN_STATUS, SCREEN_SETTINGS } Screen;
static Screen cur_screen = SCREEN_STATUS;

/* ============================================================================
 * LOGGING
 * ========================================================================== */

static void log_msg(const char *level, const char *fmt, ...)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", t);
    printf("[%s] [%s] ", tbuf, level);
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    putchar('\n'); fflush(stdout);
}

#define LOG_INFO(...)  log_msg("INFO",    __VA_ARGS__)
#define LOG_WARN(...)  log_msg("WARNING", __VA_ARGS__)
#define LOG_ERROR(...) log_msg("ERROR",   __VA_ARGS__)

/* ============================================================================
 * DRM DISPLAY
 * ========================================================================== */

static int drm_init(void)
{
    drm_fd = open(DRM_DEVICE, O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) { LOG_ERROR("DRM open %s: %s", DRM_DEVICE, strerror(errno)); return -1; }

    if (ioctl(drm_fd, DRM_IOCTL_SET_MASTER, 0) < 0)
        LOG_WARN("DRM set master: %s", strerror(errno));

    /* Check dumb buffer support */
    uint64_t cap = 0;
    struct drm_get_cap gcap = { .capability = DRM_CAP_DUMB_BUFFER };
    if (ioctl(drm_fd, DRM_IOCTL_GET_CAP, &gcap) < 0 || !gcap.value) {
        LOG_ERROR("DRM dumb buffer not supported"); return -1;
    }

    /* Get resources */
    uint32_t conn_ids[4], crtc_ids[4];
    struct drm_mode_card_res res = {0};
    res.connector_id_ptr = (uint64_t)(uintptr_t)conn_ids;
    res.crtc_id_ptr      = (uint64_t)(uintptr_t)crtc_ids;
    res.count_connectors = 4;
    res.count_crtcs      = 4;
    if (ioctl(drm_fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        LOG_ERROR("DRM getresources: %s", strerror(errno)); return -1;
    }

    /* Find a connected connector with a valid mode */
    struct drm_mode_modeinfo mode = {0};
    int found = 0;
    for (uint32_t i = 0; i < res.count_connectors && !found; i++) {
        struct drm_mode_modeinfo modes[32];
        struct drm_mode_get_connector conn = {0};
        conn.connector_id  = conn_ids[i];
        conn.modes_ptr     = (uint64_t)(uintptr_t)modes;
        conn.count_modes   = 32;
        if (ioctl(drm_fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0) continue;
        if (conn.connection != 1 || conn.count_modes == 0) continue;

        mode = modes[0];   /* best mode (first = preferred) */
        drm_conn_id = conn_ids[i];

        /* Get encoder → CRTC */
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

    /* Create dumb buffer (32bpp XRGB8888) */
    struct drm_mode_create_dumb cr = { .width=W, .height=H, .bpp=32 };
    if (ioctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &cr) < 0) {
        LOG_ERROR("DRM create dumb: %s", strerror(errno)); return -1;
    }
    drm_size     = cr.size;
    drm_pitch_px = cr.pitch / 4;   /* bytes→pixels for 32bpp */

    /* Create FB object */
    struct drm_mode_fb_cmd fb_cmd = {
        .width=W, .height=H, .pitch=cr.pitch, .bpp=32, .depth=24, .handle=cr.handle
    };
    if (ioctl(drm_fd, DRM_IOCTL_MODE_ADDFB, &fb_cmd) < 0) {
        LOG_ERROR("DRM addfb: %s", strerror(errno)); return -1;
    }
    drm_fb_id = fb_cmd.fb_id;

    /* Map buffer */
    struct drm_mode_map_dumb mp = { .handle = cr.handle };
    if (ioctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &mp) < 0) {
        LOG_ERROR("DRM map dumb: %s", strerror(errno)); return -1;
    }
    drm_map = mmap(0, drm_size, PROT_READ|PROT_WRITE, MAP_SHARED, drm_fd, mp.offset);
    if (drm_map == MAP_FAILED) { LOG_ERROR("DRM mmap: %s", strerror(errno)); return -1; }
    memset(drm_map, 0, drm_size);

    /* Set CRTC */
    struct drm_mode_crtc crtc = {0};
    crtc.crtc_id           = drm_crtc_id;
    crtc.fb_id             = drm_fb_id;
    crtc.set_connectors_ptr= (uint64_t)(uintptr_t)&drm_conn_id;
    crtc.count_connectors  = 1;
    crtc.mode              = mode;
    crtc.mode_valid        = 1;
    if (ioctl(drm_fd, DRM_IOCTL_MODE_SETCRTC, &crtc) < 0) {
        LOG_ERROR("DRM setcrtc: %s", strerror(errno)); return -1;
    }

    LOG_INFO("DRM display: %dx%d (our fb: %dx%d)", W, H, DISP_W, DISP_H);
    return 0;
}

static void drm_flush(void)
{
    if (!drm_map) return;
    for (int y = 0; y < DISP_H; y++) {
        for (int x = 0; x < DISP_W; x++) {
            uint16_t p = fb[y][x];
            uint8_t r = ((p >> 11) & 0x1F) * 255 / 31;
            uint8_t g = ((p >>  5) & 0x3F) * 255 / 63;
            uint8_t b = ( p        & 0x1F) * 255 / 31;
            drm_map[y * drm_pitch_px + x] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    }
}

static void drm_cleanup(void)
{
    if (drm_map) { munmap(drm_map, drm_size); drm_map = NULL; }
    if (drm_fd >= 0) { close(drm_fd); drm_fd = -1; }
}

/* ============================================================================
 * TOUCH INPUT  (Linux evdev)
 * ========================================================================== */

static int touch_init(void)
{
    /* Try event0..event4 and pick the first one that has ABS_X */
    const char *devs[] = {"/dev/input/event0","/dev/input/event1",
                          "/dev/input/event2","/dev/input/event3",NULL};
    for (int i = 0; devs[i]; i++) {
        int fd = open(devs[i], O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        uint8_t bits[KEY_MAX/8+1] = {0};
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(bits)), bits);
        if (bits[ABS_X/8] & (1 << (ABS_X%8))) {
            touch_fd = fd;
            LOG_INFO("Touch device: %s", devs[i]);
            return 0;
        }
        close(fd);
    }
    LOG_WARN("No touch device found — touch disabled");
    return -1;
}

static void touch_poll(void)
{
    if (touch_fd < 0) return;
    touch_tapped = 0;
    struct input_event ev;
    int prev_down = touch_down;
    while (read(touch_fd, &ev, sizeof(ev)) == sizeof(ev)) {
        if (ev.type == EV_ABS) {
            /* Support both single-touch (ABS_X/Y) and multi-touch (ABS_MT_POSITION_X/Y) */
            if (ev.code == ABS_X  || ev.code == 0x35) touch_x = ev.value;
            if (ev.code == ABS_Y  || ev.code == 0x36) touch_y = ev.value;
        } else if (ev.type == EV_KEY && ev.code == 0x14a) { /* BTN_TOUCH */
            touch_down = ev.value;
        }
    }
    if (!prev_down && touch_down) touch_tapped = 1;
}

/* Returns 1 if last tap was inside this rectangle */
static int touch_in_rect(int x, int y, int w, int h)
{
    return touch_tapped && touch_x>=x && touch_x<x+w && touch_y>=y && touch_y<y+h;
}

/* ============================================================================
 * FRAMEBUFFER DRAWING
 * ========================================================================== */

static inline void fb_pixel(int x, int y, uint16_t col)
{
    if (x>=0 && x<DISP_W && y>=0 && y<DISP_H) fb[y][x] = col;
}

static void fb_fill(uint16_t col)
{
    for (int y=0;y<DISP_H;y++) for (int x=0;x<DISP_W;x++) fb[y][x]=col;
}

static void fb_rect(int x, int y, int w, int h, uint16_t col)
{
    for (int dy=0;dy<h;dy++) for (int dx=0;dx<w;dx++) fb_pixel(x+dx,y+dy,col);
}

static void fb_hline(int x, int y, int len, uint16_t col)
{
    for (int i=0;i<len;i++) fb_pixel(x+i,y,col);
}

static void fb_border(int x, int y, int w, int h, uint16_t col)
{
    fb_hline(x,y,w,col); fb_hline(x,y+h-1,w,col);
    for (int i=0;i<h;i++) { fb_pixel(x,y+i,col); fb_pixel(x+w-1,y+i,col); }
}

static int fb_char(int x, int y, char c, uint16_t fg, uint16_t bg, int s)
{
    if (c<0x20||c>0x7E) c='?';
    const uint8_t *g = FONT5X7[(uint8_t)c - 0x20];
    for (int col=0;col<5;col++) {
        uint8_t line=g[col];
        for (int row=0;row<7;row++)
            fb_rect(x+col*s, y+row*s, s, s, (line>>row)&1 ? fg : bg);
    }
    return x+(5+1)*s;
}

static int fb_str(int x, int y, const char *s, uint16_t fg, uint16_t bg, int sc)
{
    while (*s) x = fb_char(x,y,*s++,fg,bg,sc);
    return x;
}

static int fb_strw(const char *s, int sc) { return (int)strlen(s)*6*sc; }

static void fb_str_c(int y, const char *s, uint16_t fg, uint16_t bg, int sc)
{
    int x = (DISP_W - fb_strw(s,sc)) / 2;
    fb_str(x,y,s,fg,bg,sc);
}

/* Button helper — draw and return 1 if tapped */
static int ui_button(int x, int y, int w, int h, const char *label,
                     uint16_t bg, uint16_t fg)
{
    fb_rect(x,y,w,h,bg);
    fb_border(x,y,w,h,fg);
    int tx = x + (w - fb_strw(label,1))/2;
    int ty = y + (h - 7)/2;
    fb_str(tx,ty,label,fg,bg,1);
    return touch_in_rect(x, y, w, h);
}

/* ============================================================================
 * STATUS SCREEN
 * ========================================================================== */

static void disp_status(int cycle, int mb_ok, int mq_ok, int success, int total)
{
    if (!disp_ok) return;
    fb_fill(COL_BLUE);

    /* Header */
    fb_rect(0,0,DISP_W,36,COL_HDRBLUE);
    fb_str_c(6,  "AMSET AUTOMATION PVT LTD", COL_WHITE, COL_HDRBLUE, 2);
    fb_str_c(24, "STM32MP157F-DK2",   COL_GOLD,  COL_HDRBLUE, 1);

    /* Time + cycle */
    char buf[64]; time_t now = time(NULL);
    char hms[12]; strftime(hms,sizeof(hms),"%H:%M:%S",localtime(&now));
    char dts[16]; strftime(dts,sizeof(dts),"%Y-%m-%d",localtime(&now));
    snprintf(buf,sizeof(buf),"Cycle %d   %s  %s", cycle, dts, hms);
    fb_str_c(40, buf, COL_GRAY, COL_BLUE, 1);

    /* Status indicators */
    fb_rect(8,58,14,14,mb_ok?COL_GREEN:COL_RED);
    fb_str(28,60,mb_ok?"Modbus  : Connected":"Modbus  : ERROR",
           mb_ok?COL_GREEN:COL_RED,COL_BLUE,1);

    fb_rect(8,78,14,14,mq_ok?COL_GREEN:COL_ORANGE);
    fb_str(28,80,mq_ok?"MQTT    : Online   ":"MQTT    : Offline  ",
           mq_ok?COL_GREEN:COL_ORANGE,COL_BLUE,1);

    /* Port + broker info */
    fb_str(8,100,cfg.modbus_port,COL_DKGRAY,COL_BLUE,1);
    char bshort[32]; strncpy(bshort,cfg.mqtt_broker,31); bshort[20]=0;
    strcat(bshort,"...");
    fb_str(8,114,bshort,COL_DKGRAY,COL_BLUE,1);

    /* Progress bar */
    float pct = total>0 ? (float)success/total*100.0f : 0.0f;
    snprintf(buf,sizeof(buf),"Points: %d / %d  (%.0f%%)", success, total, pct);
    fb_str_c(134, buf, COL_GRAY, COL_BLUE, 1);
    fb_rect(8,148,DISP_W-16,14,COL_DKGRAY);
    int filled=(int)((DISP_W-16)*pct/100.0f);
    if (filled>0) fb_rect(8,148,filled,14,COL_GREEN);

    /* Divider */
    fb_hline(0,168,DISP_W,COL_DKGRAY);
    fb_str_c(172,"Live Register Values",COL_PURPLE,COL_BLUE,1);
    fb_hline(0,184,DISP_W,COL_DKGRAY);

    /* Live values — two columns, portrait has lots of space */
    int y=190, col=0, shown=0;
    for (int i=0; i<point_count && shown<40 && y<DISP_H-36; i++) {
        if (!points[i].valid) continue;
        int cx = col==0 ? 4 : DISP_W/2+4;
        char lbl[18]; strncpy(lbl,points[i].label,17); lbl[17]=0;
        fb_str(cx,y,lbl,COL_GRAY,COL_BLUE,1);
        snprintf(buf,sizeof(buf),"%d%s",points[i].value,points[i].unit);
        fb_str(cx+108,y,buf,COL_CYAN,COL_BLUE,1);
        col ^= 1;
        if (col==0) { fb_hline(4,y+11,DISP_W-8,COL_DKGRAY); y+=14; }
        shown++;
    }

    /* Settings button */
    if (ui_button(DISP_W/2-50,DISP_H-32,100,28,"SETTINGS",COL_HDRBLUE,COL_GOLD))
        cur_screen = SCREEN_SETTINGS;

    drm_flush();
}

/* ============================================================================
 * SETTINGS SCREEN
 * ========================================================================== */

typedef struct { const char *label; int *val; int step; int lo; int hi; } NumSetting;

static void disp_settings(void)
{
    if (!disp_ok) return;

    static NumSetting nums[] = {
        { "Interval (s)",  &cfg.interval,     10,  5,  3600 },
        { "Modbus Baud",   &cfg.modbus_baud,  2400,1200,115200 },
        { "Modbus Slave",  &cfg.modbus_slave, 1,   1,  247  },
        { "MQTT Port",     &cfg.mqtt_port,    1,   1,  65535},
    };
    static const int NNUM = 4;

    fb_fill(COL_BLUE);
    fb_rect(0,0,DISP_W,24,COL_HDRBLUE);
    fb_str_c(4,"SETTINGS",COL_GOLD,COL_HDRBLUE,2);

    /* Numeric settings with [-] [value] [+] — portrait layout */
    for (int i=0;i<NNUM;i++) {
        int ry = 50 + i*60;
        /* Label */
        fb_str_c(ry, nums[i].label, COL_GRAY, COL_BLUE, 1);
        char vbuf[24]; snprintf(vbuf,sizeof(vbuf),"%d",*nums[i].val);
        /* [-] button */
        if (ui_button(60,ry+14,60,32,"  -  ",COL_DKBLUE,COL_WHITE)) {
            *nums[i].val -= nums[i].step;
            if (*nums[i].val < nums[i].lo) *nums[i].val = nums[i].lo;
        }
        /* value box */
        fb_rect(130,ry+14,220,32,COL_DKGRAY);
        fb_border(130,ry+14,220,32,COL_GRAY);
        int tx = 130+(220-fb_strw(vbuf,2))/2;
        fb_str(tx,ry+22,vbuf,COL_WHITE,COL_DKGRAY,2);
        /* [+] button */
        if (ui_button(360,ry+14,60,32,"  +  ",COL_DKBLUE,COL_WHITE)) {
            *nums[i].val += nums[i].step;
            if (*nums[i].val > nums[i].hi) *nums[i].val = nums[i].hi;
        }
    }

    /* Read-only string settings */
    int sy = 50 + NNUM*60 + 20;
    fb_hline(0,sy,DISP_W,COL_DKGRAY); sy+=8;
    fb_str_c(sy,"Current Configuration",COL_PURPLE,COL_BLUE,1); sy+=16;
    fb_hline(0,sy,DISP_W,COL_DKGRAY); sy+=8;

    fb_str(8,sy,"MQTT Broker:",COL_GRAY,COL_BLUE,1); sy+=14;
    char bshort[48]; strncpy(bshort,cfg.mqtt_broker,47); bshort[47]=0;
    fb_str(16,sy,bshort,COL_CYAN,COL_BLUE,1); sy+=16;

    fb_str(8,sy,"MQTT User:",COL_GRAY,COL_BLUE,1);
    fb_str(80,sy,cfg.mqtt_user,COL_CYAN,COL_BLUE,1); sy+=14;

    fb_str(8,sy,"Modbus Port:",COL_GRAY,COL_BLUE,1);
    fb_str(84,sy,cfg.modbus_port,COL_CYAN,COL_BLUE,1); sy+=16;

    fb_hline(0,sy,DISP_W,COL_DKGRAY); sy+=8;
    fb_str_c(sy,"Edit broker/user/pass via SSH:",COL_DKGRAY,COL_BLUE,1); sy+=12;
    fb_str_c(sy,"  settings.conf",COL_ORANGE,COL_BLUE,1);

    /* SAVE / BACK buttons */
    if (ui_button(40,DISP_H-50,160,38,"  SAVE  ",COL_GREEN,COL_WHITE)) {
        settings_save();
        LOG_INFO("Settings saved");
        cur_screen = SCREEN_STATUS;
    }
    if (ui_button(DISP_W-200,DISP_H-50,160,38,"  BACK  ",COL_RED,COL_WHITE)) {
        settings_load();
        cur_screen = SCREEN_STATUS;
    }

    drm_flush();
}

/* ============================================================================
 * CSV PARSER
 * ========================================================================== */

static char *str_trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e=s+strlen(s)-1;
    while (e>s && isspace((unsigned char)*e)) *e--='\0';
    return s;
}

static int split_csv(char *line, char *cols[], int max)
{
    int n=0; char *p=line;
    while (n<max) { cols[n++]=p; char *c=strchr(p,','); if(!c) break; *c='\0'; p=c+1; }
    return n;
}

static int parse_csv(void)
{
    FILE *f=fopen(CONFIG_FILE,"r");
    if (!f) {
        LOG_WARN("'%s' not found — using 20 sample points",CONFIG_FILE);
        for (int i=0;i<20;i++) {
            snprintf(points[i].label,LABEL_MAX,"Point_%d",i+1);
            points[i].address=400+i; points[i].reg_type=REG_HOLDING;
            points[i].data_type='w'; points[i].unit[0]='\0'; points[i].valid=0;
        }
        point_count=20; return 1;
    }
    char line[512]; int first=1,idx=0;
    int cl=-1,ca=-1,cr=-1,cu=-1;
    while (fgets(line,sizeof(line),f) && idx<MAX_POINTS) {
        line[strcspn(line,"\r\n")]='\0';
        if (!strlen(line)) continue;
        char *cols[16]; int nc=split_csv(line,cols,16);
        if (first) {
            for (int c=0;c<nc;c++) {
                char tmp[64]; strncpy(tmp,cols[c],63);
                for (int i=0;tmp[i];i++) tmp[i]=tolower((unsigned char)tmp[i]);
                char *s=str_trim(tmp);
                if (!strcmp(s,"label")) cl=c;
                else if (!strcmp(s,"address")) ca=c;
                else if (strstr(s,"register")) cr=c;
                else if (!strcmp(s,"unit")) cu=c;
            }
            first=0;
            if (cl<0||ca<0) { fclose(f); return 0; }
            continue;
        }
        if (nc<=cl||nc<=ca) continue;
        char *lbl=str_trim(cols[cl]); if (!strlen(lbl)) continue;
        strncpy(points[idx].label,lbl,LABEL_MAX-1);
        char ab[32]; strncpy(ab,str_trim(cols[ca]),31);
        char *sp=strchr(ab,' '); if(sp)*sp='\0';
        points[idx].address=atoi(ab);
        points[idx].reg_type=REG_HOLDING;
        if (cr>=0&&cr<nc) {
            char rt[32]; strncpy(rt,cols[cr],31);
            for (int i=0;rt[i];i++) rt[i]=tolower((unsigned char)rt[i]);
            if (strstr(rt,"coil"))         points[idx].reg_type=REG_COIL;
            else if (strstr(rt,"discrete"))points[idx].reg_type=REG_DISCRETE;
            else if (strstr(rt,"input"))   points[idx].reg_type=REG_INPUT;
        }
        points[idx].data_type='w';
        points[idx].unit[0]='\0';
        if (cu>=0&&cu<nc) strncpy(points[idx].unit,str_trim(cols[cu]),UNIT_MAX-1);
        points[idx].valid=0; idx++;
    }
    fclose(f); point_count=idx;
    LOG_INFO("Loaded %d points from %s",point_count,CONFIG_FILE);
    return 1;
}

/* ============================================================================
 * MODBUS
 * ========================================================================== */

static int mb_connect(void)
{
    LOG_INFO("Modbus: %s @ %d baud slave %d", cfg.modbus_port, cfg.modbus_baud, cfg.modbus_slave);
    mb_ctx=modbus_new_rtu(cfg.modbus_port,cfg.modbus_baud,MODBUS_PARITY,
                          MODBUS_DATA_BITS,MODBUS_STOP_BITS);
    if (!mb_ctx) { LOG_ERROR("modbus_new_rtu: %s",modbus_strerror(errno)); return 0; }
    modbus_set_slave(mb_ctx,cfg.modbus_slave);
    modbus_set_response_timeout(mb_ctx,2,0);
    if (modbus_connect(mb_ctx)==-1) {
        LOG_ERROR("modbus_connect: %s",modbus_strerror(errno));
        modbus_free(mb_ctx); mb_ctx=NULL; return 0;
    }
    LOG_INFO("Modbus connected"); return 1;
}

static int read_point(ModbusPoint *pt)
{
    if (!mb_ctx) return 0;
    for (int retry=0;retry<MAX_RETRIES;retry++) {
        uint16_t reg=0; uint8_t bit=0; int rc=-1;
        switch(pt->reg_type) {
            case REG_COIL:     rc=modbus_read_bits(mb_ctx,pt->address,1,&bit);
                               if(rc==1){pt->value=bit;pt->valid=1;return 1;} break;
            case REG_DISCRETE: rc=modbus_read_input_bits(mb_ctx,pt->address,1,&bit);
                               if(rc==1){pt->value=bit;pt->valid=1;return 1;} break;
            case REG_INPUT:    rc=modbus_read_input_registers(mb_ctx,pt->address,1,&reg);
                               if(rc==1){pt->value=reg;pt->valid=1;return 1;} break;
            default:           rc=modbus_read_registers(mb_ctx,pt->address,1,&reg);
                               if(rc==1){pt->value=reg;pt->valid=1;return 1;} break;
        }
        usleep(100000);
    }
    pt->valid=0; return 0;
}

static void read_all_points(void)
{
    int s=0,f=0; time_t start=time(NULL);
    for (int i=0;i<point_count && running;i++) {
        if (read_point(&points[i])) s++; else f++;
        usleep(POINT_DELAY_US);
    }
    LOG_INFO("READ DONE — ok:%d fail:%d time:%ds",s,f,(int)(time(NULL)-start));
}

/* ============================================================================
 * SQLITE OFFLINE STORAGE
 * ========================================================================== */

static int db_init(void)
{
    if (sqlite3_open(MQTT_STORAGE_DB,&db)!=SQLITE_OK)
        { LOG_ERROR("DB: %s",sqlite3_errmsg(db)); return 0; }
    char *err=NULL;
    sqlite3_exec(db,"CREATE TABLE IF NOT EXISTS messages("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "timestamp INTEGER,topic TEXT,data TEXT,published INTEGER DEFAULT 0);",
        NULL,NULL,&err);
    if(err){LOG_ERROR("DB init: %s",err);sqlite3_free(err);return 0;}
    return 1;
}

static void db_store(const char *payload)
{
    if (!db) return;
    pthread_mutex_lock(&db_mutex);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,"INSERT INTO messages(timestamp,topic,data) VALUES(?,?,?);",
            -1,&st,NULL)==SQLITE_OK) {
        sqlite3_bind_int64(st,1,(long long)time(NULL)*1000);
        sqlite3_bind_text(st,2,MQTT_TOPIC,-1,SQLITE_STATIC);
        sqlite3_bind_text(st,3,payload,-1,SQLITE_STATIC);
        sqlite3_step(st); sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&db_mutex);
}

/* ============================================================================
 * JSON + MQTT
 * ========================================================================== */

static void build_payload(char *buf, size_t buflen)
{
    long long ts=(long long)time(NULL)*1000;
    int pos=snprintf(buf,buflen,"{\"ts\":%lld,\"values\":{",ts);
    int first=1;
    for (int i=0;i<point_count&&pos<(int)buflen-128;i++) {
        if (!points[i].valid) continue;
        if (!first) buf[pos++]=',';
        pos+=snprintf(buf+pos,buflen-pos,"\"%s\":%d",points[i].label,points[i].value);
        first=0;
    }
    snprintf(buf+pos,buflen-pos,"}}");
}

static void on_connect(struct mosquitto *m, void *ud, int rc)
{
    (void)m;(void)ud;
    mqtt_connected=(rc==0);
    if(rc==0) LOG_INFO("MQTT connected"); else LOG_ERROR("MQTT failed (code %d)",rc);
}

static void on_disconnect(struct mosquitto *m, void *ud, int rc)
{
    (void)m;(void)ud;(void)rc; mqtt_connected=0; LOG_WARN("MQTT disconnected");
}

static int mqtt_init(void)
{
    mosquitto_lib_init();
    char cid[64]; snprintf(cid,sizeof(cid),"modbus_%ld",(long)time(NULL));
    mosq=mosquitto_new(cid,true,NULL);
    if (!mosq) return 0;
    mosquitto_username_pw_set(mosq,cfg.mqtt_user,cfg.mqtt_pass);
    mosquitto_tls_set(mosq,"/etc/ssl/certs/ca-certificates.crt",NULL,NULL,NULL,NULL);
    mosquitto_tls_opts_set(mosq,1,NULL,NULL);
    mosquitto_connect_callback_set(mosq,on_connect);
    mosquitto_disconnect_callback_set(mosq,on_disconnect);
    if (mosquitto_connect(mosq,cfg.mqtt_broker,cfg.mqtt_port,60)!=MOSQ_ERR_SUCCESS) return 0;
    mosquitto_loop_start(mosq); sleep(2); return 1;
}

static void mqtt_publish(const char *payload)
{
    if (mqtt_connected) {
        if (mosquitto_publish(mosq,NULL,MQTT_TOPIC,(int)strlen(payload),payload,1,false)
                ==MOSQ_ERR_SUCCESS)
            LOG_INFO("Published %zu bytes",strlen(payload));
        else db_store(payload);
    } else db_store(payload);
}

/* ============================================================================
 * SIGNAL HANDLER
 * ========================================================================== */

static void handle_signal(int sig) { (void)sig; running=0; }

/* ============================================================================
 * MODBUS BACKGROUND THREAD
 * Reads all points, publishes MQTT, then sleeps the configured interval.
 * Runs independently of the display so the UI never freezes.
 * ========================================================================== */

static void *mb_thread_func(void *arg)
{
    (void)arg;
    static char payload[PAYLOAD_MAX];

    while (running) {
        /* Read all Modbus points (takes as long as it takes) */
        read_all_points();

        /* Count successes and update shared status counters atomically */
        int s = 0;
        for (int i = 0; i < point_count; i++) if (points[i].valid) s++;
        pthread_mutex_lock(&points_mutex);
        mb_ok_flag     = (mb_ctx != NULL);
        mb_success_cnt = s;
        mb_cycle++;
        pthread_mutex_unlock(&points_mutex);

        LOG_INFO("MB cycle %d done — %d/%d ok", mb_cycle, s, point_count);

        /* Publish */
        build_payload(payload, sizeof(payload));
        mqtt_publish(payload);

        /* Sleep the interval in 100ms chunks so we can exit cleanly */
        for (int t = 0; t < cfg.interval * 10 && running; t++)
            usleep(100000);
    }
    return NULL;
}

/* ============================================================================
 * MAIN
 * ========================================================================== */

int main(void)
{
    signal(SIGINT,handle_signal); signal(SIGTERM,handle_signal);

    /* Load settings */
    settings_load();

    printf("\n=== MODBUS RTU READER — STM32MP157F-DK2 ===\n");
    printf("  Port: %s @ %d  Slave: %d\n",cfg.modbus_port,cfg.modbus_baud,cfg.modbus_slave);
    printf("  MQTT: %s:%d\n",cfg.mqtt_broker,cfg.mqtt_port);
    printf("  Interval: %ds\n\n",cfg.interval);

    /* Init display */
    if (drm_init()==0) { disp_ok=1; fb_fill(COL_BLACK); drm_flush(); }
    else LOG_WARN("Display disabled — running headless");

    /* Welcome screen */
    if (disp_ok) {
        fb_fill(COL_BLUE);
        fb_rect(0,0,DISP_W,40,COL_HDRBLUE);
        fb_str_c(10,"AMSET",COL_GOLD,COL_HDRBLUE,3);
        fb_str_c(60,"Modbus RTU Reader",COL_WHITE,COL_BLUE,2);
        fb_str_c(90,"STM32MP157F-DK2",COL_GRAY,COL_BLUE,1);
        fb_str_c(110,"HiveMQ Cloud MQTT",COL_GRAY,COL_BLUE,1);
        fb_hline(20,130,DISP_W-40,COL_GOLD);
        fb_str_c(140,"Starting...",COL_GOLD,COL_BLUE,1);
        drm_flush(); sleep(2);
    }

    /* Init touch */
    touch_init();

    /* Load config */
    if (!parse_csv()) return 1;

    /* Connect Modbus */
    if (!mb_connect()) {
        if (disp_ok) {
            fb_fill(COL_BLUE);
            fb_str_c(DISP_H/2-10,"Modbus Connect FAILED",COL_RED,COL_BLUE,1);
            fb_str_c(DISP_H/2+6,cfg.modbus_port,COL_GRAY,COL_BLUE,1);
            drm_flush();
        }
        return 1;
    }

    /* Init DB + MQTT */
    db_init(); mqtt_init();

    /* Start Modbus background thread — display loop runs independently */
    pthread_create(&mb_thread_id, NULL, mb_thread_func, NULL);
    LOG_INFO("Modbus background thread started");

    /* -----------------------------------------------------------------------
     * Display loop — runs at ~10 fps regardless of Modbus timing.
     * Reads mb_* counters under mutex; point values are read-only here
     * (ARM 32-bit aligned loads are naturally atomic, so no per-point lock).
     * --------------------------------------------------------------------- */
    while (running) {
        /* Touch first so button presses are registered this frame */
        touch_poll();

        if (cur_screen == SCREEN_SETTINGS) {
            disp_settings();
        } else {
            int cyc, ok, succ;
            pthread_mutex_lock(&points_mutex);
            cyc  = mb_cycle;
            ok   = mb_ok_flag;
            succ = mb_success_cnt;
            pthread_mutex_unlock(&points_mutex);
            disp_status(cyc, ok, mqtt_connected, succ, point_count);
        }

        usleep(100000); /* 100ms → ~10 fps */
    }

    /* Cleanup */
    LOG_INFO("Shutting down...");
    pthread_join(mb_thread_id, NULL);
    if (disp_ok) { fb_fill(COL_BLACK); drm_flush(); }
    if (mb_ctx)  { modbus_close(mb_ctx); modbus_free(mb_ctx); }
    if (mosq)    { mosquitto_loop_stop(mosq,true); mosquitto_destroy(mosq); mosquitto_lib_cleanup(); }
    if (db)      sqlite3_close(db);
    if (touch_fd>=0) close(touch_fd);
    drm_cleanup();
    printf("\n=== STOPPED ===\n");
    return 0;
}