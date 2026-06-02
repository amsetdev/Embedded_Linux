/*
 * Modbus RTU Reader — HiveMQ Cloud MQTT + ILI9341 TFT Display
 *
 * Build:
 *   sudo apt install libmodbus-dev libmosquitto-dev libsqlite3-dev
 *   gcc -O2 -o main main.c -lmodbus -lmosquitto -lsqlite3 -lpthread
 *
 * Wiring (BCM GPIO):
 *   ILI9341 DC  → GPIO 18
 *   ILI9341 RST → GPIO 23
 *   ILI9341 SDA → SPI0 MOSI (GPIO 10)
 *   ILI9341 SCK → SPI0 SCLK (GPIO 11)  
 *   ILI9341 CS  → SPI0 CE0  (GPIO 8)
 *   ILI9341 VCC → 3.3 V,  GND → GND
 *
 * Run: ./main
 */

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
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>
#include <modbus/modbus.h>
#include <mosquitto.h>
#include <sqlite3.h>

/* ============================================================================
 * USER CONFIGURATION
 * ========================================================================== */

#define MODBUS_PORT       "modbus_uart"
#define MODBUS_BAUD       9600
#define MODBUS_SLAVE_ID   1
#define MODBUS_PARITY     'N'
#define MODBUS_DATA_BITS  8
#define MODBUS_STOP_BITS  1

#define CONFIG_FILE       "registers.csv"

#define MQTT_BROKER       "3c333a7049c6462bafb84074d95530fe.s1.eu.hivemq.cloud"
#define MQTT_PORT         8883
#define MQTT_TOPIC        "modbus/data"
#define MQTT_USERNAME     "prasad"
#define MQTT_PASSWORD     "prasad#12$A"
#define MQTT_STORAGE_DB   "mqtt_storage.db"

#define MONITORING_INTERVAL 300   /* seconds */
#define MAX_RETRIES         2
#define POINT_DELAY_US      10000
#define MAX_POINTS          2000
#define LABEL_MAX           64
#define UNIT_MAX            16
#define PAYLOAD_MAX         131072

/* Display (ILI9341) */
#define DISP_ENABLED      1
#define DISP_SPI_DEV      "/dev/spidev1.0" /* BBB SPI1 (P9_29/P9_31) */
#define DISP_SPI_HZ       16000000         /* BBB SPI max safe speed  */
#define DISP_DC_GPIO      48               /* P9_15 = GPIO1_16        */
#define DISP_RST_GPIO     60               /* P9_12 = GPIO1_28        */
#define DISP_W            240   /* 2.8" TFT portrait */
#define DISP_H            320
#define WELCOME_SECONDS   5

/* ============================================================================
 * EMBEDDED AMSET LOGO  (SVG source — rendered programmatically below)
 * To embed your own PNG: convert it to RGB565 with ImageMagick:
 *   convert amset.png -resize 320x240! -depth 8 rgb:- | \
 *   python3 -c "
 *   import sys,struct
 *   d=sys.stdin.buffer.read()
 *   out=[]
 *   for i in range(0,len(d),3):
 *       r,g,b=d[i],d[i+1],d[i+2]
 *       out.append(((r&0xF8)<<8)|((g&0xFC)<<3)|(b>>3))
 *   print('static const uint16_t LOGO[%d]={' % len(out))
 *   print(','.join(str(x) for x in out))
 *   print('};')
 *   " > logo.h
 * then #include "logo.h" and replace draw_logo() below.
 * ========================================================================== */

/* ============================================================================
 * COLOUR HELPERS  (RGB888 → RGB565, big-endian for ILI9341)
 * ========================================================================== */

#define RGB(r,g,b) ( (uint16_t)( (((r)&0xF8u)<<8) | (((g)&0xFCu)<<3) | ((b)>>3) ) )
#define BSWAP16(x) ( (uint16_t)(((x)>>8)|((x)<<8)) )

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

/* ============================================================================
 * 5×7 FONT  (Adafruit GFX glcdfont, 95 ASCII printable chars 0x20-0x7E)
 * Each entry: 5 bytes = 5 columns; bit0=top row, bit6=bottom row
 * ========================================================================== */

