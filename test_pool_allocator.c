#include "pool_allocator.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if PA_HAVE_IOVEC
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#define TEST_STORAGE_SIZE (128u * 1024u)

static _Alignas(PA_SLOT_ALIGN) uint8_t storage[TEST_STORAGE_SIZE];
static _Alignas(PA_SLOT_ALIGN) uint8_t second_storage[TEST_STORAGE_SIZE];

static void assert_bollard_data(pa_bollard *bollard, const void *expected, size_t n);

static pa_harbor *make_harbor(const pa_berth_config *berths, size_t count,
                              unsigned flags, void (*callback)(pa_harbor *, size_t, void *),
                              void *ud)
{
    pa_harbor_config config = {
        .berths = berths,
        .berth_count = count,
        .max_threads = 8,
        .default_bow = 16,
        .default_stern = 8,
        .flags = flags,
        .low_water_cb = callback,
        .ud = ud,
    };
    assert(pa_harbor_bytes(&config) <= sizeof(storage));
    pa_harbor *harbor = pa_harbor_init(storage, sizeof(storage), &config);
    assert(harbor);
    return harbor;
}

static void finish_harbor(pa_harbor *harbor)
{
    pa_thread_detach(harbor);
    assert(pa_harbor_fini(harbor) == PA_OK);
}

static void assert_conservation(pa_harbor *harbor)
{
    pa_stats stats;
    pa_thread_stats_snapshot thread;
    pa_harbor_stats(harbor, &stats);
    pa_thread_stats(&thread);
    for (size_t i = 0; i < harbor->berth_count; ++i) {
        size_t depot = 0;
        for (pa_ship *mag = harbor->berths[i].depot; mag; mag = mag->u.f.mag_next)
            depot += mag->mag_n;
        size_t tls = thread.attached && thread.harbor == harbor
                   ? thread.berths[i].m1 + thread.berths[i].m2 : 0u;
        assert(depot + tls + stats.berths[i].n_inuse == stats.berths[i].n_total);
    }
}

static void test_ship_operations(void)
{
    static const pa_berth_config default_berths[] = {
        {.block_size = 128, .count = 1024},
        {.block_size = 512, .count = 512},
        {.block_size = 2048, .count = 256, .reserve_mags = 2},
        {.block_size = 9216, .count = 32},
    };
    const pa_harbor_config default_config = {
        .berths = default_berths,
        .berth_count = 4,
        .max_threads = 8,
        .default_bow = 64,
        .flags = PA_F_STRICT_TLS | PA_F_GUARD,
    };
    size_t default_bytes = pa_harbor_bytes(&default_config);
    assert(default_bytes <= 1536u * 1024u);

    const pa_berth_config invalid_berth = {0};
    const pa_harbor_config invalid_config = {
        .berths = &invalid_berth,
        .berth_count = 1,
    };
    assert(pa_harbor_bytes(&invalid_config) == 0);
    const pa_berth_config large_berth = {
        .block_size = 64,
        .count = 65536,
        .mag_depth = 64,
    };
    const pa_harbor_config large_config = {
        .berths = &large_berth,
        .berth_count = 1,
    };
    assert(pa_harbor_bytes(&large_config) > 0);

    static const pa_berth_config berths[] = {
        {.block_size = 128, .count = 16, .mag_depth = 4},
        {.block_size = 512, .count = 8, .mag_depth = 2},
    };
    pa_harbor *harbor = make_harbor(berths, 2, PA_F_STRICT_TLS | PA_F_GUARD,
                                    NULL, NULL);
    assert(pa_charter(harbor, 1, 0, 0) == NULL);
    assert(pa_thread_attach(harbor) == PA_OK);
    assert_conservation(harbor);
    assert(pa_harbor_max_payload(harbor) == 512);
    assert(pa_harbor_avail(harbor, 120));

    pa_ship *ship = pa_charter(harbor, 100, SIZE_MAX, SIZE_MAX);
    assert(ship && ship->cap == 128 && pa_bow_room(ship) == 16);
#if PA_DEBUG
    assert(ship->owner_file && ship->owner_line);
#endif
    assert(pa_write(ship, "payload", 7) == 7);
    memcpy(pa_push(ship, 4), "HEAD", 4);
    assert(pa_len(ship) == 11);
    assert(pa_write_at(ship, 11, "!", 1) == PA_OK);
    assert(pa_len(ship) == 12);
    assert(pa_seek(ship, 4, PA_SEEK_SET) == PA_OK);
    char buffer[32] = {0};
    assert(pa_read(ship, buffer, 7) == 7);
    assert(memcmp(buffer, "payload", 7) == 0);
    assert(pa_pull(ship, 4) == PA_OK);
    assert(pa_trim(ship, 7) == PA_OK);
    assert(pa_len(ship) == 7);

    pa_reset(ship);
    assert(pa_len(ship) == 0 && pa_bow_room(ship) == 16 && pa_tell(ship) == 0);
    assert(pa_reserve_bow(ship, 32) == PA_OK);
    assert(pa_write(ship, "clone", 5) == 5);
    pa_ship *copy = pa_clone(ship);
    assert(copy && pa_len(copy) == 5);
#if PA_DEBUG
    assert(copy->owner_file && copy->owner_line);
#endif
    assert(pa_read_at(copy, 0, buffer, sizeof(buffer)) == 5);
    assert(memcmp(buffer, "clone", 5) == 0);
    pa_release(copy);
    pa_release(ship);

    char external[] = "borrowed";
    pa_ship *wrapped = pa_wrap(harbor, external, sizeof(external) - 1u, 0);
    assert(wrapped && pa_len(wrapped) == sizeof(external) - 1u);
    assert(pa_write(wrapped, "x", 1) == PA_E_STATE);
    pa_release(wrapped);

    pa_stats stats;
    pa_harbor_stats(harbor, &stats);
    assert(stats.berth_count == 2 && stats.berths[0].n_inuse == 0);
    finish_harbor(harbor);
}

