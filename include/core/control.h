#ifndef BONGOCAT_CONTROL_H
#define BONGOCAT_CONTROL_H

#include <stddef.h>
#include <stdint.h>

#define CONTROL_MAX_CLIENTS 16
#define INSTANCE_LOCKED     (-2)

// Single-instance lock in $XDG_RUNTIME_DIR. Returns 0, INSTANCE_LOCKED while
// another instance holds it, or -1 with errno set.
int instance_lock(void);
void instance_unlock(void);

// Same-user control socket; requires the instance lock.
int control_start(void);
void control_cleanup(void);
// Fills fds with the listening socket and connected clients; returns count.
size_t control_fds(int *fds, size_t capacity);
// Accepts clients and answers complete requests. handler writes a reply and
// returns 0 on success.
void control_process(int (*handler)(const char *request, char *reply,
                                    size_t size));
// Monotonic microsecond time a waiting client times out, or 0.
int64_t control_deadline(void);

// Client side: sends request to the running instance and prints the reply.
// Returns 0 on success, 1 on failure, 2 when no instance is running.
int control_request(const char *request);

#endif  // BONGOCAT_CONTROL_H
