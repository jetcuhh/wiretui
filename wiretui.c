/* wiretui: terminal volume mixer for PipeWire, driven through wpctl. */


#define WIRETUI_VERSION "0.1.0"

/* musl needs _DEFAULT_SOURCE for cfmakeraw() */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define TB_IMPL
#define TB_OPT_ATTR_W 32
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
#include "termbox2.h"
#pragma GCC diagnostic pop

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Theme */
#define THEME_BG        0x101318
#define THEME_FG        0xEAEFF5
#define THEME_CURSOR    "#E37B66"
#define THEME_BLACK     0x222733
#define THEME_RED       0xE37B66
#define THEME_GREEN     0x8DA882
#define THEME_YELLOW    0xE0A568
#define THEME_BLUE      0x8999AA
#define THEME_MAGENTA   0xA58AA8
#define THEME_CYAN      0x809D9E
#define THEME_WHITE     0xC6CED8
#define THEME_LBLACK    0x98A0AE
#define THEME_LRED      0xF09A88
#define THEME_LGREEN    0xA9C29F
#define THEME_LYELLOW   0xEBBE8A
#define THEME_LBLUE     0xA5B3C2
#define THEME_LMAGENTA  0xBFA6C2
#define THEME_LCYAN     0x9DB8B9
#define THEME_LWHITE    0xEAEFF5

#define COL_BG        THEME_BG
#define COL_TEXT      THEME_WHITE
#define COL_BRIGHT    THEME_LWHITE
#define COL_DIM       THEME_LBLACK
#define COL_TRACK     THEME_BLACK
#define COL_BAR       THEME_BLUE
#define COL_ACCENT    THEME_LCYAN
#define COL_RED       THEME_RED
#define COL_MAXW      68

#define MAX_DEVS 128
#define NAME_MAX_LEN 128
#define OUT_MAX (128 * 1024)
#define REFRESH_MS 2000
#define CMD_TIMEOUT_MS 5000
#define MAX_VOL 150
#define DETENT_MS 450   /* hold at 100% (ms) */
#define SNAP 8          /* mouse snap to 100% (+-%) */
#define RC_TIMEOUT (-2)

enum { K_OUT, K_IN, K_APP };

struct dev {
    int id;
    char name[NAME_MAX_LEN];
    int vol;
    int muted;
    int kind;
    int is_default;
};

static struct dev devs[MAX_DEVS];
static int ndevs;
static int tab;
static int sel = -1;
static int first;
static int per_page = 1;
static int tab_sel_id[2] = {-1, -1};

static struct { int y, idx; } hit[MAX_DEVS];
static int nhit;
static int hit_bx, hit_bw;
static int hit_tabs_y;
static int hit_tab_x0[2], hit_tab_x1[2];

static int vol_streak;
static long detent_until;

static const char *wpctl_path = "wpctl";

static char status[256];
static int status_is_error;
static long status_until;
static int backend_down;
static long last_refresh;

static long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static int put(int x, int y, uintattr_t fg, uintattr_t bg, const char *s, int maxw)
{
    int used = 0;
    while (*s && used < maxw) {
        uint32_t ch;
        int len = tb_utf8_char_to_unicode(&ch, s);
        if (len <= 0) break;
        int w = tb_wcwidth(ch);
        if (w < 0) w = 1;
        if (used + w > maxw) break;
        if (w > 0) tb_set_cell(x + used, y, ch, fg, bg);
        used += w;
        s += len;
    }
    return used;
}

static int text_width(const char *s)
{
    int used = 0;
    while (*s) {
        uint32_t ch;
        int len = tb_utf8_char_to_unicode(&ch, s);
        if (len <= 0) break;
        int w = tb_wcwidth(ch);
        used += w < 0 ? 1 : w;
        s += len;
    }
    return used;
}

static void put_ell(int x, int y, uintattr_t fg, uintattr_t bg, const char *s, int maxw)
{
    if (maxw < 1) return;
    if (text_width(s) <= maxw) {
        put(x, y, fg, bg, s, maxw);
        return;
    }
    int used = put(x, y, fg, bg, s, maxw - 1);
    tb_set_cell(x + used, y, 0x2026, fg, bg);
}

