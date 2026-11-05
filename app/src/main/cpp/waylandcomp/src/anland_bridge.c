#define _GNU_SOURCE
#include "anland_bridge.h"
#include "anland_server.h"
#include "sc_layer.h"
#include "vk_present.h"
#include "droiddeck_ext.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/sync_file.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define TARGET_COUNT 3
#define INPUT_W 1920
#define INPUT_H 1080

struct target_pool;
struct target_slot {
    struct target_pool *pool;
    AHardwareBuffer *ahb;
    int fd, busy, release_fd;
    uint32_t stride;
    uint64_t modifier;
    struct wl_event_source *watch;
};

struct target_pool {
    atomic_uint refs;
    pthread_mutex_t lock;
    int width, height, wake_fd;
    struct target_slot slots[TARGET_COUNT];
};

static struct {
    struct wl_event_loop *loop;
    struct anland_server *server;
    struct target_pool *pool;
    struct wl_event_source *wake;
    int wake_fd, width, height, fps_limit;
    uint32_t refresh_mhz;
    int64_t next_frame_ns;
    int64_t report_ns;
    unsigned submitted;
    double x, y;
    int pointer_known;
    uint32_t touches;
} bridge = {.wake_fd = -1};

static atomic_int active, query_x, query_y, query_known;

extern void droiddeck_on_pointer_lock(int locked, int x, int y);
extern void droiddeck_on_game_program(int pid, const char *program);
extern void droiddeck_anland_frame(void);
extern void droiddeck_anland_session(int enabled);

static int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void pool_unref(struct target_pool *pool) {
    if (atomic_fetch_sub(&pool->refs, 1) != 1) return;
    for (int i = 0; i < TARGET_COUNT; i++) {
        struct target_slot *s = &pool->slots[i];
        if (s->release_fd >= 0) close(s->release_fd);
        if (s->fd >= 0) close(s->fd);
        if (s->ahb) AHardwareBuffer_release(s->ahb);
    }
    if (pool->wake_fd >= 0) close(pool->wake_fd);
    pthread_mutex_destroy(&pool->lock);
    free(pool);
}

static void released(void *token, int fence_fd) {
    struct target_slot *s = token;
    struct target_pool *pool = s->pool;
    pthread_mutex_lock(&pool->lock);
    s->release_fd = fence_fd;
    s->busy = fence_fd == -2;
    pthread_mutex_unlock(&pool->lock);
    eventfd_write(pool->wake_fd, 1);
    pool_unref(pool);
}

static int fence_state(int fd) {
    struct pollfd p = {.fd = fd, .events = POLLIN};
    int r;
    do { r = poll(&p, 1, 0); } while (r < 0 && errno == EINTR);
    if (!r) return 0;
    if (r < 0 || (p.revents & (POLLERR | POLLNVAL))) return -1;
    if (!(p.revents & POLLIN)) return -1;
    struct sync_file_info info = {0};
    if (ioctl(fd, SYNC_IOC_FILE_INFO, &info) != 0 || info.status < 0) return -1;
    return info.status > 0;
}

static int on_release_fence(int fd, uint32_t mask, void *data) {
    (void)mask;
    struct target_slot *s = data;
    int ready = fence_state(fd);
    if (!ready) return 0;
    wl_event_source_remove(s->watch); s->watch = NULL;
    pthread_mutex_lock(&s->pool->lock);
    close(s->release_fd); s->release_fd = ready < 0 ? -2 : -1;
    s->busy = ready < 0;
    pthread_mutex_unlock(&s->pool->lock);
    if (ready < 0) droiddeck_log("anland", "release fence failed; target stays quarantined");
    anland_server_wake(bridge.server);
    return 0;
}

static void harvest_releases(void) {
    if (!bridge.pool) return;
    for (int i = 0; i < TARGET_COUNT; i++) {
        struct target_slot *s = &bridge.pool->slots[i];
        pthread_mutex_lock(&bridge.pool->lock);
        int fd = s->release_fd;
        int needs_watch = !s->busy && fd >= 0 && !s->watch;
        pthread_mutex_unlock(&bridge.pool->lock);
        if (needs_watch) {
            s->watch = wl_event_loop_add_fd(bridge.loop, fd, WL_EVENT_READABLE, on_release_fence, s);
            if (!s->watch) {
                pthread_mutex_lock(&bridge.pool->lock);
                s->busy = 1;
                pthread_mutex_unlock(&bridge.pool->lock);
                droiddeck_log("anland", "cannot watch release fence; target stays quarantined");
            }
        }
    }
}

static int on_wake(int fd, uint32_t mask, void *data) {
    (void)mask; (void)data;
    eventfd_t count;
    while (eventfd_read(fd, &count) == 0) {}
    harvest_releases();
    anland_server_wake(bridge.server);
    return 0;
}

