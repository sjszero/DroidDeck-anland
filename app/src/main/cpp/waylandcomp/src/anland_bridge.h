#pragma once
#include <stddef.h>
#include <stdint.h>
struct wl_event_loop;

int anland_bridge_start(struct wl_event_loop *loop, const char *runtime_dir,
                         int width, int height, uint32_t refresh_mhz);
void anland_bridge_stop(void);
void anland_bridge_tick(int width, int height, uint32_t refresh_mhz, int fps_limit);
int anland_bridge_active(void);
int anland_bridge_input(int type, int a, int b, int c, int d);
int anland_bridge_clipboard(const char *text, size_t size);
int anland_bridge_text(const char *text, size_t size);
int anland_bridge_cursor_pos(int *x, int *y);