static uint32_t random_state = 0x12345678u;

static uint32_t next_random(void)
{
    random_state = random_state * 1664525u + 1013904223u;
    return random_state;
}

static void test_ship_model(void)
{
    static const pa_berth_config berths[] = {
        {.block_size = 128, .count = 16},
    };
    pa_harbor *harbor = make_harbor(berths, 1, 0, NULL, NULL);
    pa_ship *ship = pa_charter(harbor, 0, 32, 0);
    assert(ship);
    uint8_t model[128];
    size_t length = 0;

    for (size_t step = 0; step < 50000; ++step) {
        uint8_t bytes[16];
        for (size_t i = 0; i < sizeof(bytes); ++i) bytes[i] = (uint8_t)next_random();
        size_t n = next_random() % (sizeof(bytes) + 1u);
        switch (next_random() % 7u) {
        case 0:
            if (n <= pa_stern_room(ship)) {
                assert(pa_write(ship, bytes, n) == (ssize_t)n);
                memcpy(model + length, bytes, n);
                length += n;
            } else {
                assert(pa_write(ship, bytes, n) == PA_E_NOSPACE);
            }
            break;
        case 1:
            if (n <= pa_bow_room(ship)) {
                assert(pa_push(ship, n));
                memmove(model + n, model, length);
                memcpy(model, bytes, n);
                memcpy(pa_data(ship), bytes, n);
                length += n;
            } else {
                assert(pa_push(ship, n) == NULL);
            }
            break;
        case 2:
            if (n <= length) {
                assert(pa_pull(ship, n) == PA_OK);
                memmove(model, model + n, length - n);
                length -= n;
            } else {
                assert(pa_pull(ship, n) == PA_E_RANGE);
            }
            break;
        case 3: {
            size_t new_length = length ? next_random() % (length + 1u) : 0u;
            assert(pa_trim(ship, new_length) == PA_OK);
            length = new_length;
            break;
        }
        case 4: {
            size_t off = length ? next_random() % (length + 1u) : 0u;
            if (n <= ship->cap - ship->data - off) {
                assert(pa_write_at(ship, off, bytes, n) == PA_OK);
                memcpy(model + off, bytes, n);
                if (off + n > length) length = off + n;
            } else {
                assert(pa_write_at(ship, off, bytes, n) == PA_E_NOSPACE);
            }
            break;
        }
        case 5:
            if (!length) {
                size_t bow = next_random() % (ship->cap + 1u);
                assert(pa_reserve_bow(ship, bow) == PA_OK);
            } else {
                assert(pa_reserve_bow(ship, 0) == PA_E_STATE);
            }
            break;
        default:
            if (n <= ship->cap - ship->data) {
                assert(pa_write_head(ship, bytes, n) == (ssize_t)n);
                memcpy(model, bytes, n);
                length = n;
            } else {
                assert(pa_write_head(ship, bytes, n) == PA_E_NOSPACE);
            }
            break;
        }
        assert(ship->data <= ship->cursor && ship->cursor <= ship->tail);
        assert(ship->tail <= ship->cap && pa_len(ship) == length);
        assert(memcmp(pa_data(ship), model, length) == 0);
    }
    pa_release(ship);
    finish_harbor(harbor);
}