static int prepare(void *data, struct anland_pool_desc *desc) {
    (void)data;
    if (!vkp_window() || bridge.width <= 0 || bridge.height <= 0 || !bridge.refresh_mhz) return 0;
    if (!sc_layer_available()) return -1;
    if (!bridge.pool) {
        struct target_pool *pool = calloc(1, sizeof(*pool));
        if (!pool) return -1;
        atomic_init(&pool->refs, 1);
        pthread_mutex_init(&pool->lock, NULL);
        pool->width = bridge.width; pool->height = bridge.height;
        pool->wake_fd = fcntl(bridge.wake_fd, F_DUPFD_CLOEXEC, 0);
        for (int i = 0; i < TARGET_COUNT; i++) {
            pool->slots[i].pool = pool;
            pool->slots[i].fd = pool->slots[i].release_fd = -1;
        }
        if (pool->wake_fd < 0) { pool_unref(pool); return -1; }
        for (int i = 0; i < TARGET_COUNT; i++) {
            struct target_slot *s = &pool->slots[i];
            if (sc_layer_allocate_target(pool->width, pool->height, &s->ahb, &s->fd, &s->stride, &s->modifier) != 0) {
                pool_unref(pool); return -1;
            }
        }
        bridge.pool = pool;
        droiddeck_log("anland", "host pool: %dx%d RGBA8888, %d linear targets; no host image blit",
                       pool->width, pool->height, TARGET_COUNT);
    }
    desc->screen = (struct screen_info){bridge.width, bridge.height, 1, bridge.refresh_mhz};
    desc->count = TARGET_COUNT;
    for (int i = 0; i < TARGET_COUNT; i++) {
        struct target_slot *s = &bridge.pool->slots[i];
        desc->fds[i] = s->fd;
        desc->buffers[i] = (struct buf_info){.stride = s->stride, .width = bridge.width,
            .height = bridge.height, .format = 1, .modifier = s->modifier, .offset = 0};
    }
    return 1;
}

static int acquire(void *data) {
    (void)data;
    struct target_pool *pool = bridge.pool;
    if (!pool || !vkp_window() || (bridge.next_frame_ns && now_ns() < bridge.next_frame_ns)) return -1;
    pthread_mutex_lock(&pool->lock);
    int found = -1;
    for (int i = 0; i < TARGET_COUNT; i++) {
        struct target_slot *s = &pool->slots[i];
        if (!s->busy && s->release_fd == -1) { s->busy = 1; found = i; break; }
    }
    pthread_mutex_unlock(&pool->lock);
    return found;
}

static int present(void *data, int index, int fence_fd) {
    (void)data;
    struct target_pool *pool = bridge.pool;
    if (!pool || index < 0 || index >= TARGET_COUNT || !vkp_window()) {
        if (fence_fd >= 0) close(fence_fd);
        return -1;
    }
    struct target_slot *s = &pool->slots[index];
    atomic_fetch_add(&pool->refs, 1);
    sc_layer_hide_overlay();
    int r = sc_layer_present_external(s->ahb, pool->width, pool->height, fence_fd, s, released);
    if (r != 0) { pool_unref(pool); return -1; }
    vkp_base_black(pool->width, pool->height);
    bridge.next_frame_ns = bridge.fps_limit > 0 ? now_ns() + 1000000000LL / bridge.fps_limit : 0;
    bridge.submitted++;
    droiddeck_anland_frame();
    return 0;
}

static void session(void *data, int enabled) {
    (void)data;
    atomic_store(&active, enabled);
    droiddeck_on_pointer_lock(0, -1, -1);
    bridge.pointer_known = 0;
    bridge.touches = 0;
    atomic_store(&query_known, 0);
    bridge.next_frame_ns = 0;
    bridge.report_ns = now_ns(); bridge.submitted = 0;
    if (!enabled && bridge.pool) {
        struct target_pool *pool = bridge.pool;
        bridge.pool = NULL;
        sc_layer_hide();
        for (int i = 0; i < TARGET_COUNT; i++) {
            if (pool->slots[i].watch) wl_event_source_remove(pool->slots[i].watch);
            pool->slots[i].watch = NULL;
        }
        pool_unref(pool);
    }
    droiddeck_anland_session(enabled);
}

