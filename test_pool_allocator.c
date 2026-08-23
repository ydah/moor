#include "pool_allocator.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if PA_HAVE_IOVEC
#include <sys/socket.h>
#include <unistd.h>
#endif

#define TEST_STORAGE_SIZE (128u * 1024u)

static _Alignas(PA_SLOT_ALIGN) uint8_t storage[TEST_STORAGE_SIZE];

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

static void test_ship_operations(void)
{
    const pa_berth_config invalid_berth = {0};
    const pa_harbor_config invalid_config = {
        .berths = &invalid_berth,
        .berth_count = 1,
    };
    assert(pa_harbor_bytes(&invalid_config) == 0);

    static const pa_berth_config berths[] = {
        {.block_size = 128, .count = 16, .mag_depth = 4},
        {.block_size = 512, .count = 8, .mag_depth = 2},
    };
    pa_harbor *harbor = make_harbor(berths, 2, PA_F_STRICT_TLS | PA_F_GUARD,
                                    NULL, NULL);
    assert(pa_charter(harbor, 1, 0, 0) == NULL);
    assert(pa_thread_attach(harbor) == PA_OK);
    assert(pa_harbor_max_payload(harbor) == 512);
    assert(pa_harbor_avail(harbor, 120));

    pa_ship *ship = pa_charter(harbor, 100, SIZE_MAX, SIZE_MAX);
    assert(ship && ship->cap == 128 && pa_bow_room(ship) == 16);
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
    assert(pa_charter_critical(harbor, 1, 0, 0) == NULL);
    for (size_t i = 0; i < 2; ++i) {
        pa_release(normal[i]);
        pa_release(critical[i]);
    }
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

typedef struct {
    pa_harbor *harbor;
    unsigned loops;
} thread_arg;

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
    assert(pa_bollard_append_all(&outgoing, expected, sizeof(expected)) == PA_OK);
    assert(pa_bollard_send(&outgoing, sockets[0], 0) == PA_OK);
    assert(outgoing.bytes == 0);

    pa_bollard incoming;
    pa_bollard_init(&incoming, harbor);
    assert(pa_bollard_recv(&incoming, sockets[1], sizeof(expected), 0) ==
           (ssize_t)sizeof(expected));
    assert_bollard_data(&incoming, expected, sizeof(expected));
    pa_bollard_release_all(&incoming);
    close(sockets[0]);
    close(sockets[1]);
    finish_harbor(harbor);
}
#endif

int main(void)
{
    test_ship_operations();
    test_exhaustion_and_reserve();
    test_atomic_append_rollback();
    test_bollard_operations();
    test_threads();
#if PA_HAVE_IOVEC
    test_io();
#endif
    puts("all tests passed");
    return 0;
}
