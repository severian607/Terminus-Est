// pointer-gl: GPU pointer trails (fire, lightning, plasma) as a click-through layer-shell overlay.
//
//   pointer-gl fire|lightning|plasma [--color #rrggbb]
//
// One transparent EGL surface per output. The pointer position comes from the compositor's IPC socket
// (`j/cursorpos`); the last moments of its path are resampled into at most MAX_PTS points and handed to a fragment
// shader, which draws the effect around the path. Only a rectangle around the trail is shaded (GL scissor), and an
// output that has nothing to show gives its buffer back, so an idle overlay costs one cheap poll.

#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-egl.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

#define MAX_PTS 320  // most points a trail can have on the CPU side
#define CHUNK 48    // points per draw: the shader array holds 64, and each chunk is shaded only inside its own box
#define MAX_RAW 4096

typedef enum { FX_FIRE = 0, FX_LIGHTNING = 1, FX_PLASMA = 2 } Fx;

typedef struct { double x, y, t, s; } Pt;  // s: distance the pointer has travelled in total when it was here

typedef struct Output {
  struct wl_output *wl;
  uint32_t global;
  struct zxdg_output_v1 *xdg;
  struct wl_surface *surface;
  struct zwlr_layer_surface_v1 *layer;
  struct wl_egl_window *win;
  EGLSurface egl;
  int x, y, w, h;  // logical position and size
  int scale;
  bool configured;
  int refresh_mhz;          // current mode's refresh rate, mHz
  int dmg[4];               // last drawn box in buffer pixels (x, y, w, h, GL origin); w == 0: nothing
  struct Output *next;
} Output;

