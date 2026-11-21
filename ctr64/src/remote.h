#ifndef CTR64_REMOTE_H
#define CTR64_REMOTE_H
/* remote test mode (development, remote.c): <data dir>/remote.txt turns it on. Only in builds
 * configured with -DCTR64_REMOTE=ON; release builds get the stubs below (no network server). */
#include <stdbool.h>
#include <stddef.h>

#define REMOTE_PORT 5050

typedef enum { REMOTE_QUIT, REMOTE_RUN, REMOTE_RELAUNCH } RemoteAction;

#ifdef CTR64_REMOTE

bool remote_wanted(const char *data_dir);
/* serves the PC until it asks for a run (rom filled in), a restart or the game list */
RemoteAction remote_serve(const char *data_dir, char *rom, size_t cap);
/* this process is running a remote test: ctr64 restarts into remote mode when it ends */
bool remote_active(void);

/* In-game listener (remote runs only, while the pause menu is open): the PC can fetch files, save
 * a state and take a screenshot of both screens at the moment the player paused. */
void remote_pause_begin(void);
void remote_pause_poll(void); /* once per menu frame; serves a connection until it closes */
void remote_pause_end(void);
#else
static inline bool remote_wanted(const char *data_dir) { (void)data_dir; return false; }
static inline RemoteAction remote_serve(const char *data_dir, char *rom, size_t cap)
{
    (void)data_dir; (void)rom; (void)cap;
    return REMOTE_QUIT;
}
static inline bool remote_active(void) { return false; }
static inline void remote_pause_begin(void) {}
static inline void remote_pause_poll(void) {}
static inline void remote_pause_end(void) {}
#endif
#endif
