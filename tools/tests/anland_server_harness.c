#define _GNU_SOURCE
#include "anland_server.h"
#include "display_producer.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

_Static_assert(sizeof(struct InputEvent) == 20, "input wire ABI");
_Static_assert(sizeof(struct OutputEvent) == 20, "output wire ABI");
_Static_assert(sizeof(struct buf_info) == 28, "buffer wire ABI");

struct fixture {
    struct wl_event_loop *loop;
    struct anland_server *server;
    char dir[128], path[160];
    struct anland_pool_desc pool;
    int ready, free_mask, frames, last_index, last_fence, output_count, active;
    char clipboard[128];
};

static int fd_count(void) {
    DIR *dir = opendir("/proc/self/fd");
    assert(dir);
    int n = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) if (entry->d_name[0] != '.') n++;
    closedir(dir);
    return n;
}

static void pump(struct fixture *f) {
    for (int i = 0; i < 8; i++) assert(wl_event_loop_dispatch(f->loop, 0) == 0);
}

static int prepare(void *data, struct anland_pool_desc *desc) {
    struct fixture *f = data;
    if (!f->ready) return 0;
    if (f->pool.fds[0] < 0) {
        for (int i = 0; i < f->pool.count; i++) {
            f->pool.fds[i] = memfd_create("anland-test-target", MFD_CLOEXEC);
            assert(f->pool.fds[i] >= 0);
            struct buf_info *b = &f->pool.buffers[i];
            *b = (struct buf_info){.stride = f->pool.screen.width * 4, .width = f->pool.screen.width,
                .height = f->pool.screen.height, .format = 1};
            assert(ftruncate(f->pool.fds[i], (off_t)b->stride * b->height) == 0);
        }
    }
    *desc = f->pool;
    return 1;
}

static int acquire(void *data) {
    struct fixture *f = data;
    for (int i = 0; i < f->pool.count; i++) {
        if (f->free_mask & (1 << i)) { f->free_mask &= ~(1 << i); return i; }
    }
    return -1;
}

static int present(void *data, int index, int fence) {
    struct fixture *f = data;
    assert(index >= 0 && index < f->pool.count && !(f->free_mask & (1 << index)));
    assert(f->last_fence < 0);
    f->last_fence = fence;
    f->last_index = index;
    f->frames++;
    return 0;
}

static void session(void *data, int active) {
    struct fixture *f = data;
    f->active = active;
    if (active) return;
    for (int i = 0; i < f->pool.count; i++) {
        if (f->pool.fds[i] >= 0) close(f->pool.fds[i]);
        f->pool.fds[i] = -1;
    }
    if (f->last_fence >= 0) close(f->last_fence);
    f->last_fence = -1;
    f->free_mask = (1 << f->pool.count) - 1;
}

static void output(void *data, const struct OutputEvent *event, const void *payload) {
    struct fixture *f = data;
    f->output_count++;
    if (event->type == OUTPUT_TYPE_CLIPBOARD) {
        assert(event->clipboard.size < sizeof(f->clipboard));
        if (event->clipboard.size) memcpy(f->clipboard, payload, event->clipboard.size);
        f->clipboard[event->clipboard.size] = 0;
    }
}

static void setup(struct fixture *f) {
    *f = (struct fixture){.ready = 1, .free_mask = 7, .last_fence = -1};
    strcpy(f->dir, "/tmp/droiddeck-anland-XXXXXX");
    assert(mkdtemp(f->dir));
    snprintf(f->path, sizeof(f->path), "%s/anland.sock", f->dir);
    f->pool.screen = (struct screen_info){640, 360, 1, 90000};
    f->pool.count = 3;
    for (int i = 0; i < MAX_BUFS; i++) f->pool.fds[i] = -1;
    f->loop = wl_event_loop_create();
    assert(f->loop);
    struct anland_server_ops ops = {.prepare = prepare, .acquire = acquire, .present = present,
        .session = session, .output = output};
    f->server = anland_server_create(f->loop, f->path, &ops, f);
    assert(f->server);
}

static void teardown(struct fixture *f) {
    anland_server_destroy(f->server);
    wl_event_loop_destroy(f->loop);
    assert(access(f->path, F_OK) != 0);
    assert(rmdir(f->dir) == 0);
}

static display_ctx *producer(struct fixture *f) {
    display_ctx *ctx = NULL;
    assert(connect_to_deamon_deferred(&ctx, f->path) == 0);
    int ready = -1;
    for (int i = 0; i < 50 && ready != 0; i++) {
        pump(f);
        ready = try_exit_fallback(ctx);
        assert(ready == 0 || errno == EAGAIN);
    }
    assert(ready == 0 && f->active && get_buf_count(ctx) == 3);
    return ctx;
}

