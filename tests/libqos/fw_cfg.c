#include "libqos/fw_cfg.h"

void qfw_cfg_select(QFWCFG *fw_cfg, uint16_t key)
{
    fw_cfg->select(fw_cfg, key);
}

void qfw_cfg_read_data(QFWCFG *fw_cfg, void *data, size_t len)
{
    fw_cfg->read(fw_cfg, data, len);
}

void qfw_cfg_get(QFWCFG *fw_cfg, uint16_t key, void *data, size_t len)
{
    qfw_cfg_select(fw_cfg, key);
    qfw_cfg_read_data(fw_cfg, data, len);
}

uint16_t qfw_cfg_get_u16(QFWCFG *fw_cfg, uint16_t key)
{
    uint16_t value;
    qfw_cfg_get(fw_cfg, key, &value, sizeof(value));
    return value;
}

uint32_t qfw_cfg_get_u32(QFWCFG *fw_cfg, uint16_t key)
{
    uint32_t value;
    qfw_cfg_get(fw_cfg, key, &value, sizeof(value));
    return value;
}

uint64_t qfw_cfg_get_u64(QFWCFG *fw_cfg, uint16_t key)
{
    uint64_t value;
    qfw_cfg_get(fw_cfg, key, &value, sizeof(value));
    return value;
}

