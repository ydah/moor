#ifndef POOL_ALLOCATOR_H
#define POOL_ALLOCATOR_H

/*
 * Fixed-capacity C11 buffer pool with no runtime allocation.
 *
 * A harbor lives entirely in caller-provided storage and may be shared across
 * threads. Thread-local APIs use an attached context; explicit-context APIs
 * provide the same behavior without TLS. Unless stated otherwise, returned
 * ships are owned by the caller until they are moored or released.
 */

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
#ifndef PA_IOV_MAX
#define PA_IOV_MAX 64
#endif
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

/* BORROWED, READONLY, and CRITICAL are managed by the allocator. */
#define PA_SHIP_BORROWED 0x0002u
#define PA_SHIP_READONLY 0x0004u
#define PA_SHIP_CRITICAL 0x0008u
#define PA_SHIP_USER0 0x1000u
#define PA_SHIP_USER1 0x2000u
#define PA_SHIP_USER2 0x4000u
#define PA_SHIP_USER3 0x8000u

/* Harbor policies selected through pa_harbor_config.flags. */
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

/* Negative return values used by operations that do not return errno. */
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

#ifdef PA_LOCK_TYPE
typedef PA_LOCK_TYPE pa_lock;
#else
typedef struct {
    atomic_flag flag;
} pa_lock;
#endif

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
    pa_mag m1;
    pa_mag m2;
} pa_ctx_berth;

typedef struct pa_ctx {
    pa_harbor *harbor;
    bool attached;
    pa_ctx_berth b[PA_MAX_BERTHS];
} pa_ctx;

typedef struct {
    size_t block_size;       /* Payload bytes per ship; berths must be ascending. */
    size_t count;            /* Number of ships provisioned in this berth. */
    uint16_t mag_depth;      /* Ships per magazine; zero selects an automatic value. */
    uint16_t reserve_mags;   /* Depot magazines reserved for critical charters. */
} pa_berth_config;

typedef struct {
    const pa_berth_config *berths;
    size_t berth_count;
    size_t max_threads;      /* Reserved for compatibility; currently ignored. */
    size_t default_bow;      /* Used when pa_charter receives SIZE_MAX. */
    size_t default_stern;    /* Used when pa_charter receives SIZE_MAX. */
    unsigned flags;          /* PA_F_* options. */
    /* Called without the berth lock when the depot reaches its reserve. */
    void (*low_water_cb)(pa_harbor *, size_t berth_idx, void *ud);
    void *ud;
} pa_harbor_config;

