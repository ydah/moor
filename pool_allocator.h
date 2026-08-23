#ifndef POOL_ALLOCATOR_H
#define POOL_ALLOCATOR_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#if !defined(PA_HAVE_IOVEC)
#  if defined(__unix__) || defined(__APPLE__)
#    define PA_HAVE_IOVEC 1
#  else
#    define PA_HAVE_IOVEC 0
#  endif
#endif

#if PA_HAVE_IOVEC
#include <sys/socket.h>
#include <sys/uio.h>
#endif

#ifndef PA_DEBUG
#define PA_DEBUG 0
#endif

#define PA_ALIGN 16u
#define PA_SLOT_ALIGN 64u
#define PA_ALIGN_UP(x, a) (((x) + ((a) - 1u)) & ~((size_t)(a) - 1u))
#define PA_MAX_BERTHS 8u
#define PA_MAG_BYTES 8192u
#define PA_MAG_MAX 64u
#define PA_FAST_INDEX_N 4096u
#define PA_GUARD_SIZE 8u

#define PA_SHIP_BORROWED 0x0002u
#define PA_SHIP_READONLY 0x0004u
#define PA_SHIP_CRITICAL 0x0008u
#define PA_SHIP_USER0 0x1000u
#define PA_SHIP_USER1 0x2000u
#define PA_SHIP_USER2 0x4000u
#define PA_SHIP_USER3 0x8000u

#define PA_F_ZERO_ON_FREE 0x0004u
#define PA_F_POISON 0x0008u
#define PA_F_GUARD 0x0010u
#define PA_F_STRICT_TLS 0x0020u

#define PA_SEEK_SET 0
#define PA_SEEK_CUR 1
#define PA_SEEK_END 2

typedef struct pa_harbor pa_harbor;
typedef struct pa_berth pa_berth;
typedef struct pa_ship pa_ship;
typedef struct pa_bollard pa_bollard;

typedef enum {
    PA_SHIP_FREE = 0,
    PA_SHIP_SAILING = 1,
    PA_SHIP_MOORED = 2
} pa_ship_state;

typedef enum {
    PA_OK = 0,
    PA_E_INVAL = -1,
    PA_E_NOMEM = -2,
    PA_E_NOSPACE = -3,
    PA_E_RANGE = -4,
    PA_E_STATE = -5,
    PA_E_TOOBIG = -6,
    PA_E_IO = -7
} pa_err;

typedef struct {
    atomic_flag flag;
} pa_lock;

struct pa_ship {
    union {
        struct {
            pa_ship *next;
            pa_ship *prev;
            pa_bollard *moored;
        } s;
        struct {
            pa_ship *next;
            pa_ship *mag_tail;
            pa_ship *mag_next;
        } f;
    } u;
    pa_berth *berth;
    uint8_t *hull;
    uint32_t cap;
    uint32_t data;
    uint32_t tail;
    uint32_t cursor;
    uint16_t state;
    uint16_t flags;
    uint16_t mag_n;
    uint16_t _rsv;
#if PA_DEBUG
    uint32_t magic;
    const char *owner_file;
    uint32_t owner_line;
#endif
};

#define pa_next(s) ((s)->u.s.next)
#define pa_prev(s) ((s)->u.s.prev)
#define pa_moored(s) ((s)->u.s.moored)

typedef struct {
    pa_ship *head;
    pa_ship *tail;
    uint16_t n;
} pa_mag;

typedef struct {
    size_t block_size;
    size_t count;
    uint16_t mag_depth;
    uint16_t reserve_mags;
} pa_berth_config;

typedef struct {
    const pa_berth_config *berths;
    size_t berth_count;
    size_t max_threads;
    size_t default_bow;
    size_t default_stern;
    unsigned flags;
    void (*low_water_cb)(pa_harbor *, size_t berth_idx, void *ud);
    void *ud;
} pa_harbor_config;

typedef struct {
    size_t block_size;
    size_t n_total;
    uint16_t mag_depth;
    uint16_t n_mags;
    uint16_t n_mags_min;
    uint32_t n_inuse;
    uint32_t n_peak;
    uint64_t n_charter;
    uint64_t n_depot_hit;
    uint64_t n_fail;
} pa_berth_stats;

typedef struct {
    size_t berth_count;
    uint32_t n_threads_attached;
    pa_berth_stats berths[PA_MAX_BERTHS];
} pa_stats;

typedef struct {
    uint16_t m1;
    uint16_t m2;
} pa_thread_berth_stats;

typedef struct {
    bool attached;
    pa_harbor *harbor;
    pa_thread_berth_stats berths[PA_MAX_BERTHS];
} pa_thread_stats_snapshot;

struct pa_berth {
    size_t block_size;
    size_t slot_size;
    size_t n_total;
    uint16_t mag_depth;
    uint16_t reserve_mags;
    uint8_t *arena;
    pa_harbor *harbor;
    pa_lock lock;
    pa_ship *depot;
    uint16_t n_mags;
    uint16_t n_mags_min;
    _Atomic uint32_t n_inuse;
    _Atomic uint32_t n_peak;
    _Atomic uint64_t n_charter;
    _Atomic uint64_t n_depot_hit;
    _Atomic uint64_t n_fail;
};

