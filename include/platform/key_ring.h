#ifndef BONGOCAT_KEY_RING_H
#define BONGOCAT_KEY_RING_H

// Single-producer / multi-consumer broadcast ring for keycodes.
// Producer: the input child process. Consumers: each monitor's animation
// thread, each with its own process-local `tail`. Codes are never removed;
// a consumer lapped by more than KEY_RING_SIZE drops the oldest unread codes.
// Lives in MAP_SHARED memory so all processes see the same buffer.

#include <stdatomic.h>

#define KEY_RING_SIZE 64u  // must be a power of two

typedef struct {
  atomic_uint head;                 // monotonic publish counter (producer)
  atomic_int codes[KEY_RING_SIZE];  // codes[seq & (KEY_RING_SIZE-1)]
} key_ring_t;

static inline void key_ring_init(key_ring_t *r) {
  atomic_store_explicit(&r->head, 0u, memory_order_relaxed);
  for (unsigned i = 0; i < KEY_RING_SIZE; i++) {
    atomic_store_explicit(&r->codes[i], 0, memory_order_relaxed);
  }
}

static inline void key_ring_push(key_ring_t *r, int code) {
  unsigned head = atomic_load_explicit(&r->head, memory_order_relaxed);
  atomic_store_explicit(&r->codes[head & (KEY_RING_SIZE - 1)], code,
                        memory_order_relaxed);
  atomic_store_explicit(&r->head, head + 1u, memory_order_release);
}

// Copies up to out_cap codes (oldest first) into out, advances *tail, and
// returns the count written. If the consumer has been lapped (more than
// KEY_RING_SIZE codes pending), the oldest are dropped first.
static inline unsigned key_ring_drain(const key_ring_t *r, unsigned *tail,
                                      int *out, unsigned out_cap) {
  unsigned head = atomic_load_explicit(&r->head, memory_order_acquire);
  unsigned avail = head - *tail;  // unsigned wrap-safe
  if (avail > KEY_RING_SIZE) {
    *tail = head - KEY_RING_SIZE;  // drop oldest
    avail = KEY_RING_SIZE;
  }
  unsigned n = (avail < out_cap) ? avail : out_cap;
  for (unsigned i = 0; i < n; i++) {
    out[i] = atomic_load_explicit(&r->codes[(*tail + i) & (KEY_RING_SIZE - 1)],
                                  memory_order_relaxed);
  }
  *tail += n;
  return n;
}

#endif  // BONGOCAT_KEY_RING_H