static void output(void *data, const struct OutputEvent *event, const void *payload) {
    (void)data;
    switch (event->type) {
    case OUTPUT_TYPE_CLIPBOARD:
        droiddeck_on_clipboard_text(payload ? payload : "", (int)event->clipboard.size);
        break;
    case OUTPUT_TYPE_SET_CONSUMER_VAR:
        if (event->set_consumer_var.var == CONSUMER_VAR_CAPTURE_MOUSE)
            droiddeck_on_pointer_lock(event->set_consumer_var.value != 0,
                atomic_load(&query_known) ? atomic_load(&query_x) : -1,
                atomic_load(&query_known) ? atomic_load(&query_y) : -1);
        break;
    case OUTPUT_TYPE_RESOURCES_REQUEST: {
        struct InputEvent reply = {.type = INPUT_TYPE_RESOURCE_INVALID};
        reply.resource.type = event->resources_request.type;
        anland_server_input(bridge.server, &reply, NULL, 0);
        break;
    }
    case OUTPUT_TYPE_SCHEDULING:
        if ((event->scheduling.flags & SCHEDULING_FLAG_ON) && event->scheduling.pid > 0)
            droiddeck_on_game_program(event->scheduling.pid, "Anland");
        break;
    }
}

static void log_message(void *data, const char *message) {
    (void)data;
    droiddeck_log("anland", "%s", message);
}

int anland_bridge_start(struct wl_event_loop *loop, const char *runtime_dir,
                         int width, int height, uint32_t refresh_mhz) {
    if (!runtime_dir || !*runtime_dir) return -1;
    char path[512];
    if (snprintf(path, sizeof(path), "%s/anland.sock", runtime_dir) >= (int)sizeof(path)) return -1;
    bridge.loop = loop; bridge.width = width; bridge.height = height; bridge.refresh_mhz = refresh_mhz;
    bridge.wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (bridge.wake_fd < 0) return -1;
    bridge.wake = wl_event_loop_add_fd(loop, bridge.wake_fd, WL_EVENT_READABLE, on_wake, NULL);
    const struct anland_server_ops ops = {.prepare = prepare, .acquire = acquire, .present = present,
        .session = session, .output = output, .log = log_message};
    if (bridge.wake) bridge.server = anland_server_create(loop, path, &ops, NULL);
    if (!bridge.server) { anland_bridge_stop(); return -1; }
    droiddeck_log("anland", "listening on %s (explicit WM opt-in)", path);
    return 0;
}

void anland_bridge_stop(void) {
    anland_server_destroy(bridge.server); bridge.server = NULL;
    if (bridge.wake) wl_event_source_remove(bridge.wake);
    bridge.wake = NULL;
    if (bridge.wake_fd >= 0) close(bridge.wake_fd);
    bridge.wake_fd = -1;
}

void anland_bridge_tick(int width, int height, uint32_t refresh_mhz, int fps_limit) {
    if (!bridge.server) return;
    if (bridge.width != width || bridge.height != height) {
        anland_server_reset(bridge.server);
        bridge.width = width; bridge.height = height;
    }
    bridge.refresh_mhz = refresh_mhz;
    if (bridge.fps_limit != fps_limit) bridge.next_frame_ns = 0;
    bridge.fps_limit = fps_limit;
    int64_t now = now_ns();
    if (anland_bridge_active() && now - bridge.report_ns >= 10000000000LL) {
        droiddeck_log("anland", "submitted=%u submit_fps=%.1f host_blits=0 targets=%d (submission, not scanout)",
                       bridge.submitted, bridge.submitted * 1e9 / (now - bridge.report_ns), TARGET_COUNT);
        bridge.submitted = 0; bridge.report_ns = now;
    }
    anland_server_refresh(bridge.server, refresh_mhz);
    harvest_releases();
    anland_server_wake(bridge.server);
}

int anland_bridge_active(void) { return atomic_load(&active); }

static int send_event(const struct InputEvent *event) {
    return anland_server_input(bridge.server, event, NULL, 0);
}

static void pointer_motion(double x, double y, int relative, int normalized) {
    double dx, dy;
    if (relative) {
        dx = x; dy = y; x += bridge.x; y += bridge.y;
    } else {
        if (normalized) {
            int ow, oh;
            double sx, sy;
            vkp_output_size(&ow, &oh);
            if (ow > 0 && oh > 0 && vkp_output_to_scene(x * ow / INPUT_W, y * oh / INPUT_H, &sx, &sy)) {
                x = sx; y = sy;
            } else { x *= (double)bridge.width / INPUT_W; y *= (double)bridge.height / INPUT_H; }
        }
        dx = bridge.pointer_known ? x - bridge.x : 0;
        dy = bridge.pointer_known ? y - bridge.y : 0;
    }
    x = fmax(0, fmin(x, bridge.width - 1));
    y = fmax(0, fmin(y, bridge.height - 1));
    bridge.x = x; bridge.y = y; bridge.pointer_known = 1;
    int ow, oh;
    double ox, oy;
    vkp_output_size(&ow, &oh);
    if (ow > 0 && oh > 0 && vkp_scene_to_output(x, y, &ox, &oy)) {
        atomic_store(&query_x, (int)(ox * INPUT_W / ow));
        atomic_store(&query_y, (int)(oy * INPUT_H / oh));
    } else {
        atomic_store(&query_x, (int)(x * INPUT_W / bridge.width));
        atomic_store(&query_y, (int)(y * INPUT_H / bridge.height));
    }
    atomic_store(&query_known, 1);
    struct InputEvent event = {.type = INPUT_TYPE_POINTER_MOTION};
    event.pointer_motion.x = (float)x; event.pointer_motion.y = (float)y;
    event.pointer_motion.dx = (float)dx; event.pointer_motion.dy = (float)dy;
    send_event(&event);
}

