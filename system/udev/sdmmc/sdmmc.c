// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <endian.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <threads.h>

#include <ddk/binding.h>
#include <ddk/completion.h>
#include <ddk/device.h>
#include <ddk/iotxn.h>
#include <ddk/protocol/block.h>
#include <ddk/protocol/sdmmc.h>

#include <hexdump/hexdump.h>

#define OCR_SDHC      0xc0000000
#define CSD_STRUCT_V2 0x1

#define TRACE 1

#if TRACE
#define xprintf(fmt...) printf(fmt)
#else
#define xprintf(fmt...) \
    do {                \
    } while (0)
#endif

typedef struct sdmmc {
    mx_device_t device;
    mx_device_t* parent;
    uint16_t rca;
    uint64_t capacity;
} sdmmc_t;
#define get_sdmmc(dev) containerof(dev, sdmmc_t, device)

typedef struct sdmmc_setup_context {
    mx_device_t* dev;
    mx_driver_t* drv;
} sdmmc_setup_context_t;

static void sdmmc_txn_cplt(iotxn_t* request, void* cookie) {
    completion_t* cplt = (completion_t*)cookie;

    completion_signal(cplt);
};

static mx_status_t sdmmc_do_command(mx_device_t* dev, const uint32_t cmd,
                                    const uint32_t arg, iotxn_t* txn) {
    sdmmc_protocol_data_t* pdata = iotxn_pdata(txn, sdmmc_protocol_data_t);
    pdata->cmd = cmd;
    pdata->arg = arg;

    completion_t cplt = COMPLETION_INIT;
    txn->complete_cb = sdmmc_txn_cplt;
    txn->cookie = &cplt;

    iotxn_queue(dev, txn);

    completion_wait(&cplt, MX_TIME_INFINITE);

    return txn->status;
}

static ssize_t sdmmc_ioctl(mx_device_t* dev, uint32_t op, const void* cmd, size_t cmdlen, void* reply, size_t max) {
    switch (op) {
    case IOCTL_BLOCK_GET_SIZE: {
        uint64_t* disk_size = reply;
        if (max < sizeof(*disk_size))
            return ERR_BUFFER_TOO_SMALL;
        *disk_size = 2ul * 8010596352ul;
        return sizeof(*disk_size);
    }
    case IOCTL_BLOCK_GET_BLOCKSIZE: {
        uint64_t* blksize = reply;
        if (max < sizeof(*blksize))
            return ERR_BUFFER_TOO_SMALL;
        *blksize = 512;
        return sizeof(*blksize);
    }
    case IOCTL_BLOCK_GET_NAME: {
        assert(false);
        return ERR_NOT_SUPPORTED;
    }
    case IOCTL_DEVICE_SYNC: {
        assert(false);
        return ERR_NOT_SUPPORTED;
    }
    default:
        return ERR_NOT_SUPPORTED;
    }
    return 0;
}

static void sdmmc_unbind(mx_device_t* device) {
    sdmmc_t* sdmmc = get_sdmmc(device);
    device_remove(&sdmmc->device);
}

static mx_status_t sdmmc_release(mx_device_t* device) {
    sdmmc_t* sdmmc = get_sdmmc(device);
    free(sdmmc);
    return NO_ERROR;
}

