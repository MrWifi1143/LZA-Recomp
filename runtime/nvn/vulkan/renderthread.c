// The render thread (LARECOMP_RENDER_THREAD): the game's calls into the backend (command lists, presents, texture
// destruction) are queued in the order the game made them and run on one thread of ours, so the game thread goes on with
// the next frame while the Vulkan commands of this one are recorded (letsgo-recomp's docs/PESQUISA-GPU.md: without the backend's work the
// game's chain runs at ~160 fps, with it at ~90).
//
// What the console's contract gives us: after nvnQueueSubmitCommands the game may not touch what the lists use until a
// fence says the GPU is done, so running the lists later changes nothing it can see. The rest is kept in order here:
//  - fences are tickets (the jobs queued so far): nvnSyncWait waits for the render thread to pass its ticket and then for
//    the GPU submission it ended with, not for everything queued since (that would put the game thread right back in
//    step with the render thread);
//  - what the game writes to memory the GPU reads (backend_pool_written) is stamped on each list, so a list sees the writes
//    made before it was submitted and no later ones (textures.c dirty_check);
//  - destroying a texture runs on this thread, after the lists queued before it (rt_call);
//  - the counters the game and the scripted tests see (renderer_game_frame) count the presents it has made; the backend's
//    own (renderer_present_count) count the ones it has shown.
#include <SDL3/SDL.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "../../rt.h"
#include "../../settings.h"
#include "../nvn.h"

void vk_wait_seq(uint64_t seq);                    // exec.c
uint64_t renderer_fence_seq(void);                 // exec.c: GPU submissions so far (the one being filled included)
void renderer_wait_seq(uint64_t seq);              // exec.c
uint64_t backend_dirty_seq(void);                  // textures.c
void backend_set_dirty_limit(uint64_t seq);        // textures.c
uint64_t renderer_present_count(void);             // device.c: presents shown
void input_frame(uint64_t frame);                  // hle/input.c
void fnprof_frame(uint64_t frame);                 // fnprof.c
void reboot_test_frame(uint64_t frame);            // main.c
void renderer_submit_if_needed(bool idle);         // exec.c

enum { RJ_EXEC, RJ_PRESENT, RJ_CALL };
typedef struct {
    int kind;
    uint64_t handle, dirty;
    NvnTexture *tex;
    void (*fn)(void *);
    void *arg;
    volatile int *done;
} Job;

#define QN 256       // jobs queued at most (the game thread waits when it is full)
#define MAX_PRESENTS 2  // presents queued at most: the game runs up to two frames ahead of what the window shows
static Job queue[QN];
static unsigned qhead, qtail;
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t c_work = PTHREAD_COND_INITIALIZER, c_done = PTHREAD_COND_INITIALIZER;
static uint64_t enq, fin;              // tickets: jobs queued / finished
static uint64_t seq_after[1024];       // the GPU submission count after each finished job (by ticket)
static uint64_t pres_enq, pres_fin;
static uint64_t pres_seq[1024];        // the GPU submission count after each finished present (by present number)
static uint64_t game_frames;
static bool started, busy, dropping;
static __thread bool on_render_thread;
static pthread_t thread;

bool rt_active(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("LARECOMP_RENDER_THREAD");
        on = e ? atoi(e) != 0 : g_settings.render_thread;  // (the environment wins over larecomp.ini)
        if (getenv("LARECOMP_INTERP")) on = 0;  // (research: the interpolation replays lists on the game thread)
        if (on) LOG("[rt] render thread on\n");
    }
    return on;
}
bool rt_on_render_thread(void) { return on_render_thread; }

uint64_t renderer_game_frame(void) {
    return rt_active() ? __atomic_load_n(&game_frames, __ATOMIC_RELAXED) : renderer_present_count();
}

static void run(Job *j) {
    switch (j->kind) {
    case RJ_EXEC:
        backend_set_dirty_limit(j->dirty);
        backend_execute(j->handle);
        backend_set_dirty_limit(0);
        break;
    case RJ_PRESENT: backend_present(j->tex); break;
    case RJ_CALL: j->fn(j->arg); break;
    }
}