static void test_remaining_api(void)
{
    static const pa_berth_config berths[] = {
        {.block_size = 128, .count = 16, .mag_depth = 4},
    };
    pa_harbor_config config = {
        .berths = berths,
        .berth_count = 1,
        .default_bow = 8,
    };
    size_t required = pa_harbor_bytes(&config);
    assert(required < sizeof(second_storage) - 1u);
    pa_harbor *harbor = pa_harbor_init(second_storage + 1u,
                                       sizeof(second_storage) - 1u, &config);
    assert(harbor);

    pa_thread_stats_snapshot thread_stats;
    pa_thread_stats(&thread_stats);
    assert(!thread_stats.attached);
    assert(pa_thread_attach(harbor) == PA_OK);
    assert(pa_thread_attach(harbor) == PA_OK);
    pa_thread_stats(&thread_stats);
    assert(thread_stats.attached && thread_stats.harbor == harbor);

    pa_ship *first = pa_charter(harbor, 32, 32, 0);
    pa_ship *second = pa_charter_min(harbor, 32);
    pa_ship *third = pa_charter_min(harbor, 32);
    assert(first && second && third);
    assert(pa_put(first, 4));
    memcpy(pa_data(first), "one!", 4);
    assert(pa_write(second, "two", 3) == 3);
    assert(pa_write(third, "three", 5) == 5);
    assert(pa_push_aligned(first, 4, 16));
    assert((uintptr_t)pa_data(first) % 16u == 0u);
    assert(pa_push_aligned(first, 1, 3) == NULL);
    assert(pa_peek(second, 0, 3) && pa_peek(second, 1, 3) == NULL);
    assert(pa_seek(second, -1, PA_SEEK_END) == PA_OK && pa_tell(second) == 2);
    assert(pa_seek(second, -1, PA_SEEK_CUR) == PA_OK && pa_tell(second) == 1);
    assert(pa_seek(second, -2, PA_SEEK_SET) == PA_E_RANGE);
    assert(pa_seek(second, PTRDIFF_MIN, PA_SEEK_CUR) == PA_E_RANGE);
    pa_rewind(second);
    assert(pa_tell(second) == 0);

    pa_bollard bollard;
    pa_bollard_init(&bollard, harbor);
    assert(pa_moor(&bollard, second) == PA_OK);
    assert(pa_moor(&bollard, second) == PA_E_STATE);
    assert(pa_moor_front(&bollard, first) == PA_OK);
    assert(pa_moor_after(&bollard, first, third) == PA_OK);
    assert(bollard.first == first && bollard.last == second && bollard.count == 3);
    pa_ship *unmoored = pa_unmoor_first(&bollard);
    assert(unmoored == first && unmoored->state == PA_SHIP_SAILING);
    pa_release(unmoored);
#if !PA_DEBUG
    pa_release(unmoored);
#endif
    pa_reset(third);
    assert(pa_len(third) == 0 && bollard.bytes == pa_len(second));
    FILE *dump = tmpfile();
    assert(dump);
    pa_ship_dump(second, dump);
    fclose(dump);
    pa_bollard_release_all(&bollard);

    assert(pa_harbor_fini(harbor) == PA_E_STATE);
    pa_thread_detach(harbor);
    assert(pa_harbor_fini(harbor) == PA_OK);
}

