#define _GNU_SOURCE
#include "anland_server.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/memfd.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define PAYLOAD_MAX (1024u * 1024u)
#define QUEUE_MAX (2u * PAYLOAD_MAX)
#define IO_BUDGET 16

struct packet {
    struct packet *next;
    size_t size, sent;
    int fds[MAX_BUFS], nfds;
    unsigned char bytes[];
};

struct channel {
    struct anland_server *server;
    int fd, control;
    struct wl_event_source *source;
    struct packet *head, *tail;
    size_t queued, used, need;
    unsigned char header[sizeof(struct data_msg) + sizeof(struct OutputEvent)];
    unsigned char *payload;
    size_t payload_used, payload_size;
    int64_t progress_ms;
};

struct anland_server {
    struct wl_event_loop *loop;
    struct anland_server_ops ops;
    void *data;
    int listen_fd, ready_fd, shm_fd, fence_fd, pending;
    uint32_t *selected;
    struct wl_event_source *listen_source, *fence_source, *timer;
    struct channel ctrl, input;
    struct anland_pool_desc pool;
    bool hello, screen_sent, pickup, active;
    int64_t accepted_ms;
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
};

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void report(struct anland_server *s, const char *message) {
    if (s->ops.log) s->ops.log(s->data, message);
}

static void packet_free(struct packet *p) {
    for (int i = 0; i < p->nfds; i++) close(p->fds[i]);
    free(p);
}

static void channel_close(struct channel *c) {
    if (c->source) wl_event_source_remove(c->source);
    if (c->fd >= 0) close(c->fd);
    while (c->head) {
        struct packet *p = c->head;
        c->head = p->next;
        packet_free(p);
    }
    free(c->payload);
    struct anland_server *s = c->server;
    int control = c->control;
    *c = (struct channel){.server = s, .fd = -1, .control = control};
}

void anland_server_reset(struct anland_server *s) {
    if (!s) return;
    bool had_peer = s->ctrl.fd >= 0;
    s->active = false;
    channel_close(&s->ctrl);
    channel_close(&s->input);
    if (s->fence_source) wl_event_source_remove(s->fence_source);
    s->fence_source = NULL;
    if (s->fence_fd >= 0) close(s->fence_fd);
    if (s->ready_fd >= 0) close(s->ready_fd);
    if (s->selected) munmap(s->selected, sizeof(*s->selected));
    if (s->shm_fd >= 0) close(s->shm_fd);
    s->fence_fd = s->ready_fd = s->shm_fd = s->pending = -1;
    s->selected = NULL;
    s->hello = s->screen_sent = s->pickup = false;
    memset(&s->pool, 0, sizeof(s->pool));
    if (had_peer) {
        s->ops.session(s->data, 0);
        report(s, "producer disconnected; external targets retired");
    }
}

static int fail(struct anland_server *s, const char *reason) {
    report(s, reason);
    anland_server_reset(s);
    return -1;
}

static void interest(struct channel *c) {
    if (c->source)
        wl_event_source_fd_update(c->source, WL_EVENT_READABLE | (c->head ? WL_EVENT_WRITABLE : 0));
}

static int enqueue(struct channel *c, const void *bytes, size_t size, const int *fds, int nfds) {
    if (c->fd < 0 || size > QUEUE_MAX - c->queued || nfds < 0 || nfds > MAX_BUFS) {
        errno = ENOBUFS;
        return -1;
    }
    struct packet *p = calloc(1, sizeof(*p) + size);
    if (!p) return -1;
    p->size = size;
    memcpy(p->bytes, bytes, size);
    for (int i = 0; i < nfds; i++) {
        int fd = fcntl(fds[i], F_DUPFD_CLOEXEC, 0);
        if (fd < 0) { packet_free(p); return -1; }
        p->fds[p->nfds++] = fd;
    }
    if (c->tail) c->tail->next = p; else c->head = p;
    c->tail = p;
    c->queued += size;
    interest(c);
    return 0;
}