static void put_centered(int y, uintattr_t fg, uintattr_t bg, const char *s, int maxw)
{
    int w = text_width(s);
    if (w > maxw) w = maxw;
    put((tb_width() - w) / 2, y, fg, bg, s, w);
}

static uintattr_t mix(uint32_t a, uint32_t b, int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    uint32_t out = 0;
    for (int sh = 16; sh >= 0; sh -= 8) {
        int ca = (int)((a >> sh) & 0xff), cb = (int)((b >> sh) & 0xff);
        out |= (uint32_t)(ca + (cb - ca) * pct / 100) << sh;
    }
    return out;
}

static void set_status(int err, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(status, sizeof status, fmt, ap);
    va_end(ap);
    status_is_error = err;
    status_until = now_ms() + 4000;
}

static void msg_first_line(const char *raw, char *out, size_t cap)
{
    out[0] = 0;
    if (!raw) return;
    while (*raw == ' ' || *raw == '\t' || *raw == '\n' || *raw == '\r') raw++;
    size_t i = 0;
    while (*raw && *raw != '\n' && *raw != '\r' && i + 1 < cap) out[i++] = *raw++;
    while (i > 0 && (out[i - 1] == ' ' || out[i - 1] == '\t')) i--;
    out[i] = 0;
}

/* run argv, capture output; returns exit code, RC_TIMEOUT or -1 */
static int run(char *const argv[], char **out)
{
    *out = NULL;
    int fds[2];
    if (pipe(fds) != 0) return -1;
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);

    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, 0);
            close(devnull);
        }
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[0]);
        close(fds[1]);
        execvp(argv[0], argv);
        _exit(127);
    }

    close(fds[1]);
    char *buf = malloc(OUT_MAX + 1);
    if (!buf) {
        close(fds[0]);
        kill(pid, SIGKILL);
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
        return -1;
    }

    size_t len = 0;
    int timed_out = 0;
    long deadline = now_ms() + CMD_TIMEOUT_MS;
    for (;;) {
        long left = deadline - now_ms();
        if (left <= 0) {
            timed_out = 1;
            break;
        }
        struct pollfd pf = {fds[0], POLLIN, 0};
        int pr = poll(&pf, 1, (int)(left < 100 ? left : 100));
        if (pr < 0) {
            if (errno == EINTR) continue;
            kill(pid, SIGKILL);
            break;
        }
        if (pr == 0) continue;
        ssize_t r = read(fds[0], buf + len, OUT_MAX - len);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) {
            kill(pid, SIGKILL);
            break;
        }
        if (r == 0) break;
        len += (size_t)r;
        if (len >= OUT_MAX) {
            kill(pid, SIGKILL);
            break;
        }
    }
    if (timed_out) kill(pid, SIGKILL);
    close(fds[0]);

    int st = 0, reaped = 0;
    while (!reaped) {
        pid_t w = waitpid(pid, &st, 0);
        if (w == pid) reaped = 1;
        else if (w < 0 && errno == EINTR) continue;
        else break;
    }

    buf[len] = 0;
    *out = buf;
    if (timed_out) return RC_TIMEOUT;
    return reaped && WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int to_percent(double v)
{
    int p = (int)(v * 100.0 + 0.5);
    if (p < 0) p = 0;
    if (p > MAX_VOL) p = MAX_VOL;
    return p;
}

static void store_dev(int id, const char *name, size_t namelen,
                      int vol, int muted, int kind, int is_default)
{
    if (ndevs >= MAX_DEVS) return;
    struct dev *d = &devs[ndevs++];
    d->id = id;
    if (namelen >= sizeof d->name) namelen = sizeof d->name - 1;
    memcpy(d->name, name, namelen);
    d->name[namelen] = 0;
    if (!d->name[0]) snprintf(d->name, sizeof d->name, "Device %d", id);
    d->vol = vol;
    d->muted = muted;
    d->kind = kind;
    d->is_default = is_default;
}