static void test_explicit_context(void)
{
    static const pa_berth_config berths[] = {
        {.block_size = 64, .count = 16, .mag_depth = 4},
    };
    pa_harbor *harbor = make_harbor(berths, 1, PA_F_STRICT_TLS, NULL, NULL);
    pa_ctx ctx = {0};
    assert(pa_charter_min_ctx(&ctx, harbor, 8) == NULL);
    assert(pa_ctx_attach(&ctx, harbor) == PA_OK);
    assert(pa_harbor_avail_ctx(&ctx, harbor, 8));

    pa_ship *ship = pa_charter_ctx(&ctx, harbor, 16, 8, 0);
    assert(ship && pa_write(ship, "explicit", 8) == 8);
    pa_ship *clone = pa_clone_ctx(&ctx, ship);
    assert(clone && pa_len(clone) == 8);
    pa_release_ctx(&ctx, clone);
    pa_release_ctx(&ctx, ship);

    char borrowed[] = "ctx";
    ship = pa_wrap_ctx(&ctx, harbor, borrowed, 3, 0);
    assert(ship);
    pa_release_ctx(&ctx, ship);

    pa_bollard bollard;
    pa_bollard_init_ctx(&bollard, harbor, &ctx);
    assert(pa_bollard_append_all(&bollard, "context-bollard", 15) == PA_OK);
    pa_bollard tail;
    pa_bollard_init_ctx(&tail, harbor, &ctx);
    assert(pa_bollard_split(&bollard, 7, &tail) == PA_OK);
    assert(pa_bollard_concat(&bollard, &tail) == PA_OK);
    assert(pa_bollard_linearize(&bollard) == PA_OK);
    assert(pa_bollard_consume(&bollard, 15) == 15);
    pa_thread_stats_snapshot stats;
    pa_ctx_stats(&ctx, &stats);
    assert(stats.attached && stats.harbor == harbor);
    pa_ctx_detach(&ctx);
    assert(pa_harbor_fini(harbor) == PA_OK);
}

static void low_water(pa_harbor *harbor, size_t index, void *ud)
{
    (void)harbor;
    assert(index == 0);
    ++*(unsigned *)ud;
}

static void test_exhaustion_and_reserve(void)
{
    static const pa_berth_config berths[] = {
        {.block_size = 64, .count = 4, .mag_depth = 2, .reserve_mags = 1},
    };
    unsigned notifications = 0;
    pa_harbor *harbor = make_harbor(berths, 1, PA_F_STRICT_TLS, low_water,
                                    &notifications);
    assert(pa_thread_attach(harbor) == PA_OK);
    pa_ship *normal[2] = {
        pa_charter_min(harbor, 1),
        pa_charter_min(harbor, 1),
    };
    assert(normal[0] && normal[1]);
    assert(pa_charter_min(harbor, 1) == NULL);
    assert(notifications == 1);
    pa_ship *critical[2] = {
        pa_charter_critical(harbor, 1, 0, 0),
        pa_charter_critical(harbor, 1, 0, 0),
    };
    assert(critical[0] && critical[1]);
    assert_conservation(harbor);
    assert(pa_charter_critical(harbor, 1, 0, 0) == NULL);
    pa_release(critical[0]);
    critical[0] = pa_charter_critical(harbor, 1, 0, 0);
    assert(critical[0]);
    assert_conservation(harbor);
    for (size_t i = 0; i < 2; ++i) {
        pa_release(normal[i]);
        pa_release(critical[i]);
    }
    assert_conservation(harbor);
    finish_harbor(harbor);
}

static void test_atomic_append_rollback(void)
{
    static const pa_berth_config berths[] = {
        {.block_size = 16, .count = 2, .mag_depth = 1},
    };
    pa_harbor *harbor = make_harbor(berths, 1, 0, NULL, NULL);
    assert(pa_thread_attach(harbor) == PA_OK);
    pa_bollard bollard;
    pa_bollard_init(&bollard, harbor);
    assert(pa_bollard_put(&bollard, 0) == NULL && bollard.count == 0);
    assert(pa_bollard_append_all(&bollard, "keep", 4) == PA_OK);
    assert(pa_bollard_append_all(&bollard, "this cannot fit in one remaining ship", 37) ==
           PA_E_NOMEM);
    assert(bollard.bytes == 4 && bollard.count == 1);
    assert_bollard_data(&bollard, "keep", 4);
    pa_bollard_release_all(&bollard);
    finish_harbor(harbor);
}

