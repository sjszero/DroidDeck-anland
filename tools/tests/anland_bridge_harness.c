#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <dirent.h>
#include <linux/sync_file.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include "anland_bridge.h"
#include "display_producer.h"
#include "sc_layer.c"

struct AHardwareBuffer {
    AHardwareBuffer_Desc desc;
    struct { int version, numFds, numInts, data[3]; } handle;
    void *pixels;
    int refs;
};
struct ASurfaceControl { int unused; };
struct ASurfaceTransaction { sc_complete_fn fn; void *context; int acquire_fd; };
struct ASurfaceTransactionStats { int fence; };
struct completion { sc_complete_fn fn; void *context; };
static struct completion completions[64];
static int completed, queued, live_buffers, frame_count, wayland_releases, external_releases, captures;
static int layout_mode, fail_fence_info;
static int output_w = 640, output_h = 360;
static ANativeWindow *window = (ANativeWindow *)(uintptr_t)1;

int AHardwareBuffer_allocate(const AHardwareBuffer_Desc *desc, AHardwareBuffer **out) {
    AHardwareBuffer *b = calloc(1, sizeof(*b));
    assert(b);
    b->desc = *desc; b->desc.stride = (desc->width + 63) & ~63u; b->refs = 1;
    b->handle.numFds = 1; b->handle.numInts = 2;
    b->handle.data[0] = memfd_create("mock-gralloc", MFD_CLOEXEC);
    b->handle.data[1] = ('g' << 24) | ('m' << 16) | ('s' << 8) | 'm';
    if (layout_mode == 1) b->handle.data[1] = 0;
    if (layout_mode == 2) b->handle.data[2] = 0x08000000;
    assert(b->handle.data[0] >= 0);
    assert(ftruncate(b->handle.data[0], (off_t)b->desc.stride * 4 * desc->height) == 0);
    if (layout_mode == 3) assert(ftruncate(b->handle.data[0], 16) == 0);
    live_buffers++; *out = b; return 0;
}
void AHardwareBuffer_describe(const AHardwareBuffer *b, AHardwareBuffer_Desc *desc) { *desc = b->desc; }
void AHardwareBuffer_acquire(AHardwareBuffer *b) { b->refs++; }
void AHardwareBuffer_release(AHardwareBuffer *b) {
    if (--b->refs) return;
    close(b->handle.data[0]); free(b->pixels); free(b); live_buffers--;
}
int AHardwareBuffer_lock(AHardwareBuffer *b, uint64_t usage, int fd, const ARect *rect, void **out) {
    (void)usage; (void)fd; (void)rect;
    if (!b->pixels) b->pixels = calloc(b->desc.stride * b->desc.height, 4);
    *out = b->pixels; return 0;
}
int AHardwareBuffer_unlock(AHardwareBuffer *b, int *fd) { (void)b; if (fd) *fd = -1; return 0; }
static const void *native_handle(const AHardwareBuffer *b) { return &b->handle; }
static ASurfaceControl *create_sc(ANativeWindow *w, const char *name) { (void)w; (void)name; return calloc(1, sizeof(ASurfaceControl)); }
static void release_sc(ASurfaceControl *sc) { free(sc); }
static ASurfaceTransaction *create_tx(void) {
    ASurfaceTransaction *tx = calloc(1, sizeof(*tx)); assert(tx); tx->acquire_fd = -1; return tx;
}
static void delete_tx(ASurfaceTransaction *tx) { free(tx); }
static void set_buffer(ASurfaceTransaction *tx, ASurfaceControl *sc, AHardwareBuffer *b, int fd) {
    (void)sc; assert(b && b->refs > 0); assert(tx->acquire_fd < 0); tx->acquire_fd = fd;
}
static void apply_tx(ASurfaceTransaction *tx) {
    if (tx->fn) { assert(queued < 64); completions[queued++] = (struct completion){tx->fn, tx->context}; }
    if (tx->acquire_fd >= 0) close(tx->acquire_fd);
    tx->acquire_fd = -1;
}
static void set_complete(ASurfaceTransaction *tx, void *ctx, sc_complete_fn fn) { tx->context = ctx; tx->fn = fn; }
static int release_fence(ASurfaceTransactionStats *stats, ASurfaceControl *sc) {
    (void)sc; return stats->fence >= 0 ? dup(stats->fence) : -1;
}
static void set_order(ASurfaceTransaction *tx, ASurfaceControl *sc, int32_t z) { (void)tx; (void)sc; (void)z; }
static void set_flag(ASurfaceTransaction *tx, ASurfaceControl *sc, int8_t flag) { (void)tx; (void)sc; (void)flag; }
static void set_geometry(ASurfaceTransaction *tx, ASurfaceControl *sc, const ARect *a, const ARect *b, int32_t t) {
    (void)tx; (void)sc; (void)a; (void)b; (void)t;
}
static void reparent(ASurfaceTransaction *tx, ASurfaceControl *a, ASurfaceControl *b) { (void)tx; (void)a; (void)b; }