/* Parse `wpctl status` into devs[]. */
static void parse_status(char *text)
{
    int in_audio = 0, section = 0;

    for (char *line = text; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;

        if (!strncmp(line, "Audio", 5)) in_audio = 1;
        else if (line[0] && line[0] != ' ' && line[0] != '\t') in_audio = 0;

        if (in_audio) {
            if (strstr(line, "Sinks:")) {
                section = 1;
            } else if (strstr(line, "Sources:")) {
                section = 2;
            } else if (strstr(line, "Streams:")) {
                section = 3;
            } else if (strstr(line, "Devices:") || strstr(line, "Filters:")) {
                section = 0;
            } else if (section) {
                char *vp = strstr(line, "[vol:");
                char *digits = line;
                while (*digits && (*digits < '0' || *digits > '9')) digits++;
                if (*digits && (vp || section == 3)) {
                    char *end = NULL;
                    long id = strtol(digits, &end, 10);
                    while (*end == ' ') end++;
                    if (*end == '.') {
                        end++;
                        while (*end == ' ') end++;
                        char *ne = vp ? vp : end + strlen(end);
                        while (ne > end && (ne[-1] == ' ' || ne[-1] == '\t')) ne--;

                        if (section == 3) {
                            size_t n = (size_t)(ne - end);
                            char tmp[NAME_MAX_LEN];
                            if (n >= sizeof tmp) n = sizeof tmp - 1;
                            memcpy(tmp, end, n);
                            tmp[n] = 0;
                            if (n && !strstr(tmp, " > ") && !strstr(tmp, " < ") &&
                                !strstr(tmp, " <> "))
                                store_dev((int)id, end, (size_t)(ne - end),
                                          -1, 0, K_APP, 0);
                        } else {
                            double v = strtod(vp + 5, NULL);
                            char *q = digits;
                            while (q > line && q[-1] == ' ') q--;
                            int is_default = q > line && q[-1] == '*';
                            store_dev((int)id, end, (size_t)(ne - end),
                                      to_percent(v), strstr(vp, "MUTED") != NULL,
                                      section - 1, is_default);
                        }
                    }
                }
            }
        }

        if (!nl) break;
        line = nl + 1;
    }
}

static int get_stream_vol(int id, int *vol, int *muted)
{
    char sid[16];
    snprintf(sid, sizeof sid, "%d", id);
    char *argv[] = {(char *)wpctl_path, "get-volume", sid, NULL};
    char *out = NULL;
    int rc = run(argv, &out);
    int res = 1;
    if (rc == RC_TIMEOUT) {
        res = 2;
    } else if (rc == 0 && out) {
        char *p = strstr(out, "Volume:");
        if (p) {
            *vol = to_percent(strtod(p + 7, NULL));
            *muted = strstr(out, "MUTED") != NULL;
            res = 0;
        }
    }
    free(out);
    return res;
}

/* `wpctl status` has no app volumes, so ask per stream. */
static void fill_stream_vols(void)
{
    int w = 0, stop = 0;
    for (int i = 0; i < ndevs; i++) {
        struct dev *d = &devs[i];
        if (d->kind == K_APP) {
            if (stop) continue;
            int r = get_stream_vol(d->id, &d->vol, &d->muted);
            if (r == 2) stop = 1;
            if (r) continue;
        }
        if (w != i) devs[w] = devs[i];
        w++;
    }
    ndevs = w;
}

static int tab_of(int kind)
{
    return kind == K_IN;
}

/* Rows of the active tab: apps first, then devices. */
static int build_rows(int *idx)
{
    int n = 0;
    if (tab == 0) {
        for (int i = 0; i < ndevs; i++)
            if (devs[i].kind == K_APP) idx[n++] = i;
        for (int i = 0; i < ndevs; i++)
            if (devs[i].kind == K_OUT) idx[n++] = i;
    } else {
        for (int i = 0; i < ndevs; i++)
            if (devs[i].kind == K_IN) idx[n++] = i;
    }
    return n;
}