static int flush(struct channel *c) {
    for (int budget = 0; budget < IO_BUDGET && c->head; budget++) {
        struct packet *p = c->head;
        struct iovec iov = {.iov_base = p->bytes + p->sent, .iov_len = p->size - p->sent};
        union { struct cmsghdr align; char bytes[CMSG_SPACE(MAX_BUFS * sizeof(int))]; } ancillary;
        struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1};
        if (p->nfds) {
            msg.msg_control = ancillary.bytes;
            msg.msg_controllen = CMSG_SPACE(p->nfds * sizeof(int));
            memset(ancillary.bytes, 0, msg.msg_controllen);
            struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
            cm->cmsg_level = SOL_SOCKET;
            cm->cmsg_type = SCM_RIGHTS;
            cm->cmsg_len = CMSG_LEN(p->nfds * sizeof(int));
            memcpy(CMSG_DATA(cm), p->fds, p->nfds * sizeof(int));
        }
        ssize_t n = sendmsg(c->fd, &msg, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n <= 0) return fail(c->server, "output channel closed");
        for (int i = 0; i < p->nfds; i++) close(p->fds[i]);
        p->nfds = 0;
        p->sent += (size_t)n;
        if (p->sent == p->size) {
            c->head = p->next;
            if (!c->head) c->tail = NULL;
            c->queued -= p->size;
            packet_free(p);
        }
    }
    interest(c);
    return 0;
}

static ssize_t read_plain(int fd, void *bytes, size_t size) {
    struct iovec iov = {.iov_base = bytes, .iov_len = size};
    union { struct cmsghdr align; char bytes[CMSG_SPACE(MAX_BUFS * sizeof(int))]; } ancillary;
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = ancillary.bytes, .msg_controllen = sizeof(ancillary.bytes)};
    ssize_t n = recvmsg(fd, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (n < 0) return n;
    bool bad = msg.msg_flags & (MSG_CTRUNC | MSG_TRUNC);
    for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm)) {
        bad = true;
        if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS) continue;
        size_t count = (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (size_t i = 0; i < count; i++) { int fd; memcpy(&fd, (char *)CMSG_DATA(cm) + i * sizeof(int), sizeof(fd)); close(fd); }
    }
    if (bad) { errno = EPROTO; return -1; }
    return n;
}

static void kick(struct anland_server *s) {
    if (!s->active || s->pending >= 0) return;
    int index = s->ops.acquire(s->data);
    if (index < 0) return;
    if (index >= s->pool.count) { fail(s, "invalid writable target"); return; }
    __atomic_store_n(s->selected, (uint32_t)index, __ATOMIC_RELEASE);
    s->pending = index;
    if (eventfd_write(s->ready_fd, 1) != 0) fail(s, "target notification failed");
}

static int on_fence(int fd, uint32_t mask, void *data) {
    struct anland_server *s = data;
    for (int budget = 0; budget < IO_BUDGET && s->active; budget++) {
        unsigned char byte;
        struct iovec iov = {.iov_base = &byte, .iov_len = 1};
        union { struct cmsghdr align; char bytes[CMSG_SPACE(MAX_BUFS * sizeof(int))]; } ancillary;
        struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1,
            .msg_control = ancillary.bytes, .msg_controllen = sizeof(ancillary.bytes)};
        ssize_t n = recvmsg(fd, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        int fence = -1;
        bool bad = n != 1 || (msg.msg_flags & (MSG_CTRUNC | MSG_TRUNC));
        for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm)) {
            if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS) { bad = true; continue; }
            size_t bytes = cm->cmsg_len - CMSG_LEN(0);
            if (bytes % sizeof(int)) bad = true;
            for (size_t i = 0; i < bytes / sizeof(int); i++) {
                int received;
                memcpy(&received, (char *)CMSG_DATA(cm) + i * sizeof(int), sizeof(received));
                if (fence < 0) fence = received; else { close(received); bad = true; }
            }
        }
        if (bad || s->pending < 0) {
            if (fence >= 0) close(fence);
            fail(s, "invalid or closed render-fence channel");
            return 0;
        }
        int index = s->pending;
        s->pending = -1;
        if (s->ops.present(s->data, index, fence) != 0) {
            fail(s, "external target presentation failed");
            return 0;
        }
        kick(s);
    }
    if (s->active && (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)))
        fail(s, "render-fence channel disconnected");
    return 0;
}

static int on_channel(int fd, uint32_t mask, void *data);