static const uint8_t FONT5X7[][5] = {
  {0x00,0x00,0x00,0x00,0x00}, /* 0x20 space */
  {0x00,0x00,0x5F,0x00,0x00}, /* 0x21 ! */
  {0x00,0x07,0x00,0x07,0x00}, /* 0x22 " */
  {0x14,0x7F,0x14,0x7F,0x14}, /* 0x23 # */
  {0x24,0x2A,0x7F,0x2A,0x12}, /* 0x24 $ */
  {0x23,0x13,0x08,0x64,0x62}, /* 0x25 % */
  {0x36,0x49,0x55,0x22,0x50}, /* 0x26 & */
  {0x00,0x05,0x03,0x00,0x00}, /* 0x27 ' */
  {0x00,0x1C,0x22,0x41,0x00}, /* 0x28 ( */
  {0x00,0x41,0x22,0x1C,0x00}, /* 0x29 ) */
  {0x14,0x08,0x3E,0x08,0x14}, /* 0x2A * */
  {0x08,0x08,0x3E,0x08,0x08}, /* 0x2B + */
  {0x00,0x50,0x30,0x00,0x00}, /* 0x2C , */
  {0x08,0x08,0x08,0x08,0x08}, /* 0x2D - */
  {0x00,0x60,0x60,0x00,0x00}, /* 0x2E . */
  {0x20,0x10,0x08,0x04,0x02}, /* 0x2F / */
  {0x3E,0x51,0x49,0x45,0x3E}, /* 0x30 0 */
  {0x00,0x42,0x7F,0x40,0x00}, /* 0x31 1 */
  {0x42,0x61,0x51,0x49,0x46}, /* 0x32 2 */
  {0x21,0x41,0x45,0x4B,0x31}, /* 0x33 3 */
  {0x18,0x14,0x12,0x7F,0x10}, /* 0x34 4 */
  {0x27,0x45,0x45,0x45,0x39}, /* 0x35 5 */
  {0x3C,0x4A,0x49,0x49,0x30}, /* 0x36 6 */
  {0x01,0x71,0x09,0x05,0x03}, /* 0x37 7 */
  {0x36,0x49,0x49,0x49,0x36}, /* 0x38 8 */
  {0x06,0x49,0x49,0x29,0x1E}, /* 0x39 9 */
  {0x00,0x36,0x36,0x00,0x00}, /* 0x3A : */
  {0x00,0x56,0x36,0x00,0x00}, /* 0x3B ; */
  {0x08,0x14,0x22,0x41,0x00}, /* 0x3C < */
  {0x14,0x14,0x14,0x14,0x14}, /* 0x3D = */
  {0x00,0x41,0x22,0x14,0x08}, /* 0x3E > */
  {0x02,0x01,0x51,0x09,0x06}, /* 0x3F ? */
  {0x32,0x49,0x79,0x41,0x3E}, /* 0x40 @ */
  {0x7E,0x11,0x11,0x11,0x7E}, /* 0x41 A */
  {0x7F,0x49,0x49,0x49,0x36}, /* 0x42 B */
  {0x3E,0x41,0x41,0x41,0x22}, /* 0x43 C */
  {0x7F,0x41,0x41,0x22,0x1C}, /* 0x44 D */
  {0x7F,0x49,0x49,0x49,0x41}, /* 0x45 E */
  {0x7F,0x09,0x09,0x09,0x01}, /* 0x46 F */
  {0x3E,0x41,0x49,0x49,0x7A}, /* 0x47 G */
  {0x7F,0x08,0x08,0x08,0x7F}, /* 0x48 H */
  {0x00,0x41,0x7F,0x41,0x00}, /* 0x49 I */
  {0x20,0x40,0x41,0x3F,0x01}, /* 0x4A J */
  {0x7F,0x08,0x14,0x22,0x41}, /* 0x4B K */
  {0x7F,0x40,0x40,0x40,0x40}, /* 0x4C L */
  {0x7F,0x02,0x0C,0x02,0x7F}, /* 0x4D M */
  {0x7F,0x04,0x08,0x10,0x7F}, /* 0x4E N */
  {0x3E,0x41,0x41,0x41,0x3E}, /* 0x4F O */
  {0x7F,0x09,0x09,0x09,0x06}, /* 0x50 P */
  {0x3E,0x41,0x51,0x21,0x5E}, /* 0x51 Q */
  {0x7F,0x09,0x19,0x29,0x46}, /* 0x52 R */
  {0x46,0x49,0x49,0x49,0x31}, /* 0x53 S */
  {0x01,0x01,0x7F,0x01,0x01}, /* 0x54 T */
  {0x3F,0x40,0x40,0x40,0x3F}, /* 0x55 U */
  {0x1F,0x20,0x40,0x20,0x1F}, /* 0x56 V */
  {0x3F,0x40,0x38,0x40,0x3F}, /* 0x57 W */
  {0x63,0x14,0x08,0x14,0x63}, /* 0x58 X */
  {0x07,0x08,0x70,0x08,0x07}, /* 0x59 Y */
  {0x61,0x51,0x49,0x45,0x43}, /* 0x5A Z */
  {0x00,0x7F,0x41,0x41,0x00}, /* 0x5B [ */
  {0x02,0x04,0x08,0x10,0x20}, /* 0x5C \ */
  {0x00,0x41,0x41,0x7F,0x00}, /* 0x5D ] */
  {0x04,0x02,0x01,0x02,0x04}, /* 0x5E ^ */
  {0x40,0x40,0x40,0x40,0x40}, /* 0x5F _ */
  {0x00,0x01,0x02,0x04,0x00}, /* 0x60 ` */
  {0x20,0x54,0x54,0x54,0x78}, /* 0x61 a */
  {0x7F,0x48,0x44,0x44,0x38}, /* 0x62 b */
  {0x38,0x44,0x44,0x44,0x20}, /* 0x63 c */
  {0x38,0x44,0x44,0x48,0x7F}, /* 0x64 d */
  {0x38,0x54,0x54,0x54,0x18}, /* 0x65 e */
  {0x08,0x7E,0x09,0x01,0x02}, /* 0x66 f */
  {0x0C,0x52,0x52,0x52,0x3E}, /* 0x67 g */
  {0x7F,0x08,0x04,0x04,0x78}, /* 0x68 h */
  {0x00,0x44,0x7D,0x40,0x00}, /* 0x69 i */
  {0x20,0x40,0x44,0x3D,0x00}, /* 0x6A j */
  {0x7F,0x10,0x28,0x44,0x00}, /* 0x6B k */
  {0x00,0x41,0x7F,0x40,0x00}, /* 0x6C l */
  {0x7C,0x04,0x18,0x04,0x78}, /* 0x6D m */
  {0x7C,0x08,0x04,0x04,0x78}, /* 0x6E n */
  {0x38,0x44,0x44,0x44,0x38}, /* 0x6F o */
  {0x7C,0x14,0x14,0x14,0x08}, /* 0x70 p */
  {0x08,0x14,0x14,0x18,0x7C}, /* 0x71 q */
  {0x7C,0x08,0x04,0x04,0x08}, /* 0x72 r */
  {0x48,0x54,0x54,0x54,0x20}, /* 0x73 s */
  {0x04,0x3F,0x44,0x40,0x20}, /* 0x74 t */
  {0x3C,0x40,0x40,0x20,0x7C}, /* 0x75 u */
  {0x1C,0x20,0x40,0x20,0x1C}, /* 0x76 v */
  {0x3C,0x40,0x30,0x40,0x3C}, /* 0x77 w */
  {0x44,0x28,0x10,0x28,0x44}, /* 0x78 x */
  {0x0C,0x50,0x50,0x50,0x3C}, /* 0x79 y */
  {0x44,0x64,0x54,0x4C,0x44}, /* 0x7A z */
  {0x00,0x08,0x36,0x41,0x00}, /* 0x7B { */
  {0x00,0x00,0x7F,0x00,0x00}, /* 0x7C | */
  {0x00,0x41,0x36,0x08,0x00}, /* 0x7D } */
  {0x10,0x08,0x08,0x10,0x08}, /* 0x7E ~ */
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

/* Display state */
static int               disp_ok        = 0;
static int               spi_fd         = -1;
static uint16_t          fb[DISP_H][DISP_W]; /* RGB565 framebuffer */

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
#define LOG_DEBUG(...) log_msg("DEBUG",   __VA_ARGS__)

/* ============================================================================
 * GPIO — via Linux sysfs  (no wiringPi needed)
 * ========================================================================== */

static int gpio_export(int pin)
{
    char path[64]; int fd;
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", pin);
    if (access(path, F_OK) == 0) return 0; /* already exported */
    fd = open("/sys/class/gpio/export", O_WRONLY);
    if (fd < 0) return -1;
    char buf[8]; int n = snprintf(buf, sizeof(buf), "%d", pin);
    write(fd, buf, n); close(fd);
    usleep(100000); /* wait for udev */
    return 0;
}

static int gpio_dir(int pin, const char *dir)
{
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", pin);
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    write(fd, dir, strlen(dir)); close(fd); return 0;
}

static int gpio_write(int pin, int val)
{
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", pin);
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    write(fd, val ? "1" : "0", 1); close(fd); return 0;
}

/* ============================================================================
 * SPI
 * ========================================================================== */

static int spi_open(void)
{
    spi_fd = open(DISP_SPI_DEV, O_RDWR);
    if (spi_fd < 0) { LOG_WARN("SPI open failed: %s", strerror(errno)); return -1; }

    uint8_t  mode  = SPI_MODE_0;
    uint8_t  bits  = 8;
    uint32_t speed = DISP_SPI_HZ;
    ioctl(spi_fd, SPI_IOC_WR_MODE,          &mode);
    ioctl(spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits);
    ioctl(spi_fd, SPI_IOC_WR_MAX_SPEED_HZ,  &speed);
    return 0;
}

static void spi_write_bytes(const uint8_t *data, size_t len)
{
    if (spi_fd < 0 || !data || !len) return;
    size_t chunk = 4096;
    for (size_t off = 0; off < len; off += chunk) {
        size_t n = (len - off < chunk) ? (len - off) : chunk;
        struct spi_ioc_transfer tr = {0};
        tr.tx_buf = (unsigned long)(data + off);
        tr.len    = (uint32_t)n;
        tr.speed_hz     = DISP_SPI_HZ;
        tr.bits_per_word = 8;
        ioctl(spi_fd, SPI_IOC_MESSAGE(1), &tr);
    }
}

/* ============================================================================
 * ILI9341 DRIVER
 * ========================================================================== */

static void ili_cmd(uint8_t cmd)
{
    gpio_write(DISP_DC_GPIO, 0);
    spi_write_bytes(&cmd, 1);
}

static void ili_data(const uint8_t *d, size_t n)
{
    gpio_write(DISP_DC_GPIO, 1);
    spi_write_bytes(d, n);
}

static void ili_data1(uint8_t b)  { ili_data(&b, 1); }

static void ili9341_init(void)
{
    /* Hard reset */
    gpio_write(DISP_RST_GPIO, 0); usleep(15000);
    gpio_write(DISP_RST_GPIO, 1); usleep(120000);

    ili_cmd(0x01); usleep(5000);   /* SWRESET */

    ili_cmd(0xCF); { uint8_t d[]={0x00,0xC1,0x30}; ili_data(d,3); }
    ili_cmd(0xED); { uint8_t d[]={0x64,0x03,0x12,0x81}; ili_data(d,4); }
    ili_cmd(0xE8); { uint8_t d[]={0x85,0x00,0x78}; ili_data(d,3); }
    ili_cmd(0xCB); { uint8_t d[]={0x39,0x2C,0x00,0x34,0x02}; ili_data(d,5); }
    ili_cmd(0xF7); ili_data1(0x20);
    ili_cmd(0xEA); { uint8_t d[]={0x00,0x00}; ili_data(d,2); }
    ili_cmd(0xC0); ili_data1(0x23);            /* Power control */
    ili_cmd(0xC1); ili_data1(0x10);            /* Power control 2 */
    ili_cmd(0xC5); { uint8_t d[]={0x3E,0x28}; ili_data(d,2); } /* VCOM */
    ili_cmd(0xC7); ili_data1(0x86);
    ili_cmd(0x36); ili_data1(0x48);            /* MADCTL: portrait MX|BGR */
    ili_cmd(0x3A); ili_data1(0x55);            /* Pixel format: 16-bit RGB565 */
    ili_cmd(0xB1); { uint8_t d[]={0x00,0x18}; ili_data(d,2); } /* Frame rate */
    ili_cmd(0xB6); { uint8_t d[]={0x08,0x82,0x27}; ili_data(d,3); }
    ili_cmd(0xF2); ili_data1(0x00);
    ili_cmd(0x26); ili_data1(0x01);            /* Gamma curve */
    ili_cmd(0xE0); { uint8_t d[]={0x0F,0x31,0x2B,0x0C,0x0E,0x08,0x4E,0xF1,
                                   0x37,0x07,0x10,0x03,0x0E,0x09,0x00}; ili_data(d,15); }
    ili_cmd(0xE1); { uint8_t d[]={0x00,0x0E,0x14,0x03,0x11,0x07,0x31,0xC1,
                                   0x48,0x08,0x0F,0x0C,0x31,0x36,0x0F}; ili_data(d,15); }
    ili_cmd(0x11); usleep(120000); /* Sleep out */
    ili_cmd(0x29);                 /* Display on */
    LOG_INFO("ILI9341 display initialised (%dx%d)", DISP_W, DISP_H);
}

/* Set pixel write window */
static void ili_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    ili_cmd(0x2A);
    uint8_t cx[]={ x0>>8, x0&0xFF, x1>>8, x1&0xFF }; ili_data(cx,4);
    ili_cmd(0x2B);
    uint8_t cy[]={ y0>>8, y0&0xFF, y1>>8, y1&0xFF }; ili_data(cy,4);
    ili_cmd(0x2C);
}