int anland_bridge_input(int type, int a, int b, int c, int d) {
    if (!anland_bridge_active()) return 0;
    struct InputEvent event = {0};
    switch (type) {
    case 0:
        pointer_motion(b, c, 0, 1);
        if (a == 1) return 1;
        event.type = INPUT_TYPE_POINTER_BUTTON;
        event.pointer_button.button = 0x110;
        event.pointer_button.pressed = a == 0;
        break;
    case 1:
        event.type = INPUT_TYPE_KEY;
        event.key.keycode = a; event.key.action = b ? INPUT_ACTION_DOWN : INPUT_ACTION_UP;
        break;
    case 2: pointer_motion(a, b, 0, 0); return 1;
    case 3:
        event.type = INPUT_TYPE_POINTER_BUTTON;
        event.pointer_button.button = (uint32_t)a; event.pointer_button.pressed = b != 0;
        break;
    case 6: pointer_motion(a / 256.0, b / 256.0, 1, 0); return 1;
    case 4: case 11: case 12:
        for (int axis = 0; axis < 2; axis++) {
            int value = axis ? b : type == 4 ? a * 120 : a;
            if (!value) continue;
            event.type = INPUT_TYPE_POINTER_AXIS;
            event.pointer_axis.axis = (uint32_t)axis;
            event.pointer_axis.value = value / 12.0f;
            event.pointer_axis.discrete = type == 12 ? 0 : value / 120;
            send_event(&event);
        }
        return 1;
    case 7: {
        if (a == 3) {
            for (int id = 0; id < 32; id++) {
                if (!(bridge.touches & (1u << id))) continue;
                struct InputEvent up = {.type = INPUT_TYPE_TOUCH};
                up.touch.action = INPUT_ACTION_UP; up.touch.pointer_id = id;
                send_event(&up);
            }
            bridge.touches = 0;
            event.type = INPUT_TYPE_TOUCH_FRAME;
            break;
        }
        if (b < 0 || b >= 32 || a < 0 || a > 2) return 1;
        if (a != 0 && !(bridge.touches & (1u << b))) return 1;
        if (a == 0) bridge.touches |= 1u << b;
        if (a == 2) bridge.touches &= ~(1u << b);
        double x = (double)c * bridge.width / INPUT_W, y = (double)d * bridge.height / INPUT_H;
        int ow, oh;
        vkp_output_size(&ow, &oh);
        if (ow > 0 && oh > 0) vkp_output_to_scene((double)c * ow / INPUT_W, (double)d * oh / INPUT_H, &x, &y);
        event.type = INPUT_TYPE_TOUCH;
        event.touch.action = a == 0 ? INPUT_ACTION_DOWN : a == 1 ? INPUT_ACTION_MOVE : INPUT_ACTION_UP;
        event.touch.pointer_id = b;
        event.touch.x = (float)fmax(0, fmin(x, bridge.width - 1));
        event.touch.y = (float)fmax(0, fmin(y, bridge.height - 1));
        send_event(&event);
        event = (struct InputEvent){.type = INPUT_TYPE_TOUCH_FRAME};
        break;
    }
    default: return 0;
    }
    send_event(&event);
    return 1;
}

int anland_bridge_clipboard(const char *text, size_t size) {
    if (!anland_bridge_active()) return 0;
    struct InputEvent event = {.type = INPUT_TYPE_CLIPBOARD};
    event.clipboard.size = (uint32_t)size;
    anland_server_input(bridge.server, &event, text, size);
    return 1;
}

int anland_bridge_text(const char *text, size_t size) {
    if (!anland_bridge_active()) return 0;
    struct InputEvent event = {.type = INPUT_TYPE_TEXT_INPUT};
    event.text_input.size = (uint32_t)size;
    anland_server_input(bridge.server, &event, text, size);
    return 1;
}

int anland_bridge_cursor_pos(int *x, int *y) {
    if (!atomic_load(&query_known)) return 0;
    *x = atomic_load(&query_x); *y = atomic_load(&query_y);
    return 1;
}