static void test_guard_quarantine(void)
{
#if !PA_DEBUG
    static const pa_berth_config berths[] = {
        {.block_size = 64, .count = 4, .mag_depth = 2},
    };
    pa_harbor *harbor = make_harbor(berths, 1, PA_F_GUARD, NULL, NULL);
    assert(pa_thread_attach(harbor) == PA_OK);
    pa_bollard bollard;
    pa_bollard_init(&bollard, harbor);
    pa_ship *ship = pa_charter_min(harbor, 8);
    assert(ship && pa_moor(&bollard, ship) == PA_OK);
    ship->hull[ship->cap] ^= 0xffu;
    pa_bollard_release_all(&bollard);
    assert(!bollard.first && !bollard.count && !bollard.bytes);
    pa_thread_detach(harbor);
    assert(pa_harbor_fini(harbor) == PA_E_STATE);
#endif
}

static void assert_bollard_data(pa_bollard *bollard, const void *expected, size_t n)
{
    uint8_t *actual = malloc(n ? n : 1u);
    assert(actual);
    assert(pa_bollard_read_at(bollard, 0, actual, n) == n);
    assert(memcmp(actual, expected, n) == 0);
    free(actual);
}

static void test_bollard_operations(void)
{
    static const pa_berth_config berths[] = {
        {.block_size = 16, .count = 24, .mag_depth = 4},
        {.block_size = 128, .count = 8, .mag_depth = 2},
    };
    pa_harbor *harbor = make_harbor(berths, 2, 0, NULL, NULL);
    assert(pa_thread_attach(harbor) == PA_OK);
    pa_bollard bollard;
    pa_bollard_init(&bollard, harbor);

    const char data[] = "abcdefghijklmnopqrstuvwxyz";
    for (size_t i = 0; i < 3; ++i) {
        pa_ship *ship = pa_charter_min(harbor, 8);
        assert(ship && pa_write(ship, data + i * 8u, 8) == 8);
        assert(pa_moor(&bollard, ship) == PA_OK);
    }
    assert(bollard.count == 3 && bollard.bytes == 24);
    assert_bollard_data(&bollard, data, 24);
#if PA_HAVE_IOVEC
    struct iovec too_small[1];
    assert(pa_bollard_iovec(&bollard, too_small, 1) == PA_E_TOOBIG);
#endif

    pa_bollard tail;
    pa_bollard_init(&tail, harbor);
    assert(pa_bollard_split(&bollard, 10, &tail) == PA_OK);
    assert_bollard_data(&bollard, data, 10);
    assert_bollard_data(&tail, data + 10, 14);
    assert(pa_bollard_concat(&bollard, &tail) == PA_OK);
    assert_bollard_data(&bollard, data, 24);
    char cursor_data[24];
    assert(pa_bollard_read(&bollard, cursor_data, 23) == 23);
    assert(pa_bollard_consume(&bollard, 16) == 16);
    assert(bollard.rcursor == 7);
    pa_bollard_release_all(&bollard);

    for (size_t i = 0; i < 3; ++i) {
        pa_ship *ship = pa_charter_min(harbor, 8);
        assert(ship && pa_write(ship, data + i * 8u, 8) == 8);
        assert(pa_moor(&bollard, ship) == PA_OK);
    }
    assert(pa_bollard_linearize(&bollard) == PA_OK);
    assert(bollard.count == 1 && bollard.bytes == 24);
    assert(pa_bollard_consume(&bollard, 5) == 5);
    assert_bollard_data(&bollard, data + 5, 19);
    pa_bollard_release_all(&bollard);

    uint8_t large[300];
    for (size_t i = 0; i < sizeof(large); ++i) large[i] = (uint8_t)i;
    assert(pa_bollard_append_all(&bollard, large, sizeof(large)) == PA_OK);
    assert(bollard.count == 3 && bollard.bytes == sizeof(large));
    assert_bollard_data(&bollard, large, sizeof(large));
    assert(pa_bollard_linearize(&bollard) == PA_E_TOOBIG);
    pa_bollard_release_all(&bollard);
    finish_harbor(harbor);
}

