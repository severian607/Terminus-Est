// pointer-fx: click ripple and pointer trail as a click-through layer-shell overlay.
//
//   pointer-fx ripple|trail|mirror [--color #rrggbb]
//
// One transparent overlay surface per output. A surface only holds a buffer while something is animated; when
// nothing is, the buffer is detached and freed. The pointer position comes from the compositor's own IPC socket
// (`j/cursorpos`), found through XDG_RUNTIME_DIR and HYPRLAND_INSTANCE_SIGNATURE.
//
//   ripple: Wayland has no global click events, so the daemon registers two non-consuming Lua binds (mouse:272,
//           mouse:273, any modifiers) through the IPC `eval` request. Each bind writes one byte to a FIFO the
//           daemon owns. The binds are registered again after every `configreloaded` event and removed on exit.
//           No polling while idle.
//   mirror: the old "hall of mirrors" glitch: copies of the pointer stay where it has been until the overlay is
//           stopped (the oldest goes only after MAX_POINTS copies). Same pointer polling as trail; the screen is
//           redrawn only when a copy is added.
//   trail:  polls the pointer position, about 90 times a second while it moves, 25 times a second just after, 10 times a second when it has been still for 5 s.

#define _GNU_SOURCE
#include <cairo.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

// ---- geometry ---------------------------------------------------------------------------------------------

typedef struct { int x0, y0, x1, y1; } Rect;  // half-open, empty when x1 <= x0 or y1 <= y0

static bool rect_empty(Rect r) { return r.x1 <= r.x0 || r.y1 <= r.y0; }
static Rect rect_union(Rect a, Rect b) {
  if (rect_empty(a)) return b;
  if (rect_empty(b)) return a;
  return (Rect){a.x0 < b.x0 ? a.x0 : b.x0, a.y0 < b.y0 ? a.y0 : b.y0, a.x1 > b.x1 ? a.x1 : b.x1, a.y1 > b.y1 ? a.y1 : b.y1};
}
static Rect rect_isect(Rect a, Rect b) {
  return (Rect){a.x0 > b.x0 ? a.x0 : b.x0, a.y0 > b.y0 ? a.y0 : b.y0, a.x1 < b.x1 ? a.x1 : b.x1, a.y1 < b.y1 ? a.y1 : b.y1};
}

// ---- state ------------------------------------------------------------------------------------------------

typedef struct {
  struct wl_buffer *buffer;
  void *data;
  size_t size;
  cairo_surface_t *cs;
  bool busy;
  Rect dirty;  // output-local logical rectangle this buffer's pixels may be non-transparent in
} Buf;

typedef struct Output {
  struct wl_output *wl;
  uint32_t global;
  struct zxdg_output_v1 *xdg;
  struct wl_surface *surface;
  struct zwlr_layer_surface_v1 *layer;
  int x, y, w, h;  // logical position and size
  int scale;
  bool configured;
  Buf bufs[2];
  bool mapped;
  Rect shown;  // output-local rectangle drawn in the buffer the compositor currently shows
  struct Output *next;
} Output;

typedef enum { MODE_RIPPLE, MODE_TRAIL, MODE_MIRROR } Mode;

#define MAX_CLICKS 32
#define MAX_POINTS 6000  // the trail never holds more than ~100; copies stay, so the oldest one goes only past this

typedef struct { double x, y, t; int set; } Pt;  // set: cursor look of a mirror copy (-1: plain arrow)

static struct {
  Mode mode;
  double rgb[3];
  struct wl_display *display;
  struct wl_registry *registry;
  struct wl_compositor *compositor;
  struct wl_shm *shm;
  struct zwlr_layer_shell_v1 *layer_shell;
  struct zxdg_output_manager_v1 *xdg_manager;
  Output *outputs;
  bool ready;
  bool dirty_retry;  // a frame was skipped because both buffers were still held by the compositor
  Pt clicks[MAX_CLICKS];
  int n_clicks;
  Pt points[MAX_POINTS];
  int n_points;
  double last_move;  // time of the last observed pointer movement (trail)
  char ipc_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
  char fifo_path[256];
  int timer_fd, fifo_fd, events_fd;  // -1 when unused
  double timer_period;
} S;

static volatile sig_atomic_t quit_flag;

static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// ---- compositor IPC ---------------------------------------------------------------------------------------

