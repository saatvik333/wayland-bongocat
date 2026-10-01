#ifndef BONGOCAT_CONTROL_H
#define BONGOCAT_CONTROL_H
#include <stddef.h>
int instance_lock(void);
void instance_unlock(void);
int control_start(void);
void control_cleanup(void);
int control_request(const char *request);
void control_process(int (*handler)(const char *, char *, size_t));
int control_timeout(void);
int control_fds(int *fds, size_t capacity);
#endif