static struct {
  Fx fx;
  double rgb[3];
  int maxpts;   // points handed to the shader
  double life;  // seconds a point stays in the trail
  double pad;   // logical pixels the effect reaches beyond the path
  struct wl_display *display;
  struct wl_registry *registry;
  struct wl_compositor *compositor;
  struct zwlr_layer_shell_v1 *layer_shell;
  struct zxdg_output_manager_v1 *xdg_manager;
  Output *outputs;
  double frame;  // seconds per frame: one frame per display refresh is all anyone can see
  bool ready;
  EGLDisplay edpy;
  EGLConfig ecfg;
  EGLContext ectx;
  PFNEGLSWAPBUFFERSWITHDAMAGEKHRPROC swap_damage;
  GLuint prog;
  GLint u_res, u_scale, u_time, u_n, u_pts, u_color, u_life, u_mode, u_tail, u_vis;
  Pt raw[MAX_RAW];
  double travel;  // total pointer travel so far; patterns are pinned to it so they stay put as the pointer moves
  double lx, ly;  // last sampled position
  int n_raw;
  double last_move;
  int timer_fd;
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

static bool cursor_pos(double *x, double *y) {
  int fd = ipc_connect(".socket.sock");
  if (fd < 0) return false;
  const char *req = "j/cursorpos";
  char buf[128];
  size_t n = 0;
  bool ok = write(fd, req, strlen(req)) == (ssize_t)strlen(req);
  while (ok && n + 1 < sizeof buf) {
    ssize_t r = read(fd, buf + n, sizeof buf - 1 - n);
    if (r <= 0) break;
    n += (size_t)r;
  }
  buf[n] = 0;
  close(fd);
  const char *px = strstr(buf, "\"x\""), *py = strstr(buf, "\"y\"");
  if (!ok || !px || !py || !(px = strchr(px, ':')) || !(py = strchr(py, ':'))) return false;
  *x = strtod(px + 1, NULL);
  *y = strtod(py + 1, NULL);
  return true;
}

// ---- shaders ----------------------------------------------------------------------------------------------

static const char *VERT =
    "#version 300 es\n"
    "void main() {\n"
    "  vec2 v = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "  gl_Position = vec4(v * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char *FRAG =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform vec2 u_res;\n"     // buffer size in physical pixels
    "uniform float u_scale;\n"  // physical pixels per logical pixel
    "uniform float u_time;\n"
    "uniform int u_n;\n"
    "uniform vec4 u_pts[64];\n"  // x, y (output-local logical px), age (s), absolute path position (px)
    "uniform vec3 u_color;\n"
    "uniform float u_life;\n"
    "uniform float u_vis;\n"         // 0..1: the plasma ribbon fades away as it winds in to a stopped pointer
    "uniform float u_tail;\n"        // path position of the oldest point: the trail tapers in over its first pixels
    "uniform int u_mode;\n"
    "out vec4 o;\n"
    "\n"
    "float h21(vec2 p) { p = fract(p * vec2(123.34, 456.21)); p += dot(p, p + 45.32); return fract(p.x * p.y); }\n"
    "float vn(vec2 p) {\n"
    "  vec2 i = floor(p), f = fract(p); f = f * f * (3.0 - 2.0 * f);\n"
    "  return mix(mix(h21(i), h21(i + vec2(1, 0)), f.x), mix(h21(i + vec2(0, 1)), h21(i + vec2(1, 1)), f.x), f.y);\n"
    "}\n"
    "float fbm(vec2 p) { float s = 0.0, a = 0.5; for (int i = 0; i < 4; i++) { s += a * vn(p); p = p * 2.03 + 17.1; a *= 0.5; } return s; }\n"
    "\n"
    // Fire: every segment is a flame that rises and widens as it ages; heat maps to colour.
    "vec4 fire(vec2 p) {\n"
    "  vec2 w = vec2(fbm(p * 0.045 + vec2(0.0, u_time * 2.2)), fbm(p * 0.045 + vec2(7.3, u_time * 2.6))) - 0.5;\n"
    "  float acc = 0.0;\n"
    "  for (int i = 0; i < 63; i++) {\n"
    "    if (i >= u_n - 1) break;\n"
    "    vec4 a = u_pts[i], b = u_pts[i + 1];\n"
    "    float u = clamp((a.z + b.z) * 0.5 / u_life, 0.0, 1.0);\n"
    "    vec2 q = p + vec2(0.0, 6.0 + 80.0 * u) + w * (12.0 + 34.0 * u);\n"
    "    vec2 ba = b.xy - a.xy;\n"
    "    float h = clamp(dot(q - a.xy, ba) / max(dot(ba, ba), 1e-4), 0.0, 1.0);\n"
    "    float d = length(q - a.xy - ba * h);\n"
    "    float wd = mix(8.0, 20.0, u);\n"
    "    acc = max(acc, exp(-(d * d) / (wd * wd)) * (1.0 - u) * (1.0 - u));\n"
    "  }\n"
    "  float t = clamp(acc * 1.3, 0.0, 1.0);\n"
    "  vec3 c = t < 0.33 ? mix(vec3(0.28, 0.0, 0.0), vec3(0.95, 0.15, 0.02), t / 0.33)\n"
    "         : t < 0.66 ? mix(vec3(0.95, 0.15, 0.02), vec3(1.0, 0.65, 0.05), (t - 0.33) / 0.33)\n"
    "                    : mix(vec3(1.0, 0.65, 0.05), vec3(1.0, 0.96, 0.75), (t - 0.66) / 0.34);\n"
    "  float al = smoothstep(0.04, 0.35, t);\n"
    "  return vec4(c * al, al);\n"
    "}\n"
    "\n"
    // Nearest point on the polyline: x = distance, y = signed side, z = age fraction, w = arclength.
    "vec4 nearest(vec2 p) {\n"
    "  float best = 1e9; vec4 r = vec4(1e9, 0.0, 1.0, 0.0);\n"
    "  for (int i = 0; i < 63; i++) {\n"
    "    if (i >= u_n - 1) break;\n"
    "    vec4 a = u_pts[i], b = u_pts[i + 1];\n"
    "    vec2 ba = b.xy - a.xy;\n"
    "    float l2 = max(dot(ba, ba), 1e-4);\n"
    "    float h = clamp(dot(p - a.xy, ba) / l2, 0.0, 1.0);\n"
    "    vec2 d = p - a.xy - ba * h;\n"
    "    float dist = length(d);\n"
    "    if (dist < best) {\n"
    "      best = dist;\n"
    "      vec2 nrm = vec2(-ba.y, ba.x) * inversesqrt(l2);\n"
    "      r = vec4(dist, dot(d, nrm), mix(a.z, b.z, h) / u_life, mix(a.w, b.w, h));\n"
    "    }\n"
    "  }\n"
    "  return r;\n"
    "}\n"
    "\n"
    // Lightning: a jittering white-hot bolt that re-rolls many times a second, with a blue glow and pulses along it.
    // Every segment is drawn on its own (no nearest-segment switch), so bends and joints stay smooth.
    "vec4 lightning(vec2 p) {\n"
    "  float core = 0.0, glow = 0.0;\n"
    "  float t = floor(u_time * 24.0);\n"                      // the bolt re-rolls 24 times a second, like a real arc
    "  for (int i = 0; i < 63; i++) {\n"
    "    if (i >= u_n - 1) break;\n"
    "    vec4 a = u_pts[i], b = u_pts[i + 1];\n"
    "    vec2 ba = b.xy - a.xy;\n"
    "    float l2 = dot(ba, ba);\n"
    "    if (l2 < 0.25) continue;\n"                                          // a zero-length segment has no direction
    "    float h = clamp(dot(p - a.xy, ba) / l2, 0.0, 1.0);\n"
    "    vec2 d = p - a.xy - ba * h;\n"
    "    float dist = length(d);\n"
    "    if (dist > 70.0) continue;\n"
    "    float u = clamp(mix(a.z, b.z, h) / u_life, 0.0, 1.0);\n"
    "    float fade = pow(1.0 - u, 1.2);\n"
    "    float len = sqrt(l2), along = dot(p - a.xy, ba) / len;\n"
    "    float over = max(max(-along, along - len), 0.0);\n"
    "    float cap = exp(-over * over / 40.0), taper = smoothstep(0.0, 45.0, mix(a.w, b.w, h) - u_tail);\n"
    "    float side = dot(d, vec2(-ba.y, ba.x) * inversesqrt(l2));\n"
    "    float s = mix(a.w, b.w, h);\n"
    "    float amp = 14.0 * (0.5 + u);\n"
    "    float j1 = (fbm(vec2(s * 0.03, t * 1.7)) - 0.5) * 2.0 * amp + (vn(vec2(s * 0.2, t)) - 0.5) * 5.0;\n"
    "    float j2 = (fbm(vec2(s * 0.045 + 40.0, t * 2.3)) - 0.5) * 2.0 * amp + (vn(vec2(s * 0.25, t + 9.0)) - 0.5) * 4.0;\n"
    "    float j3 = (fbm(vec2(s * 0.06 + 90.0, t * 3.1)) - 0.5) * 2.0 * amp * 1.4;\n"
    "    float d1 = abs(side - j1), d2 = abs(side - j2), d3 = abs(side - j3);\n"
    "    float flow = 0.7 + 0.3 * sin(s * 0.06 - u_time * 32.0);\n"
    "    float c = (exp(-d1 * d1 / 7.0) + 0.7 * exp(-d2 * d2 / 4.5) + 0.35 * exp(-d3 * d3 / 3.0)) * flow;\n"
    "    float dm = min(min(d1, d2), d3);\n"
    "    float g = (exp(-dm * dm / 120.0) * 0.55 + exp(-dist * dist / 900.0) * 0.35) * flow;\n"
    "    core = max(core, c * fade * cap * taper);\n"
    "    glow = max(glow, g * fade * cap * taper);\n"
    "  }\n"
    "  vec3 c = (vec3(1.0, 0.98, 1.0) * core + vec3(0.3, 0.5, 1.0) * glow) * 1.2;\n"
    "  float al = clamp(max(c.r, max(c.g, c.b)), 0.0, 1.0);\n"
    "  return vec4(c, al);\n"
    "}\n"
    "\n"
    // Plasma: two intertwined strands in the accent colour with energy flowing along them. Patterns are pinned to the
    // path (not to the pointer) and slow enough not to strobe; brightness is tone-mapped so nothing clips to white.
    "vec4 plasma(vec2 p) {\n"
    "  float acc = 0.0, glow = 0.0;\n"
    "  for (int i = 0; i < 63; i++) {\n"
    "    if (i >= u_n - 1) break;\n"
    "    vec4 a = u_pts[i], b = u_pts[i + 1];\n"
    "    vec2 ba = b.xy - a.xy;\n"
    "    float l2 = dot(ba, ba);\n"
    "    if (l2 < 0.25) continue;\n"                                          // a zero-length segment has no direction
    "    float h = clamp(dot(p - a.xy, ba) / l2, 0.0, 1.0);\n"
    "    vec2 d = p - a.xy - ba * h;\n"
    "    float dist = length(d);\n"
    "    if (dist > 60.0) continue;\n"
    "    float fade = pow(clamp(1.0 - mix(a.z, b.z, h) / u_life, 0.0, 1.0), 0.6);\n"
    "    float side = dot(d, vec2(-ba.y, ba.x) * inversesqrt(l2));\n"
    "    float len = sqrt(l2), along = dot(p - a.xy, ba) / len;\n"
    "    float over = max(max(-along, along - len), 0.0);\n"                 // past either end of the segment
    "    float cap = exp(-over * over / 40.0), taper = smoothstep(0.0, 45.0, mix(a.w, b.w, h) - u_tail);\n"
    "    float s = mix(a.w, b.w, h);\n"
    "    float wave = 7.0 * sin(s * 0.07 - u_time * 6.0);\n"
    "    float s1 = exp(-(side - wave) * (side - wave) / 5.0), s2 = exp(-(side + wave) * (side + wave) / 5.0);\n"
    "    float flow = 0.75 + 0.25 * sin(s * 0.045 - u_time * 7.0);\n"
    "    acc = max(acc, (max(s1, s2) + 0.3 * min(s1, s2)) * flow * fade * cap * taper);\n"
    "    glow = max(glow, exp(-dist * dist / 220.0) * 0.38 * fade * cap * taper);\n"
    "  }\n"                                                          // each segment is judged on its own, so joints never jump
    "  float e = 1.0 - exp(-2.0 * (acc + glow));\n"
    "  e *= u_vis;\n"
    "  vec3 c = mix(u_color, vec3(1.0), 0.15 * smoothstep(0.55, 1.0, acc)) * e;\n"
    "  return vec4(c, clamp(e, 0.0, 1.0));\n"
    "}\n"
    "\n"
    "void main() {\n"
    "  vec2 p = vec2(gl_FragCoord.x, u_res.y - gl_FragCoord.y) / u_scale;\n"
    "  o = u_mode == 0 ? fire(p) : u_mode == 1 ? lightning(p) : plasma(p);\n"
    "}\n";

static GLuint compile(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[2048];
    glGetShaderInfoLog(s, sizeof log, NULL, log);
    fprintf(stderr, "pointer-gl: shader compile failed:\n%s\n", log);
    return 0;
  }
  return s;
}

static bool build_program(void) {
  GLuint vs = compile(GL_VERTEX_SHADER, VERT), fs = compile(GL_FRAGMENT_SHADER, FRAG);
  if (!vs || !fs) return false;
  S.prog = glCreateProgram();
  glAttachShader(S.prog, vs);
  glAttachShader(S.prog, fs);
  glLinkProgram(S.prog);
  GLint ok = 0;
  glGetProgramiv(S.prog, GL_LINK_STATUS, &ok);
  if (!ok) {
    fprintf(stderr, "pointer-gl: shader link failed\n");
    return false;
  }
  S.u_res = glGetUniformLocation(S.prog, "u_res");
  S.u_scale = glGetUniformLocation(S.prog, "u_scale");
  S.u_time = glGetUniformLocation(S.prog, "u_time");
  S.u_n = glGetUniformLocation(S.prog, "u_n");
  S.u_pts = glGetUniformLocation(S.prog, "u_pts");
  S.u_color = glGetUniformLocation(S.prog, "u_color");
  S.u_life = glGetUniformLocation(S.prog, "u_life");
  S.u_mode = glGetUniformLocation(S.prog, "u_mode");
  S.u_tail = glGetUniformLocation(S.prog, "u_tail");
  S.u_vis = glGetUniformLocation(S.prog, "u_vis");
  return true;
}

// ---- EGL --------------------------------------------------------------------------------------------------

static bool egl_init(void) {
  PFNEGLGETPLATFORMDISPLAYEXTPROC get_pd = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
  S.edpy = get_pd ? get_pd(EGL_PLATFORM_WAYLAND_KHR, S.display, NULL) : eglGetDisplay((EGLNativeDisplayType)S.display);
  if (S.edpy == EGL_NO_DISPLAY || !eglInitialize(S.edpy, NULL, NULL)) return false;
  eglBindAPI(EGL_OPENGL_ES_API);
  const EGLint cfg[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8,
                        EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
  EGLint n = 0;
  if (!eglChooseConfig(S.edpy, cfg, &S.ecfg, 1, &n) || n < 1) return false;
  S.swap_damage = (PFNEGLSWAPBUFFERSWITHDAMAGEKHRPROC)eglGetProcAddress("eglSwapBuffersWithDamageKHR");
  if (!S.swap_damage) S.swap_damage = (PFNEGLSWAPBUFFERSWITHDAMAGEKHRPROC)eglGetProcAddress("eglSwapBuffersWithDamageEXT");
  const EGLint ctx[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  S.ectx = eglCreateContext(S.edpy, S.ecfg, EGL_NO_CONTEXT, ctx);
  return S.ectx != EGL_NO_CONTEXT;
}

static bool output_show(Output *o) {
  if (o->egl != EGL_NO_SURFACE) return true;
  if (!o->configured || o->w <= 0 || o->h <= 0) return false;
  o->win = wl_egl_window_create(o->surface, o->w * o->scale, o->h * o->scale);
  if (!o->win) return false;
  o->egl = eglCreateWindowSurface(S.edpy, S.ecfg, (EGLNativeWindowType)o->win, NULL);
  if (o->egl == EGL_NO_SURFACE) {
    wl_egl_window_destroy(o->win);
    o->win = NULL;
    return false;
  }
  wl_surface_set_buffer_scale(o->surface, o->scale);
  return true;
}

static void output_hide(Output *o) {
  if (o->egl == EGL_NO_SURFACE) return;
  eglMakeCurrent(S.edpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroySurface(S.edpy, o->egl);
  o->egl = EGL_NO_SURFACE;
  o->dmg[2] = 0;
  wl_egl_window_destroy(o->win);
  o->win = NULL;
  // A null buffer unmaps a layer surface; it maps again only through a fresh configure, so wait for one.
  wl_surface_attach(o->surface, NULL, 0, 0);
  wl_surface_commit(o->surface);
  o->configured = false;
  wl_surface_commit(o->surface);
}

// ---- trail ------------------------------------------------------------------------------------------------

static void expire(double now) {
  int drop = 0;
  while (drop < S.n_raw && now - S.raw[drop].t > S.life) drop++;
  if (drop) {
    memmove(S.raw, S.raw + drop, (size_t)(S.n_raw - drop) * sizeof(Pt));
    S.n_raw -= drop;
  }
  if (S.n_raw == 1) S.n_raw = 0;  // one point has no segment
}

static void sample(double now) {
  double x, y;
  if (!cursor_pos(&x, &y)) return;
  Pt *last = S.n_raw ? &S.raw[S.n_raw - 1] : NULL;
  if (last && hypot(x - last->x, y - last->y) < 0.5) return;
  S.last_move = now;
  S.travel += hypot(x - S.lx, y - S.ly);
  S.lx = x;
  S.ly = y;
  if (S.n_raw == MAX_RAW) {
    memmove(S.raw, S.raw + 1, (MAX_RAW - 1) * sizeof(Pt));
    S.n_raw--;
  }
  if (last && now - last->t > 0.15) S.n_raw = 0;  // a pause starts a new trail
  if (S.n_raw == 0) S.raw[S.n_raw++] = (Pt){x, y, now - 1e-3, S.travel};
  S.raw[S.n_raw++] = (Pt){x, y, now, S.travel};
}

// At most MAX_PTS points, oldest first, always ending with the newest: x, y, age, absolute path position.
static int resample(double now, float out[MAX_PTS * 4]) {
  int n = S.n_raw;
  if (n < 2) return 0;
  int stride = (n - 1 + S.maxpts - 2) / (S.maxpts - 1);
  if (stride < 1) stride = 1;
  int idx[MAX_PTS], m = 0;
  for (int i = n - 1; i >= 0 && m < S.maxpts; i -= stride) idx[m++] = i;  // newest first
  float tmp[MAX_PTS][4];
  for (int k = 0; k < m; k++) {
    Pt *p = &S.raw[idx[k]];
    tmp[k][0] = (float)p->x;
    tmp[k][1] = (float)p->y;
    tmp[k][2] = (float)(now - p->t);
    tmp[k][3] = (float)fmod(p->s, 100000.0);
  }
  for (int k = 0; k < m; k++) memcpy(&out[k * 4], tmp[m - 1 - k], sizeof tmp[0]);  // oldest first
  return m;
}

// ---- plasma ribbon ----------------------------------------------------------------------------------------
//
// Plasma is a bright head that always circles a centre, trailing a ribbon of its own recent path. The centre
// follows the pointer a little behind it, so while the pointer moves the circling head sweeps a coil along the
// pointer's route; when the pointer rests, the centre settles on it and the head goes round it in a circle,
// the ribbon shortening to about one lap. The path is recorded as the head moves and never shifts afterwards;
// the ribbon only reveals more or less of it.

#define CHAIN_MIN 64       // ribbon nodes when curled; the look of the weave depends on node spacing (3.6 px curled, 9 px stretched)
#define CHAIN_MAX 300      // ribbon nodes at full length
#define CHAIN_STEP 14.0    // pattern position (px) per node: the weave is pinned to the nodes, not to the path
#define CENTER_RATE 9.0    // 1/s: how fast the circle's centre closes on the pointer; smaller lags more
#define RING_RADIUS 38.0   // logical px
#define RING_SPEED 2.6     // rad/s
#define RING_AFTER 0.5     // s without pointer movement before the ribbon settles to one lap
#define LEN_MOVING 2700.0  // px of path in the ribbon while the pointer moves
#define LEN_IDLE 230.0     // about one lap of the circle
#define PATH_MAX 8000
#define PATH_STEP 1.5      // px the head must travel before a path point is recorded

typedef struct { double x, y, s; } PathPt;  // s: distance along the path

static PathPt path[PATH_MAX];
static int n_path;
static bool chain_ready;
static double chain_t, chain_len, chain_w, chain_phase, centre_x, centre_y, tail_s, head_s;

// The point at distance s along the recorded path.
static void path_at(double s, double *x, double *y) {
  int lo = 0, hi = n_path - 1;
  if (s <= path[0].s) { *x = path[0].x; *y = path[0].y; return; }
  if (s >= path[hi].s) { *x = path[hi].x; *y = path[hi].y; return; }
  while (hi - lo > 1) {
    int mid = (lo + hi) / 2;
    if (path[mid].s <= s) lo = mid; else hi = mid;
  }
  double f = (s - path[lo].s) / fmax(path[hi].s - path[lo].s, 1e-6);
  *x = path[lo].x + (path[hi].x - path[lo].x) * f;
  *y = path[lo].y + (path[hi].y - path[lo].y) * f;
}

static void chain_step(double now) {
  static double cx, cy;
  static int tick;
  // While the circle rests, ask the compositor for the pointer only every 4th frame; the circle still turns every frame.
  if (!(chain_ready && chain_w > 0.95 && (++tick & 3))) {
    if (!cursor_pos(&cx, &cy)) return;
  }
  double x = cx, y = cy;
  double dt = chain_ready ? now - chain_t : 0.0;
  if (dt > 0.1) dt = 0.1;
  chain_t = now;
  static double px, py;
  if (!chain_ready) {
    centre_x = px = x;
    centre_y = py = y;
    path[0] = (PathPt){x + RING_RADIUS, y, 0};
    n_path = 1;
    S.last_move = now;
    chain_ready = true;
  }
  if (hypot(x - px, y - py) >= 0.5) S.last_move = now;
  px = x;
  py = y;

  double target = now - S.last_move > RING_AFTER ? 1.0 : 0.0;  // 1: the pointer rests
  chain_w += (target - chain_w) * (1.0 - exp(-dt * (target > 0.5 ? 1.0 : 5.0)));  // the tail lingers before it winds in
  chain_phase += dt * RING_SPEED;
  double c = 1.0 - exp(-dt * CENTER_RATE);
  centre_x += (x - centre_x) * c;
  centre_y += (y - centre_y) * c;

  // The head circles the centre; its path is what the ribbon is made of.
  double hx = centre_x + RING_RADIUS * cos(chain_phase), hy = centre_y + RING_RADIUS * sin(chain_phase);
  PathPt *last = &path[n_path - 1];
  double moved = hypot(hx - last->x, hy - last->y);
  if (moved >= PATH_STEP) {
    if (n_path == PATH_MAX) {
      memmove(path, path + 1, (PATH_MAX - 1) * sizeof(PathPt));
      n_path--;
    }
    path[n_path] = (PathPt){hx, hy, last->s + moved};
    n_path++;
  }
  head_s = path[n_path - 1].s;
  chain_len = LEN_MOVING + (LEN_IDLE - LEN_MOVING) * chain_w;
  tail_s = head_s - chain_len;
  while (n_path > 2 && path[1].s < head_s - LEN_MOVING - 50.0) {  // history older than the longest ribbon
    memmove(path, path + 1, (size_t)(n_path - 1) * sizeof(PathPt));
    n_path--;
  }
}

// The ribbon as shader points, oldest (tail) first: x, y, age fraction x life, pattern position. The number of
// nodes follows the ribbon's length so the spacing, and with it the weave, looks the same short or long.
static int chain_points(float out[MAX_PTS * 4]) {
  if (!chain_ready) return 0;
  double f = (chain_len - LEN_IDLE) / (LEN_MOVING - LEN_IDLE);
  int n = CHAIN_MIN + (int)lround((CHAIN_MAX - CHAIN_MIN) * fmin(1.0, fmax(0.0, f)));
  for (int j = 0; j < n; j++) {
    int i = n - 1 - j;  // node 0 is the head
    double s = tail_s + chain_len * j / (n - 1.0), x, y;
    path_at(s, &x, &y);
    out[j * 4] = (float)x;
    out[j * 4 + 1] = (float)y;
    out[j * 4 + 2] = (float)(S.life * i / (n - 1.0));
    out[j * 4 + 3] = (float)(j * CHAIN_STEP);
  }
  return n;
}

static void render(double now) {
  float pts[MAX_PTS * 4];
  int n;
  if (S.fx == FX_PLASMA) {
    n = chain_points(pts);
  } else {
    expire(now);
    n = resample(now, pts);
  }
  for (Output *o = S.outputs; o; o = o->next) {
    // Trail points are global; the shader works in output-local logical pixels.
    float local[MAX_PTS * 4];
    for (int i = 0; i < n; i++) {
      local[i * 4] = pts[i * 4] - o->x;
      local[i * 4 + 1] = pts[i * 4 + 1] - o->y;
      local[i * 4 + 2] = pts[i * 4 + 2];
      local[i * 4 + 3] = pts[i * 4 + 3];
    }
    // The trail is drawn in chunks of CHUNK points that share their end points; each chunk is shaded only inside
    // its own padded box, so a trail that crosses the screen costs what its length costs, not the whole screen.
    int start[MAX_PTS / (CHUNK - 1) + 2], count[MAX_PTS / (CHUNK - 1) + 2], nr = 0;
    double box[MAX_PTS / (CHUNK - 1) + 2][4];
    for (int s = 0; s + 1 < n; s += CHUNK - 1) {
      int c = n - s < CHUNK ? n - s : CHUNK;
      double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
      for (int i = s; i < s + c; i++) {
        if (local[i * 4] < x0) x0 = local[i * 4];
        if (local[i * 4] > x1) x1 = local[i * 4];
        if (local[i * 4 + 1] < y0) y0 = local[i * 4 + 1];
        if (local[i * 4 + 1] > y1) y1 = local[i * 4 + 1];
      }
      x0 -= S.pad;
      x1 += S.pad;
      y0 -= S.pad * 1.6;
      y1 += S.pad;
      if (x0 < 0) x0 = 0;
      if (y0 < 0) y0 = 0;
      if (x1 > o->w) x1 = o->w;
      if (y1 > o->h) y1 = o->h;
      if (x1 <= x0 || y1 <= y0) continue;
      start[nr] = s;
      count[nr] = c;
      box[nr][0] = x0, box[nr][1] = y0, box[nr][2] = x1, box[nr][3] = y1;
      nr++;
    }
    if (nr == 0) {
      output_hide(o);
      continue;
    }
    if (!output_show(o)) continue;
    eglMakeCurrent(S.edpy, o->egl, o->egl, S.ectx);
    eglSwapInterval(S.edpy, 0);
    if (!S.prog && !build_program()) {
      quit_flag = 1;
      return;
    }
    int pw = o->w * o->scale, ph = o->h * o->scale;
    glViewport(0, 0, pw, ph);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);  // chunks overlap at their edges; every effect already combines segments with max
    glBlendEquation(GL_MAX);
    glBlendFunc(GL_ONE, GL_ONE);
    glUseProgram(S.prog);
    glUniform2f(S.u_res, (float)pw, (float)ph);
    glUniform1f(S.u_scale, (float)o->scale);
    glUniform1f(S.u_time, (float)fmod(now, 1000.0));
    glUniform3f(S.u_color, (float)S.rgb[0], (float)S.rgb[1], (float)S.rgb[2]);
    glUniform1f(S.u_life, (float)S.life);
    glUniform1i(S.u_mode, (int)S.fx);
    glUniform1f(S.u_tail, local[3]);
    glUniform1f(S.u_vis, 1.0f);
    int ux0 = 1 << 30, uy0 = 1 << 30, ux1 = 0, uy1 = 0;
    for (int k = 0; k < nr; k++) {
      int sx = (int)floor(box[k][0] * o->scale), sy = (int)floor((o->h - box[k][3]) * o->scale);
      int sw = (int)ceil((box[k][2] - box[k][0]) * o->scale) + 1, sh = (int)ceil((box[k][3] - box[k][1]) * o->scale) + 1;
      if (sx < ux0) ux0 = sx;
      if (sy < uy0) uy0 = sy;
      if (sx + sw > ux1) ux1 = sx + sw;
      if (sy + sh > uy1) uy1 = sy + sh;
      glScissor((GLint)floor(box[k][0] * o->scale), (GLint)floor((o->h - box[k][3]) * o->scale),
                (GLsizei)ceil((box[k][2] - box[k][0]) * o->scale) + 1, (GLsizei)ceil((box[k][3] - box[k][1]) * o->scale) + 1);
      glUniform1i(S.u_n, count[k]);
      glUniform4fv(S.u_pts, count[k], &local[start[k] * 4]);
      glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    // Only what changed since the last frame needs compositing: where the trail was plus where it is now.
    if (S.swap_damage) {
      int ax = ux0, ay = uy0, bx = ux1, by = uy1;
      if (o->dmg[2] > 0) {
        if (o->dmg[0] < ax) ax = o->dmg[0];
        if (o->dmg[1] < ay) ay = o->dmg[1];
        if (o->dmg[0] + o->dmg[2] > bx) bx = o->dmg[0] + o->dmg[2];
        if (o->dmg[1] + o->dmg[3] > by) by = o->dmg[1] + o->dmg[3];
      } else {
        ax = 0, ay = 0, bx = pw, by = ph;  // first frame after being hidden: everything
      }
      if (ax < 0) ax = 0;
      if (ay < 0) ay = 0;
      if (bx > pw) bx = pw;
      if (by > ph) by = ph;
      EGLint rect[4] = {ax, ay, bx - ax, by - ay};
      S.swap_damage(S.edpy, o->egl, rect, 1);
    } else {
      eglSwapBuffers(S.edpy, o->egl);
    }
    o->dmg[0] = ux0, o->dmg[1] = uy0, o->dmg[2] = ux1 - ux0, o->dmg[3] = uy1 - uy0;
  }
  wl_display_flush(S.display);
}

static void set_timer(double period) {
  if (period == S.timer_period) return;
  S.timer_period = period;
  struct itimerspec its = {0};
  its.it_value.tv_sec = (time_t)period;
  its.it_value.tv_nsec = (long)((period - (time_t)period) * 1e9);
  its.it_interval = its.it_value;
  timerfd_settime(S.timer_fd, 0, &its, NULL);
}

// ---- wayland objects --------------------------------------------------------------------------------------

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *ls, uint32_t serial, uint32_t w, uint32_t h) {
  Output *o = data;
  zwlr_layer_surface_v1_ack_configure(ls, serial);
  if ((int)w != o->w || (int)h != o->h) {
    o->w = (int)w;
    o->h = (int)h;
    if (o->win) wl_egl_window_resize(o->win, o->w * o->scale, o->h * o->scale, 0, 0);
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
  (void)w; (void)a; (void)b;
  if (f & WL_OUTPUT_MODE_CURRENT) ((Output *)d)->refresh_mhz = r;
}
static void wo_done(void *d, struct wl_output *w) { (void)d; (void)w; }
static void wo_scale(void *data, struct wl_output *w, int32_t s) {
  (void)w;
  Output *o = data;
  if (s < 1) s = 1;
  if (s != o->scale) {
    o->scale = s;
    output_hide(o);
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
  o->layer = zwlr_layer_shell_v1_get_layer_surface(S.layer_shell, o->surface, o->wl, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "terminus-pointer-gl");
  zwlr_layer_surface_v1_add_listener(o->layer, &layer_listener, o);
  zwlr_layer_surface_v1_set_anchor(o->layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                                 ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
  zwlr_layer_surface_v1_set_exclusive_zone(o->layer, -1);
  zwlr_layer_surface_v1_set_keyboard_interactivity(o->layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
  wl_surface_commit(o->surface);
}

static void output_destroy(Output *o) {
  output_hide(o);
  if (o->layer) zwlr_layer_surface_v1_destroy(o->layer);
  if (o->surface) wl_surface_destroy(o->surface);
  if (o->xdg) zxdg_output_v1_destroy(o->xdg);
  wl_output_destroy(o->wl);
  free(o);
}

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
  (void)d;
  if (!strcmp(iface, wl_compositor_interface.name)) S.compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
  else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name)) S.layer_shell = wl_registry_bind(r, name, &zwlr_layer_shell_v1_interface, 1);
  else if (!strcmp(iface, zxdg_output_manager_v1_interface.name)) S.xdg_manager = wl_registry_bind(r, name, &zxdg_output_manager_v1_interface, 1);
  else if (!strcmp(iface, wl_output_interface.name)) {
    Output *o = calloc(1, sizeof *o);
    o->global = name;
    o->scale = 1;
    o->egl = EGL_NO_SURFACE;
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

// ---- main loop --------------------------------------------------------------------------------------------

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

int main(int argc, char **argv) {
  if (argc < 2 || (strcmp(argv[1], "fire") && strcmp(argv[1], "lightning") && strcmp(argv[1], "plasma"))) {
    fprintf(stderr, "usage: pointer-gl fire|lightning|plasma [--color #rrggbb]\n");
    return 2;
  }
  S.fx = !strcmp(argv[1], "fire") ? FX_FIRE : !strcmp(argv[1], "lightning") ? FX_LIGHTNING : FX_PLASMA;
  S.life = S.fx == FX_FIRE ? 1.4 : S.fx == FX_LIGHTNING ? 0.9 : 1.0;  // plasma: fade runs along the ribbon, not over time
  S.maxpts = S.fx == FX_PLASMA ? MAX_PTS : 48;
  S.pad = S.fx == FX_FIRE ? 70 : 90;
  parse_color("#ffcccd", S.rgb);
  for (int i = 2; i < argc; i++) {
    if (!strcmp(argv[i], "--color") && i + 1 < argc) {
      if (!parse_color(argv[++i], S.rgb)) {
        fprintf(stderr, "pointer-gl: bad colour %s\n", argv[i]);
        return 2;
      }
    } else {
      fprintf(stderr, "pointer-gl: unknown argument %s\n", argv[i]);
      return 2;
    }
  }

  S.display = wl_display_connect(NULL);
  if (!S.display) {
    fprintf(stderr, "pointer-gl: cannot connect to the Wayland compositor\n");
    return 1;
  }
  S.registry = wl_display_get_registry(S.display);
  wl_registry_add_listener(S.registry, &registry_listener, NULL);
  wl_display_roundtrip(S.display);
  if (!S.compositor || !S.layer_shell) {
    fprintf(stderr, "pointer-gl: compositor lacks wl_compositor or wlr-layer-shell\n");
    return 1;
  }
  if (!egl_init()) {
    fprintf(stderr, "pointer-gl: EGL with OpenGL ES 3 is unavailable\n");
    return 1;
  }
  for (Output *o = S.outputs; o; o = o->next) output_init(o);
  S.ready = true;
  wl_display_roundtrip(S.display);
  wl_display_roundtrip(S.display);

  struct sigaction sa = {.sa_handler = on_signal};
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGHUP, &sa, NULL);

  cursor_pos(&S.lx, &S.ly);
  {
    int mhz = 0;
    for (Output *o = S.outputs; o; o = o->next) if (o->refresh_mhz > mhz) mhz = o->refresh_mhz;
    S.frame = mhz > 20000 ? 1000.0 / mhz : 1.0 / 60.0;
    if (S.frame < 0.004) S.frame = 0.004;
  }
  S.timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
  set_timer(0.04);

  int exit_code = 0;
  bool still_moving = false;
  while (!quit_flag) {
    while (wl_display_prepare_read(S.display) != 0) wl_display_dispatch_pending(S.display);
    wl_display_flush(S.display);
    struct pollfd fds[2] = {{wl_display_get_fd(S.display), POLLIN, 0}, {S.timer_fd, POLLIN, 0}};
    int r = poll(fds, 2, -1);
    if (r < 0 && errno != EINTR) {
      wl_display_cancel_read(S.display);
      exit_code = 1;
      break;
    }
    if (r > 0 && (fds[0].revents & POLLIN)) {
      if (wl_display_read_events(S.display) < 0) break;
    } else {
      wl_display_cancel_read(S.display);
    }
    if (wl_display_dispatch_pending(S.display) < 0) {
      exit_code = 1;
      break;
    }
    if (r <= 0) continue;
    if (fds[0].revents & (POLLERR | POLLHUP)) {
      exit_code = 1;
      break;
    }
    double now = now_s();
    if (fds[1].revents & POLLIN) {
      uint64_t ticks;
      if (read(S.timer_fd, &ticks, sizeof ticks) < 0) ticks = 0;
      bool animating;
      if (S.fx == FX_PLASMA) {
        chain_step(now);
        animating = chain_ready;
      } else {
        sample(now);
        expire(now);
        animating = S.n_raw > 0;
      }
      bool any = false;
      for (Output *o = S.outputs; o; o = o->next) any |= o->egl != EGL_NO_SURFACE;
      if (animating || any) render(now);  // after the last frame: hand the buffers back
      still_moving = animating;
    }
    set_timer(S.fx == FX_PLASMA ? S.frame : still_moving || now - S.last_move < 0.5 ? S.frame : now - S.last_move < 5 ? 0.04 : 0.1);
  }

  for (Output *o = S.outputs; o; o = o->next) output_hide(o);
  return exit_code;
}
