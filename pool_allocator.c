#include "pool_allocator.h"

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <string.h>

#if PA_DEBUG
#define PA_MAGIC 0x50415348u
#define PA_ASSERT(x) assert(x)
#else
#define PA_ASSERT(x) ((void)0)
#endif

typedef struct {
    pa_mag m1;
    pa_mag m2;
} pa_tls_berth;

typedef struct {
    pa_harbor *harbor;
    bool attached;
    pa_tls_berth b[PA_MAX_BERTHS];
} pa_tls;

static _Thread_local pa_tls pa_tls_self;

static bool add_overflow(size_t a, size_t b, size_t *out)
{
    if (a > SIZE_MAX - b) return true;
    *out = a + b;
    return false;
}

static bool mul_overflow(size_t a, size_t b, size_t *out)
{
    if (a && b > SIZE_MAX / a) return true;
    *out = a * b;
    return false;
}

static void pa_lock_acquire(pa_lock *lock)
{
    while (atomic_flag_test_and_set_explicit(&lock->flag, memory_order_acquire)) {
#if defined(__x86_64__) || defined(__i386__)
        __asm__ volatile("pause");
#elif defined(__aarch64__) || defined(__arm__)
        __asm__ volatile("yield");
#endif
    }
}

static void pa_lock_release(pa_lock *lock)
{
    atomic_flag_clear_explicit(&lock->flag, memory_order_release);
}

static uint16_t pa_auto_depth(size_t block_size)
{
    size_t depth = PA_MAG_BYTES / block_size;
    if (depth < 2u) depth = 2u;
    if (depth > PA_MAG_MAX) depth = PA_MAG_MAX;
    return (uint16_t)depth;
}

static size_t guard_size(const pa_harbor_config *cfg)
{
    return (cfg->flags & PA_F_GUARD) ? PA_GUARD_SIZE : 0u;
}

static size_t ship_header_size(void)
{
    return PA_ALIGN_UP(sizeof(pa_ship), PA_ALIGN);
}

static bool config_valid(const pa_harbor_config *cfg)
{
    if (!cfg || !cfg->berths || !cfg->berth_count ||
        cfg->berth_count > PA_MAX_BERTHS) return false;
    if (cfg->default_bow > UINT32_MAX || cfg->default_stern > UINT32_MAX) return false;

    size_t previous = 0;
    for (size_t i = 0; i < cfg->berth_count; ++i) {
        const pa_berth_config *bc = &cfg->berths[i];
        if (!bc->block_size || bc->block_size > UINT32_MAX || !bc->count ||
            bc->count > UINT16_MAX || bc->block_size <= previous) return false;
        uint16_t depth = bc->mag_depth ? bc->mag_depth : pa_auto_depth(bc->block_size);
        if (!depth || depth > PA_MAG_MAX) return false;
        size_t mags = bc->count / depth + (bc->count % depth != 0u);
        if (mags > UINT16_MAX || bc->reserve_mags > mags) return false;
        previous = bc->block_size;
    }
    return true;
}

size_t pa_harbor_bytes(const pa_harbor_config *cfg)
{
    if (!config_valid(cfg)) return 0;

    size_t total = PA_SLOT_ALIGN - 1u;
    if (add_overflow(total, PA_ALIGN_UP(sizeof(pa_harbor), PA_SLOT_ALIGN), &total)) return 0;
    for (size_t i = 0; i < cfg->berth_count; ++i) {
        size_t raw;
        size_t slots;
        if (add_overflow(ship_header_size(), cfg->berths[i].block_size, &raw) ||
            add_overflow(raw, guard_size(cfg), &raw)) return 0;
        size_t slot_size = PA_ALIGN_UP(raw, PA_SLOT_ALIGN);
        if (mul_overflow(slot_size, cfg->berths[i].count, &slots) ||
            add_overflow(total, slots, &total)) return 0;
    }
    return total;
}

static void mag_push(pa_mag *mag, pa_ship *ship)
{
    ship->u.f.next = mag->head;
    mag->head = ship;
    if (!mag->n) mag->tail = ship;
    ++mag->n;
}

static void depot_push_mag(pa_berth *berth, const pa_mag *mag)
{
    if (!mag->n) return;
    pa_ship *head = mag->head;
    head->u.f.mag_tail = mag->tail;
    head->mag_n = mag->n;
    pa_lock_acquire(&berth->lock);
    head->u.f.mag_next = berth->depot;
    berth->depot = head;
    ++berth->n_mags;
    pa_lock_release(&berth->lock);
}

static bool depot_pop_mag(pa_berth *berth, pa_mag *out)
{
    bool low_water = false;
    pa_lock_acquire(&berth->lock);
    pa_ship *head = berth->depot;
    if (!head || berth->n_mags <= berth->reserve_mags) {
        atomic_fetch_add_explicit(&berth->n_fail, 1u, memory_order_relaxed);
        head = NULL;
    } else {
        berth->depot = head->u.f.mag_next;
        --berth->n_mags;
        if (berth->n_mags < berth->n_mags_min) berth->n_mags_min = berth->n_mags;
        low_water = berth->n_mags == berth->reserve_mags;
        atomic_fetch_add_explicit(&berth->n_depot_hit, 1u, memory_order_relaxed);
    }
    pa_lock_release(&berth->lock);
    if (!head) return false;

    out->head = head;
    out->tail = head->u.f.mag_tail;
    out->n = head->mag_n;
    if (low_water && berth->harbor->low_water_cb) {
        berth->harbor->low_water_cb(berth->harbor,
                                    (size_t)(berth - berth->harbor->berths),
                                    berth->harbor->ud);
    }
    return true;
}

