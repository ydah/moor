#include "pool_allocator.h"

#include <assert.h>
#include <string.h>

static _Alignas(PA_SLOT_ALIGN) uint8_t storage[16u * 1024u];

int main(void)
{
    static const pa_berth_config berths[] = {
        {.block_size = 64, .count = 16, .mag_depth = 4},
    };
    const pa_harbor_config config = {
        .berths = berths,
        .berth_count = 1,
        .flags = PA_F_STRICT_TLS,
    };
    pa_harbor *harbor = pa_harbor_init(storage, sizeof(storage), &config);
    assert(harbor);
    pa_ctx ctx = {0};
    assert(pa_ctx_attach(&ctx, harbor) == PA_OK);

    pa_bollard bollard;
    pa_bollard_init_ctx(&bollard, harbor, &ctx);
    assert(pa_bollard_append_all(&bollard, "explicit context", 16) == PA_OK);
    char copy[16];
    assert(pa_bollard_read_at(&bollard, 0, copy, sizeof(copy)) == sizeof(copy));
    assert(memcmp(copy, "explicit context", sizeof(copy)) == 0);
    pa_bollard_release_all(&bollard);

    pa_ctx_detach(&ctx);
    assert(pa_harbor_fini(harbor) == PA_OK);
    return 0;
}
