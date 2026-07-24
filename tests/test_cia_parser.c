#include "cia_util.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    unsigned char data[0x2300];
    const size_t tmd_offset = 0x2040;
    const size_t id_offset = tmd_offset + 0x240 + 0x4C;
    const unsigned char expected_id[8] = {
        0x00, 0x04, 0x00, 0x00, 0x0F, 0x00, 0x02, 0x00
    };
    uint64_t title_id = 0;

    /*
     * Minimal synthetic CIA prefix: 0x2020-byte header aligned to 0x40,
     * no certificate/ticket blocks, RSA-4096 TMD signature type 0.
     * It contains no copyrighted game data.
     */
    memset(data, 0, sizeof(data));
    data[0] = 0x20;
    data[1] = 0x20;
    data[tmd_offset + 3] = 0;
    memcpy(data + id_offset, expected_id, sizeof(expected_id));

    assert(cia_extract_title_id(data, sizeof(data), &title_id));
    assert(title_id == UINT64_C(0x000400000F000200));
    assert(cia_title_is_safe_sd(title_id));
    assert(!cia_title_is_safe_sd(UINT64_C(0x0004013000000002)));
    printf("CIA header: title ID %016llX, safe SD validation passed\n",
           (unsigned long long)title_id);
    return 0;
}