static pa_ship *depot_pop_one(pa_berth *berth)
{
    pa_lock_acquire(&berth->lock);
    pa_ship *ship = berth->depot;
    if (!ship) {
        atomic_fetch_add_explicit(&berth->n_fail, 1u, memory_order_relaxed);
        pa_lock_release(&berth->lock);
        return NULL;
    }

    uint16_t n = ship->mag_n;
    pa_ship *next_ship = ship->u.f.next;
    pa_ship *next_mag = ship->u.f.mag_next;
    pa_ship *tail = ship->u.f.mag_tail;
    if (n == 1u) {
        berth->depot = next_mag;
        --berth->n_mags;
    } else {
        next_ship->u.f.mag_tail = tail;
        next_ship->u.f.mag_next = next_mag;
        next_ship->mag_n = (uint16_t)(n - 1u);
        berth->depot = next_ship;
    }
    if (berth->n_mags < berth->n_mags_min) berth->n_mags_min = berth->n_mags;
    atomic_fetch_add_explicit(&berth->n_depot_hit, 1u, memory_order_relaxed);
    pa_lock_release(&berth->lock);
    return ship;
}

static void guard_set(pa_ship *ship)
{
    if (!(ship->berth->harbor->flags & PA_F_GUARD)) return;
    memset(ship->hull + ship->cap, 0xa5, PA_GUARD_SIZE);
}

static bool guard_valid(const pa_ship *ship)
{
    if ((ship->flags & PA_SHIP_BORROWED) ||
        !(ship->berth->harbor->flags & PA_F_GUARD)) return true;
    for (size_t i = 0; i < PA_GUARD_SIZE; ++i) {
        if (ship->hull[ship->cap + i] != 0xa5) return false;
    }
    return true;
}

static void store_reset_bow(pa_ship *ship, uint32_t bow)
{
    ship->mag_n = (uint16_t)bow;
    ship->_rsv = (uint16_t)(bow >> 16u);
}

static uint32_t load_reset_bow(const pa_ship *ship)
{
    return (uint32_t)ship->mag_n | ((uint32_t)ship->_rsv << 16u);
}

static int berth_index(const pa_harbor *harbor, size_t need)
{
    if (need > harbor->max_payload) return -1;
    size_t i = 0;
    if (need && need < PA_FAST_INDEX_N) i = harbor->size_index[(need - 1u) / 64u];
    while (i < harbor->berth_count && harbor->berths[i].block_size < need) ++i;
    return i < harbor->berth_count ? (int)i : -1;
}

static pa_ship *tls_pop(pa_tls_berth *tls, pa_berth *berth)
{
    if (!tls->m1.n) {
        if (tls->m2.n) {
            pa_mag swap = tls->m1;
            tls->m1 = tls->m2;
            tls->m2 = swap;
        } else if (!depot_pop_mag(berth, &tls->m1)) {
            return NULL;
        }
    }

    pa_ship *ship = tls->m1.head;
    tls->m1.head = ship->u.f.next;
    if (!--tls->m1.n) tls->m1.tail = NULL;
    return ship;
}

static void tls_push(pa_tls_berth *tls, pa_berth *berth, pa_ship *ship)
{
    if (tls->m1.n == berth->mag_depth) {
        if (!tls->m2.n) {
            pa_mag swap = tls->m1;
            tls->m1 = tls->m2;
            tls->m2 = swap;
        } else {
            depot_push_mag(berth, &tls->m1);
            memset(&tls->m1, 0, sizeof(tls->m1));
        }
    }
    mag_push(&tls->m1, ship);
}

pa_harbor *pa_harbor_init(void *mem, size_t len, const pa_harbor_config *cfg)
{
    size_t required = pa_harbor_bytes(cfg);
    if (!mem || !required || len < required) return NULL;

    uintptr_t start = (uintptr_t)mem;
    uintptr_t aligned = PA_ALIGN_UP(start, PA_SLOT_ALIGN);
    size_t prefix = (size_t)(aligned - start);
    size_t header = PA_ALIGN_UP(sizeof(pa_harbor), PA_SLOT_ALIGN);
    if (prefix > len || header > len - prefix) return NULL;

    pa_harbor *harbor = (pa_harbor *)aligned;
    memset(harbor, 0, sizeof(*harbor));
    harbor->berth_count = cfg->berth_count;
    harbor->default_bow = cfg->default_bow;
    harbor->default_stern = cfg->default_stern;
    harbor->flags = cfg->flags;
    harbor->base = mem;
    harbor->bytes = len;
    harbor->low_water_cb = cfg->low_water_cb;
    harbor->ud = cfg->ud;

    uint8_t *cursor = (uint8_t *)harbor + header;
    for (size_t i = 0; i < cfg->berth_count; ++i) {
        const pa_berth_config *bc = &cfg->berths[i];
        pa_berth *berth = &harbor->berths[i];
        berth->block_size = bc->block_size;
        berth->slot_size = PA_ALIGN_UP(ship_header_size() + bc->block_size + guard_size(cfg),
                                       PA_SLOT_ALIGN);
        berth->n_total = bc->count;
        berth->mag_depth = bc->mag_depth ? bc->mag_depth : pa_auto_depth(bc->block_size);
        berth->reserve_mags = bc->reserve_mags;
        berth->arena = cursor;
        berth->harbor = harbor;
        atomic_flag_clear(&berth->lock.flag);

        pa_mag mag = {0};
        for (size_t j = 0; j < berth->n_total; ++j, cursor += berth->slot_size) {
            pa_ship *ship = (pa_ship *)cursor;
            memset(ship, 0, sizeof(*ship));
            ship->berth = berth;
            ship->hull = cursor + ship_header_size();
            ship->cap = (uint32_t)berth->block_size;
            ship->state = PA_SHIP_FREE;
#if PA_DEBUG
            ship->magic = PA_MAGIC;
#endif
            guard_set(ship);
            mag_push(&mag, ship);
            if (mag.n == berth->mag_depth) {
                depot_push_mag(berth, &mag);
                memset(&mag, 0, sizeof(mag));
            }
        }
        depot_push_mag(berth, &mag);
        berth->n_mags_min = berth->n_mags;
        harbor->max_payload = berth->block_size;
    }

    for (size_t bucket = 0; bucket < sizeof(harbor->size_index); ++bucket) {
        size_t need = bucket * 64u + 1u;
        size_t i = 0;
        while (i + 1u < harbor->berth_count && harbor->berths[i].block_size < need) ++i;
        harbor->size_index[bucket] = (uint8_t)i;
    }
    return harbor;
}

