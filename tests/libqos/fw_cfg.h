#ifndef LIBQOS_FW_CFG_H
#define LIBQOS_FW_CFG_H

#include <stdint.h>
#include <sys/types.h>

typedef struct QFWCFG QFWCFG;

struct QFWCFG
{
    void (*select)(QFWCFG *fw_cfg, uint16_t key);
    void (*read)(QFWCFG *fw_cfg, void *data, size_t len);
};

void qfw_cfg_select(QFWCFG *fw_cfg, uint16_t key);
void qfw_cfg_read_data(QFWCFG *fw_cfg, void *data, size_t len);
void qfw_cfg_get(QFWCFG *fw_cfg, uint16_t key, void *data, size_t len);
uint16_t qfw_cfg_get_u16(QFWCFG *fw_cfg, uint16_t key);
uint32_t qfw_cfg_get_u32(QFWCFG *fw_cfg, uint16_t key);
uint64_t qfw_cfg_get_u64(QFWCFG *fw_cfg, uint16_t key);

#endif