static int ipc_connect(const char *name) {
  const char *run = getenv("XDG_RUNTIME_DIR");
  const char *sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
  if (!run || !sig) return -1;
  struct sockaddr_un a = {.sun_family = AF_UNIX};
  if (snprintf(a.sun_path, sizeof a.sun_path, "%s/hypr/%s/%s", run, sig, name) >= (int)sizeof a.sun_path) return -1;
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  struct timeval tv = {.tv_sec = 0, .tv_usec = 200000};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static bool ipc_request(const char *req, char *out, size_t cap) {
  int fd = ipc_connect(".socket.sock");
  if (fd < 0) return false;
  size_t len = strlen(req);
  bool ok = write(fd, req, len) == (ssize_t)len;
  size_t n = 0;
  while (ok && n + 1 < cap) {
    ssize_t r = read(fd, out + n, cap - 1 - n);
    if (r <= 0) break;
    n += (size_t)r;
  }
  out[n] = 0;
  close(fd);
  return ok;
}

static bool cursor_pos(double *x, double *y) {
  char buf[128];
  if (!ipc_request("j/cursorpos", buf, sizeof buf)) return false;
  const char *px = strstr(buf, "\"x\"");
  const char *py = strstr(buf, "\"y\"");
  if (!px || !py) return false;
  px = strchr(px, ':');
  py = strchr(py, ':');
  if (!px || !py) return false;
  *x = strtod(px + 1, NULL);
  *y = strtod(py + 1, NULL);
  return true;
}

static void register_binds(void) {
  char lua[1024];
  snprintf(lua, sizeof lua,
           "if _G.terminus_overlay_click then for _, b in ipairs(_G.terminus_overlay_click) do b:unbind() end end "
           "local cmd = [[test -p '%s' && { exec 3<>'%s'; echo >&3; }]] "
           "local o = { non_consuming = true, ignore_mods = true } "
           "_G.terminus_overlay_click = { hl.bind('mouse:272', hl.dsp.exec_cmd(cmd), o), "
           "hl.bind('mouse:273', hl.dsp.exec_cmd(cmd), o) }",
           S.fifo_path, S.fifo_path);
  char req[1200], reply[256] = "";
  snprintf(req, sizeof req, "/eval %s", lua);
  if (!ipc_request(req, reply, sizeof reply) || strncmp(reply, "ok", 2) != 0)
    fprintf(stderr, "pointer-fx: registering click binds failed: %s\n", reply);
}

static void unregister_binds(void) {
  char reply[256];
  ipc_request("/eval if _G.terminus_overlay_click then for _, b in ipairs(_G.terminus_overlay_click) do b:unbind() end "
              "_G.terminus_overlay_click = nil end",
              reply, sizeof reply);
}

// ---- buffers ----------------------------------------------------------------------------------------------

static void buffer_release(void *data, struct wl_buffer *b) {
  (void)b;
  ((Buf *)data)->busy = false;
}
static const struct wl_buffer_listener buffer_listener = {.release = buffer_release};

static bool buf_alloc(Output *o, Buf *b) {
  int pw = o->w * o->scale, ph = o->h * o->scale;
  int stride = pw * 4;
  b->size = (size_t)stride * ph;
  int fd = memfd_create("pointer-fx", MFD_CLOEXEC);
  if (fd < 0 || ftruncate(fd, (off_t)b->size) < 0) return false;
  b->data = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (b->data == MAP_FAILED) {
    close(fd);
    b->data = NULL;
    return false;
  }
  struct wl_shm_pool *pool = wl_shm_create_pool(S.shm, fd, (int32_t)b->size);
  b->buffer = wl_shm_pool_create_buffer(pool, 0, pw, ph, stride, WL_SHM_FORMAT_ARGB8888);
  wl_shm_pool_destroy(pool);
  close(fd);
  wl_buffer_add_listener(b->buffer, &buffer_listener, b);
  b->cs = cairo_image_surface_create_for_data(b->data, CAIRO_FORMAT_ARGB32, pw, ph, stride);
  b->busy = false;
  b->dirty = (Rect){0, 0, 0, 0};
  return true;
}

static void buf_free(Buf *b) {
  if (b->cs) cairo_surface_destroy(b->cs);
  if (b->buffer) wl_buffer_destroy(b->buffer);
  if (b->data) munmap(b->data, b->size);
  memset(b, 0, sizeof *b);
}

static void output_free_buffers(Output *o) {
  buf_free(&o->bufs[0]);
  buf_free(&o->bufs[1]);
}

// ---- effects ----------------------------------------------------------------------------------------------

#define RIPPLE_DUR 0.85
#define RIPPLE_DELAY 0.11
#define RIPPLE_RINGS 3
#define RIPPLE_RADIUS 70.0
#define RIPPLE_PAD 14

#define TRAIL_LIFE 0.55
#define TRAIL_PAD 22
#define TRAIL_BUCKETS 48

#define MIRROR_STEP 15.0  // pixels between copies
#define MIRROR_SCALE 1.5
#define MIRROR_PAD 40

#define MOVE_EPSILON_PX 0.5       // smaller pointer jumps are not movement
#define TRAIL_PAUSE_S 0.15        // a gap this long between samples starts a new trail
#define TRAIL_SEED_LEAD_S 1e-3    // the seed point of a new trail is this much older than its first sample
#define POLL_FAST_S 0.011         // pointer poll period while moving or animating
#define POLL_FAST_WINDOW_S 0.5    // stay fast this long after the last movement
#define POLL_SLOW_S 0.04
#define POLL_SLOW_WINDOW_S 5      // then slow, then idle
#define POLL_IDLE_S 0.1
#define RIPPLE_FRAME_S 0.007

static Rect fx_bounds(double now) {
  Rect r = {0, 0, 0, 0};
  if (S.mode == MODE_RIPPLE) {
    for (int i = 0; i < S.n_clicks; i++) {
      if (now - S.clicks[i].t > RIPPLE_DUR + RIPPLE_DELAY * (RIPPLE_RINGS - 1)) continue;
      double c = RIPPLE_RADIUS + RIPPLE_PAD;
      r = rect_union(r, (Rect){(int)floor(S.clicks[i].x - c), (int)floor(S.clicks[i].y - c), (int)ceil(S.clicks[i].x + c), (int)ceil(S.clicks[i].y + c)});
    }
  } else if (S.n_points > 0) {
    double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
    for (int i = 0; i < S.n_points; i++) {
      if (S.points[i].x < x0) x0 = S.points[i].x;
      if (S.points[i].x > x1) x1 = S.points[i].x;
      if (S.points[i].y < y0) y0 = S.points[i].y;
      if (S.points[i].y > y1) y1 = S.points[i].y;
    }
    int pad = S.mode == MODE_MIRROR ? MIRROR_PAD : TRAIL_PAD;
    r = (Rect){(int)floor(x0 - pad), (int)floor(y0 - pad), (int)ceil(x1 + pad), (int)ceil(y1 + pad)};
  }
  return r;
}

static void draw_ripples(cairo_t *cr, double now) {
  for (int i = 0; i < S.n_clicks; i++) {
    double age = now - S.clicks[i].t;
    double cx = S.clicks[i].x, cy = S.clicks[i].y;
    // soft flash under the pointer
    if (age < 0.3) {
      double u = age / 0.3, a = (1 - u) * (1 - u) * 0.55;
      cairo_pattern_t *g = cairo_pattern_create_radial(cx, cy, 0, cx, cy, 10 + 22 * u);
      cairo_pattern_add_color_stop_rgba(g, 0, S.rgb[0], S.rgb[1], S.rgb[2], a);
      cairo_pattern_add_color_stop_rgba(g, 1, S.rgb[0], S.rgb[1], S.rgb[2], 0);
      cairo_set_source(cr, g);
      cairo_arc(cr, cx, cy, 10 + 22 * u, 0, 2 * M_PI);
      cairo_fill(cr);
      cairo_pattern_destroy(g);
    }
    for (int k = 0; k < RIPPLE_RINGS; k++) {
      double u = (age - k * RIPPLE_DELAY) / RIPPLE_DUR;
      if (u <= 0 || u >= 1) continue;
      double e = 1 - pow(1 - u, 3);
      double radius = 5 + (RIPPLE_RADIUS - 5) * e;
      double a = pow(1 - u, 1.6) * (k == 0 ? 0.95 : 0.7);
      double lw = 0.9 + 2.6 * (1 - u);
      cairo_arc(cr, cx, cy, radius, 0, 2 * M_PI);
      cairo_set_source_rgba(cr, S.rgb[0], S.rgb[1], S.rgb[2], a * 0.22);
      cairo_set_line_width(cr, lw * 4);
      cairo_stroke_preserve(cr);
      cairo_set_source_rgba(cr, S.rgb[0], S.rgb[1], S.rgb[2], a);
      cairo_set_line_width(cr, lw);
      cairo_stroke(cr);
    }
  }
}

// Strokes points [a, b] as one smooth path (quadratic curves through the midpoints).
static void trail_path(cairo_t *cr, const Pt *p, int a, int b) {
  cairo_new_path(cr);
  cairo_move_to(cr, p[a].x, p[a].y);
  if (b - a == 1) {
    cairo_line_to(cr, p[b].x, p[b].y);
    return;
  }
  double cx0 = p[a].x, cy0 = p[a].y;
  for (int i = a + 1; i < b; i++) {
    double mx = (p[i].x + p[i + 1].x) / 2, my = (p[i].y + p[i + 1].y) / 2;
    cairo_curve_to(cr, cx0 + 2.0 / 3 * (p[i].x - cx0), cy0 + 2.0 / 3 * (p[i].y - cy0), mx + 2.0 / 3 * (p[i].x - mx),
                   my + 2.0 / 3 * (p[i].y - my), mx, my);
    cx0 = mx;
    cy0 = my;
  }
  cairo_line_to(cr, p[b].x, p[b].y);
}

static void draw_trail(cairo_t *cr, double now) {
  const Pt *p = S.points;
  int n = S.n_points;
  if (n < 2) return;
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  double core[3] = {S.rgb[0] * 0.45 + 0.55, S.rgb[1] * 0.45 + 0.55, S.rgb[2] * 0.45 + 0.55};
  // Segments of the same age bucket form one stroke with one alpha and width. Each layer (glow, body, core) is
  // drawn into its own group with the SOURCE operator, so the round caps of neighbouring buckets replace each
  // other instead of piling up alpha.
  int bk[MAX_POINTS];
  for (int i = 0; i < n; i++) {
    double f = 1 - (now - p[i].t) / TRAIL_LIFE;
    bk[i] = f <= 0 ? -1 : (int)(f * TRAIL_BUCKETS);
    if (bk[i] >= TRAIL_BUCKETS) bk[i] = TRAIL_BUCKETS - 1;
  }
  static const struct { double w0, w1, alpha; int color; } layers[3] = {
      {6, 22, 0.16, 0}, {2.5, 8, 0.42, 0}, {1, 3.2, 0.95, 1}};
  for (int l = 0; l < 3; l++) {
    const double *c = layers[l].color ? core : S.rgb;
    cairo_push_group(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    int start = 0;
    for (int i = 1; i < n; i++) {
      if (i != n - 1 && bk[i + 1] == bk[i]) continue;
      if (bk[i] >= 0) {
        double fr = (bk[i] + 0.5) / TRAIL_BUCKETS;
        trail_path(cr, p, start, i);
        cairo_set_source_rgba(cr, c[0], c[1], c[2], layers[l].alpha * pow(fr, 1.4));
        cairo_set_line_width(cr, layers[l].w0 + (layers[l].w1 - layers[l].w0) * fr);
        cairo_stroke(cr);
      }
      start = i;
    }
    cairo_pop_group_to_source(cr);
    cairo_paint(cr);
  }
}

// The user's own pointer: the frames of an XCursor theme's left_ptr, read by hand from the file (format:
// xcursor(3)). The theme is the one the desktop currently uses (state file of terminus-cursor, else XCURSOR_THEME),
// looked at again while running: every copy keeps the look the pointer had when the copy was made. Without a
// readable theme the copies are drawn as a classic arrow.
#define MAX_FRAMES 64
#define MAX_SETS 8
typedef struct { cairo_surface_t *img[MAX_FRAMES]; int hot_x[MAX_FRAMES], hot_y[MAX_FRAMES]; uint32_t *px[MAX_FRAMES]; int n; double frame_s; } CursorSet;
static CursorSet SETS[MAX_SETS];
static int n_sets = -1;      // -1: not looked at yet
static int cur_set = -1;     // set used for new copies, -1 for the arrow
static char cur_key[160];    // "theme size" the current set was loaded for
static time_t cur_stamp;     // mtime of the state file when it was last read
static double cur_checked;

static uint32_t rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static bool load_set(CursorSet *c, const char *theme, uint32_t want) {
  const char *home = getenv("HOME");
  char path[512];
  FILE *f = NULL;
  for (int i = 0; i < 3 && !f; i++) {
    if (i == 0) snprintf(path, sizeof path, "%s/.local/share/icons/%s/cursors/left_ptr", home ? home : "", theme);
    else if (i == 1) snprintf(path, sizeof path, "%s/.icons/%s/cursors/left_ptr", home ? home : "", theme);
    else snprintf(path, sizeof path, "/usr/share/icons/%s/cursors/left_ptr", theme);
    f = fopen(path, "rb");
  }
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char *buf = len >= 16 && len <= (8 << 20) ? malloc((size_t)len) : NULL;
  if (!buf || fread(buf, 1, (size_t)len, f) != (size_t)len || memcmp(buf, "Xcur", 4)) {
    free(buf);
    fclose(f);
    return false;
  }
  fclose(f);
  uint32_t ntoc = rd32(buf + 12), best = 0;
  for (uint32_t i = 0; i < ntoc && 16 + 12 * (i + 1) <= (size_t)len; i++) {
    const unsigned char *e = buf + 16 + 12 * i;
    if (rd32(e) != 0xfffd0002) continue;
    uint32_t sz = rd32(e + 4);
    if (!best || (sz > want ? sz - want : want - sz) < (best > want ? best - want : want - best)) best = sz;
  }
  for (uint32_t i = 0; i < ntoc && c->n < MAX_FRAMES && 16 + 12 * (i + 1) <= (size_t)len; i++) {
    const unsigned char *e = buf + 16 + 12 * i;
    if (rd32(e) != 0xfffd0002 || rd32(e + 4) != best) continue;
    uint32_t pos = rd32(e + 8);
    if ((size_t)pos + 36 > (size_t)len) continue;
    uint32_t w = rd32(buf + pos + 16), h = rd32(buf + pos + 20);
    if (!w || !h || w > 512 || h > 512 || (size_t)pos + 36 + (size_t)w * h * 4 > (size_t)len) continue;
    uint32_t *px = malloc((size_t)w * h * 4);
    if (!px) continue;
    memcpy(px, buf + pos + 36, (size_t)w * h * 4);  // premultiplied ARGB, native little endian: cairo's ARGB32
    c->px[c->n] = px;
    c->img[c->n] = cairo_image_surface_create_for_data((unsigned char *)px, CAIRO_FORMAT_ARGB32, (int)w, (int)h, (int)w * 4);
    c->hot_x[c->n] = (int)rd32(buf + pos + 24);
    c->hot_y[c->n] = (int)rd32(buf + pos + 28);
    uint32_t delay = rd32(buf + pos + 32);
    c->frame_s = delay ? delay / 1000.0 : 0.1;
    c->n++;
  }
  free(buf);
  return c->n > 0;
}

// Looks at the desktop's cursor theme at most every 0.3 s and loads a new set when it changed.
static void refresh_cursor(double now) {
  if (n_sets >= 0 && now - cur_checked < 0.3) return;
  cur_checked = now;
  char key[160] = "";
  const char *home = getenv("HOME");
  char state[512];
  snprintf(state, sizeof state, "%s/.local/state/terminus-est/cursor/theme", home ? home : "");
  struct stat st;
  if (n_sets >= 0 && stat(state, &st) == 0 && st.st_mtime == cur_stamp) return;
  FILE *f = fopen(state, "r");
  if (f) {
    if (fgets(key, sizeof key, f)) key[strcspn(key, "\n")] = 0;
    fclose(f);
    if (stat(state, &st) == 0) cur_stamp = st.st_mtime;
  }
  if (!key[0]) {
    const char *t = getenv("XCURSOR_THEME"), *z = getenv("XCURSOR_SIZE");
    snprintf(key, sizeof key, "%s %s", t && *t ? t : "Mita", z ? z : "32");
  }
  if (n_sets >= 0 && !strcmp(key, cur_key)) return;
  if (n_sets < 0) n_sets = 0;
  snprintf(cur_key, sizeof cur_key, "%s", key);
  char theme[128];
  unsigned size = 32;
  if (sscanf(key, "%127s %u", theme, &size) < 1) return;
  if (n_sets < MAX_SETS && load_set(&SETS[n_sets], theme, size)) cur_set = n_sets++;
  else if (n_sets >= MAX_SETS) fprintf(stderr, "pointer-fx: too many cursor themes in one session, keeping the last\\n");
  else cur_set = -1;
}

// A pointer copy with its hotspot at (x, y). Each copy keeps the look and animation frame the pointer had when it
// was left.
static void draw_mirror(cairo_t *cr, double now) {
  (void)now;
  for (int i = 0; i < S.n_points; i++) {
    int si = S.points[i].set;
    if (si < 0) continue;
    const CursorSet *c = &SETS[si];
    int k = (int)(S.points[i].t / c->frame_s) % c->n;
    cairo_set_source_surface(cr, c->img[k], S.points[i].x - c->hot_x[k], S.points[i].y - c->hot_y[k]);
    cairo_paint(cr);
  }
  bool arrows = false;
  for (int i = 0; i < S.n_points && !arrows; i++) arrows = S.points[i].set < 0;
  if (!arrows) return;
  static const double arrow[][2] = {{0, 0}, {0, 16.5}, {4, 12.8}, {6.8, 19.5}, {9.6, 18.3}, {6.9, 11.8}, {12, 11.8}};
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
  for (int i = 0; i < S.n_points; i++) {
    if (S.points[i].set >= 0) continue;
    cairo_save(cr);
    cairo_translate(cr, S.points[i].x, S.points[i].y);
    cairo_scale(cr, MIRROR_SCALE, MIRROR_SCALE);
    cairo_move_to(cr, arrow[0][0], arrow[0][1]);
    for (size_t k = 1; k < sizeof arrow / sizeof arrow[0]; k++) cairo_line_to(cr, arrow[k][0], arrow[k][1]);
    cairo_close_path(cr);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_set_line_width(cr, 1.1);
    cairo_stroke(cr);
    cairo_restore(cr);
  }
}

// ---- drawing ----------------------------------------------------------------------------------------------

static void output_commit_blank(Output *o) {
  if (!o->mapped) return;
  // A null buffer unmaps a layer surface; it needs a fresh configure (requested by an empty commit) before the
  // next buffer may be attached.
  wl_surface_attach(o->surface, NULL, 0, 0);
  wl_surface_commit(o->surface);
  wl_surface_commit(o->surface);
  o->configured = false;
  o->mapped = false;
  o->shown = (Rect){0, 0, 0, 0};
  output_free_buffers(o);
}

static void output_draw(Output *o, double now, Rect fx) {
  if (!o->configured) return;
  Rect local = {0, 0, 0, 0};
  if (!rect_empty(fx)) {
    Rect shifted = {fx.x0 - o->x, fx.y0 - o->y, fx.x1 - o->x, fx.y1 - o->y};
    local = rect_isect(shifted, (Rect){0, 0, o->w, o->h});
  }
  if (rect_empty(local)) {
    output_commit_blank(o);
    return;
  }
  Buf *b = NULL;
  for (int i = 0; i < 2 && !b; i++)
    if (!o->bufs[i].busy && o->bufs[i].buffer) b = &o->bufs[i];
  for (int i = 0; i < 2 && !b; i++)
    if (!o->bufs[i].buffer) {
      if (!buf_alloc(o, &o->bufs[i])) return;
      b = &o->bufs[i];
    }
  if (!b) {
    S.dirty_retry = true;
    return;
  }
  cairo_t *cr = cairo_create(b->cs);
  cairo_scale(cr, o->scale, o->scale);
  Rect clear = rect_union(b->dirty, local);
  cairo_save(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
  cairo_rectangle(cr, clear.x0, clear.y0, clear.x1 - clear.x0, clear.y1 - clear.y0);
  cairo_fill(cr);
  cairo_restore(cr);
  cairo_rectangle(cr, local.x0, local.y0, local.x1 - local.x0, local.y1 - local.y0);
  cairo_clip(cr);
  cairo_translate(cr, -o->x, -o->y);
  if (S.mode == MODE_RIPPLE) draw_ripples(cr, now);
  else if (S.mode == MODE_MIRROR) draw_mirror(cr, now);
  else draw_trail(cr, now);
  cairo_destroy(cr);
  cairo_surface_flush(b->cs);

  Rect damage = rect_union(o->shown, local);
  if (!o->mapped) damage = local;
  wl_surface_set_buffer_scale(o->surface, o->scale);
  wl_surface_attach(o->surface, b->buffer, 0, 0);
  wl_surface_damage(o->surface, damage.x0, damage.y0, damage.x1 - damage.x0, damage.y1 - damage.y0);
  wl_surface_commit(o->surface);
  b->busy = true;
  b->dirty = local;
  o->shown = local;
  o->mapped = true;
}

static void expire(double now) {
  if (S.mode == MODE_RIPPLE) {
    int k = 0;
    for (int i = 0; i < S.n_clicks; i++)
      if (now - S.clicks[i].t <= RIPPLE_DUR + RIPPLE_DELAY * (RIPPLE_RINGS - 1)) S.clicks[k++] = S.clicks[i];
    S.n_clicks = k;
  } else {
    int drop = 0;
    while (drop < S.n_points && S.mode != MODE_MIRROR && now - S.points[drop].t > TRAIL_LIFE) drop++;
    if (drop > 0) {
      memmove(S.points, S.points + drop, (size_t)(S.n_points - drop) * sizeof(Pt));
      S.n_points -= drop;
    }
    if (S.n_points == 1 && S.mode == MODE_TRAIL) S.n_points = 0;  // a single point has no segment
  }
}

static void render(double now) {
  expire(now);
  Rect fx = fx_bounds(now);
  S.dirty_retry = false;
  for (Output *o = S.outputs; o; o = o->next) output_draw(o, now, fx);
  wl_display_flush(S.display);
}

static bool animating(void) { return S.mode == MODE_RIPPLE ? S.n_clicks > 0 : S.mode == MODE_MIRROR ? false : S.n_points > 0; }

static void set_timer(double period) {
  if (period == S.timer_period) return;
  S.timer_period = period;
  struct itimerspec its = {0};
  if (period > 0) {
    its.it_value.tv_sec = (time_t)period;
    its.it_value.tv_nsec = (long)((period - (time_t)period) * 1e9);
    its.it_interval = its.it_value;
  }
  timerfd_settime(S.timer_fd, 0, &its, NULL);
}

// ---- wayland objects --------------------------------------------------------------------------------------

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *ls, uint32_t serial, uint32_t w, uint32_t h) {
  Output *o = data;
  zwlr_layer_surface_v1_ack_configure(ls, serial);
  if ((int)w != o->w || (int)h != o->h) {
    o->w = (int)w;
    o->h = (int)h;
    output_free_buffers(o);
    o->mapped = false;
    o->shown = (Rect){0, 0, 0, 0};
  }
  if (!o->configured) wl_surface_commit(o->surface);
  o->configured = true;
}
static void layer_closed(void *data, struct zwlr_layer_surface_v1 *ls) {
  (void)ls;
  ((Output *)data)->configured = false;
}
static const struct zwlr_layer_surface_v1_listener layer_listener = {.configure = layer_configure, .closed = layer_closed};

static void xo_pos(void *data, struct zxdg_output_v1 *x, int32_t px, int32_t py) {
  (void)x;
  Output *o = data;
  o->x = px;
  o->y = py;
}
static void xo_size(void *d, struct zxdg_output_v1 *x, int32_t w, int32_t h) { (void)d; (void)x; (void)w; (void)h; }
static void xo_done(void *d, struct zxdg_output_v1 *x) { (void)d; (void)x; }
static void xo_name(void *d, struct zxdg_output_v1 *x, const char *n) { (void)d; (void)x; (void)n; }
static void xo_desc(void *d, struct zxdg_output_v1 *x, const char *n) { (void)d; (void)x; (void)n; }
static const struct zxdg_output_v1_listener xdg_listener = {
    .logical_position = xo_pos, .logical_size = xo_size, .done = xo_done, .name = xo_name, .description = xo_desc};

static void wo_geometry(void *d, struct wl_output *w, int32_t x, int32_t y, int32_t pw, int32_t ph, int32_t sp, const char *mk,
                        const char *md, int32_t tr) {
  (void)d; (void)w; (void)x; (void)y; (void)pw; (void)ph; (void)sp; (void)mk; (void)md; (void)tr;
}
static void wo_mode(void *d, struct wl_output *w, uint32_t f, int32_t a, int32_t b, int32_t r) {
  (void)d; (void)w; (void)f; (void)a; (void)b; (void)r;
}
static void wo_done(void *d, struct wl_output *w) { (void)d; (void)w; }
static void wo_scale(void *data, struct wl_output *w, int32_t s) {
  (void)w;
  Output *o = data;
  if (s < 1) s = 1;
  if (s != o->scale) {
    o->scale = s;
    output_free_buffers(o);
    o->mapped = false;
  }
}
static const struct wl_output_listener output_listener = {.geometry = wo_geometry, .mode = wo_mode, .done = wo_done, .scale = wo_scale};

static void output_init(Output *o) {
  if (S.xdg_manager) {
    o->xdg = zxdg_output_manager_v1_get_xdg_output(S.xdg_manager, o->wl);
    zxdg_output_v1_add_listener(o->xdg, &xdg_listener, o);
  }
  o->surface = wl_compositor_create_surface(S.compositor);
  struct wl_region *empty = wl_compositor_create_region(S.compositor);
  wl_surface_set_input_region(o->surface, empty);
  wl_region_destroy(empty);
  o->layer = zwlr_layer_shell_v1_get_layer_surface(S.layer_shell, o->surface, o->wl, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "terminus-pointer-fx");
  zwlr_layer_surface_v1_add_listener(o->layer, &layer_listener, o);
  zwlr_layer_surface_v1_set_anchor(o->layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                                 ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
  zwlr_layer_surface_v1_set_exclusive_zone(o->layer, -1);
  zwlr_layer_surface_v1_set_keyboard_interactivity(o->layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
  wl_surface_commit(o->surface);
}

static void output_destroy(Output *o) {
  output_free_buffers(o);
  if (o->layer) zwlr_layer_surface_v1_destroy(o->layer);
  if (o->surface) wl_surface_destroy(o->surface);
  if (o->xdg) zxdg_output_v1_destroy(o->xdg);
  wl_output_destroy(o->wl);
  free(o);
}

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
  (void)d;
  if (!strcmp(iface, wl_compositor_interface.name)) S.compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
  else if (!strcmp(iface, wl_shm_interface.name)) S.shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
  else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name)) S.layer_shell = wl_registry_bind(r, name, &zwlr_layer_shell_v1_interface, 1);
  else if (!strcmp(iface, zxdg_output_manager_v1_interface.name)) S.xdg_manager = wl_registry_bind(r, name, &zxdg_output_manager_v1_interface, 1);
  else if (!strcmp(iface, wl_output_interface.name)) {
    Output *o = calloc(1, sizeof *o);
    o->global = name;
    o->scale = 1;
    o->wl = wl_registry_bind(r, name, &wl_output_interface, ver < 2 ? ver : 2);
    wl_output_add_listener(o->wl, &output_listener, o);
    o->next = S.outputs;
    S.outputs = o;
    if (S.ready) output_init(o);
  }
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) {
  (void)d; (void)r;
  for (Output **pp = &S.outputs; *pp; pp = &(*pp)->next)
    if ((*pp)->global == name) {
      Output *o = *pp;
      *pp = o->next;
      output_destroy(o);
      return;
    }
}
static const struct wl_registry_listener registry_listener = {.global = reg_global, .global_remove = reg_remove};

// ---- startup helpers --------------------------------------------------------------------------------------

static void on_signal(int s) { (void)s; quit_flag = 1; }

static bool parse_color(const char *s, double rgb[3]) {
  if (*s == '#') s++;
  if (strlen(s) != 6) return false;
  char *end;
  unsigned long v = strtoul(s, &end, 16);
  if (*end) return false;
  rgb[0] = ((v >> 16) & 0xff) / 255.0;
  rgb[1] = ((v >> 8) & 0xff) / 255.0;
  rgb[2] = (v & 0xff) / 255.0;
  return true;
}

static void points_push(Pt p) {
  if (S.n_points == MAX_POINTS) {
    memmove(S.points, S.points + 1, (MAX_POINTS - 1) * sizeof(Pt));
    S.n_points--;
  }
  S.points[S.n_points++] = p;
}

// Adds a copy when the pointer has left the last one by MIRROR_STEP; true when the screen needs a redraw.
static bool mirror_sample(double now) {
  refresh_cursor(now);
  double x, y;
  if (!cursor_pos(&x, &y)) return false;
  static double mx = -1e9, my = -1e9;
  if (hypot(x - mx, y - my) >= MOVE_EPSILON_PX) S.last_move = now;
  mx = x;
  my = y;
  const Pt *last = S.n_points ? &S.points[S.n_points - 1] : NULL;
  if (last && hypot(x - last->x, y - last->y) < MIRROR_STEP) return false;
  points_push((Pt){x, y, now, cur_set});
  return true;
}

static void trail_sample(double now) {
  double x, y;
  if (!cursor_pos(&x, &y)) return;
  const Pt *last = S.n_points ? &S.points[S.n_points - 1] : NULL;
  if (last && hypot(x - last->x, y - last->y) < MOVE_EPSILON_PX) return;
  S.last_move = now;
  // A point after a pause starts a new trail instead of joining the old end to it; its seed point sits an instant
  // earlier so the first segment has a direction.
  if (!last || now - last->t > TRAIL_PAUSE_S) {
    S.n_points = 0;
    points_push((Pt){x, y, now - TRAIL_SEED_LEAD_S, 0});
  }
  points_push((Pt){x, y, now, 0});
}

// ---- inputs -----------------------------------------------------------------------------------------------

static bool parse_args(int argc, char **argv) {
  bool known_mode = argc >= 2 && (!strcmp(argv[1], "ripple") || !strcmp(argv[1], "trail") || !strcmp(argv[1], "mirror"));
  if (!known_mode) {
    fprintf(stderr, "usage: pointer-fx ripple|trail|mirror [--color #rrggbb]\n");
    return false;
  }
  S.mode = !strcmp(argv[1], "ripple") ? MODE_RIPPLE : !strcmp(argv[1], "mirror") ? MODE_MIRROR : MODE_TRAIL;
  parse_color("#ffcccd", S.rgb);
  for (int i = 2; i < argc; i++) {
    if (!strcmp(argv[i], "--color") && i + 1 < argc) {
      if (!parse_color(argv[++i], S.rgb)) {
        fprintf(stderr, "pointer-fx: bad colour %s\n", argv[i]);
        return false;
      }
    } else {
      fprintf(stderr, "pointer-fx: unknown argument %s\n", argv[i]);
      return false;
    }
  }
  return true;
}

static bool connect_wayland(void) {
  S.display = wl_display_connect(NULL);
  if (!S.display) {
    fprintf(stderr, "pointer-fx: cannot connect to the Wayland compositor\n");
    return false;
  }
  S.registry = wl_display_get_registry(S.display);
  wl_registry_add_listener(S.registry, &registry_listener, NULL);
  wl_display_roundtrip(S.display);
  if (!S.compositor || !S.shm || !S.layer_shell) {
    fprintf(stderr, "pointer-fx: compositor lacks wl_compositor, wl_shm or wlr-layer-shell\n");
    return false;
  }
  for (Output *o = S.outputs; o; o = o->next) output_init(o);
  S.ready = true;
  wl_display_roundtrip(S.display);
  wl_display_roundtrip(S.display);
  return true;
}

// Ripple mode: the FIFO the click binds write to and the compositor's event socket (for `configreloaded`).
static bool open_click_inputs(void) {
  const char *run = getenv("XDG_RUNTIME_DIR");
  snprintf(S.fifo_path, sizeof S.fifo_path, "%s/terminus-pointer-fx-%d.fifo", run ? run : "/tmp", getpid());
  unlink(S.fifo_path);
  if (mkfifo(S.fifo_path, 0600) < 0 || (S.fifo_fd = open(S.fifo_path, O_RDWR | O_NONBLOCK | O_CLOEXEC)) < 0) {
    perror("pointer-fx: fifo");
    return false;
  }
  S.events_fd = ipc_connect(".socket2.sock");
  if (S.events_fd < 0) {
    fprintf(stderr, "pointer-fx: cannot reach the compositor event socket\n");
    return false;
  }
  register_binds();
  return true;
}

static void on_click(double now) {
  char drained[64];
  double x, y;
  if (read(S.fifo_fd, drained, sizeof drained) <= 0 || !cursor_pos(&x, &y)) return;
  if (S.n_clicks == MAX_CLICKS) memmove(S.clicks, S.clicks + 1, (MAX_CLICKS - 1) * sizeof(Pt));
  else S.n_clicks++;
  S.clicks[S.n_clicks - 1] = (Pt){x, y, now, 0};
  render(now);
}

// False when the event socket closed (the compositor is gone).
static bool on_compositor_event(void) {
  char text[4096];
  ssize_t n = read(S.events_fd, text, sizeof text - 1);
  if (n <= 0) {
    fprintf(stderr, "pointer-fx: compositor event socket closed\n");
    return false;
  }
  text[n] = 0;
  if (strstr(text, "configreloaded")) register_binds();
  return true;
}

static void on_tick(double now) {
  uint64_t ticks;
  if (read(S.timer_fd, &ticks, sizeof ticks) < 0) ticks = 0;  // nothing pending: a spurious wake-up
  if (S.mode == MODE_TRAIL) trail_sample(now);
  bool added = S.mode == MODE_MIRROR && mirror_sample(now);
  if (animating() || added || S.dirty_retry) render(now);
}

// Poll fast while the pointer moves, slower after it stops; click ripples only need frames while they animate.
static double next_timer_period(double now) {
  if (S.mode == MODE_RIPPLE) return animating() ? RIPPLE_FRAME_S : 0;
  double still_s = now - S.last_move;
  if (animating() || still_s < POLL_FAST_WINDOW_S) return POLL_FAST_S;
  return still_s < POLL_SLOW_WINDOW_S ? POLL_SLOW_S : POLL_IDLE_S;
}

// ---- main loop --------------------------------------------------------------------------------------------

// Returns the exit code.
static int run(void) {
  enum { FD_DISPLAY, FD_TIMER, FD_FIFO, FD_EVENTS, FD_COUNT };
  while (!quit_flag) {
    while (wl_display_prepare_read(S.display) != 0) wl_display_dispatch_pending(S.display);
    wl_display_flush(S.display);
    struct pollfd fds[FD_COUNT] = {{wl_display_get_fd(S.display), POLLIN, 0},
                                   {S.timer_fd, POLLIN, 0},
                                   {S.fifo_fd, POLLIN, 0},
                                   {S.events_fd, POLLIN, 0}};
    int ready = poll(fds, FD_COUNT, -1);
    if (ready < 0 && errno != EINTR) {
      wl_display_cancel_read(S.display);
      return 1;
    }
    if (ready > 0 && (fds[FD_DISPLAY].revents & POLLIN)) {
      if (wl_display_read_events(S.display) < 0) return 0;
    } else {
      wl_display_cancel_read(S.display);
    }
    if (wl_display_dispatch_pending(S.display) < 0) return 1;
    if (ready <= 0) continue;
    if (fds[FD_DISPLAY].revents & (POLLERR | POLLHUP)) return 1;

    double now = now_s();
    if (S.fifo_fd >= 0 && (fds[FD_FIFO].revents & POLLIN)) on_click(now);
    if (S.events_fd >= 0 && (fds[FD_EVENTS].revents & (POLLIN | POLLHUP)) && !on_compositor_event()) return 1;
    if (fds[FD_TIMER].revents & POLLIN) on_tick(now);
    set_timer(next_timer_period(now));
  }
  return 0;
}

static void shutdown_overlay(void) {
  if (S.mode == MODE_RIPPLE) {
    if (S.events_fd >= 0) unregister_binds();  // binds exist only once the event socket was reached
    unlink(S.fifo_path);
  }
  while (S.outputs) {
    Output *o = S.outputs;
    S.outputs = o->next;
    output_destroy(o);
  }
  if (S.display) wl_display_disconnect(S.display);
  const int fds[] = {S.timer_fd, S.fifo_fd, S.events_fd};
  for (size_t i = 0; i < sizeof fds / sizeof *fds; i++)
    if (fds[i] >= 0) close(fds[i]);
}

int main(int argc, char **argv) {
  if (!parse_args(argc, argv)) return 2;
  S.timer_fd = S.fifo_fd = S.events_fd = -1;
  if (!connect_wayland()) return 1;

  struct sigaction sa = {.sa_handler = on_signal};
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGHUP, &sa, NULL);

  S.timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
  if (S.mode == MODE_RIPPLE) {
    if (!open_click_inputs()) {
      shutdown_overlay();
      return 1;
    }
  } else {
    set_timer(POLL_SLOW_S);
  }

  int exit_code = run();
  shutdown_overlay();
  return exit_code;
}