static void complete_one(int fd) {
    assert(completed < queued);
    struct completion c = completions[completed++];
    ASurfaceTransactionStats stats = {.fence = fd};
    c.fn(c.context, fd == -2 ? NULL : &stats);
}
static void complete_all(void) { while (completed < queued) complete_one(-1); }

ANativeWindow *vkp_window(void) { return window; }
int vkp_update_map(int w, int h) { return w > 0 && h > 0 ? 0 : -1; }
int vkp_map_rect(int w, int h, int sw, int sh, int r[8]) {
    int rect[8] = {0, 0, w, h, 0, 0, sw, sh}; memcpy(r, rect, sizeof(rect)); return 1;
}
void vkp_output_size(int *w, int *h) { *w = output_w; *h = output_h; }
int vkp_output_to_scene(double x, double y, double *sx, double *sy) { *sx = x; *sy = y; return 1; }
int vkp_scene_to_output(double x, double y, double *ox, double *oy) { *ox = x; *oy = y; return 1; }
void vkp_signal_first_frame(void) {}
int vkp_base_black(int w, int h) { (void)w; (void)h; return 0; }
void vkp_image_destroy(struct vkp_image *img) { assert(!img); }
void vkp_wait_idle(void) {}
const char *vkp_modifier_name(uint64_t mod) { (void)mod; return "linear"; }
float droiddeck_color_desired_headroom(const struct droiddeck_color *c, char *why, size_t n) { (void)c; (void)why; (void)n; return 1; }
void droiddeck_color_note_headroom_request(float ratio) { (void)ratio; }
void droiddeck_color_frame_shown(const struct droiddeck_color *c, int path, uint32_t fmt) { (void)c; (void)path; (void)fmt; }
void droiddeck_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
void droiddeck_on_pointer_lock(int locked, int x, int y) { (void)x; (void)y; captures = locked; }
void droiddeck_on_game_program(int pid, const char *program) { (void)pid; (void)program; }
void droiddeck_on_clipboard_text(const char *text, int size) { (void)text; (void)size; }
void droiddeck_anland_session(int enabled) { (void)enabled; }
void droiddeck_anland_frame(void) { frame_count++; }
void ahb_swapchain_layer_released(void *token, int fd) { assert(token == (void *)(uintptr_t)37); wayland_releases++; if (fd >= 0) close(fd); }
static void external_released(void *token, int fd) { assert(token == (void *)(uintptr_t)38); external_releases++; if (fd >= 0) close(fd); }

int ioctl(int fd, unsigned long request, ...) {
    if (request != SYNC_IOC_FILE_INFO) { errno = ENOTTY; return -1; }
    if (fail_fence_info) { errno = EIO; return -1; }
    va_list ap; va_start(ap, request); struct sync_file_info *info = va_arg(ap, struct sync_file_info *); va_end(ap);
    struct pollfd p = {.fd = fd, .events = POLLIN};
    info->status = poll(&p, 1, 0) > 0 ? 1 : 0;
    return 0;
}

static int fd_count(void) {
    DIR *dir = opendir("/proc/self/fd"); assert(dir);
    int n = 0; struct dirent *entry;
    while ((entry = readdir(dir))) if (entry->d_name[0] != '.') n++;
    closedir(dir); return n;
}
static void pump(struct wl_event_loop *loop) { for (int i = 0; i < 8; i++) assert(wl_event_loop_dispatch(loop, 0) == 0); }
static display_ctx *producer(struct wl_event_loop *loop, const char *path) {
    display_ctx *ctx = NULL; assert(connect_to_deamon_deferred(&ctx, path) == 0);
    int ready = -1;
    for (int i = 0; i < 50 && ready != 0; i++) { pump(loop); ready = try_exit_fallback(ctx); }
    assert(ready == 0 && anland_bridge_active()); return ctx;
}
static int target(display_ctx *ctx) {
    eventfd_t value; assert(poll_buffer_ready(ctx, 0) == 1);
    assert(eventfd_read(get_buffer_ready_fd(ctx), &value) == 0 && value == 1);
    return get_selected_idx_raw(ctx);
}