int pa_harbor_fini(pa_harbor *harbor)
{
    if (!harbor) return PA_E_INVAL;
    if (atomic_load_explicit(&harbor->n_threads_attached, memory_order_relaxed))
        return PA_E_STATE;

    for (size_t i = 0; i < harbor->berth_count; ++i) {
        pa_berth *berth = &harbor->berths[i];
        if (atomic_load_explicit(&berth->n_inuse, memory_order_relaxed)) return PA_E_STATE;
        size_t free_ships = 0;
        pa_lock_acquire(&berth->lock);
        for (pa_ship *mag = berth->depot; mag; mag = mag->u.f.mag_next)
            free_ships += mag->mag_n;
        pa_lock_release(&berth->lock);
        if (free_ships != berth->n_total) return PA_E_STATE;
        for (size_t j = 0; j < berth->n_total; ++j) {
            pa_ship *ship = (pa_ship *)(berth->arena + j * berth->slot_size);
            if (ship->state != PA_SHIP_FREE || !guard_valid(ship)) return PA_E_STATE;
        }
    }
    return PA_OK;
}

size_t pa_harbor_max_payload(const pa_harbor *harbor)
{
    return harbor ? harbor->max_payload : 0u;
}

bool pa_harbor_avail(const pa_harbor *harbor, size_t need)
{
    if (!harbor) return false;
    int index = berth_index(harbor, need);
    if (index < 0) return false;
    if (pa_tls_self.attached && pa_tls_self.harbor == harbor) {
        const pa_tls_berth *tls = &pa_tls_self.b[index];
        if (tls->m1.n || tls->m2.n) return true;
    }
    pa_berth *berth = (pa_berth *)&harbor->berths[index];
    pa_lock_acquire(&berth->lock);
    bool available = berth->depot && berth->n_mags > berth->reserve_mags;
    pa_lock_release(&berth->lock);
    return available;
}

void pa_harbor_stats(const pa_harbor *harbor, pa_stats *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!harbor) return;
    out->berth_count = harbor->berth_count;
    out->n_threads_attached = atomic_load_explicit(&harbor->n_threads_attached,
                                                   memory_order_relaxed);
    for (size_t i = 0; i < harbor->berth_count; ++i) {
        const pa_berth *berth = &harbor->berths[i];
        pa_berth_stats *stats = &out->berths[i];
        stats->block_size = berth->block_size;
        stats->n_total = berth->n_total;
        stats->mag_depth = berth->mag_depth;
        pa_lock_acquire((pa_lock *)&berth->lock);
        stats->n_mags = berth->n_mags;
        stats->n_mags_min = berth->n_mags_min;
        pa_lock_release((pa_lock *)&berth->lock);
        stats->n_inuse = atomic_load_explicit(&berth->n_inuse, memory_order_relaxed);
        stats->n_peak = atomic_load_explicit(&berth->n_peak, memory_order_relaxed);
        stats->n_charter = atomic_load_explicit(&berth->n_charter, memory_order_relaxed);
        stats->n_depot_hit = atomic_load_explicit(&berth->n_depot_hit, memory_order_relaxed);
        stats->n_fail = atomic_load_explicit(&berth->n_fail, memory_order_relaxed);
    }
}

int pa_thread_attach(pa_harbor *harbor)
{
    if (!harbor) return PA_E_INVAL;
    if (pa_tls_self.attached)
        return pa_tls_self.harbor == harbor ? PA_OK : PA_E_STATE;
    memset(&pa_tls_self, 0, sizeof(pa_tls_self));
    pa_tls_self.harbor = harbor;
    pa_tls_self.attached = true;
    atomic_fetch_add_explicit(&harbor->n_threads_attached, 1u, memory_order_relaxed);
    return PA_OK;
}

void pa_thread_detach(pa_harbor *harbor)
{
    if (!harbor || !pa_tls_self.attached || pa_tls_self.harbor != harbor) return;
    for (size_t i = 0; i < harbor->berth_count; ++i) {
        pa_tls_berth *tls = &pa_tls_self.b[i];
        depot_push_mag(&harbor->berths[i], &tls->m1);
        depot_push_mag(&harbor->berths[i], &tls->m2);
    }
    memset(&pa_tls_self, 0, sizeof(pa_tls_self));
    atomic_fetch_sub_explicit(&harbor->n_threads_attached, 1u, memory_order_relaxed);
}

void pa_thread_stats(pa_thread_stats_snapshot *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->attached = pa_tls_self.attached;
    out->harbor = pa_tls_self.harbor;
    for (size_t i = 0; i < PA_MAX_BERTHS; ++i) {
        out->berths[i].m1 = pa_tls_self.b[i].m1.n;
        out->berths[i].m2 = pa_tls_self.b[i].m2.n;
    }
}

