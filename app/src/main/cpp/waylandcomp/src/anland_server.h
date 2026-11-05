#pragma once
#include <stddef.h>
#include <wayland-server-core.h>
#include "protocol.h"

struct anland_server;
struct anland_pool_desc {
    struct screen_info screen;
    int count;
    int fds[MAX_BUFS];
    struct buf_info buffers[MAX_BUFS];
};

struct anland_server_ops {
    int (*prepare)(void *data, struct anland_pool_desc *pool);
    int (*acquire)(void *data);
    int (*present)(void *data, int index, int fence_fd);
    void (*session)(void *data, int active);
    void (*output)(void *data, const struct OutputEvent *event, const void *payload);
    void (*log)(void *data, const char *message);
};

struct anland_server *anland_server_create(struct wl_event_loop *loop, const char *path,
                                          const struct anland_server_ops *ops, void *data);
void anland_server_destroy(struct anland_server *server);
void anland_server_reset(struct anland_server *server);
void anland_server_wake(struct anland_server *server);
int anland_server_active(const struct anland_server *server);
int anland_server_input(struct anland_server *server, const struct InputEvent *event,
                         const void *payload, size_t size);
void anland_server_refresh(struct anland_server *server, uint32_t refresh_mhz);