/* Flush entire framebuffer to display */
static void disp_flush(void)
{
    if (!disp_ok) return;
    ili_window(0, 0, DISP_W-1, DISP_H-1);
    gpio_write(DISP_DC_GPIO, 1);
    /* byte-swap each pixel (ILI9341 expects big-endian) */
    static uint8_t tx[DISP_W * DISP_H * 2];
    int i = 0;
    for (int y = 0; y < DISP_H; y++)
        for (int x = 0; x < DISP_W; x++) {
            uint16_t p = fb[y][x];
            tx[i++] = p >> 8;
            tx[i++] = p & 0xFF;
        }
    spi_write_bytes(tx, sizeof(tx));
}

/* ============================================================================
 * FRAMEBUFFER DRAWING
 * ========================================================================== */

static inline void fb_pixel(int x, int y, uint16_t col)
{
    if (x >= 0 && x < DISP_W && y >= 0 && y < DISP_H)
        fb[y][x] = col;
}

static void fb_fill(uint16_t col)
{
    for (int y = 0; y < DISP_H; y++)
        for (int x = 0; x < DISP_W; x++)
            fb[y][x] = col;
}

static void fb_rect(int x, int y, int w, int h, uint16_t col)
{
    for (int dy = 0; dy < h; dy++)
        for (int dx = 0; dx < w; dx++)
            fb_pixel(x+dx, y+dy, col);
}