static void test_bollard_model(void)
{
    static const pa_berth_config berths[] = {
        {.block_size = 32, .count = 64, .mag_depth = 8},
        {.block_size = 128, .count = 64, .mag_depth = 4},
    };
    pa_harbor *harbor = make_harbor(berths, 2, 0, NULL, NULL);
    assert(pa_thread_attach(harbor) == PA_OK);
    pa_bollard bollard;
    pa_bollard_init(&bollard, harbor);
    uint8_t model[2048];
    uint8_t actual[2048];
    size_t length = 0;

    for (size_t step = 0; step < 20000; ++step) {
        if (step && step % 50u == 0u) {
            pa_bollard_release_all(&bollard);
            assert(pa_bollard_append_all(&bollard, model, length) == PA_OK);
        }
        size_t n = next_random() % 65u;
        switch (next_random() % 5u) {
        case 0:
            if (n > sizeof(model) - length) n = sizeof(model) - length;
            for (size_t i = 0; i < n; ++i) actual[i] = (uint8_t)next_random();
            assert(pa_bollard_append_all(&bollard, actual, n) == PA_OK);
            memcpy(model + length, actual, n);
            length += n;
            break;
        case 1: {
            size_t requested = length ? next_random() % (length + 32u) : n;
            size_t consumed = requested < length ? requested : length;
            assert(pa_bollard_consume(&bollard, requested) == consumed);
            memmove(model, model + consumed, length - consumed);
            length -= consumed;
            break;
        }
        case 2: {
            size_t off = length ? next_random() % (length + 1u) : 0u;
            pa_bollard tail;
            pa_bollard_init(&tail, harbor);
            assert(pa_bollard_split(&bollard, off, &tail) == PA_OK);
            assert(bollard.bytes == off && tail.bytes == length - off);
            assert(pa_bollard_concat(&bollard, &tail) == PA_OK);
            break;
        }
        case 3:
            if (n > sizeof(model) - length) n = sizeof(model) - length;
            if (n) {
                uint8_t *put = pa_bollard_put(&bollard, n);
                assert(put);
                for (size_t i = 0; i < n; ++i) put[i] = (uint8_t)next_random();
                memcpy(model + length, put, n);
                length += n;
            }
            break;
        default:
            if (length <= pa_harbor_max_payload(harbor))
                assert(pa_bollard_linearize(&bollard) == PA_OK);
            else if (bollard.count > 1u)
                assert(pa_bollard_linearize(&bollard) == PA_E_TOOBIG);
            break;
        }
        assert(bollard.bytes == length);
        assert(pa_bollard_read_at(&bollard, 0, actual, sizeof(actual)) == length);
        assert(memcmp(actual, model, length) == 0);
    }
    pa_bollard_release_all(&bollard);
    finish_harbor(harbor);
}

typedef struct {
    pa_harbor *harbor;
    unsigned loops;
} thread_arg;

typedef struct {
    pa_harbor *harbor;
    pa_ship **ships;
    size_t count;
} release_arg;

static void *allocator_worker(void *opaque)
{
    thread_arg *arg = opaque;
    assert(pa_thread_attach(arg->harbor) == PA_OK);
    for (unsigned i = 0; i < arg->loops; ++i) {
        pa_ship *ship;
        while (!(ship = pa_charter_min(arg->harbor, 32))) {
        }
        assert(pa_write(ship, &i, sizeof(i)) == (ssize_t)sizeof(i));
        pa_release(ship);
    }
    pa_thread_detach(arg->harbor);
    return NULL;
}

static void *release_worker(void *opaque)
{
    release_arg *arg = opaque;
    assert(pa_thread_attach(arg->harbor) == PA_OK);
    for (size_t i = 0; i < arg->count; ++i) pa_release(arg->ships[i]);
    pa_thread_detach(arg->harbor);
    return NULL;
}

static void test_threads(void)
{
    static const pa_berth_config berths[] = {
        {.block_size = 64, .count = 256, .mag_depth = 8},
    };
    pa_harbor *harbor = make_harbor(berths, 1, PA_F_STRICT_TLS, NULL, NULL);
    enum { THREADS = 8 };
    pthread_t threads[THREADS];
    thread_arg arg = {.harbor = harbor, .loops = 10000};
    for (size_t i = 0; i < THREADS; ++i)
        assert(pthread_create(&threads[i], NULL, allocator_worker, &arg) == 0);
    for (size_t i = 0; i < THREADS; ++i)
        assert(pthread_join(threads[i], NULL) == 0);
    pa_stats stats;
    pa_harbor_stats(harbor, &stats);
    assert(stats.n_threads_attached == 0);
    assert(stats.berths[0].n_inuse == 0);
    assert(stats.berths[0].n_charter == THREADS * arg.loops);
    assert(pa_harbor_fini(harbor) == PA_OK);
}

