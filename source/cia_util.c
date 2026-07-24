#include "cia_util.h"

#include <string.h>

static uint32_t read_le32(const unsigned char *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t read_be64(const unsigned char *p)
{
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) value = (value << 8) | p[i];
    return value;
}

static bool align64_add(size_t *value, uint32_t amount)
{
    size_t aligned = ((size_t)amount + 0x3Fu) & ~(size_t)0x3F;
    if (*value > SIZE_MAX - aligned) return false;
    *value += aligned;
    return true;
}

bool cia_extract_title_id(const void *data, size_t size, uint64_t *title_id)
{
    static const uint32_t signature_sizes[] = {
        0x240, 0x140, 0x80, 0x240, 0x140, 0x80
    };
    const unsigned char *cia = data;
    size_t tmd_offset;
    size_t id_offset;
    unsigned signature_type;
    uint32_t header_size;

    if (!cia || !title_id || size < 0x20) return false;
    header_size = read_le32(cia);
    if (header_size != 0x2020) return false;

    tmd_offset = 0;
    if (!align64_add(&tmd_offset, header_size) ||
        !align64_add(&tmd_offset, read_le32(cia + 0x08)) ||
        !align64_add(&tmd_offset, read_le32(cia + 0x0C))) {
        return false;
    }
    if (tmd_offset > size || size - tmd_offset < 4) return false;

    signature_type = cia[tmd_offset + 3];
    if (signature_type >= sizeof(signature_sizes) / sizeof(signature_sizes[0])) {
        return false;
    }
    id_offset = tmd_offset + signature_sizes[signature_type] + 0x4C;
    if (id_offset > size || size - id_offset < sizeof(uint64_t)) return false;

    *title_id = read_be64(cia + id_offset);
    return true;
}

bool cia_title_is_safe_sd(uint64_t title_id)
{
    uint16_t platform = (uint16_t)(title_id >> 48);
    uint16_t category = (uint16_t)(title_id >> 32);
    if (platform != 0x0004) return false;

    return category == 0x0000 || /* Application */
           category == 0x0002 || /* Demo */
           category == 0x000E || /* Update */
           category == 0x008C;   /* DLC */
}

bool cia_title_requires_new3ds(uint64_t title_id)
{
    return ((title_id >> 28) & 0xFu) == 2;
}