static void fb_hline(int x, int y, int len, uint16_t col)
{
    for (int i = 0; i < len; i++) fb_pixel(x+i, y, col);
}

/* Draw one character at (x,y), scale s, return next x */
static int fb_char(int x, int y, char c, uint16_t fg, uint16_t bg, int s)
{
    if (c < 0x20 || c > 0x7E) c = '?';
    const uint8_t *g = FONT5X7[(uint8_t)c - 0x20];
    for (int col = 0; col < 5; col++) {
        uint8_t line = g[col];
        for (int row = 0; row < 7; row++) {
            uint16_t pix = (line >> row) & 1 ? fg : bg;
            fb_rect(x + col*s, y + row*s, s, s, pix);
        }
    }
    return x + (5+1)*s;
}

/* Draw string; returns x after last char */
static int fb_str(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale)
{
    while (*s) x = fb_char(x, y, *s++, fg, bg, scale);
    return x;
}

/* String width in pixels */
static int fb_strw(const char *s, int scale) { return (int)strlen(s) * 6 * scale; }

/* Centre a string horizontally */
static void fb_str_c(int y, const char *s, uint16_t fg, uint16_t bg, int scale)
{
    int x = (DISP_W - fb_strw(s, scale)) / 2;
    fb_str(x, y, s, fg, bg, scale);
}

/* Dim all pixels in fb by factor/256 */
static void fb_dim(int factor)
{
    for (int y = 0; y < DISP_H; y++)
        for (int x = 0; x < DISP_W; x++) {
            uint16_t p = fb[y][x];
            uint8_t r = ((p >> 11) & 0x1F) * factor / 256;
            uint8_t g = ((p >>  5) & 0x3F) * factor / 256;
            uint8_t b = ( p        & 0x1F) * factor / 256;
            fb[y][x]  = (uint16_t)((r<<11)|(g<<5)|b);
        }
}

/* ============================================================================
 * AMSET LOGO — drawn programmatically into a separate buffer
 * ========================================================================== */

static uint16_t logo_buf[DISP_H][DISP_W];

static void draw_logo(void)
{
    /* Dark navy gradient background */
    for (int y = 0; y < DISP_H; y++) {
        uint8_t shade = (uint8_t)(5 + y * 10 / DISP_H);
        uint16_t col  = RGB(shade, shade*2, shade*6);
        for (int x = 0; x < DISP_W; x++) logo_buf[y][x] = col;
    }

    /* Gold top/bottom bars */
    for (int y = 0; y < 6; y++)
        for (int x = 0; x < DISP_W; x++) logo_buf[y][x] = COL_GOLD;
    for (int y = DISP_H-6; y < DISP_H; y++)
        for (int x = 0; x < DISP_W; x++) logo_buf[y][x] = COL_GOLD;

    /* Copy into fb so drawing helpers work */
    memcpy(fb, logo_buf, sizeof(fb));

    /* "AMSET" scale 3 → 18px tall, centred vertically around y=115 */
    int tx = (DISP_W - fb_strw("AMSET", 3)) / 2;
    fb_str(tx, 105, "AMSET", COL_GOLD, RGB(0,0,0), 3);

    /* Subtitle scale 2 */
    fb_str_c(138, "Modbus RTU", COL_GRAY, RGB(0,0,0), 2);
    fb_str_c(154, "Reader", COL_GRAY, RGB(0,0,0), 2);

    /* Gold accent lines */
    for (int x = 20; x < DISP_W-20; x++) {
        fb[175][x] = COL_GOLD;
        fb[176][x] = COL_GOLD;
    }

    /* Bottom label */
    fb_str_c(185, "HiveMQ Cloud MQTT", RGB(70,90,140), RGB(0,0,0), 1);
    fb_str_c(200, MQTT_BROKER+0, RGB(50,65,100), RGB(0,0,0), 1); /* broker hint */

    memcpy(logo_buf, fb, sizeof(fb));
}