static int find_dev(int id)
{
    for (int i = 0; i < ndevs; i++)
        if (devs[i].id == id && tab_of(devs[i].kind) == tab) return i;
    return -1;
}

static int default_in_tab(void)
{
    for (int i = 0; i < ndevs; i++)
        if (devs[i].kind != K_APP && tab_of(devs[i].kind) == tab && devs[i].is_default)
            return i;
    return -1;
}

static int is_active_tab(int i)
{
    return i >= 0 && i < ndevs && tab_of(devs[i].kind) == tab;
}

static void set_sel(int i)
{
    if (!is_active_tab(i)) return;
    sel = i;
    tab_sel_id[tab] = devs[i].id;
}

static int pick_in_tab(void)
{
    int want = tab_sel_id[tab];
    int i = want >= 0 ? find_dev(want) : -1;
    if (i < 0) i = default_in_tab();
    if (i < 0) {
        int idx[MAX_DEVS];
        if (build_rows(idx) > 0) i = idx[0];
    }
    return i;
}

static void refresh(void)
{
    char *out = NULL;
    char *argv[] = {(char *)wpctl_path, "status", NULL};
    int rc = run(argv, &out);
    ndevs = 0;
    if (rc == 0 && out) parse_status(out);
    free(out);
    if (rc == 0) fill_stream_vols();
    last_refresh = now_ms();

    if (rc != 0) {
        backend_down = 1;
        set_status(1, rc == RC_TIMEOUT ? "wpctl timed out"
                                       : "wpctl failed; is PipeWire running?");
        sel = -1;
        first = 0;
        return;
    }
    if (backend_down) {
        backend_down = 0;
        status[0] = 0;
        status_is_error = 0;
        status_until = 0;
    }

    int i = pick_in_tab();
    if (i >= 0) set_sel(i);
    else {
        sel = -1;
        first = 0;
    }
}

static int do_cmd(char *const argv[])
{
    char *out = NULL;
    int rc = run(argv, &out);
    if (rc != 0) {
        char msg[160];
        msg_first_line(out, msg, sizeof msg);
        if (msg[0]) set_status(1, "%s", msg);
        else set_status(1, rc == RC_TIMEOUT ? "wpctl timed out" : "wpctl failed");
    }
    free(out);
    return rc;
}

static void apply_volume(int v)
{
    if (!is_active_tab(sel)) return;
    if (v < 0) v = 0;
    if (v > MAX_VOL) v = MAX_VOL;
    if (v == devs[sel].vol) return;

    char id[16], val[16];
    snprintf(id, sizeof id, "%d", devs[sel].id);
    snprintf(val, sizeof val, "%d%%", v);
    char *argv[] = {(char *)wpctl_path, "set-volume", id, val, NULL};
    /* update locally; the periodic refresh reconciles */
    if (do_cmd(argv) == 0) devs[sel].vol = v;
    else refresh();
}

/* Crossing 100% lands on it, and a held key pauses there. */
static void adjust_volume(int delta)
{
    if (!is_active_tab(sel)) return;
    int cur = devs[sel].vol, t = cur + delta;
    if ((cur < 100 && t > 100) || (cur > 100 && t < 100)) {
        t = 100;
        detent_until = now_ms() + DETENT_MS;
    } else if (cur == 100 && vol_streak > 0 && now_ms() < detent_until) {
        return;
    }
    apply_volume(t);
}

static void set_mute(int on)
{
    if (!is_active_tab(sel)) return;
    char id[16];
    snprintf(id, sizeof id, "%d", devs[sel].id);
    char *argv[] = {(char *)wpctl_path, "set-mute", id, on ? "1" : "0", NULL};
    if (do_cmd(argv) == 0) devs[sel].muted = on;
    else refresh();
}

static void toggle_mute(void)
{
    if (!is_active_tab(sel)) return;
    set_mute(!devs[sel].muted);
}

