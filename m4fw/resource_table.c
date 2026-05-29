#include "resource_table.h"

/*
 * Resource table at fixed address 0x10048000 (mcu-rsc-table region).
 * The A7 remoteproc driver locates this via the st,syscfg-rsc-tbl DT property.
 */
__attribute__((section(".resource_table")))
__attribute__((used))
struct resource_table resource_table = {
    .ver      = 1,
    .num      = RSC_NUM,
    .reserved = {0, 0},
    .offset   = { offsetof(struct resource_table, vdev) },

    .vdev = {
        .type          = RSC_VDEV,
        .id            = VIRTIO_ID_RPMSG,
        .notifyid      = 0,
        .dfeatures     = VIRTIO_RPMSG_F_NS,
        .gfeatures     = 0,
        .config_len    = 0,
        .status        = 0,
        .num_of_vrings = 2,
        .reserved      = {0, 0},
        .vring = {
            {   /* vring0: A7->M4 */
                .da       = VRING0_BASE,
                .align    = VRING_ALIGNMENT,
                .num      = VRING_NUM_DESCS,
                .notifyid = 0,
                .reserved = 0,
            },
            {   /* vring1: M4->A7 */
                .da       = VRING1_BASE,
                .align    = VRING_ALIGNMENT,
                .num      = VRING_NUM_DESCS,
                .notifyid = 1,
                .reserved = 0,
            },
        },
    },
};