/* ============================================================================
 * WELCOME ANIMATION — slide left→right with brightness glow
 * ========================================================================== */

static void disp_welcome(void)
{
    if (!disp_ok) return;

    draw_logo();

    int   FRAMES      = 35;
    float slide_time  = 0.7f;
    int   delay_us    = (int)(slide_time / FRAMES * 1e6f);

    /* Slide in */
    for (int f = 0; f <= FRAMES; f++) {
        float t      = (float)f / FRAMES;
        float eased  = 1.0f - (1.0f-t)*(1.0f-t)*(1.0f-t); /* ease-out cubic */
        int   x_off  = (int)(-(float)DISP_W + (float)DISP_W * eased);
        int   bright = (int)(80 + 176 * t);  /* 80..256 → dim to full */

        fb_fill(COL_BLACK);
        /* Paste logo shifted by x_off */
        for (int y = 0; y < DISP_H; y++)
            for (int x = 0; x < DISP_W; x++) {
                int src = x - x_off;
                if (src >= 0 && src < DISP_W)
                    fb[y][x] = logo_buf[y][src];
            }
        fb_dim(bright);
        disp_flush();
        usleep(delay_us);
    }

    /* Hold */
    memcpy(fb, logo_buf, sizeof(fb));
    disp_flush();
    sleep(WELCOME_SECONDS);

    /* Fade out */
    int steps[]  = {210, 160, 100, 50, 10, 0};
    for (int i = 0; i < 6; i++) {
        memcpy(fb, logo_buf, sizeof(fb));
        fb_dim(steps[i]);
        disp_flush();
        usleep(80000);
    }
    fb_fill(COL_BLACK);
    disp_flush();
}

/* ============================================================================
 * STATUS SCREEN
 * ========================================================================== */

static void disp_status(int cycle, int mb_ok, int mq_ok,
                         int success, int total)
{
    if (!disp_ok) return;

    fb_fill(COL_BLUE);

    /* ── Header bar ─────────────────────────────────────── */
    fb_rect(0, 0, DISP_W, 22, COL_HDRBLUE);
    fb_str_c(4, "MODBUS RTU READER", COL_WHITE, COL_HDRBLUE, 1);

    /* Time on second line */
    char ts[16]; time_t now = time(NULL);
    strftime(ts, sizeof(ts), "%Y-%m-%d", localtime(&now));
    fb_str_c(16, ts, RGB(150,180,220), COL_HDRBLUE, 1);

    /* ── Cycle & clock ──────────────────────────────────── */
    char buf[48];
    char hms[12]; strftime(hms, sizeof(hms), "%H:%M:%S", localtime(&now));
    snprintf(buf, sizeof(buf), "Cycle %d   %s", cycle, hms);
    fb_str_c(30, buf, COL_GRAY, COL_BLUE, 1);

    /* ── Status indicators ──────────────────────────────── */
    fb_rect(6,  46, 10, 10, mb_ok ? COL_GREEN : COL_RED);
    fb_str(22, 47, mb_ok ? "Modbus : Connected" : "Modbus : ERROR",
           mb_ok ? COL_GREEN : COL_RED, COL_BLUE, 1);

    fb_rect(6,  62, 10, 10, mq_ok ? COL_GREEN : COL_ORANGE);
    fb_str(22, 63, mq_ok ? "MQTT   : Online   " : "MQTT   : Offline  ",
           mq_ok ? COL_GREEN : COL_ORANGE, COL_BLUE, 1);

    /* ── Point count + progress bar ─────────────────────── */
    float pct = total > 0 ? (float)success / total * 100.0f : 0.0f;
    snprintf(buf, sizeof(buf), "Points: %d / %d  (%.0f%%)", success, total, pct);
    fb_str_c(78, buf, COL_GRAY, COL_BLUE, 1);

    fb_rect(6, 92, DISP_W-12, 10, COL_DKGRAY);
    int filled = (int)((DISP_W-12) * pct / 100.0f);
    if (filled > 0) fb_rect(6, 92, filled, 10, COL_GREEN);

    /* ── Divider ─────────────────────────────────────────── */
    fb_hline(0, 108, DISP_W, COL_DKGRAY);
    fb_str_c(112, "Live Register Values", COL_PURPLE, COL_BLUE, 1);
    fb_hline(0, 124, DISP_W, COL_DKGRAY);

    /* ── Live values (portrait gives us ~12 rows) ────────── */
    int y = 130, shown = 0;
    for (int i = 0; i < point_count && shown < 12 && y < DISP_H - 12; i++) {
        if (!points[i].valid) continue;
        char lbl[18]; strncpy(lbl, points[i].label, 17); lbl[17] = '\0';
        fb_str(4, y, lbl, COL_GRAY, COL_BLUE, 1);
        snprintf(buf, sizeof(buf), "%d %s", points[i].value, points[i].unit);
        fb_str(DISP_W - 6*(int)strlen(buf) - 4, y, buf, COL_CYAN, COL_BLUE, 1);
        fb_hline(4, y+11, DISP_W-8, RGB(20,20,50));
        y += 14; shown++;
    }

    disp_flush();
}

/* Simple centred two-line message */
static void disp_message(const char *l1, const char *l2, uint16_t col)
{
    if (!disp_ok) return;
    fb_fill(COL_BLUE);
    fb_rect(0, 0, DISP_W, 22, COL_HDRBLUE);
    fb_str_c(5, "MODBUS RTU READER", COL_WHITE, COL_HDRBLUE, 1);
    fb_str_c(DISP_H/2 - 14, l1, col,      COL_BLUE, 2);
    fb_str_c(DISP_H/2 + 8,  l2, COL_GRAY, COL_BLUE, 1);
    disp_flush();
}

