#ifndef QEMU_DMA_CONTROLLER_H
#define QEMU_DMA_CONTROLLER_H

#include "ioport.h"
#include "memory.h"
#include "qdev.h"
#include "qemu/pin.h"
#include "isa.h"

/* dma.c */
typedef struct DMARegisters
{
    int now[2];
    uint16_t base[2];
    uint8_t mode;
    uint8_t page;
    uint8_t pageh;
    uint8_t dack;
    uint8_t eop;
    DMA_transfer_handler transfer_handler;
    void *opaque;
} DMARegisters;

struct DMAController
{
    uint8_t status;
    uint8_t command;
    uint8_t mask;
    uint8_t flip_flop;
    int dshift;
    DMARegisters regs[4];
};

int DMA_get_channel_mode (DMAController *d, int nchan);
int DMA_read_memory (DMAController *d, int nchan, void *buf, int pos, int size);
int DMA_write_memory (DMAController *d, int nchan, void *buf, int pos, int size);
void DMA_hold_DREQ (DMAController *d, int nchan);
void DMA_release_DREQ (DMAController *d, int nchan);
void DMA_register_channel (DMAController *d, int nchan,
                           DMA_transfer_handler transfer_handler,
                           void *opaque);

#endif