static void sdmmc_iotxn_queue(mx_device_t* dev, iotxn_t* txn) {
    if (txn->offset % SDHC_BLOCK_SIZE) {
        xprintf("sdmmc: iotxn offset not aligned to block boundary, "
                "offset =%" PRIu64 ", block size = %d\n", txn->offset, SDHC_BLOCK_SIZE);
        txn->ops->complete(txn, ERR_INVALID_ARGS, 0);
        return;
    }

    if (txn->length % SDHC_BLOCK_SIZE) {
        xprintf("sdmmc: iotxn length not aligned to block boundary, "
                "offset =%" PRIu64 ", block size = %d\n", txn->length, SDHC_BLOCK_SIZE);
        txn->ops->complete(txn, ERR_INVALID_ARGS, 0);
        return;
    }

    sdmmc_t* sdmmc = get_sdmmc(dev);
    mx_device_t* parent = sdmmc->parent;
    uint32_t cmd = 0;

    // Figure out which SD command we need to issue.
    switch(txn->opcode) {
        case IOTXN_OP_READ:
            if (txn->length > SDHC_BLOCK_SIZE) {
                cmd = SDMMC_READ_MULTIPLE_BLOCK;
            } else {
                cmd = SDMMC_READ_BLOCK;
            }
            break;
        case IOTXN_OP_WRITE:
            if (txn->length > SDHC_BLOCK_SIZE) {
                cmd = SDMMC_WRITE_MULTIPLE_BLOCK;
            } else {
                cmd = SDMMC_WRITE_BLOCK;
            }
            break;
        default:
            // Invalid opcode?
            txn->ops->complete(txn, ERR_INVALID_ARGS, 0);
            return;
    }

    iotxn_t* emmc_txn;
    if (iotxn_alloc(&emmc_txn, 0, txn->length, 0) != NO_ERROR) {
        xprintf("sdmmc: error allocating emmc iotxn\n");
        txn->ops->complete(txn, ERR_INTERNAL, 0);
        return;
    }
    emmc_txn->opcode = txn->opcode;
    emmc_txn->flags = txn->flags;
    emmc_txn->offset = txn->offset;
    emmc_txn->length = txn->length;
    emmc_txn->protocol = MX_PROTOCOL_MMC;
    sdmmc_protocol_data_t* pdata = iotxn_pdata(emmc_txn, sdmmc_protocol_data_t);

    uint8_t current_state;
    do {
        mx_status_t rc = sdmmc_do_command(parent, SDMMC_SEND_STATUS, sdmmc->rca << 16, emmc_txn);
        if (rc != NO_ERROR) {
            txn->ops->complete(txn, rc, 0);
            // TODO(gkalsi): goto out?
            return;
        }

        current_state = (pdata->response[0] >> 9) & 0xf;
        
        if (current_state == 5) {
            rc = sdmmc_do_command(parent, SDMMC_STOP_TRANSMISSION, 0, emmc_txn);
            continue;
        } else if (current_state == 4) {
            break;
        }

        mx_nanosleep(MX_MSEC(100));
    } while (current_state != 4);


    // Which block to operate against.
    const uint32_t blkid = emmc_txn->offset / SDHC_BLOCK_SIZE;

    pdata->blockcount = txn->length / SDHC_BLOCK_SIZE;
    pdata->blocksize = SDHC_BLOCK_SIZE;

    void* buffer;
    if (txn->opcode == IOTXN_OP_WRITE) {
        txn->ops->mmap(txn, &buffer);
        // hexdump(buffer, txn->length);
        emmc_txn->ops->copyto(emmc_txn, buffer, txn->length, 0);
    }

    mx_status_t rc = sdmmc_do_command(parent, cmd, blkid, emmc_txn);
    if (rc != NO_ERROR) {
        txn->ops->complete(txn, rc, 0);
    }

    // TODO(gkalsi): Make sure we're not copying more than the amount of space
    // we have available.
    if (txn->opcode == IOTXN_OP_READ) {
        emmc_txn->ops->mmap(emmc_txn, &buffer);
        txn->ops->copyto(txn, buffer, emmc_txn->actual, 0);
    }

    txn->ops->complete(txn, NO_ERROR, emmc_txn->actual);
}

static mx_off_t sdmmc_get_size(mx_device_t* dev) {
    sdmmc_t* sdmmc = get_sdmmc(dev);
    return sdmmc->capacity;
}

// Block device protocol.
static mx_protocol_device_t sdmmc_device_proto = {
    .ioctl = sdmmc_ioctl,
    .unbind = sdmmc_unbind,
    .release = sdmmc_release,
    .iotxn_queue = sdmmc_iotxn_queue,
    .get_size = sdmmc_get_size,
};