/* ============================================================================
 * DISPLAY INIT
 * ========================================================================== */

static void disp_init(void)
{
#if DISP_ENABLED
    if (spi_open() < 0) return;
    gpio_export(DISP_DC_GPIO);  gpio_dir(DISP_DC_GPIO,  "out");
    gpio_export(DISP_RST_GPIO); gpio_dir(DISP_RST_GPIO, "out");
    ili9341_init();
    disp_ok = 1;
#endif
}

/* ============================================================================
 * CSV PARSER
 * ========================================================================== */

#define MAX_COLS 16

static int split_csv(char *line, char *cols[], int max)
{
    int n = 0; char *p = line;
    while (n < max) {
        cols[n++] = p;
        char *c = strchr(p, ',');
        if (!c) break;
        *c = '\0'; p = c + 1;
    }
    return n;
}

static char *strtrim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s) - 1;
    while (e > s && isspace((unsigned char)*e)) *e-- = '\0';
    return s;
}

static int parse_csv(void)
{
    FILE *f = fopen(CONFIG_FILE, "r");
    if (!f) {
        LOG_WARN("'%s' not found — using 100 sample points", CONFIG_FILE);
        for (int i = 0; i < 100; i++) {
            snprintf(points[i].label, LABEL_MAX, "Sample_Point_%d", i+1);
            points[i].address   = 400 + i;  /* reads holding registers 400..499 */
            points[i].reg_type  = REG_HOLDING;
            points[i].data_type = 'w';
            points[i].unit[0]   = '\0';
            points[i].valid     = 0;
        }
        point_count = 100;

        /* Print the address table so user can see exactly what will be read */
        printf("\n  %-4s  %-24s  %-8s  %-10s\n", "No.", "Label", "Address", "Type");
        printf("  %s\n", "----  ------------------------  --------  ----------");
        for (int i = 0; i < point_count; i++)
            printf("  %-4d  %-24s  %-8d  %s\n",
                   i+1, points[i].label, points[i].address, "holding");
        printf("\n  Total: %d addresses  (range %d – %d)\n\n",
               point_count,
               points[0].address,
               points[point_count-1].address);

        return 1;
    }

    char line[512]; int first = 1, idx = 0;
    int cl = -1, ca = -1, cr = -1, cd = -1, cu = -1;

    while (fgets(line, sizeof(line), f) && idx < MAX_POINTS) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!strlen(line)) continue;
        char *cols[MAX_COLS]; int nc = split_csv(line, cols, MAX_COLS);

        if (first) {
            for (int c = 0; c < nc; c++) {
                char tmp[64]; strncpy(tmp, cols[c], 63);
                for (int i = 0; tmp[i]; i++) tmp[i] = tolower((unsigned char)tmp[i]);
                char *s = strtrim(tmp);
                if (!strcmp(s,"label"))        cl = c;
                else if (!strcmp(s,"address")) ca = c;
                else if (strstr(s,"register")) cr = c;
                else if (strstr(s,"data"))     cd = c;
                else if (!strcmp(s,"unit"))    cu = c;
            }
            first = 0;
            if (cl < 0 || ca < 0) { LOG_ERROR("CSV needs Label+Address columns"); fclose(f); return 0; }
            continue;
        }

        if (nc <= cl || nc <= ca) continue;
        char *lbl = strtrim(cols[cl]);
        if (!strlen(lbl)) continue;

        strncpy(points[idx].label, lbl, LABEL_MAX-1);
        char ab[32]; strncpy(ab, strtrim(cols[ca]), 31);
        char *sp = strchr(ab,' '); if (sp) *sp='\0';
        points[idx].address = atoi(ab);

        points[idx].reg_type  = REG_HOLDING;
        if (cr >= 0 && cr < nc) {
            char rt[32]; strncpy(rt, cols[cr], 31);
            for (int i=0;rt[i];i++) rt[i]=tolower((unsigned char)rt[i]);
            if (strstr(rt,"coil"))          points[idx].reg_type = REG_COIL;
            else if (strstr(rt,"discrete")) points[idx].reg_type = REG_DISCRETE;
            else if (strstr(rt,"input"))    points[idx].reg_type = REG_INPUT;
        }
        points[idx].data_type = 'w';
        if (cd >= 0 && cd < nc && strlen(cols[cd]))
            points[idx].data_type = (char)tolower((unsigned char)cols[cd][0]);
        points[idx].unit[0] = '\0';
        if (cu >= 0 && cu < nc) strncpy(points[idx].unit, strtrim(cols[cu]), UNIT_MAX-1);
        points[idx].valid = 0;
        idx++;
    }
    fclose(f);
    point_count = idx;
    LOG_INFO("Loaded %d points from %s", point_count, CONFIG_FILE);

    printf("\n  %-4s  %-24s  %-8s  %-10s  %-6s\n",
           "No.", "Label", "Address", "Type", "Unit");
    printf("  %s\n", "----  ------------------------  --------  ----------  ------");
    for (int i = 0; i < point_count; i++) {
        const char *rtype =
            points[i].reg_type == REG_COIL     ? "coil"     :
            points[i].reg_type == REG_DISCRETE  ? "discrete" :
            points[i].reg_type == REG_INPUT     ? "input"    : "holding";
        printf("  %-4d  %-24s  %-8d  %-10s  %s\n",
               i+1, points[i].label, points[i].address, rtype, points[i].unit);
    }
    printf("\n  Total: %d addresses\n\n", point_count);
    return 1;
}

/* ============================================================================
 * MODBUS
 * ========================================================================== */