static void reset_volume(void)
{
    if (!is_active_tab(sel)) return;
    if (devs[sel].muted) set_mute(0);
    apply_volume(100);
}

static void set_default(void)
{
    if (!is_active_tab(sel)) return;
    if (devs[sel].kind == K_APP) return;
    char id[16];
    snprintf(id, sizeof id, "%d", devs[sel].id);
    char *argv[] = {(char *)wpctl_path, "set-default", id, NULL};
    do_cmd(argv);
    refresh();
}

static void move_sel(int delta)
{
    int idx[MAX_DEVS], pos = -1;
    int n = build_rows(idx);
    if (!n) return;

    for (int k = 0; k < n; k++)
        if (idx[k] == sel) pos = k;
    if (pos < 0) pos = 0;
    else pos += delta;
    if (pos < 0) pos = 0;
    if (pos >= n) pos = n - 1;
    set_sel(idx[pos]);
}

static void move_edge(int last)
{
    int idx[MAX_DEVS];
    int n = build_rows(idx);
    if (n) set_sel(idx[last ? n - 1 : 0]);
}

static void switch_tab(void)
{
    if (is_active_tab(sel)) tab_sel_id[tab] = devs[sel].id;
    tab = !tab;
    first = 0;
    int i = pick_in_tab();
    if (i >= 0) set_sel(i);
    else sel = -1;
}

static void draw_tabs(int y, int W, int cx, int colw, int lo, int hi, int n)
{
    static const char *const full[2] = {"Output", "Input"};
    static const char *const tiny[2] = {"Out", "In"};
    const char *const *labels = W >= 20 ? full : tiny;
    int w0 = text_width(labels[0]), w1 = text_width(labels[1]);
    int x = (W - (w0 + 3 + w1)) / 2;
    if (x < 0) x = 0;

    for (int i = 0; i < colw; i++)
        tb_set_cell(cx + i, y + 1, 0x2500, COL_TRACK, COL_BG);

    hit_tabs_y = y;
    int end = x;
    for (int t = 0; t < 2; t++) {
        int w = t ? w1 : w0;
        hit_tab_x0[t] = x;
        hit_tab_x1[t] = x + w + 1;
        if (t == tab) {
            put(x, y, COL_ACCENT | TB_BOLD, COL_BG, labels[t], W - x);
            for (int i = 0; i < w && x + i < W; i++)
                tb_set_cell(x + i, y + 1, 0x2501, COL_ACCENT, COL_BG);
        } else {
            put(x, y, COL_DIM, COL_BG, labels[t], W - x);
        }
        end = x + w;
        x += w + 3;
    }

    if (lo > 0 || hi < n - 1) {
        char pg[48];
        snprintf(pg, sizeof pg, "%d-%d/%d", lo + 1, hi + 1, n);
        int pw = text_width(pg);
        if (cx + colw - pw > end + 2) put(cx + colw - pw, y, COL_DIM, COL_BG, pg, pw);
    }
}