static int start_session(struct anland_server *s) {
    int data[2] = {-1, -1}, fence[2] = {-1, -1}, audio[2] = {-1, -1};
    s->ready_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    s->shm_fd = (int)syscall(SYS_memfd_create, "anland-target", MFD_CLOEXEC);
    if (s->ready_fd < 0 || s->shm_fd < 0 || ftruncate(s->shm_fd, sizeof(uint32_t)) != 0) goto error;
    s->selected = mmap(NULL, sizeof(uint32_t), PROT_READ | PROT_WRITE, MAP_SHARED, s->shm_fd, 0);
    if (s->selected == MAP_FAILED) { s->selected = NULL; goto error; }
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, data) != 0 ||
        socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, fence) != 0 ||
        socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, audio) != 0) goto error;
    s->input.fd = data[0]; data[0] = -1;
    s->fence_fd = fence[0]; fence[0] = -1;
    s->input.source = wl_event_loop_add_fd(s->loop, s->input.fd, WL_EVENT_READABLE, on_channel, &s->input);
    s->fence_source = wl_event_loop_add_fd(s->loop, s->fence_fd, WL_EVENT_READABLE, on_fence, s);
    if (!s->input.source || !s->fence_source) goto error;
    int fds[5] = {s->ready_fd, fence[1], data[1], s->shm_fd, audio[1]};
    struct ctrl_msg hello = {.type = CTRL_MSG_FDS_READY, .size = 0};
    if (enqueue(&s->ctrl, &hello, sizeof(hello), fds, 5) != 0) goto error;
    unsigned char bytes[sizeof(struct data_msg) + MAX_BUFS * sizeof(struct buf_info)];
    struct data_msg header = {.type = DATA_MSG_BUFS_READY, .size = s->pool.count * sizeof(struct buf_info)};
    memcpy(bytes, &header, sizeof(header));
    memcpy(bytes + sizeof(header), s->pool.buffers, header.size);
    if (enqueue(&s->input, bytes, sizeof(header) + header.size, s->pool.fds, s->pool.count) != 0) goto error;
    close(data[1]); close(fence[1]); close(audio[0]); close(audio[1]);
    s->active = true;
    s->ops.session(s->data, 1);
    report(s, "Anland v5 producer connected; host targets and native fences enabled");
    kick(s);
    return 0;
error:
    for (int i = 0; i < 2; i++) {
        if (data[i] >= 0) close(data[i]);
        if (fence[i] >= 0) close(fence[i]);
        if (audio[i] >= 0) close(audio[i]);
    }
    return fail(s, "cannot establish Anland v5 session");
}

void anland_server_wake(struct anland_server *s) {
    if (!s || !s->hello) return;
    if (!s->screen_sent) {
        int ready = s->ops.prepare(s->data, &s->pool);
        if (ready < 0) { fail(s, "host target allocation unavailable"); return; }
        if (!ready) return;
        if (!s->pool.screen.width || !s->pool.screen.height || !s->pool.screen.refresh ||
            s->pool.screen.format != 1 || s->pool.count < 2 || s->pool.count > MAX_BUFS) {
            fail(s, "invalid host pool description"); return;
        }
        for (int i = 0; i < s->pool.count; i++) {
            const struct buf_info *b = &s->pool.buffers[i];
            if (s->pool.fds[i] < 0 || b->width != s->pool.screen.width || b->height != s->pool.screen.height ||
                b->format != s->pool.screen.format || (uint64_t)b->stride < (uint64_t)b->width * 4) {
                fail(s, "invalid host buffer layout"); return;
            }
        }
        struct ctrl_msg hdr = {.type = CTRL_MSG_SCREEN_INFO, .size = sizeof(struct screen_info)};
        unsigned char bytes[sizeof(hdr) + sizeof(s->pool.screen)];
        memcpy(bytes, &hdr, sizeof(hdr));
        memcpy(bytes + sizeof(hdr), &s->pool.screen, sizeof(s->pool.screen));
        if (enqueue(&s->ctrl, bytes, sizeof(bytes), NULL, 0) != 0) { fail(s, "screen-info queue failed"); return; }
        s->screen_sent = true;
    }
    if (s->pickup && !s->active && start_session(s) != 0) return;
    kick(s);
}

static int control_record(struct anland_server *s, const struct ctrl_msg *msg) {
    if (msg->size) return fail(s, "unexpected control payload");
    if (!s->hello && msg->type == CTRL_MSG_PRODUCER_HELLO) {
        s->hello = true;
    } else if (s->hello && !s->pickup && msg->type == CTRL_MSG_PICKUP_FDS) {
        s->pickup = true;
    } else {
        return fail(s, "invalid Anland producer handshake");
    }
    anland_server_wake(s);
    return s->ctrl.fd >= 0 ? 0 : -1;
}

