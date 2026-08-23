# moor

`moor` is a C11 fixed-size memory pool that performs no dynamic allocation at runtime.
It provides multiple size classes, thread-local caches, non-blocking allocation, and scatter/gather I/O.

## Requirements

- C11 compiler
- POSIX threads
- Make

## Build and test

```sh
make test
```

Run the complete verification suite, including sanitizers, debug mode, TLS/iovec-disabled builds, strict warnings, and custom lock hooks:

```sh
make verify
```

Remove build outputs with `make clean`.

## Quick start

```c
#include "pool_allocator.h"

#include <assert.h>

static const pa_berth_config berths[] = {
    { .block_size = 256,  .count = 128, .mag_depth = 16 },
    { .block_size = 1024, .count = 64,  .mag_depth = 8 },
};

static const pa_harbor_config config = {
    .berths = berths,
    .berth_count = sizeof berths / sizeof berths[0],
    .max_threads = 1,
};

PA_HARBOR_STORAGE(storage, 256 * 1024);

int main(void)
{
    assert(pa_harbor_bytes(&config) <= sizeof storage);

    pa_harbor *harbor = pa_harbor_init(storage, sizeof storage, &config);
    assert(harbor != NULL);
    assert(pa_thread_attach(harbor) == PA_OK);

    pa_ship *ship = pa_charter(harbor, 100, 16, 0);
    assert(ship != NULL);
    assert(pa_write(ship, "hello", 5) == 5);
    pa_release(ship);

    pa_thread_detach(harbor);
    assert(pa_harbor_fini(harbor) == PA_OK);
}
```

The caller owns a ship returned by `pa_charter`. `pa_moor` transfers ownership to a bollard, and `pa_unmoor` returns it to the caller. Always call `pa_release` when a ship is no longer needed.

## Layout

- `pool_allocator.c` / `pool_allocator.h`: implementation and public API
- `test_pool_allocator.c`: default configuration tests
- `test_explicit_context.c`: TLS-disabled configuration test
- `test_lock_hooks.h`: custom lock hook test