struct pa_harbor {
    pa_berth berths[PA_MAX_BERTHS];
    size_t berth_count;
    size_t default_bow;
    size_t default_stern;
    size_t max_payload;
    unsigned flags;
    uint8_t size_index[PA_FAST_INDEX_N / 64u];
    uint8_t *base;
    size_t bytes;
    _Atomic uint32_t n_threads_attached;
    void (*low_water_cb)(pa_harbor *, size_t, void *);
    void *ud;
};

struct pa_bollard {
    pa_ship *first;
    pa_ship *last;
    size_t bytes;
    size_t rcursor;
    uint32_t count;
    pa_harbor *harbor;
    void *user;
};

#if !PA_DEBUG
_Static_assert(sizeof(pa_ship) == 64u, "pa_ship must fit one cache line");
#endif

static inline size_t pa_len(const pa_ship *s) { return s->tail - s->data; }
static inline size_t pa_bow_room(const pa_ship *s) { return s->data; }
static inline size_t pa_stern_room(const pa_ship *s) { return s->cap - s->tail; }
static inline void *pa_data(const pa_ship *s) { return s->hull + s->data; }
static inline void *pa_tailp(const pa_ship *s) { return s->hull + s->tail; }
static inline size_t pa_tell(const pa_ship *s) { return s->cursor - s->data; }
static inline size_t pa_bollard_len(const pa_bollard *b) { return b->bytes; }

#define PA_HARBOR_STORAGE(name, bytes) \
    _Alignas(PA_SLOT_ALIGN) static uint8_t name[bytes]

#define pa_bollard_foreach(s, b) \
    for (pa_ship *(s) = (b)->first; (s); (s) = pa_next(s))

size_t pa_harbor_bytes(const pa_harbor_config *cfg);
pa_harbor *pa_harbor_init(void *mem, size_t len, const pa_harbor_config *cfg);
int pa_harbor_fini(pa_harbor *h);
size_t pa_harbor_max_payload(const pa_harbor *h);
bool pa_harbor_avail(const pa_harbor *h, size_t need);
void pa_harbor_stats(const pa_harbor *h, pa_stats *out);

int pa_thread_attach(pa_harbor *h);
void pa_thread_detach(pa_harbor *h);
void pa_thread_stats(pa_thread_stats_snapshot *out);

pa_ship *pa_charter(pa_harbor *h, size_t payload, size_t bow, size_t stern);
pa_ship *pa_charter_min(pa_harbor *h, size_t total);
pa_ship *pa_charter_critical(pa_harbor *h, size_t payload, size_t bow, size_t stern);
void pa_release(pa_ship *s);
pa_ship *pa_wrap(pa_harbor *h, void *buf, size_t len, unsigned flags);
void pa_reset(pa_ship *s);
pa_ship *pa_clone(pa_ship *s);

ssize_t pa_write(pa_ship *s, const void *src, size_t n);
ssize_t pa_write_head(pa_ship *s, const void *src, size_t n);
int pa_write_at(pa_ship *s, size_t off, const void *src, size_t n);
void *pa_put(pa_ship *s, size_t n);
void *pa_push(pa_ship *s, size_t n);
void *pa_push_aligned(pa_ship *s, size_t n, size_t align);
int pa_pull(pa_ship *s, size_t n);
int pa_trim(pa_ship *s, size_t new_len);
int pa_reserve_bow(pa_ship *s, size_t n);

size_t pa_read(pa_ship *s, void *dst, size_t n);
size_t pa_read_at(const pa_ship *s, size_t off, void *dst, size_t n);
const void *pa_peek(const pa_ship *s, size_t off, size_t need);
int pa_seek(pa_ship *s, ptrdiff_t off, int whence);
void pa_rewind(pa_ship *s);

void pa_bollard_init(pa_bollard *b, pa_harbor *h);
void pa_bollard_release_all(pa_bollard *b);
int pa_moor(pa_bollard *b, pa_ship *s);
int pa_moor_front(pa_bollard *b, pa_ship *s);
int pa_moor_after(pa_bollard *b, pa_ship *at, pa_ship *s);
int pa_unmoor(pa_bollard *b, pa_ship *s);
pa_ship *pa_unmoor_first(pa_bollard *b);
ssize_t pa_bollard_append(pa_bollard *b, const void *src, size_t n);
int pa_bollard_append_all(pa_bollard *b, const void *src, size_t n);
void *pa_bollard_put(pa_bollard *b, size_t n);
size_t pa_bollard_read_at(const pa_bollard *b, size_t off, void *dst, size_t n);
size_t pa_bollard_read(pa_bollard *b, void *dst, size_t n);
int pa_bollard_linearize(pa_bollard *b);
int pa_bollard_split(pa_bollard *b, size_t off, pa_bollard *tail);
int pa_bollard_concat(pa_bollard *dst, pa_bollard *src);
size_t pa_bollard_consume(pa_bollard *b, size_t n);

#if PA_HAVE_IOVEC
int pa_bollard_iovec(const pa_bollard *b, struct iovec *iov, int max);
int pa_bollard_send(pa_bollard *b, int fd, int flags);
int pa_bollard_sendto(pa_bollard *b, int fd, int flags,
                       const struct sockaddr *to, socklen_t tolen);
ssize_t pa_bollard_recv(pa_bollard *b, int fd, size_t want, int flags);
#endif

void pa_ship_dump(const pa_ship *s, FILE *out);

#endif