static int read_channel(struct channel *c) {
    struct anland_server *s = c->server;
    for (int budget = 0; budget < IO_BUDGET; budget++) {
        if (!c->need) c->need = c->control ? sizeof(struct ctrl_msg) : sizeof(struct data_msg) + sizeof(struct OutputEvent);
        bool payload = c->payload != NULL;
        void *where = payload ? c->payload + c->payload_used : c->header + c->used;
        size_t remaining = payload ? c->payload_size - c->payload_used : c->need - c->used;
        ssize_t n = read_plain(c->fd, where, remaining);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        if (n <= 0) return fail(s, "invalid or closed protocol channel");
        c->progress_ms = now_ms();
        if (payload) {
            c->payload_used += (size_t)n;
            if (c->payload_used != c->payload_size) continue;
        } else {
            c->used += (size_t)n;
            if (c->used != c->need) continue;
        }
        if (c->control) {
            struct ctrl_msg msg;
            memcpy(&msg, c->header, sizeof(msg));
            c->used = c->need = 0;
            if (control_record(s, &msg) != 0) return -1;
        } else {
            struct data_msg hdr;
            struct OutputEvent event;
            memcpy(&hdr, c->header, sizeof(hdr));
            memcpy(&event, c->header + sizeof(hdr), sizeof(event));
            if (hdr.type != DATA_MSG_OUTPUT_EVENT || hdr.size != sizeof(event)) return fail(s, "invalid output event header");
            if (!payload && event.type == OUTPUT_TYPE_CLIPBOARD && event.clipboard.size) {
                if (event.clipboard.size > PAYLOAD_MAX) return fail(s, "clipboard payload exceeds limit");
                c->payload = malloc((size_t)event.clipboard.size + 1);
                if (!c->payload) return fail(s, "clipboard allocation failed");
                c->payload_size = event.clipboard.size;
                continue;
            }
            if (c->payload) c->payload[c->payload_size] = 0;
            s->ops.output(s->data, &event, c->payload);
            if (c->fd < 0) return -1;
            free(c->payload); c->payload = NULL;
            c->used = c->need = c->payload_used = c->payload_size = 0;
        }
    }
    return 0;
}

static int on_channel(int fd, uint32_t mask, void *data) {
    (void)fd;
    struct channel *c = data;
    if ((mask & WL_EVENT_READABLE) && read_channel(c) != 0) return 0;
    if (c->fd < 0) return 0;
    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) { fail(c->server, "producer transport disconnected"); return 0; }
    if (mask & WL_EVENT_WRITABLE) flush(c);
    return 0;
}