static void draw_row(const struct dev *d, int on, int x, int y, int w)
{
    uintattr_t bg = COL_BG;

    int rw = 0;
    if (d->muted) rw += 5;
    if (d->is_default) rw += (rw ? 2 : 0) + 7;
    if (w < 24) rw = d->muted ? 5 : 0;
    int namew = w - (rw ? rw + 2 : 0);

    put_ell(x, y, on ? (COL_BRIGHT | TB_BOLD) : COL_DIM, bg, d->name, namew);
    if (rw) {
        int rx = x + w - rw;
        if (w >= 24 && d->is_default)
            rx += put(rx, y, COL_ACCENT, bg, "default", 7) + (d->muted ? 2 : 0);
        if (d->muted) put(rx, y, COL_RED, bg, "muted", 5);
    }

    int pctw = 4;
    int bw = w - pctw - 2;
    if (bw < 6) {
        bw = w;
        pctw = 0;
    }
    hit_bx = x;
    hit_bw = bw;
    int v = d->vol < 0 ? 0 : d->vol;
    int fillw = (v * bw + MAX_VOL / 2) / MAX_VOL;
    int c100 = (100 * bw + MAX_VOL / 2) / MAX_VOL;

    uintattr_t track = THEME_BLACK;
    uintattr_t zone = mix(track, THEME_RED, 30);
    uintattr_t base = d->muted ? COL_DIM : (on ? COL_ACCENT : COL_BAR);
    uintattr_t hot = d->muted ? COL_DIM : COL_RED;
    for (int i = 0; i < bw; i++) {
        int red = i >= c100;
        uintattr_t c;
        if (i < fillw) c = red ? hot : base;
        else c = red ? zone : track;
        tb_set_cell(x + i, y + 1, i < fillw ? 0x2501 : 0x2500, c, bg);
    }
    if (pctw) {
        char pct[16];
        snprintf(pct, sizeof pct, "%d%%", v);
        int over = v > 100 ? 30 + (v - 100) * 70 / (MAX_VOL - 100) : 0;
        uintattr_t pc = mix(on ? COL_BRIGHT : COL_DIM, THEME_RED, over);
        put(x + bw + 2, y + 1, pc, bg, pct, pctw);
    }
}

static void draw(void)
{
    if (status_until && now_ms() > status_until) {
        status[0] = 0;
        status_is_error = 0;
        status_until = 0;
    }

    int W = tb_width(), H = tb_height();
    tb_clear();
    tb_hide_cursor();
    nhit = 0;

    if (H < 7 || W < 14) {
        if (H >= 1 && W >= 12)
            put_centered(H / 2, COL_DIM, COL_BG, "terminal too small", W);
        tb_present();
        return;
    }

    int mx = W >= 24 ? 2 : 0;
    int cw = W - 2 * mx;
    int colw = cw > COL_MAXW ? COL_MAXW : cw;
    int cx = (W - colw) / 2;
    int pad = colw >= 20 ? 2 : 0;
    int tx = cx + pad, tw = colw - 2 * pad;

    int tabs_y = H >= 12 ? 1 : 0;
    int top = tabs_y + 3;
    int status_y = H - 1;
    int area = status_y - 1 - top;

    per_page = (area + 1) / 3;
    if (per_page < 1) per_page = 1;

    int idx[MAX_DEVS];
    int n = build_rows(idx);
    int napps = 0;
    for (int p = 0; p < n; p++)
        if (devs[idx[p]].kind == K_APP) napps++;

    int ent[MAX_DEVS + 2], ne = 0, selent = -1;
    int labels = tab == 0 && napps > 0;
    for (int p = 0; p < n; p++) {
        if (labels && p == 0) ent[ne++] = -1;
        if (labels && p == napps) ent[ne++] = -2;
        if (idx[p] == sel) selent = ne;
        ent[ne++] = p;
    }

    if (selent >= 0) {
        if (selent < first) {
            first = selent;
            if (first > 0 && ent[first - 1] < 0) first--;
        }
        for (; first < selent; first++) {
            int need = 2;
            for (int e = first; e < selent; e++) need += ent[e] >= 0 ? 3 : 2;
            if (need <= area) break;
        }
    }
    if (first > ne - 1) first = ne - 1;
    if (first < 0) first = 0;

    int y = top, lo = -1, hi = -1;
    for (int e = first; e < ne; e++) {
        if (ent[e] < 0) {
            if (y + 4 > top + area) break;
            put(tx, y, mix(COL_DIM, COL_BG, 40), COL_BG, ent[e] == -1 ? "Apps" : "Devices", tw);
            y += 2;
            continue;
        }
        if (y + 2 > top + area) break;
        int di = idx[ent[e]];
        int on = di == sel;
        draw_row(&devs[di], on, tx, y, tw);
        hit[nhit].y = y;
        hit[nhit].idx = di;
        nhit++;
        if (lo < 0) lo = ent[e];
        hi = ent[e];
        y += 3;
    }

    if (n == 0)
        put_centered(top, COL_DIM, COL_BG,
                     backend_down ? "wpctl unavailable"
                                  : tab == 0 ? "no output devices" : "no input devices",
                     cw);

    draw_tabs(tabs_y, W, cx, colw, lo < 0 ? 0 : lo, hi < 0 ? 0 : hi, n);

    if (status[0]) {
        put_centered(status_y, COL_RED, COL_BG, status, cw);
    } else if (H >= 12) {
        static const char *const hints[] = {
            "j/k move", "h/l volume", "m mute", "d 100%",
            "enter default", "tab switch", "q quit"};
        int nh = 0, total = 0;
        for (size_t i = 0; i < sizeof hints / sizeof *hints; i++) {
            int w = text_width(hints[i]) + (nh ? 3 : 0);
            if (total + w > cw) break;
            total += w;
            nh++;
        }
        uintattr_t hc = mix(COL_DIM, COL_BG, 40);
        int x = (W - total) / 2;
        for (int i = 0; i < nh; i++) {
            x += put(x, status_y, hc, COL_BG, hints[i], cw) + 3;
        }
    }

    tb_present();
}