static void test_cross_thread_release(void)
{
    static const pa_berth_config berths[] = {
        {.block_size = 64, .count = 64, .mag_depth = 8},
    };
    pa_harbor *harbor = make_harbor(berths, 1, PA_F_STRICT_TLS, NULL, NULL);
    assert(pa_thread_attach(harbor) == PA_OK);
    pa_ship *ships[32];
    for (size_t i = 0; i < 32; ++i) {
        ships[i] = pa_charter_min(harbor, 8);
        assert(ships[i]);
    }
    pa_thread_detach(harbor);
    release_arg arg = {.harbor = harbor, .ships = ships, .count = 32};
    pthread_t thread;
    assert(pthread_create(&thread, NULL, release_worker, &arg) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(pa_harbor_fini(harbor) == PA_OK);
}

#if PA_HAVE_IOVEC
static void test_io(void)
{
    static const pa_berth_config berths[] = {
        {.block_size = 32, .count = 32, .mag_depth = 4},
    };
    pa_harbor *harbor = make_harbor(berths, 1, 0, NULL, NULL);
    assert(pa_thread_attach(harbor) == PA_OK);
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    uint8_t expected[100];
    for (size_t i = 0; i < sizeof(expected); ++i) expected[i] = (uint8_t)(255u - i);

    pa_bollard outgoing;
    pa_bollard_init(&outgoing, harbor);
    pa_bollard incoming;
    pa_bollard_init(&incoming, harbor);
    assert(pa_bollard_recv(&incoming, sockets[1], sizeof(expected), MSG_DONTWAIT) ==
           -EAGAIN);
    assert(!incoming.count && !incoming.bytes);
    assert(pa_bollard_append_all(&outgoing, expected, sizeof(expected)) == PA_OK);
    assert(pa_bollard_send(&outgoing, sockets[0], 0) == PA_OK);
    assert(outgoing.bytes == 0);
    assert(pa_bollard_recv(&incoming, sockets[1], sizeof(expected), 0) ==
           (ssize_t)sizeof(expected));
    assert_bollard_data(&incoming, expected, sizeof(expected));
    pa_bollard_release_all(&incoming);
    close(sockets[0]);
    close(sockets[1]);

    int datagrams[2];
    assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, datagrams) == 0);
    pa_bollard_init(&outgoing, harbor);
    assert(pa_bollard_append_all(&outgoing, expected, sizeof(expected)) == PA_OK);
    assert(pa_bollard_sendto(&outgoing, datagrams[0], 0, NULL, 0) == PA_OK);
    uint8_t received[sizeof(expected)];
    assert(recv(datagrams[1], received, sizeof(received), 0) == (ssize_t)sizeof(received));
    assert(memcmp(received, expected, sizeof(received)) == 0);
    close(datagrams[0]);
    close(datagrams[1]);

    int blocked[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, blocked) == 0);
    assert(fcntl(blocked[0], F_SETFL, O_NONBLOCK) == 0);
    assert(fcntl(blocked[1], F_SETFL, O_NONBLOCK) == 0);
    uint8_t junk[4096] = {0};
    while (send(blocked[0], junk, sizeof(junk), 0) > 0) {
    }
    assert(errno == EAGAIN || errno == EWOULDBLOCK);
    pa_bollard_init(&outgoing, harbor);
    assert(pa_bollard_append_all(&outgoing, expected, sizeof(expected)) == PA_OK);
    assert(pa_bollard_send(&outgoing, blocked[0], MSG_DONTWAIT) == -EAGAIN);
    assert(outgoing.bytes == sizeof(expected));
    while (recv(blocked[1], junk, sizeof(junk), 0) > 0) {
    }
    assert(pa_bollard_send(&outgoing, blocked[0], MSG_DONTWAIT) == PA_OK);
    close(blocked[0]);
    close(blocked[1]);
    finish_harbor(harbor);
}
#endif

int main(void)
{
    test_ship_operations();
    test_ship_model();
    test_remaining_api();
    test_explicit_context();
    test_exhaustion_and_reserve();
    test_atomic_append_rollback();
    test_guard_quarantine();
    test_bollard_operations();
    test_bollard_model();
    test_threads();
    test_cross_thread_release();
#if PA_HAVE_IOVEC
    test_io();
#endif
    puts("all tests passed");
    return 0;
}