// More jobs queued after the one running (backend_execute: batch the next list into the same submission)
bool rt_more_queued(void) { return __atomic_load_n(&qtail, __ATOMIC_RELAXED) - __atomic_load_n(&qhead, __ATOMIC_RELAXED) > 1; }

// A thread waits for the pending submission (exec.c renderer_wait_seq): wake the render thread if it sleeps
void rt_wake(void) {
    pthread_mutex_lock(&lk);
    pthread_cond_signal(&c_work);
    pthread_mutex_unlock(&lk);
}

static void *render_main(void *arg) {
    (void)arg;
    uint64_t idle_seq = 0;  // renderer_fence_seq when the queue last ran empty with everything submitted
    on_render_thread = true;
    crash_handler_install();
    pthread_mutex_lock(&lk);
    for (;;) {
        while (qhead == qtail) {
            if (renderer_fence_seq() > idle_seq) {  // (lists batched into the pending submission: backend_execute)
                pthread_mutex_unlock(&lk);
                renderer_submit_if_needed(true);
                pthread_mutex_lock(&lk);
                idle_seq = renderer_fence_seq();
                continue;
            }
            pthread_cond_wait(&c_work, &lk);
        }
        Job j = queue[qhead % QN];
        busy = true;
        pthread_mutex_unlock(&lk);
        if (!dropping || j.kind == RJ_CALL) run(&j);  // (a reboot drops queued lists and presents, never a caller's call)
        renderer_submit_if_needed(false);
        uint64_t seq = renderer_fence_seq();
        pthread_mutex_lock(&lk);
        qhead++;
        fin++;
        seq_after[fin & 1023] = seq;
        if (j.kind == RJ_PRESENT) pres_fin++, pres_seq[pres_fin & 1023] = seq;
        if (j.done) *j.done = 1;
        busy = false;
        pthread_cond_broadcast(&c_done);
    }
    return NULL;
}

// A game thread waiting for the render thread gives its emulated core up (runtime/kernel.c "cores") and takes it
// again once it lets go of lk (lk_unlock; nothing on the render thread itself, which is no guest thread).
void kernel_block_begin(void);
void kernel_block_end(void);
static void lk_unlock(void) {
    pthread_mutex_unlock(&lk);
    kernel_block_end();
}

// Waits on c_done for up to 5 ms (the caller holds lk); false once a reboot is stopping the guest threads.
static bool wait_done(void) {
    kernel_block_begin();
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 5000000;
    if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
    pthread_cond_timedwait(&c_done, &lk, &ts);
    return !g_rt_unwind;
}

// Queues a job; returns its ticket (0: not queued, a reboot is stopping the guest threads).
static uint64_t push(Job j) {
    pthread_mutex_lock(&lk);
    if (!started) {
        started = true;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 64ull << 20);
        pthread_create(&thread, &attr, render_main, NULL);
        pthread_attr_destroy(&attr);
    }
    while (qtail - qhead >= QN)
        if (!wait_done()) { lk_unlock(); return 0; }
    queue[qtail % QN] = j;
    qtail++;
    uint64_t t = ++enq;
    pthread_cond_signal(&c_work);
    lk_unlock();
    return t;
}

void rt_submit(uint64_t handle) {
    if (!rt_active()) { backend_execute(handle); return; }
    Job j = {.kind = RJ_EXEC, .handle = handle, .dirty = backend_dirty_seq()};
    push(j);
}

// Game thread: the frame's present. The window shows it when the render thread gets there; the game's own bookkeeping that a
// present used to run at its end (scripted input, function profile, reboot tests) happens here, in the game's order.
void rt_present(NvnTexture *t) {
    if (!t) return;
    if (!rt_active()) { backend_present(t); return; }
    pthread_mutex_lock(&lk);
    while (pres_enq - pres_fin >= MAX_PRESENTS)
        if (!wait_done()) { lk_unlock(); return; }
    pres_enq++;
    lk_unlock();
    Job j = {.kind = RJ_PRESENT, .tex = t};
    push(j);
    uint64_t n = __atomic_add_fetch(&game_frames, 1, __ATOMIC_RELAXED);
    input_frame(n);
    fnprof_frame(n);
    reboot_test_frame(n);
}