static long vol_last_ms;
static int vol_dir;
static int pend_delta, pend_abs = -1;
static int dragging;

static int vol_step(int dir)
{
    long t = now_ms();
    if (dir == vol_dir && t - vol_last_ms < 120) vol_streak++;
    else vol_streak = 0;
    vol_dir = dir;
    vol_last_ms = t;
    return vol_streak < 8 ? 1 : vol_streak < 20 ? 2 : vol_streak < 40 ? 4 : 8;
}

static void flush_pending(void)
{
    if (pend_abs >= 0) {
        apply_volume(pend_abs);
        pend_abs = -1;
    }
    if (pend_delta) {
        adjust_volume(pend_delta);
        pend_delta = 0;
    }
}

static void select_row(int idx)
{
    if (idx == sel) return;
    flush_pending();
    set_sel(idx);
}

static int bar_volume(int x)
{
    int i = x - hit_bx;
    if (hit_bw < 2) return 0;
    if (i < 0) i = 0;
    if (i > hit_bw - 1) i = hit_bw - 1;
    int v = (i * MAX_VOL + (hit_bw - 1) / 2) / (hit_bw - 1);
    if (v >= 100 - SNAP && v <= 100 + SNAP) v = 100;
    return v;
}

static void handle_mouse(const struct tb_event *ev)
{
    int x = ev->x, y = ev->y;
    int motion = ev->mod & TB_MOD_MOTION;

    if (ev->key == TB_KEY_MOUSE_RELEASE) {
        dragging = 0;
        return;
    }
    if (dragging && motion && ev->key == TB_KEY_MOUSE_LEFT) {
        pend_abs = bar_volume(x);
        return;
    }
    if (motion) return;
    dragging = 0;

    if (ev->key == TB_KEY_MOUSE_LEFT && (y == hit_tabs_y || y == hit_tabs_y + 1)) {
        for (int t = 0; t < 2; t++) {
            if (x >= hit_tab_x0[t] && x <= hit_tab_x1[t]) {
                flush_pending();
                if (tab != t) switch_tab();
                return;
            }
        }
        return;
    }

    int row = -1, on_bar = 0;
    for (int k = 0; k < nhit; k++) {
        if (y == hit[k].y || y == hit[k].y + 1) {
            row = hit[k].idx;
            on_bar = y == hit[k].y + 1;
        }
    }

    if (ev->key == TB_KEY_MOUSE_WHEEL_UP || ev->key == TB_KEY_MOUSE_WHEEL_DOWN) {
        int up = ev->key == TB_KEY_MOUSE_WHEEL_UP;
        if (row >= 0) select_row(row);
        pend_delta += up ? vol_step(1) : -vol_step(-1);
        return;
    }

    if (row < 0) return;
    select_row(row);
    if (ev->key == TB_KEY_MOUSE_LEFT) {
        if (on_bar) {
            dragging = 1;
            pend_abs = bar_volume(x);
        }
    } else if (ev->key == TB_KEY_MOUSE_RIGHT) {
        flush_pending();
        toggle_mute();
    } else if (ev->key == TB_KEY_MOUSE_MIDDLE) {
        flush_pending();
        reset_volume();
    }
}

