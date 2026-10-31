#ifndef PULL_H
#define PULL_H

/* Copy a file from the PC (plain http) into /data/homebrew on a detached
 * worker; progress is visible via the accessors below (GET /api/status).
 *
 * pull_start returns 0 = started, 1 = busy (one copy at a time),
 * -1 = out of memory, -2 = worker failed to spawn. */
int pull_start(const char *url, const char *local, int resume);

int pull_active(void);
int pull_paused(void);
void pull_set_paused(int paused);
void pull_cancel(void);
long long pull_got(void);
long long pull_want(void);
const char *pull_name(void);

#endif /* PULL_H */