static void update_peak(pa_berth *berth, uint32_t current)
{
    uint32_t peak = atomic_load_explicit(&berth->n_peak, memory_order_relaxed);
    while (peak < current &&
           !atomic_compare_exchange_weak_explicit(&berth->n_peak, &peak, current,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) {
    }
}

static pa_ship *charter_common(pa_harbor *harbor, size_t payload, size_t bow,
                               size_t stern, bool critical)
{
    if (!harbor) return NULL;
    if (bow == SIZE_MAX) bow = harbor->default_bow;
    if (stern == SIZE_MAX) stern = harbor->default_stern;
    size_t need;
    if (add_overflow(bow, payload, &need) || add_overflow(need, stern, &need) ||
        bow > UINT32_MAX) return NULL;
    int index = berth_index(harbor, need);
    if (index < 0) return NULL;

    pa_ship *ship;
    if (critical) {
        ship = depot_pop_one(&harbor->berths[index]);
    } else {
        if (!pa_tls_self.attached) {
            if (harbor->flags & PA_F_STRICT_TLS) return NULL;
            if (pa_thread_attach(harbor) != PA_OK) return NULL;
        }
        if (pa_tls_self.harbor != harbor) return NULL;
        ship = tls_pop(&pa_tls_self.b[index], &harbor->berths[index]);
    }
    if (!ship) return NULL;

    ship->u.s.next = NULL;
    ship->u.s.prev = NULL;
    ship->u.s.moored = NULL;
    ship->data = (uint32_t)bow;
    ship->tail = (uint32_t)bow;
    ship->cursor = (uint32_t)bow;
    ship->state = PA_SHIP_SAILING;
    ship->flags = critical ? PA_SHIP_CRITICAL : 0u;
    store_reset_bow(ship, (uint32_t)bow);
    uint32_t inuse = atomic_fetch_add_explicit(&ship->berth->n_inuse, 1u,
                                               memory_order_relaxed) + 1u;
    atomic_fetch_add_explicit(&ship->berth->n_charter, 1u, memory_order_relaxed);
    update_peak(ship->berth, inuse);
    return ship;
}

pa_ship *pa_charter(pa_harbor *harbor, size_t payload, size_t bow, size_t stern)
{
    return charter_common(harbor, payload, bow, stern, false);
}

pa_ship *pa_charter_min(pa_harbor *harbor, size_t total)
{
    return pa_charter(harbor, total, 0u, 0u);
}

pa_ship *pa_charter_critical(pa_harbor *harbor, size_t payload, size_t bow,
                             size_t stern)
{
    return charter_common(harbor, payload, bow, stern, true);
}

void pa_release(pa_ship *ship)
{
    if (!ship) return;
#if PA_DEBUG
    PA_ASSERT(ship->magic == PA_MAGIC);
#endif
    PA_ASSERT(ship->state != PA_SHIP_FREE);
    PA_ASSERT(guard_valid(ship));
    if (ship->state == PA_SHIP_FREE || !guard_valid(ship)) return;
    if (ship->u.s.moored) (void)pa_unmoor(ship->u.s.moored, ship);

    pa_berth *berth = ship->berth;
    pa_harbor *harbor = berth->harbor;
    bool borrowed = (ship->flags & PA_SHIP_BORROWED) != 0u;
    if (borrowed) {
        ship->hull = (uint8_t *)ship + ship_header_size();
        ship->cap = (uint32_t)berth->block_size;
        guard_set(ship);
    } else if (harbor->flags & PA_F_ZERO_ON_FREE) {
        memset(ship->hull, 0, ship->cap);
    } else if (harbor->flags & PA_F_POISON) {
        memset(ship->hull, 0xdd, ship->cap);
    }

    ship->state = PA_SHIP_FREE;
    ship->flags = 0u;
    ship->_rsv = 0u;
    atomic_fetch_sub_explicit(&berth->n_inuse, 1u, memory_order_relaxed);
    size_t index = (size_t)(berth - harbor->berths);
    if (pa_tls_self.attached && pa_tls_self.harbor == harbor) {
        tls_push(&pa_tls_self.b[index], berth, ship);
    } else {
        pa_mag mag = {0};
        mag_push(&mag, ship);
        depot_push_mag(berth, &mag);
    }
}

pa_ship *pa_wrap(pa_harbor *harbor, void *buf, size_t len, unsigned flags)
{
    if ((!buf && len) || len > UINT32_MAX) return NULL;
    pa_ship *ship = pa_charter_min(harbor, 0u);
    if (!ship) return NULL;
    ship->hull = buf;
    ship->cap = (uint32_t)len;
    ship->data = 0u;
    ship->tail = (uint32_t)len;
    ship->cursor = 0u;
    ship->flags = (uint16_t)(flags | PA_SHIP_BORROWED | PA_SHIP_READONLY);
    store_reset_bow(ship, 0u);
    return ship;
}

void pa_reset(pa_ship *ship)
{
    if (!ship || ship->state == PA_SHIP_FREE || (ship->flags & PA_SHIP_READONLY)) return;
    uint32_t bow = load_reset_bow(ship);
    ship->data = bow;
    ship->tail = bow;
    ship->cursor = bow;
    if (ship->u.s.moored) {
        pa_bollard *bollard = ship->u.s.moored;
        bollard->bytes = 0u;
        for (pa_ship *it = bollard->first; it; it = it->u.s.next)
            bollard->bytes += pa_len(it);
        if (bollard->rcursor > bollard->bytes) bollard->rcursor = bollard->bytes;
    }
}

pa_ship *pa_clone(pa_ship *ship)
{
    if (!ship || ship->state == PA_SHIP_FREE) return NULL;
    size_t length = pa_len(ship);
    pa_ship *copy = pa_charter(ship->berth->harbor, length, ship->data,
                               ship->cap - ship->tail);
    if (!copy) return NULL;
    if (pa_write(copy, pa_data(ship), length) < 0) {
        pa_release(copy);
        return NULL;
    }
    copy->flags |= ship->flags & (PA_SHIP_USER0 | PA_SHIP_USER1 |
                                  PA_SHIP_USER2 | PA_SHIP_USER3);
    return copy;
}

static bool ship_writable(const pa_ship *ship)
{
    return ship && ship->state != PA_SHIP_FREE && !(ship->flags & PA_SHIP_READONLY);
}

ssize_t pa_write(pa_ship *ship, const void *src, size_t n)
{
    if (!ship_writable(ship)) return PA_E_STATE;
    if (!src && n) return PA_E_INVAL;
    if (n > (size_t)SSIZE_MAX) return PA_E_TOOBIG;
    if (n > pa_stern_room(ship)) return PA_E_NOSPACE;
    if (n) memcpy(pa_tailp(ship), src, n);
    ship->tail += (uint32_t)n;
    if (ship->u.s.moored) ship->u.s.moored->bytes += n;
    return (ssize_t)n;
}

ssize_t pa_write_head(pa_ship *ship, const void *src, size_t n)
{
    if (!ship_writable(ship)) return PA_E_STATE;
    if (!src && n) return PA_E_INVAL;
    if (n > (size_t)SSIZE_MAX) return PA_E_TOOBIG;
    if (n > ship->cap - ship->data) return PA_E_NOSPACE;
    size_t old_len = pa_len(ship);
    if (n) memcpy(ship->hull + ship->data, src, n);
    ship->tail = ship->data + (uint32_t)n;
    ship->cursor = ship->data;
    if (ship->u.s.moored)
        ship->u.s.moored->bytes = ship->u.s.moored->bytes - old_len + n;
    return (ssize_t)n;
}

int pa_write_at(pa_ship *ship, size_t off, const void *src, size_t n)
{
    if (!ship_writable(ship)) return PA_E_STATE;
    if (!src && n) return PA_E_INVAL;
    size_t length = pa_len(ship);
    if (off > length) return PA_E_RANGE;
    if (n > ship->cap - ship->data - off) return PA_E_NOSPACE;
    if (n) memcpy(ship->hull + ship->data + off, src, n);
    size_t new_len = off + n;
    if (new_len > length) {
        ship->tail = ship->data + (uint32_t)new_len;
        if (ship->u.s.moored) ship->u.s.moored->bytes += new_len - length;
    }
    return PA_OK;
}

void *pa_put(pa_ship *ship, size_t n)
{
    if (!ship_writable(ship) || n > pa_stern_room(ship)) return NULL;
    void *result = pa_tailp(ship);
    ship->tail += (uint32_t)n;
    if (ship->u.s.moored) ship->u.s.moored->bytes += n;
    return result;
}

void *pa_push(pa_ship *ship, size_t n)
{
    if (!ship_writable(ship) || n > pa_bow_room(ship)) return NULL;
    ship->data -= (uint32_t)n;
    if (ship->cursor > ship->data) ship->cursor = ship->data;
    if (ship->u.s.moored) ship->u.s.moored->bytes += n;
    return pa_data(ship);
}

void *pa_push_aligned(pa_ship *ship, size_t n, size_t align)
{
    if (!ship_writable(ship) || !align || (align & (align - 1u)) || n > ship->data)
        return NULL;
    uintptr_t raw = (uintptr_t)(ship->hull + ship->data - n);
    uintptr_t aligned = raw & ~((uintptr_t)align - 1u);
    size_t used = (size_t)((uintptr_t)(ship->hull + ship->data) - aligned);
    return used <= ship->data ? pa_push(ship, used) : NULL;
}

int pa_pull(pa_ship *ship, size_t n)
{
    if (!ship_writable(ship)) return PA_E_STATE;
    if (n > pa_len(ship)) return PA_E_RANGE;
    ship->data += (uint32_t)n;
    if (ship->cursor < ship->data) ship->cursor = ship->data;
    if (ship->u.s.moored) {
        ship->u.s.moored->bytes -= n;
        if (ship->u.s.moored->rcursor > ship->u.s.moored->bytes)
            ship->u.s.moored->rcursor = ship->u.s.moored->bytes;
    }
    return PA_OK;
}

int pa_trim(pa_ship *ship, size_t new_len)
{
    if (!ship_writable(ship)) return PA_E_STATE;
    size_t length = pa_len(ship);
    if (new_len > length) return PA_E_RANGE;
    ship->tail = ship->data + (uint32_t)new_len;
    if (ship->cursor > ship->tail) ship->cursor = ship->tail;
    if (ship->u.s.moored) {
        ship->u.s.moored->bytes -= length - new_len;
        if (ship->u.s.moored->rcursor > ship->u.s.moored->bytes)
            ship->u.s.moored->rcursor = ship->u.s.moored->bytes;
    }
    return PA_OK;
}

int pa_reserve_bow(pa_ship *ship, size_t n)
{
    if (!ship_writable(ship)) return PA_E_STATE;
    if (pa_len(ship)) return PA_E_STATE;
    if (n > ship->cap || n > UINT32_MAX) return PA_E_NOSPACE;
    ship->data = (uint32_t)n;
    ship->tail = (uint32_t)n;
    ship->cursor = (uint32_t)n;
    store_reset_bow(ship, (uint32_t)n);
    return PA_OK;
}

size_t pa_read(pa_ship *ship, void *dst, size_t n)
{
    if (!ship || ship->state == PA_SHIP_FREE || (!dst && n)) return 0u;
    size_t available = ship->tail - ship->cursor;
    if (n > available) n = available;
    if (n) memcpy(dst, ship->hull + ship->cursor, n);
    ship->cursor += (uint32_t)n;
    return n;
}

size_t pa_read_at(const pa_ship *ship, size_t off, void *dst, size_t n)
{
    if (!ship || ship->state == PA_SHIP_FREE || (!dst && n)) return 0u;
    size_t length = pa_len(ship);
    if (off > length) return 0u;
    if (n > length - off) n = length - off;
    if (n) memcpy(dst, ship->hull + ship->data + off, n);
    return n;
}

const void *pa_peek(const pa_ship *ship, size_t off, size_t need)
{
    if (!ship || ship->state == PA_SHIP_FREE || off > pa_len(ship) ||
        need > pa_len(ship) - off) return NULL;
    return ship->hull + ship->data + off;
}

int pa_seek(pa_ship *ship, ptrdiff_t off, int whence)
{
    if (!ship || ship->state == PA_SHIP_FREE) return PA_E_STATE;
    intmax_t base;
    if (whence == PA_SEEK_SET) base = 0;
    else if (whence == PA_SEEK_CUR) base = (intmax_t)pa_tell(ship);
    else if (whence == PA_SEEK_END) base = (intmax_t)pa_len(ship);
    else return PA_E_INVAL;
    if ((off > 0 && base > INTMAX_MAX - (intmax_t)off) ||
        (off < 0 && base < INTMAX_MIN - (intmax_t)off)) return PA_E_RANGE;
    intmax_t target = base + (intmax_t)off;
    if (target < 0 || (uintmax_t)target > pa_len(ship)) return PA_E_RANGE;
    ship->cursor = ship->data + (uint32_t)target;
    return PA_OK;
}

void pa_rewind(pa_ship *ship)
{
    if (ship && ship->state != PA_SHIP_FREE) ship->cursor = ship->data;
}

void pa_bollard_init(pa_bollard *bollard, pa_harbor *harbor)
{
    if (!bollard) return;
    memset(bollard, 0, sizeof(*bollard));
    bollard->harbor = harbor;
}

static bool bollard_accepts(const pa_bollard *bollard, const pa_ship *ship)
{
    return !bollard->harbor || bollard->harbor == ship->berth->harbor;
}

int pa_moor(pa_bollard *bollard, pa_ship *ship)
{
    if (!bollard || !ship) return PA_E_INVAL;
    if (ship->state != PA_SHIP_SAILING || ship->u.s.moored ||
        !bollard_accepts(bollard, ship)) return PA_E_STATE;
    if (pa_len(ship) > SIZE_MAX - bollard->bytes) return PA_E_TOOBIG;
    ship->u.s.prev = bollard->last;
    ship->u.s.next = NULL;
    if (bollard->last) bollard->last->u.s.next = ship;
    else bollard->first = ship;
    bollard->last = ship;
    ship->u.s.moored = bollard;
    ship->state = PA_SHIP_MOORED;
    bollard->bytes += pa_len(ship);
    ++bollard->count;
    return PA_OK;
}

int pa_moor_front(pa_bollard *bollard, pa_ship *ship)
{
    if (!bollard || !ship) return PA_E_INVAL;
    if (ship->state != PA_SHIP_SAILING || ship->u.s.moored ||
        !bollard_accepts(bollard, ship)) return PA_E_STATE;
    if (pa_len(ship) > SIZE_MAX - bollard->bytes) return PA_E_TOOBIG;
    ship->u.s.prev = NULL;
    ship->u.s.next = bollard->first;
    if (bollard->first) bollard->first->u.s.prev = ship;
    else bollard->last = ship;
    bollard->first = ship;
    ship->u.s.moored = bollard;
    ship->state = PA_SHIP_MOORED;
    bollard->bytes += pa_len(ship);
    ++bollard->count;
    return PA_OK;
}

int pa_moor_after(pa_bollard *bollard, pa_ship *at, pa_ship *ship)
{
    if (!bollard || !at || !ship) return PA_E_INVAL;
    if (at->u.s.moored != bollard || ship->state != PA_SHIP_SAILING ||
        ship->u.s.moored || !bollard_accepts(bollard, ship)) return PA_E_STATE;
    if (pa_len(ship) > SIZE_MAX - bollard->bytes) return PA_E_TOOBIG;
    ship->u.s.prev = at;
    ship->u.s.next = at->u.s.next;
    if (at->u.s.next) at->u.s.next->u.s.prev = ship;
    else bollard->last = ship;
    at->u.s.next = ship;
    ship->u.s.moored = bollard;
    ship->state = PA_SHIP_MOORED;
    bollard->bytes += pa_len(ship);
    ++bollard->count;
    return PA_OK;
}

int pa_unmoor(pa_bollard *bollard, pa_ship *ship)
{
    if (!bollard || !ship) return PA_E_INVAL;
    if (ship->state != PA_SHIP_MOORED || ship->u.s.moored != bollard)
        return PA_E_STATE;
    if (ship->u.s.prev) ship->u.s.prev->u.s.next = ship->u.s.next;
    else bollard->first = ship->u.s.next;
    if (ship->u.s.next) ship->u.s.next->u.s.prev = ship->u.s.prev;
    else bollard->last = ship->u.s.prev;
    bollard->bytes -= pa_len(ship);
    --bollard->count;
    if (bollard->rcursor > bollard->bytes) bollard->rcursor = bollard->bytes;
    ship->u.s.next = NULL;
    ship->u.s.prev = NULL;
    ship->u.s.moored = NULL;
    ship->state = PA_SHIP_SAILING;
    return PA_OK;
}

pa_ship *pa_unmoor_first(pa_bollard *bollard)
{
    if (!bollard || !bollard->first) return NULL;
    pa_ship *ship = bollard->first;
    return pa_unmoor(bollard, ship) == PA_OK ? ship : NULL;
}

void pa_bollard_release_all(pa_bollard *bollard)
{
    if (!bollard) return;
    while (bollard->first) pa_release(bollard->first);
    bollard->rcursor = 0u;
}

ssize_t pa_bollard_append(pa_bollard *bollard, const void *src, size_t n)
{
    if (!bollard || (!src && n)) return PA_E_INVAL;
    if (n > (size_t)SSIZE_MAX || n > SIZE_MAX - bollard->bytes) return PA_E_TOOBIG;
    const uint8_t *cursor = src;
    size_t left = n;
    while (left) {
        pa_ship *ship = bollard->last;
        size_t room = ship && ship_writable(ship) ? pa_stern_room(ship) : 0u;
        if (!room) {
            if (!bollard->harbor) break;
            size_t chunk = left;
            if (chunk > bollard->harbor->max_payload) chunk = bollard->harbor->max_payload;
            ship = pa_charter(bollard->harbor, chunk, 0u, 0u);
            if (!ship || pa_moor(bollard, ship) != PA_OK) {
                if (ship) pa_release(ship);
                break;
            }
            room = pa_stern_room(ship);
        }
        size_t chunk = room < left ? room : left;
        if (chunk) memcpy(pa_tailp(ship), cursor, chunk);
        ship->tail += (uint32_t)chunk;
        bollard->bytes += chunk;
        cursor += chunk;
        left -= chunk;
    }
    return (ssize_t)(n - left);
}

static void bollard_rollback(pa_bollard *bollard, pa_ship *last, uint32_t tail,
                             size_t bytes, size_t rcursor)
{
    pa_ship *ship = last ? last->u.s.next : bollard->first;
    while (ship) {
        pa_ship *next = ship->u.s.next;
        pa_release(ship);
        ship = next;
    }
    if (last) last->tail = tail;
    bollard->bytes = bytes;
    bollard->rcursor = rcursor;
}

int pa_bollard_append_all(pa_bollard *bollard, const void *src, size_t n)
{
    if (!bollard || (!src && n)) return PA_E_INVAL;
    pa_ship *last = bollard->last;
    uint32_t tail = last ? last->tail : 0u;
    size_t bytes = bollard->bytes;
    size_t rcursor = bollard->rcursor;
    ssize_t written = pa_bollard_append(bollard, src, n);
    if (written < 0) return (int)written;
    if ((size_t)written == n) return PA_OK;
    bollard_rollback(bollard, last, tail, bytes, rcursor);
    return PA_E_NOMEM;
}

void *pa_bollard_put(pa_bollard *bollard, size_t n)
{
    if (!bollard) return NULL;
    pa_ship *ship = bollard->last;
    if (!n) return ship ? pa_tailp(ship) : NULL;
    if (!ship || !ship_writable(ship) || pa_stern_room(ship) < n) {
        if (!bollard->harbor || n > bollard->harbor->max_payload) return NULL;
        ship = pa_charter(bollard->harbor, n, 0u, 0u);
        if (!ship || pa_moor(bollard, ship) != PA_OK) {
            if (ship) pa_release(ship);
            return NULL;
        }
    }
    return pa_put(ship, n);
}

size_t pa_bollard_read_at(const pa_bollard *bollard, size_t off, void *dst, size_t n)
{
    if (!bollard || (!dst && n) || off > bollard->bytes) return 0u;
    size_t left = n < bollard->bytes - off ? n : bollard->bytes - off;
    size_t done = 0u;
    uint8_t *cursor = dst;
    for (pa_ship *ship = bollard->first; ship && left; ship = ship->u.s.next) {
        size_t length = pa_len(ship);
        if (off >= length) {
            off -= length;
            continue;
        }
        size_t chunk = length - off;
        if (chunk > left) chunk = left;
        if (chunk) memcpy(cursor + done, ship->hull + ship->data + off, chunk);
        done += chunk;
        left -= chunk;
        off = 0u;
    }
    return done;
}

size_t pa_bollard_read(pa_bollard *bollard, void *dst, size_t n)
{
    if (!bollard) return 0u;
    size_t read = pa_bollard_read_at(bollard, bollard->rcursor, dst, n);
    bollard->rcursor += read;
    return read;
}

int pa_bollard_linearize(pa_bollard *bollard)
{
    if (!bollard) return PA_E_INVAL;
    if (bollard->count <= 1u) return PA_OK;
    if (!bollard->harbor || bollard->bytes > bollard->harbor->max_payload)
        return PA_E_TOOBIG;
    pa_ship *ship = pa_charter(bollard->harbor, bollard->bytes, 0u, 0u);
    if (!ship) return PA_E_NOMEM;
    if (pa_bollard_read_at(bollard, 0u, pa_tailp(ship), bollard->bytes) != bollard->bytes) {
        pa_release(ship);
        return PA_E_STATE;
    }
    ship->tail += (uint32_t)bollard->bytes;
    size_t rcursor = bollard->rcursor;
    pa_bollard_release_all(bollard);
    int result = pa_moor(bollard, ship);
    if (result != PA_OK) {
        pa_release(ship);
        return result;
    }
    bollard->rcursor = rcursor;
    return PA_OK;
}

int pa_bollard_split(pa_bollard *bollard, size_t off, pa_bollard *tail)
{
    if (!bollard || !tail || bollard == tail) return PA_E_INVAL;
    if (off > bollard->bytes) return PA_E_RANGE;
    if (tail->first || tail->count || tail->bytes) return PA_E_STATE;
    if (tail->harbor && bollard->harbor && tail->harbor != bollard->harbor)
        return PA_E_STATE;
    if (!tail->harbor) tail->harbor = bollard->harbor;

    size_t old_cursor = bollard->rcursor;
    size_t position = 0u;
    pa_ship *move = bollard->first;
    while (move && position + pa_len(move) <= off) {
        position += pa_len(move);
        move = move->u.s.next;
    }

    if (move && position < off) {
        size_t prefix = off - position;
        size_t suffix_len = pa_len(move) - prefix;
        pa_ship *suffix = pa_charter(move->berth->harbor, suffix_len, 0u, 0u);
        if (!suffix) return PA_E_NOMEM;
        if (pa_write(suffix, move->hull + move->data + prefix, suffix_len) < 0) {
            pa_release(suffix);
            return PA_E_NOMEM;
        }
        pa_ship *next = move->u.s.next;
        int rc = pa_trim(move, prefix);
        if (rc != PA_OK || pa_moor(tail, suffix) != PA_OK) {
            pa_release(suffix);
            return rc != PA_OK ? rc : PA_E_STATE;
        }
        move = next;
    }

    while (move) {
        pa_ship *next = move->u.s.next;
        int rc = pa_unmoor(bollard, move);
        if (rc != PA_OK || pa_moor(tail, move) != PA_OK) return PA_E_STATE;
        move = next;
    }
    bollard->rcursor = old_cursor < off ? old_cursor : off;
    tail->rcursor = old_cursor > off ? old_cursor - off : 0u;
    return PA_OK;
}

int pa_bollard_concat(pa_bollard *dst, pa_bollard *src)
{
    if (!dst || !src || dst == src) return PA_E_INVAL;
    if (dst->harbor && src->harbor && dst->harbor != src->harbor) return PA_E_STATE;
    if (!src->first) return PA_OK;
    if (src->bytes > SIZE_MAX - dst->bytes || src->count > UINT32_MAX - dst->count)
        return PA_E_TOOBIG;
    if (!dst->harbor) dst->harbor = src->harbor;

    if (dst->last) {
        dst->last->u.s.next = src->first;
        src->first->u.s.prev = dst->last;
    } else {
        dst->first = src->first;
    }
    dst->last = src->last;
    dst->bytes += src->bytes;
    dst->count += src->count;
    for (pa_ship *ship = src->first; ship; ship = ship->u.s.next)
        ship->u.s.moored = dst;

    src->first = NULL;
    src->last = NULL;
    src->bytes = 0u;
    src->rcursor = 0u;
    src->count = 0u;
    return PA_OK;
}

size_t pa_bollard_consume(pa_bollard *bollard, size_t n)
{
    if (!bollard) return 0u;
    size_t old_cursor = bollard->rcursor;
    size_t done = 0u;
    while (n && bollard->first) {
        pa_ship *ship = bollard->first;
        size_t length = pa_len(ship);
        if (n < length) {
            ship->data += (uint32_t)n;
            if (ship->cursor < ship->data) ship->cursor = ship->data;
            bollard->bytes -= n;
            done += n;
            break;
        }
        n -= length;
        done += length;
        (void)pa_unmoor(bollard, ship);
        pa_release(ship);
    }
    bollard->rcursor = old_cursor > done ? old_cursor - done : 0u;
    return done;
}

#if PA_HAVE_IOVEC
static int bollard_iovec_prefix(const pa_bollard *bollard, struct iovec *iov, int max,
                                size_t *represented)
{
    if (!bollard || !iov || max <= 0) return PA_E_INVAL;
    int count = 0;
    *represented = 0u;
    for (pa_ship *ship = bollard->first; ship && count < max; ship = ship->u.s.next) {
        if (!pa_len(ship)) continue;
        iov[count].iov_base = pa_data(ship);
        iov[count].iov_len = pa_len(ship);
        *represented += pa_len(ship);
        ++count;
    }
    return count;
}

int pa_bollard_iovec(const pa_bollard *bollard, struct iovec *iov, int max)
{
    size_t represented;
    int count = bollard_iovec_prefix(bollard, iov, max, &represented);
    if (count < 0) return count;
    return represented == bollard->bytes ? count : PA_E_TOOBIG;
}

int pa_bollard_send(pa_bollard *bollard, int fd, int flags)
{
    if (!bollard || fd < 0) return PA_E_INVAL;
    struct iovec iov[PA_IOV_MAX];
    while (bollard->bytes) {
        size_t represented;
        int count = bollard_iovec_prefix(bollard, iov, PA_IOV_MAX, &represented);
        if (count <= 0) return PA_E_STATE;
        struct msghdr message;
        memset(&message, 0, sizeof(message));
        message.msg_iov = iov;
        message.msg_iovlen = (size_t)count;
        ssize_t sent = sendmsg(fd, &message, flags);
        if (sent < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return -EAGAIN;
            return PA_E_IO;
        }
        if (!sent) return PA_E_IO;
        pa_bollard_consume(bollard, (size_t)sent);
    }
    return PA_OK;
}

int pa_bollard_sendto(pa_bollard *bollard, int fd, int flags,
                      const struct sockaddr *to, socklen_t tolen)
{
    if (!bollard || fd < 0 || (!to && tolen)) return PA_E_INVAL;
    struct iovec iov[PA_IOV_MAX];
    size_t represented;
    int count = bollard_iovec_prefix(bollard, iov, PA_IOV_MAX, &represented);
    if (count < 0) return count;
    if (represented != bollard->bytes) return PA_E_TOOBIG;

    struct msghdr message;
    memset(&message, 0, sizeof(message));
    message.msg_name = (void *)to;
    message.msg_namelen = tolen;
    message.msg_iov = iov;
    message.msg_iovlen = (size_t)count;
    ssize_t sent;
    do {
        sent = sendmsg(fd, &message, flags);
    } while (sent < 0 && errno == EINTR);
    if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -EAGAIN;
        return PA_E_IO;
    }
    if ((size_t)sent != bollard->bytes) return PA_E_IO;
    pa_bollard_consume(bollard, (size_t)sent);
    return PA_OK;
}