static int handle_event(const struct tb_event *ev)
{
    if (ev->type == TB_EVENT_MOUSE) {
        handle_mouse(ev);
        return 0;
    }
    if (ev->type != TB_EVENT_KEY) return 0;

    if (ev->ch == 'l' || ev->key == TB_KEY_ARROW_RIGHT || ev->ch == '+' || ev->ch == '=' ||
        ev->ch == 'n') {
        pend_delta += vol_step(1);
        return 0;
    }
    if (ev->ch == 'h' || ev->key == TB_KEY_ARROW_LEFT || ev->ch == '-' || ev->ch == 'b') {
        pend_delta -= vol_step(-1);
        return 0;
    }

    flush_pending();
    if (ev->ch == 'q' || ev->key == TB_KEY_CTRL_Q) return 1;
    else if (ev->ch == 'j' || ev->key == TB_KEY_ARROW_DOWN) move_sel(1);
    else if (ev->ch == 'k' || ev->key == TB_KEY_ARROW_UP) move_sel(-1);
    else if (ev->key == TB_KEY_PGDN || ev->key == TB_KEY_CTRL_D) move_sel(per_page);
    else if (ev->key == TB_KEY_PGUP || ev->key == TB_KEY_CTRL_U) move_sel(-per_page);
    else if (ev->ch == 'g' || ev->key == TB_KEY_HOME) move_edge(0);
    else if (ev->ch == 'G' || ev->key == TB_KEY_END) move_edge(1);
    else if (ev->ch == 'm' || ev->ch == ' ') toggle_mute();
    else if (ev->ch == 'd') reset_volume();
    else if (ev->key == TB_KEY_ENTER) set_default();
    else if (ev->key == TB_KEY_TAB || ev->key == TB_KEY_BACK_TAB ||
             ev->ch == 'H' || ev->ch == 'L') switch_tab();
    else if (ev->ch >= '1' && ev->ch <= '9') apply_volume((int)(ev->ch - '0') * 10);
    else if (ev->ch == '0') apply_volume(100);
    else if (ev->ch == '`') apply_volume(0);
    else if (ev->ch == 'r') refresh();
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        if (argc == 2 && (!strcmp(argv[1], "-v") || !strcmp(argv[1], "--version"))) {
            printf("wiretui %s\n", WIRETUI_VERSION);
            return 0;
        }
        fprintf(stderr, "usage: wiretui [-v | --version]\n");
        return 2;
    }

    const char *env = getenv("WIRETUI_WPCTL");
    if (env && *env) wpctl_path = env;

    signal(SIGPIPE, SIG_IGN);
    if (tb_init() != 0) {
        fprintf(stderr, "wiretui: cannot initialise the terminal\n");
        return 1;
    }
    tb_set_input_mode(TB_INPUT_ESC | TB_INPUT_MOUSE);
    tb_set_output_mode(TB_OUTPUT_TRUECOLOR);
    tb_set_clear_attrs(THEME_FG, THEME_BG);
    tb_sendf("\x1b]12;%s\x07", THEME_CURSOR);

    refresh();
    draw();

    for (;;) {
        long wait = REFRESH_MS - (now_ms() - last_refresh);
        if (wait <= 0) {
            refresh();
            draw();
            continue;
        }

        struct tb_event ev;
        if (tb_peek_event(&ev, (int)wait) != TB_OK) continue;
        /* drain queued events: a held key becomes one wpctl call */
        int quit = handle_event(&ev);
        while (!quit && tb_peek_event(&ev, 0) == TB_OK) quit = handle_event(&ev);
        if (quit) break;
        flush_pending();
        draw();
    }

    tb_shutdown();
    return 0;
}