static int on_accept(int fd, uint32_t mask, void *data) {
    (void)mask;
    struct anland_server *s = data;
    for (int budget = 0; budget < IO_BUDGET; budget++) {
        int peer = accept4(fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (peer < 0) break;
        struct ucred cred;
        socklen_t size = sizeof(cred);
        if (s->ctrl.fd >= 0 || getsockopt(peer, SOL_SOCKET, SO_PEERCRED, &cred, &size) != 0 || cred.uid != getuid()) {
            close(peer); continue;
        }
        s->ctrl.fd = peer;
        s->accepted_ms = now_ms();
        s->ctrl.source = wl_event_loop_add_fd(s->loop, peer, WL_EVENT_READABLE, on_channel, &s->ctrl);
        if (!s->ctrl.source) { anland_server_reset(s); continue; }
        wl_event_source_timer_update(s->timer, 1000);
    }
    return 0;
}

static int on_timer(void *data) {
    struct anland_server *s = data;
    int64_t now = now_ms();
    struct channel *channels[] = {&s->ctrl, &s->input};
    for (size_t i = 0; i < sizeof(channels) / sizeof(channels[0]); i++) {
        struct channel *c = channels[i];
        if ((c->used || c->payload) && now - c->progress_ms >= 5000) {
            fail(s, "incomplete protocol record timed out"); return 0;
        }
    }
    if (s->ctrl.fd >= 0 && !s->hello && now - s->accepted_ms >= 5000) {
        fail(s, "producer hello timed out"); return 0;
    }
    anland_server_wake(s);
    if (s->ctrl.fd >= 0) wl_event_source_timer_update(s->timer, 1000);
    return 0;
}

struct anland_server *anland_server_create(struct wl_event_loop *loop, const char *path,
                                          const struct anland_server_ops *ops, void *data) {
    if (!loop || !path || !*path || strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path) ||
        !ops || !ops->prepare || !ops->acquire || !ops->present || !ops->session || !ops->output) {
        errno = EINVAL; return NULL;
    }
    struct anland_server *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->loop = loop; s->ops = *ops; s->data = data;
    s->listen_fd = s->ready_fd = s->shm_fd = s->fence_fd = s->pending = -1;
    s->ctrl = (struct channel){.server = s, .fd = -1, .control = 1};
    s->input = (struct channel){.server = s, .fd = -1};
    struct stat st;
    if (lstat(path, &st) == 0) {
        if (!S_ISSOCK(st.st_mode) || st.st_uid != getuid()) { errno = EEXIST; goto error; }
        int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        struct sockaddr_un addr = {.sun_family = AF_UNIX};
        strcpy(addr.sun_path, path);
        if (probe < 0) goto error;
        int rc = connect(probe, (struct sockaddr *)&addr, sizeof(addr));
        int saved = errno;
        close(probe);
        if (rc == 0 || saved != ECONNREFUSED) { errno = EADDRINUSE; goto error; }
        if (unlink(path) != 0) goto error;
    } else if (errno != ENOENT) goto error;
    s->listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strcpy(addr.sun_path, path);
    if (s->listen_fd < 0 || bind(s->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) goto error;
    strcpy(s->path, path);
    if (chmod(path, 0600) != 0 || listen(s->listen_fd, 4) != 0) goto error;
    s->listen_source = wl_event_loop_add_fd(loop, s->listen_fd, WL_EVENT_READABLE, on_accept, s);
    s->timer = wl_event_loop_add_timer(loop, on_timer, s);
    if (!s->listen_source || !s->timer) goto error;
    report(s, "Anland v5 endpoint ready alongside Wayland");
    return s;
error:
    { int saved = errno; anland_server_destroy(s); errno = saved; }
    return NULL;
}

void anland_server_destroy(struct anland_server *s) {
    if (!s) return;
    anland_server_reset(s);
    if (s->timer) wl_event_source_remove(s->timer);
    if (s->listen_source) wl_event_source_remove(s->listen_source);
    if (s->listen_fd >= 0) close(s->listen_fd);
    if (s->path[0]) unlink(s->path);
    free(s);
}

int anland_server_active(const struct anland_server *s) { return s && s->active; }

int anland_server_input(struct anland_server *s, const struct InputEvent *event,
                         const void *payload, size_t size) {
    if (!s || !s->active) return 0;
    if (!event || size > PAYLOAD_MAX || (size && !payload) ||
        (event->type == INPUT_TYPE_CLIPBOARD ? event->clipboard.size != size :
         event->type == INPUT_TYPE_TEXT_INPUT ? event->text_input.size != size : size != 0)) {
        errno = EINVAL; return -1;
    }
    size_t total = sizeof(struct data_msg) + sizeof(*event) + size;
    unsigned char *bytes = malloc(total);
    if (!bytes) return -1;
    struct data_msg hdr = {.type = DATA_MSG_INPUT_EVENT, .size = sizeof(*event)};
    memcpy(bytes, &hdr, sizeof(hdr));
    memcpy(bytes + sizeof(hdr), event, sizeof(*event));
    if (size) memcpy(bytes + sizeof(hdr) + sizeof(*event), payload, size);
    int ret = enqueue(&s->input, bytes, total, NULL, 0);
    free(bytes);
    if (ret != 0) return fail(s, "input queue full; reconnecting to preserve input state");
    return flush(&s->input);
}

void anland_server_refresh(struct anland_server *s, uint32_t refresh_mhz) {
    if (!s || !s->active || !refresh_mhz || s->pool.screen.refresh == refresh_mhz) return;
    s->pool.screen.refresh = refresh_mhz;
    struct InputEvent event = {.type = INPUT_TYPE_DISPLAY_REFRESH};
    event.display.refresh_mhz = refresh_mhz;
    anland_server_input(s, &event, NULL, 0);
}