static void test_frames(struct wl_event_loop *loop, const char *path) {
    display_ctx *ctx = producer(loop, path);
    assert(live_buffers == 3);
    int fences[2] = {eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK), eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)};
    for (int i = 0; i < 3; i++) {
        assert(target(ctx) == i);
        if (i == 0) set_render_fence(ctx, eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
        assert(trigger_refresh(ctx) == 0); pump(loop);
        assert(frame_count == i + 1);
        complete_one(i ? fences[i - 1] : -1);
        pump(loop);
    }
    assert(poll_buffer_ready(ctx, 0) == 0);
    assert(eventfd_write(fences[0], 1) == 0); pump(loop);
    assert(target(ctx) == 0);
    assert(trigger_refresh(ctx) == 0); pump(loop);
    assert(frame_count == 4);
    output_w = 800;
    anland_bridge_tick(800, 360, 90000, 0);
    assert(poll_buffer_ready(ctx, 0) == -1);
    display_ctx *fresh = producer(loop, path);
    assert(live_buffers >= 7);
    complete_all(); pump(loop);
    assert(live_buffers == 4 && target(fresh) == 0);
    disconnect(ctx); disconnect(fresh); pump(loop);
    close(fences[0]); close(fences[1]);
}

static void test_input(struct wl_event_loop *loop, const char *path) {
    display_ctx *ctx = producer(loop, path);
    struct InputEvent got;
    assert(anland_bridge_input(7, 0, 2, 960, 540));
    assert(poll_input_event(ctx, &got, 0) == 1 && got.type == INPUT_TYPE_TOUCH && got.touch.action == INPUT_ACTION_DOWN);
    assert(got.touch.x == 320 && got.touch.y == 180 && got.touch.pointer_id == 2);
    assert(poll_input_event(ctx, &got, 0) == 1 && got.type == INPUT_TYPE_TOUCH_FRAME);
    assert(anland_bridge_input(7, 1, 2, 1000, 600));
    assert(poll_input_event(ctx, &got, 0) == 1 && got.touch.action == INPUT_ACTION_MOVE);
    assert(poll_input_event(ctx, &got, 0) == 1 && got.type == INPUT_TYPE_TOUCH_FRAME);
    assert(anland_bridge_input(7, 3, 0, 0, 0));
    assert(poll_input_event(ctx, &got, 0) == 1 && got.touch.action == INPUT_ACTION_UP && got.touch.pointer_id == 2);
    assert(poll_input_event(ctx, &got, 0) == 1 && got.type == INPUT_TYPE_TOUCH_FRAME);
    assert(anland_bridge_input(0, 1, 960, 540, 0));
    assert(poll_input_event(ctx, &got, 0) == 1 && got.type == INPUT_TYPE_POINTER_MOTION && got.pointer_motion.x == 320);
    struct OutputEvent lock = {.type = OUTPUT_TYPE_SET_CONSUMER_VAR};
    lock.set_consumer_var.var = CONSUMER_VAR_CAPTURE_MOUSE; lock.set_consumer_var.value = 1;
    assert(push_output_event(ctx, &lock) == 0); pump(loop); assert(captures == 1);
    assert(anland_bridge_text("text", 4));
    assert(poll_input_event(ctx, &got, 0) == 1 && got.type == INPUT_TYPE_TEXT_INPUT && got.text_input.size == 4);
    char text[5] = {0}; assert(poll_input_event_extend_data(ctx, text, 4, 0) == 1 && !strcmp(text, "text"));
    disconnect(ctx); pump(loop); assert(captures == 0);
}