static int target(display_ctx *ctx) {
    eventfd_t value;
    assert(poll_buffer_ready(ctx, 0) == 1);
    assert(eventfd_read(get_buffer_ready_fd(ctx), &value) == 0 && value == 1);
    return get_selected_idx_raw(ctx);
}

static void test_public(void) {
    struct fixture f;
    setup(&f);
    display_ctx *ctx = producer(&f);
    uint32_t w, h, fmt, hz;
    assert(get_screen_info(ctx, &w, &h, &fmt, &hz) == 0);
    assert(w == 640 && h == 360 && fmt == 1 && hz == 90000);
    for (int i = 0; i < 3; i++) {
        struct buf_info info;
        assert(get_dmabuf_info_at(ctx, i, &info) == 0);
        assert(info.stride == 2560 && info.width == w && info.height == h && info.modifier == 0);
        assert(target(ctx) == i);
        int sync = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        int observer = dup(sync);
        assert(sync >= 0 && observer >= 0);
        set_render_fence(ctx, sync);
        assert(trigger_refresh(ctx) == 0);
        pump(&f);
        assert(f.frames == i + 1 && f.last_index == i && f.last_fence >= 0);
        eventfd_t value;
        assert(eventfd_read(f.last_fence, &value) < 0 && errno == EAGAIN);
        assert(eventfd_write(observer, 7) == 0);
        assert(eventfd_read(f.last_fence, &value) == 0 && value == 7);
        close(observer); close(f.last_fence); f.last_fence = -1;
    }
    assert(poll_buffer_ready(ctx, 0) == 0);
    pump(&f);
    assert(f.frames == 3 && poll_buffer_ready(ctx, 0) == 0);
    f.free_mask |= 1 << 1;
    anland_server_wake(f.server);
    assert(target(ctx) == 1);
    disconnect(ctx);
    pump(&f);
    assert(!anland_server_active(f.server));
    teardown(&f);
}

static int raw_connect(struct fixture *f) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    assert(strlen(f->path) < sizeof(addr.sun_path));
    strcpy(addr.sun_path, f->path);
    assert(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    pump(f);
    return fd;
}

static void fragmented_send(struct fixture *f, int fd, const void *bytes, size_t n) {
    for (size_t i = 0; i < n; i++) {
        assert(send(fd, (const char *)bytes + i, 1, MSG_NOSIGNAL) == 1);
        pump(f);
    }
}

static int receive_fds(int fd, void *bytes, size_t size, int *fds, int expected) {
    struct iovec iov = {.iov_base = bytes, .iov_len = size};
    union { struct cmsghdr align; char bytes[CMSG_SPACE(MAX_BUFS * sizeof(int))]; } ancillary;
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = ancillary.bytes, .msg_controllen = sizeof(ancillary.bytes)};
    ssize_t n = recvmsg(fd, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    assert(n == (ssize_t)size && !(msg.msg_flags & MSG_CTRUNC));
    struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
    assert(cm && cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS);
    int count = (int)((cm->cmsg_len - CMSG_LEN(0)) / sizeof(int));
    assert(count == expected);
    memcpy(fds, CMSG_DATA(cm), count * sizeof(int));
    return count;
}

static void test_fragmented(void) {
    struct fixture f;
    setup(&f);
    f.ready = 0;
    int ctrl = raw_connect(&f);
    struct ctrl_msg hello = {.type = CTRL_MSG_PRODUCER_HELLO};
    fragmented_send(&f, ctrl, &hello, sizeof(hello));
    char info[sizeof(struct ctrl_msg) + sizeof(struct screen_info)];
    assert(recv(ctrl, info, sizeof(info), MSG_DONTWAIT) < 0 && errno == EAGAIN);
    f.ready = 1;
    anland_server_wake(f.server);
    pump(&f);
    assert(recv(ctrl, info, sizeof(info), MSG_DONTWAIT) == sizeof(info));
    struct ctrl_msg pickup = {.type = CTRL_MSG_PICKUP_FDS};
    fragmented_send(&f, ctrl, &pickup, sizeof(pickup));
    int fds[5], buffers[3];
    receive_fds(ctrl, &hello, sizeof(hello), fds, 5);
    assert(hello.type == CTRL_MSG_FDS_READY);
    char pool[sizeof(struct data_msg) + 3 * sizeof(struct buf_info)];
    receive_fds(fds[2], pool, sizeof(pool), buffers, 3);
    const char text[] = "fragmented clipboard";
    struct data_msg hdr = {.type = DATA_MSG_OUTPUT_EVENT, .size = sizeof(struct OutputEvent)};
    struct OutputEvent ev = {.type = OUTPUT_TYPE_CLIPBOARD};
    ev.clipboard.size = sizeof(text) - 1;
    fragmented_send(&f, fds[2], &hdr, sizeof(hdr));
    fragmented_send(&f, fds[2], &ev, sizeof(ev));
    assert(f.output_count == 0);
    fragmented_send(&f, fds[2], text, sizeof(text) - 1);
    assert(f.output_count == 1 && !strcmp(f.clipboard, text));
    for (int i = 0; i < 3; i++) close(buffers[i]);
    for (int i = 0; i < 5; i++) close(fds[i]);
    close(ctrl);
    pump(&f);
    teardown(&f);
}