static int mb_connect(void)
{
    LOG_INFO("Modbus: connecting to %s @ %d baud (slave %d)...",
             cfg.modbus_port, cfg.modbus_baud, cfg.modbus_slave);
    mb_ctx = modbus_new_rtu(cfg.modbus_port, cfg.modbus_baud, MODBUS_PARITY,
                            MODBUS_DATA_BITS, MODBUS_STOP_BITS);
    if (!mb_ctx) { LOG_ERROR("modbus_new_rtu: %s", modbus_strerror(errno)); return 0; }
    modbus_set_slave(mb_ctx, cfg.modbus_slave);
    modbus_set_response_timeout(mb_ctx, 2, 0);
    if (modbus_connect(mb_ctx) == -1) {
        LOG_ERROR("modbus_connect: %s", modbus_strerror(errno));
        modbus_free(mb_ctx); mb_ctx = NULL; return 0;
    }
    LOG_INFO("Modbus connected");
    return 1;
}

static int read_point(ModbusPoint *pt)
{
    if (!mb_ctx) return 0;
    for (int retry = 0; retry < MAX_RETRIES; retry++) {
        int rc = -1; uint16_t reg = 0; uint8_t bit = 0;
        switch (pt->reg_type) {
            case REG_COIL:
                rc = modbus_read_bits(mb_ctx, pt->address, 1, &bit);
                if (rc==1){ pt->value=bit; pt->valid=1; return 1; } break;
            case REG_DISCRETE:
                rc = modbus_read_input_bits(mb_ctx, pt->address, 1, &bit);
                if (rc==1){ pt->value=bit; pt->valid=1; return 1; } break;
            case REG_INPUT:
                rc = modbus_read_input_registers(mb_ctx, pt->address, 1, &reg);
                if (rc==1){ pt->value=reg; pt->valid=1; return 1; } break;
            default:
                rc = modbus_read_registers(mb_ctx, pt->address, 1, &reg);
                if (rc==1){ pt->value=reg; pt->valid=1; return 1; } break;
        }
        usleep(100000);
    }
    pt->valid = 0; return 0;
}

static void read_all_points(void)
{
    int s=0, f=0; time_t start = time(NULL);
    LOG_INFO("Reading %d points from slave %d", point_count, cfg.modbus_slave);
    for (int i=0; i<point_count; i++) {
        if (read_point(&points[i])) s++; else f++;
        usleep(POINT_DELAY_US);
        if ((i+1)%10==0 || i==point_count-1) {
            int el = (int)(time(NULL)-start);
            float sp = el>0 ? (float)(i+1)/el : 0;
            LOG_INFO("Progress: %d/%d (%.0f%%) | %.1f pts/sec",
                     i+1, point_count, (float)(i+1)/point_count*100, sp);
        }
    }
    LOG_INFO("READ DONE — success:%d fail:%d time:%ds", s, f, (int)(time(NULL)-start));
}

/* ============================================================================
 * SQLITE OFFLINE STORAGE
 * ========================================================================== */

static int db_init(void)
{
    if (sqlite3_open(MQTT_STORAGE_DB, &db) != SQLITE_OK)
        { LOG_ERROR("DB open: %s", sqlite3_errmsg(db)); return 0; }
    const char *sql =
        "CREATE TABLE IF NOT EXISTS messages("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "timestamp INTEGER, topic TEXT, data TEXT, published INTEGER DEFAULT 0);";
    char *err=NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK)
        { LOG_ERROR("DB init: %s", err); sqlite3_free(err); return 0; }
    LOG_INFO("Offline storage: %s", MQTT_STORAGE_DB);
    return 1;
}

static void db_store(const char *payload)
{
    if (!db) return;
    pthread_mutex_lock(&db_mutex);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO messages(timestamp,topic,data) VALUES(?,?,?);",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st,1,(long long)time(NULL)*1000);
        sqlite3_bind_text(st,2,MQTT_TOPIC,-1,SQLITE_STATIC);
        sqlite3_bind_text(st,3,payload,-1,SQLITE_STATIC);
        sqlite3_step(st); sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&db_mutex);
}

static void db_publish_stored(void);  /* forward decl */

static void db_cleanup(void)
{
    if (!db) return;
    pthread_mutex_lock(&db_mutex);
    char sql[128];
    long long wa = ((long long)time(NULL) - 7*24*3600)*1000;
    snprintf(sql,sizeof(sql),
             "DELETE FROM messages WHERE published=1 AND timestamp<%lld;", wa);
    sqlite3_exec(db, sql, NULL, NULL, NULL);
    pthread_mutex_unlock(&db_mutex);
}

/* ============================================================================
 * JSON PAYLOAD
 * ========================================================================== */

static void build_payload(char *buf, size_t buflen)
{
    long long ts = (long long)time(NULL)*1000;
    int pos = snprintf(buf, buflen, "{\"ts\":%lld,\"values\":{", ts);
    int first = 1;
    for (int i=0; i<point_count && pos<(int)buflen-128; i++) {
        if (!points[i].valid) continue;
        if (!first) buf[pos++]=',';
        pos += snprintf(buf+pos, buflen-pos, "\"%s\":%d",
                        points[i].label, points[i].value);
        first = 0;
    }
    snprintf(buf+pos, buflen-pos, "}}");
}

/* ============================================================================
 * MQTT
 * ========================================================================== */

static void on_connect(struct mosquitto *m, void *ud, int rc)
{
    (void)m; (void)ud;
    if (rc==0) {
        mqtt_connected=1;
        LOG_INFO("MQTT connected: %s:%d", MQTT_BROKER, MQTT_PORT);
        db_publish_stored();
    } else {
        mqtt_connected=0; LOG_ERROR("MQTT connect failed (code %d)", rc);
    }
}

static void on_disconnect(struct mosquitto *m, void *ud, int rc)
{
    (void)m; (void)ud; (void)rc;
    mqtt_connected=0; LOG_WARN("MQTT disconnected");
}