ssize_t pa_bollard_recv(pa_bollard *bollard, int fd, size_t want, int flags)
{
    if (!bollard || fd < 0) return PA_E_INVAL;
    if (!want) return 0;
    if (!bollard->harbor) return PA_E_STATE;

    pa_ship *old_last = bollard->last;
    uint32_t old_tail = old_last ? old_last->tail : 0u;
    size_t old_bytes = bollard->bytes;
    size_t old_cursor = bollard->rcursor;
    struct iovec iov[PA_IOV_MAX];
    pa_ship *ships[PA_IOV_MAX];
    uint32_t tails[PA_IOV_MAX];
    int count = 0;
    size_t capacity = 0u;

    pa_ship *ship = old_last;
    if (ship && ship_writable(ship) && pa_stern_room(ship)) {
        size_t room = pa_stern_room(ship);
        if (room > want) room = want;
        iov[count].iov_base = pa_tailp(ship);
        iov[count].iov_len = room;
        ships[count] = ship;
        tails[count] = ship->tail;
        capacity += room;
        ++count;
    }
    while (capacity < want && count < PA_IOV_MAX) {
        size_t chunk = want - capacity;
        if (chunk > bollard->harbor->max_payload) chunk = bollard->harbor->max_payload;
        ship = pa_charter(bollard->harbor, chunk, 0u, 0u);
        if (!ship) break;
        if (pa_moor(bollard, ship) != PA_OK) {
            pa_release(ship);
            break;
        }
        size_t room = pa_stern_room(ship);
        if (room > want - capacity) room = want - capacity;
        iov[count].iov_base = pa_tailp(ship);
        iov[count].iov_len = room;
        ships[count] = ship;
        tails[count] = ship->tail;
        capacity += room;
        ++count;
    }
    if (!count) return PA_E_NOMEM;

    struct msghdr message;
    memset(&message, 0, sizeof(message));
    message.msg_iov = iov;
    message.msg_iovlen = (size_t)count;
    ssize_t received;
    do {
        received = recvmsg(fd, &message, flags);
    } while (received < 0 && errno == EINTR);
    if (received < 0) {
        bollard_rollback(bollard, old_last, old_tail, old_bytes, old_cursor);
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -EAGAIN;
        return PA_E_IO;
    }

    size_t left = (size_t)received;
    for (int i = 0; i < count; ++i) {
        size_t used = left < iov[i].iov_len ? left : iov[i].iov_len;
        ships[i]->tail = tails[i] + (uint32_t)used;
        bollard->bytes += used;
        left -= used;
    }
    ship = old_last ? old_last->u.s.next : bollard->first;
    while (ship) {
        pa_ship *next = ship->u.s.next;
        if (!pa_len(ship)) pa_release(ship);
        ship = next;
    }
    return received;
}
#endif

void pa_ship_dump(const pa_ship *ship, FILE *out)
{
    if (!out) out = stderr;
    if (!ship) {
        fputs("pa_ship: (null)\n", out);
        return;
    }
    fprintf(out, "pa_ship %p state=%u flags=0x%04x cap=%" PRIu32
                 " data=%" PRIu32 " cursor=%" PRIu32 " tail=%" PRIu32 "\n",
            (const void *)ship, ship->state, ship->flags, ship->cap,
            ship->data, ship->cursor, ship->tail);
    if (ship->state == PA_SHIP_FREE) return;
    const uint8_t *data = pa_data(ship);
    for (size_t i = 0; i < pa_len(ship); i += 16u) {
        fprintf(out, "%04zx:", i);
        size_t end = pa_len(ship) - i < 16u ? pa_len(ship) : i + 16u;
        for (size_t j = i; j < end; ++j) fprintf(out, " %02x", data[j]);
        fputc('\n', out);
    }
}
