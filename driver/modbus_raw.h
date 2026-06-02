/**
 * modbus_raw.h — Raw Modbus RTU over /dev/modbus_uart
 * Drop-in replacement for libmodbus calls.
 * Include this header directly — no .c file needed.
 */
#ifndef MODBUS_RAW_H
#define MODBUS_RAW_H

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

/* ── CRC-16 Modbus ───────────────────────────────────────────────────────── */
static inline uint16_t mb_crc16(const uint8_t *buf, int len)
{
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1) ? ((crc >> 1) ^ 0xA001) : (crc >> 1);
    }
    return crc;
}

/* ── Write full frame ────────────────────────────────────────────────────── */
static inline int mb_write_all(int fd, const uint8_t *buf, int len)
{
    int written = 0;
    while (written < len) {
        int n = write(fd, buf + written, len - written);
        if (n <= 0) return -1;
        written += n;
    }
    return written;
}

/* ── Read response with timeout (ms) ────────────────────────────────────── */
static inline int mb_read_response(int fd, uint8_t *buf, int maxlen, int timeout_ms)
{
    int total = 0;
    struct timeval tv;
    fd_set fds;

    while (total < maxlen) {
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        int r = select(fd + 1, &fds, NULL, NULL, &tv);
        if (r <= 0) break;   /* timeout or error */

        int n = read(fd, buf + total, maxlen - total);
        if (n <= 0) break;
        total += n;

        /* Check if we have a complete Modbus response */
        if (total >= 5) {
            /* FC03/04: expect 3 + bytecount + 2 CRC bytes */
            if (buf[1] == 0x03 || buf[1] == 0x04) {
                int expected = 3 + buf[2] + 2;
                if (total >= expected) break;
            }
            /* FC01/02: coils */
            if (buf[1] == 0x01 || buf[1] == 0x02) {
                int expected = 3 + buf[2] + 2;
                if (total >= expected) break;
            }
            /* Exception response */
            if (buf[1] & 0x80) { if (total >= 5) break; }
        }
    }
    return total;
}

/* ── Read holding registers (FC03) ──────────────────────────────────────── */
/* Returns number of registers read, or -1 on error */
static inline int mb_read_registers(int fd, int slave, int addr,
                                     int qty, uint16_t *regs)
{
    uint8_t req[8];
    req[0] = (uint8_t)slave;
    req[1] = 0x03;
    req[2] = (uint8_t)(addr >> 8);
    req[3] = (uint8_t)(addr & 0xFF);
    req[4] = (uint8_t)(qty >> 8);
    req[5] = (uint8_t)(qty & 0xFF);
    uint16_t crc = mb_crc16(req, 6);
    req[6] = (uint8_t)(crc & 0xFF);
    req[7] = (uint8_t)(crc >> 8);

    if (mb_write_all(fd, req, 8) != 8) return -1;

    uint8_t rsp[256];
    int n = mb_read_response(fd, rsp, sizeof(rsp), 1000);
    if (n < 5) return -1;

    /* Validate CRC */
    uint16_t calc = mb_crc16(rsp, n - 2);
    uint16_t recv = (uint16_t)rsp[n-2] | ((uint16_t)rsp[n-1] << 8);
    if (calc != recv) return -1;

    if (rsp[0] != (uint8_t)slave) return -1;
    if (rsp[1] & 0x80) return -1;   /* exception */
    if (rsp[1] != 0x03) return -1;

    int count = rsp[2] / 2;
    for (int i = 0; i < count && i < qty; i++)
        regs[i] = ((uint16_t)rsp[3 + i*2] << 8) | rsp[4 + i*2];

    return count;
}

/* ── Read input registers (FC04) ─────────────────────────────────────────── */
static inline int mb_read_input_registers(int fd, int slave, int addr,
                                           int qty, uint16_t *regs)
{
    uint8_t req[8];
    req[0]=(uint8_t)slave; req[1]=0x04;
    req[2]=(uint8_t)(addr>>8); req[3]=(uint8_t)(addr&0xFF);
    req[4]=(uint8_t)(qty>>8);  req[5]=(uint8_t)(qty&0xFF);
    uint16_t crc=mb_crc16(req,6);
    req[6]=(uint8_t)(crc&0xFF); req[7]=(uint8_t)(crc>>8);
    if (mb_write_all(fd,req,8)!=8) return -1;

    uint8_t rsp[256];
    int n=mb_read_response(fd,rsp,sizeof(rsp),1000);
    if (n<5) return -1;
    uint16_t calc=mb_crc16(rsp,n-2);
    uint16_t recv=(uint16_t)rsp[n-2]|((uint16_t)rsp[n-1]<<8);
    if (calc!=recv||rsp[0]!=(uint8_t)slave||rsp[1]!=0x04) return -1;
    int count=rsp[2]/2;
    for (int i=0;i<count&&i<qty;i++)
        regs[i]=((uint16_t)rsp[3+i*2]<<8)|rsp[4+i*2];
    return count;
}

/* ── Read coils (FC01) ───────────────────────────────────────────────────── */
static inline int mb_read_bits(int fd, int slave, int addr,
                                int qty, uint8_t *bits)
{
    uint8_t req[8];
    req[0]=(uint8_t)slave; req[1]=0x01;
    req[2]=(uint8_t)(addr>>8); req[3]=(uint8_t)(addr&0xFF);
    req[4]=(uint8_t)(qty>>8);  req[5]=(uint8_t)(qty&0xFF);
    uint16_t crc=mb_crc16(req,6);
    req[6]=(uint8_t)(crc&0xFF); req[7]=(uint8_t)(crc>>8);
    if (mb_write_all(fd,req,8)!=8) return -1;

    uint8_t rsp[256];
    int n=mb_read_response(fd,rsp,sizeof(rsp),1000);
    if (n<5) return -1;
    uint16_t calc=mb_crc16(rsp,n-2);
    uint16_t recv=(uint16_t)rsp[n-2]|((uint16_t)rsp[n-1]<<8);
    if (calc!=recv||rsp[0]!=(uint8_t)slave||rsp[1]!=0x01) return -1;
    for (int i=0;i<qty;i++)
        bits[i]=(rsp[3+(i/8)]>>(i%8))&1;
    return qty;
}

/* ── Read discrete inputs (FC02) ─────────────────────────────────────────── */
static inline int mb_read_input_bits(int fd, int slave, int addr,
                                      int qty, uint8_t *bits)
{
    uint8_t req[8];
    req[0]=(uint8_t)slave; req[1]=0x02;
    req[2]=(uint8_t)(addr>>8); req[3]=(uint8_t)(addr&0xFF);
    req[4]=(uint8_t)(qty>>8);  req[5]=(uint8_t)(qty&0xFF);
    uint16_t crc=mb_crc16(req,6);
    req[6]=(uint8_t)(crc&0xFF); req[7]=(uint8_t)(crc>>8);
    if (mb_write_all(fd,req,8)!=8) return -1;

    uint8_t rsp[256];
    int n=mb_read_response(fd,rsp,sizeof(rsp),1000);
    if (n<5) return -1;
    uint16_t calc=mb_crc16(rsp,n-2);
    uint16_t recv=(uint16_t)rsp[n-2]|((uint16_t)rsp[n-1]<<8);
    if (calc!=recv||rsp[0]!=(uint8_t)slave||rsp[1]!=0x02) return -1;
    for (int i=0;i<qty;i++)
        bits[i]=(rsp[3+(i/8)]>>(i%8))&1;
    return qty;
}

#endif /* MODBUS_RAW_H */