// Runs fn(arg) on the render thread, after everything queued before it, and returns when it is done.
void rt_call(void (*fn)(void *), void *arg) {
    if (!rt_active() || on_render_thread) { fn(arg); return; }
    volatile int done = 0;
    Job j = {.kind = RJ_CALL, .fn = fn, .arg = arg, .done = &done};
    if (!push(j)) { return; }  // (a reboot: the call is dropped with the guest thread's work)
    pthread_mutex_lock(&lk);
    kernel_block_begin();
    while (!done) pthread_cond_wait(&c_done, &lk);  // (the render thread never waits for the game: this ends)
    lk_unlock();
}

// Everything queued so far has run.
void rt_drain(void) {
    if (!rt_active() || on_render_thread) return;
    pthread_mutex_lock(&lk);
    uint64_t target = enq;
    while (fin < target)
        if (!wait_done()) break;
    lk_unlock();
}

uint64_t rt_fence_ticket(void) {
    pthread_mutex_lock(&lk);
    uint64_t t = enq;
    lk_unlock();
    return t;
}

// The fence of a ticket: the render thread has passed it, and the GPU has run what it submitted up to there.
void rt_wait_ticket(uint64_t ticket) {
    if (!ticket) return;
    pthread_mutex_lock(&lk);
    while (fin < ticket)
        if (!wait_done()) { lk_unlock(); return; }
    uint64_t seq = seq_after[ticket & 1023];
    lk_unlock();
    renderer_wait_seq(seq);
}

// Stereo (plugin.h set_stereo): a frame is drawn again at its present (exec.c vk_stereo_render) from the same game
// memory - its command lists, constant buffers, vertices, textures read in place. So a fence the game sets stands for
// the next present too: the game reuses that memory only once it's been drawn both times. rt_next_present: that
// present's number (game thread: the next one queued; render thread: the next one it runs); rt_wait_present: waits for
// it and its GPU work - when it isn't queued yet (the game waits for this frame's own work before presenting it), for
// the jobs up to `ticket` as before (0: everything queued), the frame's memory not being reused then.
uint64_t rt_next_present(void) {
    pthread_mutex_lock(&lk);
    uint64_t p = on_render_thread ? pres_fin + 1 : pres_enq + 1;
    pthread_mutex_unlock(&lk);
    return p;
}
volatile uint64_t g_st_released_early;  // (a present whose memory a fence gave back before it was queued: exec.c)
uint64_t rt_running_present(void) { return __atomic_load_n(&pres_fin, __ATOMIC_RELAXED) + 1; }  // (render thread: the present job running)
void rt_wait_present(uint64_t p, uint64_t ticket, uint64_t seq_of_list) {
    pthread_mutex_lock(&lk);
    if (pres_enq < p) {
        lk_unlock();
        // (the game waits before its present is queued: the work up to the fence, the GPU's too - and that frame isn't
        // drawn again, its memory may be rewritten before its present: exec.c vk_stereo_render skips it)
        __atomic_store_n(&g_st_released_early, p, __ATOMIC_RELEASE);
        if (ticket) rt_wait_ticket(ticket);
        else { rt_drain(); renderer_wait_seq(seq_of_list); }
        return;
    }
    while (pres_fin < p)
        if (!wait_done()) { lk_unlock(); return; }
    uint64_t seq = pres_seq[p & 1023];
    lk_unlock();
    renderer_wait_seq(seq);
}

// Reboot (backend_reset, the guest threads stopped): the queued work belongs to the game that is going away.
void rt_reset(void) {
    if (!started) return;
    pthread_mutex_lock(&lk);
    dropping = true;
    while (qhead != qtail || busy) pthread_cond_wait(&c_done, &lk);
    dropping = false;
    pres_enq = pres_fin = 0;
    lk_unlock();
    __atomic_store_n(&game_frames, renderer_present_count(), __ATOMIC_RELAXED);
}