static void test_failed_release(struct wl_event_loop *loop, const char *path) {
    display_ctx *ctx = producer(loop, path);
    assert(target(ctx) == 0);
    assert(trigger_refresh(ctx) == 0); pump(loop); complete_one(-1); pump(loop);
    assert(target(ctx) == 1);
    assert(trigger_refresh(ctx) == 0); pump(loop);
    int fence = eventfd(1, EFD_CLOEXEC | EFD_NONBLOCK); assert(fence >= 0);
    fail_fence_info = 1;
    complete_one(fence); pump(loop);
    fail_fence_info = 0; close(fence);
    assert(target(ctx) == 2);
    assert(trigger_refresh(ctx) == 0); pump(loop); complete_one(-2); pump(loop);
    assert(poll_buffer_ready(ctx, 0) == 0);
    anland_bridge_tick(640, 360, 90000, 0); pump(loop);
    assert(poll_buffer_ready(ctx, 0) == 0 && frame_count == 3);
    disconnect(ctx); pump(loop);
}

static void test_layouts(void) {
    for (layout_mode = 1; layout_mode <= 3; layout_mode++) {
        AHardwareBuffer *b = NULL; int fd = -1;
        uint32_t stride; uint64_t modifier;
        assert(sc_layer_allocate_target(640, 360, &b, &fd, &stride, &modifier) < 0);
        assert(!b && fd == -1 && live_buffers == 0);
    }
    layout_mode = 0;
    AHardwareBuffer *b = NULL; int fd = -1;
    uint32_t stride; uint64_t modifier;
    assert(sc_layer_allocate_target(640, 360, &b, &fd, &stride, &modifier) == 0);
    assert(b && fd >= 0 && stride >= 640 * 4 && modifier == 0);
    close(fd); AHardwareBuffer_release(b);
}

static void test_wayland_routing(void) {
    AHardwareBuffer *b;
    AHardwareBuffer_Desc desc = {.width = 640, .height = 360, .layers = 1, .format = 1};
    assert(AHardwareBuffer_allocate(&desc, &b) == 0);
    assert(sc_layer_present_ahb(b, 640, 360, -1, (void *)(uintptr_t)37, 640, 360, NULL, 1) == 0);
    complete_all();
    assert(sc_layer_present_external(b, 640, 360, -1, (void *)(uintptr_t)38, external_released) == 0);
    complete_all();
    assert(wayland_releases == 1 && external_releases == 0);
    assert(sc_layer_present_ahb(b, 640, 360, -1, (void *)(uintptr_t)37, 640, 360, NULL, 1) == 0);
    complete_all();
    assert(wayland_releases == 1 && external_releases == 1);
    sc_layer_hide(); complete_all();
    assert(wayland_releases == 2 && external_releases == 1);
    AHardwareBuffer_release(b);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    int before = fd_count();
    api.state = 1; api.getNativeHandle = native_handle;
    api.createFromWindow = create_sc; api.release = release_sc;
    api.txCreate = create_tx; api.txDelete = delete_tx; api.txApply = apply_tx;
    api.setBuffer = set_buffer; api.setZOrder = set_order; api.setVisibility = set_flag;
    api.setBufferTransparency = set_flag; api.setGeometry = set_geometry; api.reparent = reparent;
    api.setOnComplete = set_complete; api.prevReleaseFence = release_fence;
    char dir[] = "/tmp/droiddeck-anland-bridge-XXXXXX"; assert(mkdtemp(dir));
    char path[160]; snprintf(path, sizeof(path), "%s/anland.sock", dir);
    struct wl_event_loop *loop = wl_event_loop_create(); assert(loop);
    assert(anland_bridge_start(loop, dir, 640, 360, 90000) == 0);
    if (!strcmp(argv[1], "frames")) test_frames(loop, path);
    else if (!strcmp(argv[1], "input")) test_input(loop, path);
    else if (!strcmp(argv[1], "routing")) test_wayland_routing();
    else if (!strcmp(argv[1], "failed-release")) test_failed_release(loop, path);
    else if (!strcmp(argv[1], "layouts")) test_layouts();
    else assert(0);
    anland_bridge_stop(); complete_all();
    for (int i = 0; i < SC_LAYER_COUNT; i++) retire_sc(&g_layers[i]);
    complete_all();
    if (g_blank) { AHardwareBuffer_release(g_blank); g_blank = NULL; }
    assert(live_buffers == 0 && g_pending_cb == 0);
    wl_event_loop_destroy(loop); assert(rmdir(dir) == 0);
    assert(fd_count() == before); puts("PASS");
}