static void test_services(void) {
    struct fixture f;
    setup(&f);
    display_ctx *ctx = producer(&f);
    struct InputEvent input = {.type = INPUT_TYPE_KEY};
    input.key.keycode = 30; input.key.action = INPUT_ACTION_DOWN;
    assert(anland_server_input(f.server, &input, NULL, 0) == 0);
    struct InputEvent got;
    assert(poll_input_event(ctx, &got, 0) == 1 && got.type == INPUT_TYPE_KEY && got.key.keycode == 30);
    input = (struct InputEvent){.type = INPUT_TYPE_CLIPBOARD};
    input.clipboard.size = 6;
    assert(anland_server_input(f.server, &input, "abc123", 6) == 0);
    assert(poll_input_event(ctx, &got, 0) == 1 && got.clipboard.size == 6);
    char text[16] = {0};
    assert(poll_input_event_extend_data(ctx, text, 6, 0) == 1 && !strcmp(text, "abc123"));
    anland_server_refresh(f.server, 120000);
    assert(poll_input_event(ctx, &got, 0) == 1 && got.type == INPUT_TYPE_DISPLAY_REFRESH && got.display.refresh_mhz == 120000);
    assert(queue_clipboard(ctx, "from producer", 13) == 0);
    assert(flush_queued_output(ctx) == 0);
    pump(&f);
    assert(f.output_count == 1 && !strcmp(f.clipboard, "from producer"));
    disconnect(ctx); pump(&f); teardown(&f);
}

static void test_reconnect(void) {
    struct fixture f;
    setup(&f);
    display_ctx *old = producer(&f);
    assert(target(old) == 0);
    anland_server_reset(f.server);
    assert(poll_buffer_ready(old, 0) == -1);
    f.pool.screen.width = 800;
    display_ctx *fresh = producer(&f);
    struct buf_info info;
    assert(get_dmabuf_info(fresh, &info) == 0 && info.width == 800 && info.stride == 3200);
    assert(target(fresh) == 0);
    assert(trigger_refresh(old) == 0);
    pump(&f);
    assert(f.frames == 0 && f.active);
    assert(trigger_refresh(fresh) == 0);
    pump(&f);
    assert(f.frames == 1 && target(fresh) == 1);
    disconnect(old); disconnect(fresh); pump(&f); teardown(&f);
}

static void test_backpressure(void) {
    struct fixture f;
    setup(&f);
    display_ctx *ctx = producer(&f);
    size_t n = 1024 * 1024;
    char *text = calloc(1, n);
    assert(text);
    struct InputEvent event = {.type = INPUT_TYPE_CLIPBOARD};
    event.clipboard.size = (uint32_t)n;
    int failed = 0;
    for (int i = 0; i < 8; i++) {
        if (anland_server_input(f.server, &event, text, n) < 0) { failed = 1; break; }
    }
    assert(failed && !anland_server_active(f.server));
    free(text); disconnect(ctx); pump(&f); teardown(&f);
}

static void test_malformed(void) {
    struct fixture f;
    setup(&f);
    int fd = raw_connect(&f);
    struct ctrl_msg hello = {.type = CTRL_MSG_PRODUCER_HELLO, .size = 0xffffffff};
    fragmented_send(&f, fd, &hello, sizeof(hello));
    char byte;
    assert(recv(fd, &byte, 1, MSG_DONTWAIT) == 0);
    close(fd);
    display_ctx *ctx = producer(&f);
    assert(target(ctx) == 0);
    struct data_msg hdr = {.type = DATA_MSG_OUTPUT_EVENT, .size = sizeof(struct OutputEvent)};
    struct OutputEvent event = {.type = OUTPUT_TYPE_CLIPBOARD};
    event.clipboard.size = 1024 * 1024 + 1;
    fragmented_send(&f, get_data_fd(ctx), &hdr, sizeof(hdr));
    fragmented_send(&f, get_data_fd(ctx), &event, sizeof(event));
    assert(!anland_server_active(f.server));
    disconnect(ctx); pump(&f); teardown(&f);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    int before = fd_count();
    if (!strcmp(argv[1], "public")) test_public();
    else if (!strcmp(argv[1], "fragmented")) test_fragmented();
    else if (!strcmp(argv[1], "services")) test_services();
    else if (!strcmp(argv[1], "reconnect")) test_reconnect();
    else if (!strcmp(argv[1], "backpressure")) test_backpressure();
    else if (!strcmp(argv[1], "malformed")) test_malformed();
    else assert(0);
    assert(fd_count() == before);
    puts("PASS");
}
