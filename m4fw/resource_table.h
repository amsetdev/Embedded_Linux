#ifndef RESOURCE_TABLE_H
#define RESOURCE_TABLE_H

#include <stdint.h>
#include <stddef.h>

/* Shared memory regions — from /proc/device-tree/reserved-memory/ on DK2 */
#define VRING0_BASE     0x10040000U   /* A7->M4 vring */
#define VRING1_BASE     0x10041000U   /* M4->A7 vring */
#define VDEV_BUF_BASE   0x10042000U   /* RPMsg buffer pool */
#define RSC_TABLE_BASE  0x10048000U

#define VRING_NUM_DESCS 16U
#define VRING_ALIGNMENT 16U

/* Virtio */
#define VIRTIO_ID_RPMSG        7U
#define VIRTIO_RPMSG_F_NS      (1U << 0)

#define RSC_VDEV               3U
#define RSC_NUM                1U

struct fw_rsc_vdev_vring {
    uint32_t da;
    uint32_t align;
    uint32_t num;
    uint32_t notifyid;
    uint32_t reserved;
};

struct fw_rsc_vdev {
    uint32_t type;
    uint32_t id;
    uint32_t notifyid;
    uint32_t dfeatures;
    uint32_t gfeatures;
    uint32_t config_len;
    uint8_t  status;
    uint8_t  num_of_vrings;
    uint8_t  reserved[2];
    struct fw_rsc_vdev_vring vring[2];
};

struct resource_table {
    uint32_t ver;
    uint32_t num;
    uint32_t reserved[2];
    uint32_t offset[RSC_NUM];
    struct fw_rsc_vdev vdev;
};

extern struct resource_table resource_table;

#endif /* RESOURCE_TABLE_H */
