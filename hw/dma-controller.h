#ifndef QEMU_DMA_CONTROLLER_H
#define QEMU_DMA_CONTROLLER_H

#include "ioport.h"
#include "memory.h"
#include "qdev.h"
#include "qemu/pin.h"

/* dma.c */
typedef struct DMAController DMAController;

int DMA_get_channel_mode (int nchan);
int DMA_read_memory (int nchan, void *buf, int pos, int size);
int DMA_write_memory (int nchan, void *buf, int pos, int size);
void DMA_hold_DREQ (int nchan);
void DMA_release_DREQ (int nchan);
DMAController *DMA_init(int high_page_enable);
void DMA_register_channel (int nchan,
                           DMA_transfer_handler transfer_handler,
                           void *opaque);

#endif