static void db_publish_stored(void)
{
    if (!db || !mqtt_connected) return;
    pthread_mutex_lock(&db_mutex);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT id,data FROM messages WHERE published=0 ORDER BY timestamp;",
            -1, &st, NULL) != SQLITE_OK)
        { pthread_mutex_unlock(&db_mutex); return; }
    int cnt=0;
    while (sqlite3_step(st)==SQLITE_ROW) {
        long long id = sqlite3_column_int64(st,0);
        const char *d = (const char*)sqlite3_column_text(st,1);
        if (!d) continue;
        if (mosquitto_publish(mosq,NULL,MQTT_TOPIC,(int)strlen(d),d,1,false)==MOSQ_ERR_SUCCESS) {
            char upd[64];
            snprintf(upd,sizeof(upd),"UPDATE messages SET published=1 WHERE id=%lld;",id);
            sqlite3_exec(db,upd,NULL,NULL,NULL); cnt++;
        } else break;
    }
    sqlite3_finalize(st);
    if (cnt>0) LOG_INFO("Published %d stored messages", cnt);
    pthread_mutex_unlock(&db_mutex);
}

static int mqtt_init(void)
{
    mosquitto_lib_init();
    char cid[64]; snprintf(cid,sizeof(cid),"modbus_%ld",(long)time(NULL));
    mosq = mosquitto_new(cid, true, NULL);
    if (!mosq) { LOG_ERROR("mosquitto_new failed"); return 0; }
    mosquitto_username_pw_set(mosq, MQTT_USERNAME, MQTT_PASSWORD);
    mosquitto_tls_set(mosq, "/etc/ssl/certs/ca-certificates.crt", NULL, NULL, NULL, NULL);
    mosquitto_tls_opts_set(mosq, 1, NULL, NULL);
    mosquitto_connect_callback_set(mosq, on_connect);
    mosquitto_disconnect_callback_set(mosq, on_disconnect);
    int rc = mosquitto_connect(mosq, MQTT_BROKER, MQTT_PORT, 60);
    if (rc!=MOSQ_ERR_SUCCESS)
        { LOG_ERROR("MQTT connect: %s", mosquitto_strerror(rc)); return 0; }
    mosquitto_loop_start(mosq);
    sleep(2); return 1;
}

static void mqtt_publish(const char *payload)
{
    if (mqtt_connected) {
        int rc = mosquitto_publish(mosq,NULL,MQTT_TOPIC,
                                   (int)strlen(payload),payload,1,false);
        if (rc==MOSQ_ERR_SUCCESS)
            LOG_INFO("Published %zu bytes to %s", strlen(payload), MQTT_TOPIC);
        else { LOG_WARN("Publish failed — storing offline"); db_store(payload); }
    } else {
        LOG_WARN("MQTT offline — storing locally");
        db_store(payload);
    }
}

/* ============================================================================
 * SIGNAL HANDLER
 * ========================================================================== */

static void handle_signal(int sig) { (void)sig; running=0; }

/* ============================================================================
 * MAIN
 * ========================================================================== */

int main(void)
{
    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);

    settings_load();

    printf("\n================================================================================\n");
    printf("  MODBUS RTU READER — ThingsBoard Cloud + ILI9341 Display\n");
    printf("================================================================================\n");
    printf("  Port    : %s @ %d baud  Slave: %d\n", cfg.modbus_port, cfg.modbus_baud, cfg.modbus_slave);
    printf("  Config  : %s\n", CONFIG_FILE);
    printf("  Broker  : %s:%d  Topic: %s\n", cfg.mqtt_broker, cfg.mqtt_port, cfg.mqtt_topic);
    printf("  Interval: %d s\n\n", cfg.interval);

    /* 1. Init display + play welcome animation */
    disp_init();
    disp_welcome();

    /* 2. Load register config */
    disp_message("Loading config...", CONFIG_FILE, COL_GOLD);
    if (!parse_csv()) {
        disp_message("ERROR", "Config load failed", COL_RED);
        return 1;
    }

    /* 3. Connect Modbus */
    disp_message("Connecting Modbus...", cfg.modbus_port, COL_GOLD);
    if (!mb_connect()) {
        disp_message("ERROR", "Modbus connect failed", COL_RED);
        return 1;
    }

    /* 4. Init DB */
    db_init();

    /* 5. Connect MQTT */
    disp_message("Connecting MQTT...", cfg.mqtt_broker, COL_GOLD);
    mqtt_init();

    /* 6. Main monitoring loop */
    static char payload[PAYLOAD_MAX];
    int cycle = 0;

    while (running) {
        cycle++;
        time_t now = time(NULL);
        char tbuf[32]; strftime(tbuf,sizeof(tbuf),"%Y-%m-%d %H:%M:%S",localtime(&now));
        LOG_INFO("===== CYCLE %d — %s =====", cycle, tbuf);

        read_all_points();

        int success = 0;
        for (int i=0; i<point_count; i++) if (points[i].valid) success++;

        /* Update display with live results */
        disp_status(cycle, mb_ctx != NULL, mqtt_connected, success, point_count);

        /* Build and publish payload */
        build_payload(payload, sizeof(payload));
        mqtt_publish(payload);
        db_cleanup();

        if (running) {
            LOG_INFO("Next cycle in %d seconds...", cfg.interval);
            for (int s=0; s<cfg.interval && running; s++) sleep(1);
        }
    }

    /* Cleanup */
    LOG_INFO("Shutting down...");
    disp_message("Shutting down...", "Goodbye", COL_GRAY);

    if (mb_ctx)  { modbus_close(mb_ctx); modbus_free(mb_ctx); }
    if (mosq)    { mosquitto_loop_stop(mosq,true); mosquitto_destroy(mosq); mosquitto_lib_cleanup(); }
    if (db)      sqlite3_close(db);
    if (spi_fd >= 0) close(spi_fd);

    fb_fill(COL_BLACK); if (disp_ok) disp_flush();

    printf("\n================================================================================\n");
    printf("  STOPPED\n");
    printf("================================================================================\n");
    return 0;
}