typedef struct {
    size_t block_size;
    size_t n_total;
    uint16_t mag_depth;
    size_t n_mags;
    size_t n_mags_min;
    size_t n_inuse;
    size_t n_peak;
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
    size_t n_mags;
    size_t n_mags_min;
    _Atomic size_t n_inuse;
    _Atomic size_t n_peak;
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
    pa_ctx *ctx;
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

/* Iteration is invalidated by mooring, unmooring, or releasing a ship. */
#define pa_bollard_foreach(s, b) \
    for (pa_ship *(s) = (b)->first; (s); (s) = pa_next(s))

/*
 * Harbor lifecycle. pa_harbor_bytes returns zero for an invalid configuration
 * or size overflow. pa_harbor_fini succeeds only after every context is
 * detached and every ship is released.
 */
size_t pa_harbor_bytes(const pa_harbor_config *cfg);
pa_harbor *pa_harbor_init(void *mem, size_t len, const pa_harbor_config *cfg);
int pa_harbor_fini(pa_harbor *h);
size_t pa_harbor_max_payload(const pa_harbor *h);
/* Availability is advisory and may change before the next charter. */
bool pa_harbor_avail(const pa_harbor *h, size_t need);
bool pa_harbor_avail_ctx(const pa_ctx *ctx, const pa_harbor *h, size_t need);
void pa_harbor_stats(const pa_harbor *h, pa_stats *out);

/*
 * Contexts cache free ships and must not be shared concurrently. The thread
 * variants use TLS; the ctx variants support explicit ownership or PA_NO_TLS.
 * Detaching returns both cached magazines to the harbor.
 */
int pa_thread_attach(pa_harbor *h);
void pa_thread_detach(pa_harbor *h);
void pa_thread_stats(pa_thread_stats_snapshot *out);
int pa_ctx_attach(pa_ctx *ctx, pa_harbor *h);
void pa_ctx_detach(pa_ctx *ctx);
void pa_ctx_stats(const pa_ctx *ctx, pa_thread_stats_snapshot *out);

/*
 * Charter, clone, and wrap return caller-owned sailing ships. Mooring transfers
 * ownership to a bollard; unmooring returns it. Release invalidates the ship
 * pointer and never frees a buffer supplied to pa_wrap. Critical charters
 * bypass context caches and may consume reserved magazines.
 */
pa_ship *pa_charter(pa_harbor *h, size_t payload, size_t bow, size_t stern);
pa_ship *pa_charter_min(pa_harbor *h, size_t total);
pa_ship *pa_charter_ctx(pa_ctx *ctx, pa_harbor *h, size_t payload,
                        size_t bow, size_t stern);
pa_ship *pa_charter_min_ctx(pa_ctx *ctx, pa_harbor *h, size_t total);
pa_ship *pa_charter_critical(pa_harbor *h, size_t payload, size_t bow, size_t stern);
void pa_release(pa_ship *s);
void pa_release_ctx(pa_ctx *ctx, pa_ship *s);
pa_ship *pa_wrap(pa_harbor *h, void *buf, size_t len, unsigned flags);
pa_ship *pa_wrap_ctx(pa_ctx *ctx, pa_harbor *h, void *buf, size_t len, unsigned flags);
void pa_reset(pa_ship *s);
pa_ship *pa_clone(pa_ship *s);
pa_ship *pa_clone_ctx(pa_ctx *ctx, pa_ship *s);

/*
 * Payload mutation. ssize_t writers return bytes written or a negative pa_err.
 * pa_put and pa_push expose uninitialized appended or prepended storage; the
 * returned pointer remains valid only while the ship storage is unchanged.
 */
ssize_t pa_write(pa_ship *s, const void *src, size_t n);
ssize_t pa_write_head(pa_ship *s, const void *src, size_t n);
int pa_write_at(pa_ship *s, size_t off, const void *src, size_t n);
void *pa_put(pa_ship *s, size_t n);
void *pa_push(pa_ship *s, size_t n);
void *pa_push_aligned(pa_ship *s, size_t n, size_t align);
int pa_pull(pa_ship *s, size_t n);
int pa_trim(pa_ship *s, size_t new_len);
int pa_reserve_bow(pa_ship *s, size_t n);

/* pa_read advances the cursor; pa_read_at and pa_peek leave it unchanged. */
size_t pa_read(pa_ship *s, void *dst, size_t n);
size_t pa_read_at(const pa_ship *s, size_t off, void *dst, size_t n);
const void *pa_peek(const pa_ship *s, size_t off, size_t need);
int pa_seek(pa_ship *s, ptrdiff_t off, int whence);
void pa_rewind(pa_ship *s);

/*
 * Bollards own their moored ships. pa_bollard_append may write a prefix when
 * the pool is exhausted; pa_bollard_append_all rolls back instead. Split and
 * concat transfer ships without copying except when splitting a payload.
 */
void pa_bollard_init(pa_bollard *b, pa_harbor *h);
void pa_bollard_init_ctx(pa_bollard *b, pa_harbor *h, pa_ctx *ctx);
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
/* iovec exposes segments; send consumes, recv appends, and -EAGAIN is retryable. */
int pa_bollard_iovec(const pa_bollard *b, struct iovec *iov, int max);
int pa_bollard_send(pa_bollard *b, int fd, int flags);
int pa_bollard_sendto(pa_bollard *b, int fd, int flags,
                       const struct sockaddr *to, socklen_t tolen);
ssize_t pa_bollard_recv(pa_bollard *b, int fd, size_t want, int flags);
#endif

/* Writes a diagnostic snapshot to out, or stderr when out is NULL. */
void pa_ship_dump(const pa_ship *s, FILE *out);

#if PA_DEBUG && !defined(PA_IMPLEMENTATION)
static inline pa_ship *pa_debug_owner(pa_ship *ship, const char *file, uint32_t line)
{
    if (ship) {
        ship->owner_file = file;
        ship->owner_line = line;
    }
    return ship;
}

#define pa_charter(h, payload, bow, stern) \
    pa_debug_owner(pa_charter((h), (payload), (bow), (stern)), __FILE__, __LINE__)
#define pa_charter_min(h, total) \
    pa_debug_owner(pa_charter_min((h), (total)), __FILE__, __LINE__)
#define pa_charter_ctx(ctx, h, payload, bow, stern) \
    pa_debug_owner(pa_charter_ctx((ctx), (h), (payload), (bow), (stern)), __FILE__, __LINE__)
#define pa_charter_min_ctx(ctx, h, total) \
    pa_debug_owner(pa_charter_min_ctx((ctx), (h), (total)), __FILE__, __LINE__)
#define pa_charter_critical(h, payload, bow, stern) \
    pa_debug_owner(pa_charter_critical((h), (payload), (bow), (stern)), __FILE__, __LINE__)
#define pa_wrap(h, buf, len, flags) \
    pa_debug_owner(pa_wrap((h), (buf), (len), (flags)), __FILE__, __LINE__)
#define pa_wrap_ctx(ctx, h, buf, len, flags) \
    pa_debug_owner(pa_wrap_ctx((ctx), (h), (buf), (len), (flags)), __FILE__, __LINE__)
#define pa_clone(ship) \
    pa_debug_owner(pa_clone((ship)), __FILE__, __LINE__)
#define pa_clone_ctx(ctx, ship) \
    pa_debug_owner(pa_clone_ctx((ctx), (ship)), __FILE__, __LINE__)
#endif

#endif
