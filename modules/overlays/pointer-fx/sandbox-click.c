// sandbox-click: sends real button events to the compositor through a virtual pointer, so click effects can be
// previewed in a terminus-motion-sandbox. The click happens where the pointer currently is.
//
//   sandbox-click [COUNT [INTERVAL_MS [BUTTON]]]     defaults: 1 click, 150 ms apart, button 272 (left)

#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

#define DEFAULT_COUNT 1
#define DEFAULT_INTERVAL_MS 150
#define DEFAULT_BUTTON 272         // BTN_LEFT
#define PRESS_HOLD_US 40000        // press-to-release time of one click

static struct wl_seat *seat;
static struct zwlr_virtual_pointer_manager_v1 *manager;

static void global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
  (void)d; (void)ver;
  if (!strcmp(iface, wl_seat_interface.name)) seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
  else if (!strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name))
    manager = wl_registry_bind(r, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
}
static void global_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener listener = {.global = global, .global_remove = global_remove};

static uint32_t ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

int main(int argc, char **argv) {
  int count = argc > 1 ? atoi(argv[1]) : DEFAULT_COUNT;
  int interval_ms = argc > 2 ? atoi(argv[2]) : DEFAULT_INTERVAL_MS;
  uint32_t button = argc > 3 ? (uint32_t)atoi(argv[3]) : DEFAULT_BUTTON;
  struct wl_display *display = wl_display_connect(NULL);
  if (!display) {
    fprintf(stderr, "sandbox-click: no Wayland compositor\n");
    return 1;
  }
  struct wl_registry *registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &listener, NULL);
  wl_display_roundtrip(display);
  if (!manager) {
    fprintf(stderr, "sandbox-click: compositor has no wlr-virtual-pointer\n");
    wl_display_disconnect(display);
    return 1;
  }
  struct zwlr_virtual_pointer_v1 *pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(manager, seat);
  for (int i = 0; i < count; i++) {
    zwlr_virtual_pointer_v1_button(pointer, ms(), button, WL_POINTER_BUTTON_STATE_PRESSED);
    zwlr_virtual_pointer_v1_frame(pointer);
    wl_display_roundtrip(display);
    usleep(PRESS_HOLD_US);
    zwlr_virtual_pointer_v1_button(pointer, ms(), button, WL_POINTER_BUTTON_STATE_RELEASED);
    zwlr_virtual_pointer_v1_frame(pointer);
    wl_display_roundtrip(display);
    if (i + 1 < count) usleep((useconds_t)interval_ms * 1000);
  }
  zwlr_virtual_pointer_v1_destroy(pointer);
  wl_display_roundtrip(display);
  zwlr_virtual_pointer_manager_v1_destroy(manager);
  wl_registry_destroy(registry);
  wl_display_disconnect(display);
  return 0;
}