static int sdmmc_bootstrap_thread(void* arg) {
    sdmmc_setup_context_t* ctx = (sdmmc_setup_context_t*)arg;
    mx_device_t* dev = ctx->dev;
    mx_driver_t* drv = ctx->drv;
    free(ctx);

    mx_status_t st;
    sdmmc_t* sdmmc = NULL;
    iotxn_t* setup_txn = NULL;


    // Allocate the device.
    sdmmc = calloc(1, sizeof(*sdmmc));
    if (!sdmmc) {
        xprintf("sdmmc: no memory to allocate sdmmc device!\n");
        goto err;
    }

    // Allocate a single iotxn that we use to bootstrap the card with.
    if ((st = iotxn_alloc(&setup_txn, 0, SDHC_BLOCK_SIZE, 0)) != NO_ERROR) {
        xprintf("sdmmc: failed to allocate iotxn for setup, rc = %d\n", st);
        goto err;
    }

    // Get the protocol data from the iotxn. We use this to pass the command
    // type and command arguments to the EMMC driver.
    sdmmc_protocol_data_t* pdata = iotxn_pdata(setup_txn, sdmmc_protocol_data_t);

    // Reset the card. No matter what state the card is in, issuing the
    // GO_IDLE_STATE command will put the card into the idle state.
    if ((st = sdmmc_do_command(dev, SDMMC_GO_IDLE_STATE, 0, setup_txn)) != NO_ERROR) {
        xprintf("sdmmc: SDMMC_GO_IDLE_STATE failed, retcode = %d\n", st);
        goto err;
    }

    // Issue the SEND_IF_COND command, this will tell us that we can talk to
    // the card correctly and it will also tell us if the voltage range that we
    // have supplied has been accepted.
    if ((st = sdmmc_do_command(dev, SDMMC_SEND_IF_COND, 0x1aa, setup_txn)) != NO_ERROR) {
        xprintf("sdmmc: SDMMC_SEND_IF_COND failed, retcode = %d\n", st);
        goto err;
    }
    if ((pdata->response[0] & 0xFFF) != 0x1aa) {
        // The card should have replied with the pattern that we sent.
        xprintf("sdmmc: SDMMC_SEND_IF_COND got bad reply = %"PRIu32"\n",
                pdata->response[0]);
        goto err;
    }

    // Get the operating conditions from the card.
    if ((st = sdmmc_do_command(dev, SDMMC_APP_CMD, 0, setup_txn)) != NO_ERROR) {
        xprintf("sdmmc: SDMMC_APP_CMD failed, retcode = %d\n", st);
        goto err;
    }
    if ((sdmmc_do_command(dev, SDMMC_SD_SEND_OP_COND, 0, setup_txn)) != NO_ERROR) {
        xprintf("sdmmc: SDMMC_SD_SEND_OP_COND failed, retcode = %d\n", st);
        goto err;
    }

    while (true) {
        // Ask for high speed.
        const uint32_t flags = (1 << 30)  | 0x00ff8000 | (1 << 24);
        if ((st = sdmmc_do_command(dev, SDMMC_APP_CMD, 0, setup_txn)) != NO_ERROR) {
            xprintf("sdmmc: APP_CMD failed with retcode = %d\n", st);
            goto err;
        }
        if ((st = sdmmc_do_command(dev, SDMMC_SD_SEND_OP_COND, flags, setup_txn)) != NO_ERROR) {
            xprintf("sdmmc: SD_SEND_OP_COND failed with retcode = %d\n", st);
            goto err;
        }

        const uint32_t ocr = pdata->response[0];
        if (ocr & (1 << 31)) {
            if (!(ocr & OCR_SDHC)) {
                // Card is not an SDHC card. We currently don't support this.
                xprintf("sdmmc: unsupported card type, must use sdhc card\n");
                goto err;
            }
            break;
        }

        mx_nanosleep(MX_MSEC(5));
    }

    uint32_t new_bus_frequency = 10000000;
    st = dev->ops->ioctl(dev, IOCTL_SDMMC_SET_BUS_FREQ, &new_bus_frequency, 
                        sizeof(new_bus_frequency), NULL, 0);
    if (st != NO_ERROR) {
        // This is non-fatal but the card will run slowly.
        xprintf("sdmmc: failed to increase bus frequency.\n");
    }


    if ((st = sdmmc_do_command(dev, SDMMC_ALL_SEND_CID, 0, setup_txn)) != NO_ERROR) {
        xprintf("sdmmc: ALL_SEND_CID failed with retcode = %d\n", st);
        goto err;
    }

    if ((st = sdmmc_do_command(dev, SDMMC_SEND_RELATIVE_ADDR, 0, setup_txn)) != NO_ERROR) {
        xprintf("sdmmc: SEND_RELATIVE_ADDR failed with retcode = %d\n", st);
        goto err;
    }

    sdmmc->rca = (pdata->response[0] >> 16) & 0xffff;
    if (pdata->response[0] & 0xe000) {
        xprintf("sdmmc: SEND_RELATIVE_ADDR failed with resp = %d\n", (pdata->response[0] & 0xe000));
        st = ERR_INTERNAL;
        goto err;
    }
    if ((pdata->response[0] & (1u << 8)) == 0) {
        xprintf("sdmmc: SEND_RELATIVE_ADDR failed. Card not ready.\n");
        st = ERR_INTERNAL;
        goto err;
    }

    // Determine the size of the card.
    if ((st = sdmmc_do_command(dev, SDMMC_SEND_CSD, sdmmc->rca << 16, setup_txn)) != NO_ERROR) {
        xprintf("sdmmc: failed to send app cmd, retcode = %d\n", st);
        goto err;
    }

    // For now we only support SDHC cards. These cards must have a CSD type = 1,
    // since CSD type 0 is unable to support SDHC sized cards.
    uint8_t csd_structure = (pdata->response[0] >> 30) & 0x3;
    if (csd_structure != CSD_STRUCT_V2) {
        xprintf("sdmmc: unsupported card type, expected CSD version = %d, "
                "got version %d\n", CSD_STRUCT_V2, csd_structure);
        goto err;
    }

    const uint32_t c_size = ((pdata->response[2] >> 16) | (pdata->response[1] << 16)) & 0x3fffff;
    sdmmc->capacity = (c_size + 1ul) * 512ul * 1024ul;
    xprintf("sdmmc: found card with capacity = %"PRIu64"B\n", sdmmc->capacity);

    if ((st = sdmmc_do_command(dev, SDMMC_SELECT_CARD, sdmmc->rca << 16, setup_txn)) != NO_ERROR) {
        xprintf("sdmmc: SELECT_CARD failed with retcode = %d\n", st);
        goto err;
    }

    pdata->blockcount = 1;
    pdata->blocksize = 8;
    if ((st = sdmmc_do_command(dev, SDMMC_APP_CMD, sdmmc->rca << 16, setup_txn)) != NO_ERROR) {
        xprintf("sdmmc: APP_CMD failed with retcode = %d\n", st);
        goto err;
    }
    if ((st = sdmmc_do_command(dev, SDMMC_SEND_SCR, 0, setup_txn)) != NO_ERROR) {
        xprintf("sdmmc: SEND_SCR failed with retcode = %d\n", st);
        goto err;
    }
    pdata->blockcount = 1;
    pdata->blocksize = 512;

    uint32_t scr;
    setup_txn->ops->copyfrom(setup_txn, &scr, sizeof(scr), 0);
    scr = be32toh(scr);

    // If this card supports 4 bit mode, then put it into 4 bit mode.
    const uint32_t supported_bus_widths = (scr >> 16) & 0xf;
    if (supported_bus_widths & 0x4) {

        do {
            // First tell the card to go into four bit mode:
            if ((st = sdmmc_do_command(dev, SDMMC_APP_CMD, sdmmc->rca << 16, setup_txn)) != NO_ERROR) {
                xprintf("sdmmc: failed to send app cmd, retcode = %d\n", st);
                break;
            }
            if ((st = sdmmc_do_command(dev, SDMMC_SET_BUS_WIDTH, 2, setup_txn)) != NO_ERROR) {
                xprintf("sdmmc: failed to set card bus width, retcode = %d\n", st);
                break;
            }
            const uint32_t new_bus_width = 4;
            st = dev->ops->ioctl(dev, IOCTL_SDMMC_SET_BUS_WIDTH, &new_bus_width, 
                                 sizeof(new_bus_width), NULL, 0);
            if (st != NO_ERROR) {
                xprintf("sdmmc: failed to set host bus width, retcode = %d\n", st);
            }
        } while (false);
    }

    device_init(&sdmmc->device, drv, "sdmmc", &sdmmc_device_proto);
    sdmmc->parent = dev;
    sdmmc->device.protocol_id = MX_PROTOCOL_BLOCK;
    device_add(&sdmmc->device, dev);

    xprintf("sdmmc: bind success!\n");

    return 0;
err:
    if (sdmmc)
        free(sdmmc);

    if (setup_txn)
        setup_txn->ops->release(setup_txn);

    return -1;
}

static mx_status_t sdmmc_bind(mx_driver_t* drv, mx_device_t* dev) {
    // Create a context.
    sdmmc_setup_context_t* ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return ERR_NO_MEMORY;
    ctx->dev = dev;
    ctx->drv = drv;

    // Create a bootstrap thread.
    thrd_t bootstrap_thrd;
    mx_status_t rc = thrd_create_with_name(&bootstrap_thrd, 
                                           sdmmc_bootstrap_thread, ctx,
                                           "sdmmc_bootstrap_thread");
    if (rc != NO_ERROR) {
        free(ctx);
        return rc;
    }

    thrd_detach(bootstrap_thrd);
    return NO_ERROR;
}

mx_driver_t _driver_sdmmc = {
    .ops = {
        .bind = sdmmc_bind,
    },
};

// The formatter does not play nice with these macros.
// clang-format off
MAGENTA_DRIVER_BEGIN(_driver_sdmmc, "sdmmc", "magenta", "0.1", 1)
    BI_MATCH_IF(EQ, BIND_PROTOCOL, MX_PROTOCOL_MMC),
MAGENTA_DRIVER_END(_driver_sdmmc)
// clang-format on