#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/record.h>
#include <X11/extensions/Xinerama.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#include <sys/time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/inotify.h>

#include <poll.h>
#include <math.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>

#include <fcntl.h>
#include <spawn.h>
#include <dirent.h>
#include <unistd.h>
#include <signal.h>
#include <limits.h>
#include <libgen.h>
#include <pthread.h>

#define LOCAL_FIFO_SUFFIX "/.setuzuna/xakar/internal.sock"
#define MAX_CHAIN_DEPTH 64

struct StrutInfo {
    Window win;
    bool active;
    int win_x, win_y, win_w, win_h;
    int size;
};

typedef struct {
    Window object;
    int width, height;
    int ancestor_x, ancestor_y;
    int ancestor_width, ancestor_height;
} Geometry;

typedef enum {
    MODE_UNKNOWN = 0,
    MODE_FULLSCREEN,
    MODE_CENTER,
    MODE_HALF_LEFT,
    MODE_HALF_RIGHT,
    MODE_HALF_UP,
    MODE_HALF_DOWN,
    MODE_QUARTER_TL,
    MODE_QUARTER_TR,
    MODE_QUARTER_BL,
    MODE_QUARTER_BR
} TileMode;

typedef struct _Key_t {
    KeyCode key;
    int refcount;
    struct _Key_t *next;
} Key_t;

typedef struct _KeyMap_t {
    Bool UseKeyCode;
    KeySym from_ks;
    KeyCode from_kc;
    Key_t *mods;
    Key_t *to_keys;
    Bool used;
    Bool pressed;
    struct timeval down_at;
    char *command;
    char *internal_action;
    Bool is_mod_trigger;
    struct _KeyMap_t *next;
    struct _KeyMap_t *next_in_bucket;
} KeyMap_t;

typedef struct _Hotkeys_t {
    Display *data_conn;
    Display *ctrl_conn;
    XRecordContext record_ctx;
    KeyMap_t *map;
    int *generated_counts;
    struct timeval timeout;
    XRecordRange *rec_range;
    KeyMap_t **by_key;
    int keycode_min;
    int keycode_max;
    unsigned int cached_numlock_mask;
    unsigned int cached_capslock_mask;
    pthread_mutex_t map_mutex;
} Hotkeys_t;

static Atom _NET_WM_DESKTOP = None;
static Atom _NET_WM_STATE = None;
static Atom _NET_WM_STATE_ABOVE = None;
static Atom _NET_WM_STATE_STICKY = None;
static Atom _NET_WM_STATE_FULLSCREEN = None;
static Atom _NET_WM_STRUT = None;
static Atom _NET_WM_STRUT_PARTIAL = None;
static Atom _NET_WM_WINDOW_TYPE = None;
static Atom _NET_WM_WINDOW_TYPE_DOCK = None;
static Atom _NET_WM_NAME = None;
static Atom _NET_ACTIVE_WINDOW = None;
static Atom _MOTIF_WM_HINTS = None;
static Atom _OB_WM_STATE_UNDECORATED = None;
static Atom WM_CLASS = None;
static Atom UTF8_STRING = None;

static Display *dpy = NULL;
static Window root_window = 0;
static Visual *visual = NULL;
static Colormap colormap = 0;

static Hotkeys_t *g_hotkeys = NULL;

static pthread_t config_watcher_thread;
static pthread_t keyboard_watcher_thread;
static pthread_t fifo_watcher_thread;
static pthread_t window_watcher_thread;

static int screen_width = 0, screen_height = 0;
static int half_screen_width = 0, half_screen_height = 0;
static int screen = 0;
static int depth = 0;
static int active_strut_count = 0;
static int sigpipe_fds[2] = { -1, -1 };
static int inotify_fd = -1;
static int inotify_wd = -1;
static int saved_argc = 0;

static char FIFO_PATH[4096];
static char CONFIG_PATH[4096] = {0};
static char **saved_argv = NULL;

static const char *home = NULL;
static unsigned long black_pixel = 0;

static volatile sig_atomic_t stop_requested = 0;
static volatile int g_x_error_occurred = 0;
static volatile int g_x_error_code = 0;

static float brightness_exponent = 1.0f;

void set_brightness_exponent(float e) {
    if (e <= 0.0f) e = 1.0f;
    brightness_exponent = e;
}

static char *join_path(const char *a, const char *b) {
    size_t la = strlen(a);
    size_t lb = strlen(b);
    bool need_sep = (la == 0 || a[la-1] != '/');
    size_t total = la + (need_sep ? 1 : 0) + lb + 1;
    char *s = malloc(total);
    if (!s) return NULL;
    strcpy(s, a);
    if (need_sep) strcat(s, "/");
    strcat(s, b);
    return s;
}

static bool read_unsigned_from_file(const char *path, unsigned long *out) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    unsigned long tmp = 0;
    int r = fscanf(f, "%lu", &tmp);
    fclose(f);
    if (r != 1) return false;
    *out = tmp;
    return true;
}

static bool write_unsigned_to_file(const char *path, unsigned long val) {
    FILE *f = fopen(path, "w");
    if (!f) return false;
    int r = fprintf(f, "%lu", val);
    fclose(f);
    return r > 0;
}

static bool write_backlight_device(const char *dev_dir, unsigned long value, const char *devname) {
    char *brightness_path = join_path(dev_dir, "brightness");
    if (!brightness_path) return false;

    bool ok = false;
    if (access(brightness_path, W_OK) == 0) {
        ok = write_unsigned_to_file(brightness_path, value);
        if (!ok) fprintf(stderr, "[xakard] Failed to write %s\n", brightness_path);
    }

    free(brightness_path);
    return ok;
}

static unsigned long percent_to_value(unsigned long max_brightness, float percentage) {
    if (percentage <= 0.0f) return 0;
    if (percentage >= 100.0f) return max_brightness;
    float p = percentage / 100.0f;
    float val = powf(p, brightness_exponent) * (float)max_brightness;
    unsigned long rounded = (unsigned long) lroundf(val);
    if (rounded > max_brightness) rounded = max_brightness;
    return rounded;
}

bool set_brightness_for_device(const char *devname, float percentage) {
    if (!devname) return false;
    if (percentage < 0.0f) percentage = 0.0f;
    if (percentage > 100.0f) percentage = 100.0f;

    char *dev_dir = join_path("/sys/class/backlight", devname);
    if (!dev_dir) return false;

    unsigned long max_b = 0;
    char *max_path = join_path(dev_dir, "max_brightness");
    if (!max_path) { free(dev_dir); return false; }

    bool ok = false;
    if (!read_unsigned_from_file(max_path, &max_b)) {
        fprintf(stderr, "[xakard] Failed to read %s\n", max_path);
        ok = false;
    } else {
        unsigned long target = percent_to_value(max_b, percentage);
        ok = write_backlight_device(dev_dir, target, devname);
    }

    free(max_path);
    free(dev_dir);
    return ok;
}

bool set_brightness(float percentage) {
    DIR *d = opendir("/sys/class/backlight");
    if (!d) {
        if (errno == ENOENT) {
            fprintf(stderr, "[xakard] /sys/class/backlight not present on this system\n");
        } else {
            perror("[xakard] opendir(/sys/class/backlight)");
        }
        return false;
    }

    struct dirent *ent;
    bool any_ok = false;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char *dev_dir = join_path("/sys/class/backlight", ent->d_name);
        if (!dev_dir) continue;
        char *max_path = join_path(dev_dir, "max_brightness");
        if (!max_path) { free(dev_dir); continue; }

        unsigned long max_b = 0;
        if (!read_unsigned_from_file(max_path, &max_b)) {
            free(max_path);
            free(dev_dir);
            continue;
        }
        free(max_path);

        unsigned long target = percent_to_value(max_b, percentage);

        if (write_backlight_device(dev_dir, target, ent->d_name)) {
            any_ok = true;
        }
        free(dev_dir);
    }
    closedir(d);
    return any_ok;
}

static bool get_device_percent(const char *devname, float *out_percent) {
    if (!devname || !out_percent) return false;
    char *dev_dir = NULL;
    char *path_curr = NULL;
    char *path_max = NULL;
    unsigned long curr = 0, max_b = 0;
    bool ok = false;

    dev_dir = join_path("/sys/class/backlight", devname);
    if (!dev_dir) goto out;

    path_curr = join_path(dev_dir, "brightness");
    path_max  = join_path(dev_dir, "max_brightness");
    if (!path_curr || !path_max) goto out;

    if (!read_unsigned_from_file(path_curr, &curr)) goto out;
    if (!read_unsigned_from_file(path_max, &max_b)) goto out;
    if (max_b == 0) goto out;

    float ratio = (float)curr / (float)max_b;
    if (ratio < 0.0f) ratio = 0.0f;
    if (ratio > 1.0f) ratio = 1.0f;
    float perc = powf(ratio, 1.0f / brightness_exponent) * 100.0f;
    *out_percent = perc;
    ok = true;

out:
    free(dev_dir);
    free(path_curr);
    free(path_max);
    return ok;
}

static bool adjust_brightness_all_delta(float delta_percent) {
    DIR *d = opendir("/sys/class/backlight");
    if (!d) return false;
    struct dirent *ent;
    bool any_ok = false;

    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char *dev_dir = join_path("/sys/class/backlight", ent->d_name);
        if (!dev_dir) continue;
        char *max_path = join_path(dev_dir, "max_brightness");
        char *curr_path = join_path(dev_dir, "brightness");
        if (!max_path || !curr_path) { free(max_path); free(curr_path); free(dev_dir); continue; }

        unsigned long max_b = 0, curr = 0;
        if (!read_unsigned_from_file(max_path, &max_b) || !read_unsigned_from_file(curr_path, &curr) || max_b == 0) {
            free(max_path); free(curr_path); free(dev_dir);
            continue;
        }

        float curr_perc = powf((float)curr / (float)max_b, 1.0f / brightness_exponent) * 100.0f;
        float target_perc = curr_perc + delta_percent;
        if (target_perc < 0.0f) target_perc = 0.0f;
        if (target_perc > 100.0f) target_perc = 100.0f;

        unsigned long target_val = percent_to_value(max_b, target_perc);
        if (write_backlight_device(dev_dir, target_val, ent->d_name)) {
            any_ok = true;
        }
        free(max_path); free(curr_path); free(dev_dir);
    }
    closedir(d);
    return any_ok;
}

extern char **environ;

static struct StrutInfo struts[4] = {{0,false,0,0,0,0,0}, {0,false,0,0,0,0,0}, {0,false,0,0,0,0,0}, {0,false,0,0,0,0,0}};

static void fatal(const char *msg) {
    if (msg) fprintf(stderr, "[xakard] Fatal: %s\n", msg);
    if (FIFO_PATH[0]) unlink(FIFO_PATH);
    if (dpy) XCloseDisplay(dpy);
    exit(1);
}

static int _x_global_error_handler(Display *dpy_arg, XErrorEvent *ev) {
    (void)dpy_arg;
    g_x_error_occurred = 1;
    g_x_error_code = ev->error_code;
    return 0;
}

static void run_command_async(const char *cmd) {
    if (!cmd || !*cmd) return;
    pid_t child_pid = 0;
    posix_spawnattr_t attr;
    int rc = posix_spawnattr_init(&attr);
    if (rc == 0) {
        char *argv[] = { "sh", "-c", (char *)cmd, NULL };
        rc = posix_spawnp(&child_pid, "/bin/sh", NULL, &attr, argv, environ);
        posix_spawnattr_destroy(&attr);
        if (rc == 0) {
            return;
        } else {
            fprintf(stderr, "[xakard] posix_spawnp failed: %s\n", strerror(rc));
        }
    } else {
        fprintf(stderr, "[xakard] posix_spawnattr_init failed: %s\n", strerror(rc));
    }
}

static char *trim(char *s) {
    char *end;
    if (!s) return s;
    while (*s && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')) s++;
    if (*s == 0) return s;
    end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
        *end = '\0';
        end--;
    }
    return s;
}

static void restart_self(void) {
    if (!saved_argv || saved_argc <= 0) {
        fprintf(stderr, "[xakard] restart_self: saved argv missing ? cannot restart\n");
        return;
    }

    fflush(NULL);
    char *prog = saved_argv[0];

    if (g_hotkeys) {
        fprintf(stdout, "[xakard] Restarting self (exec %s)\n", prog);
    }

    execvp(prog, saved_argv);
    fprintf(stderr, "[xakard] execvp failed: %s\n", strerror(errno));
}

static void sig_handler(int sig) {
    stop_requested = 1;
    (void)sig;
    if (sigpipe_fds[1] != -1) {
        ssize_t r;
        char c = 1;
        do {
            r = write(sigpipe_fds[1], &c, 1);
        } while (r == -1 && errno == EINTR);
    }
}

static void stop_strut_index(int idx) {
    if (idx < 0 || idx > 3) return;
    if (!struts[idx].active) return;

    if (dpy && struts[idx].win != 0) {
        XUnmapWindow(dpy, struts[idx].win);
        XDestroyWindow(dpy, struts[idx].win);
        XFlush(dpy);
    }

    struts[idx].win = 0;
    struts[idx].active = false;
    struts[idx].size = 0;
    active_strut_count = (active_strut_count > 0) ? active_strut_count - 1 : 0;

    const char *names[4] = {"left","right","top","bottom"};
    printf("Strut stopped on %s\n", names[idx]);
}

static void stop_all_struts(void) {
    int i;
    for (i = 0; i < 4; ++i) stop_strut_index(i);
}

static int side_to_index(const char *side) {
    if (!side) return -1;
    if (strcmp(side, "left") == 0) return 0;
    if (strcmp(side, "right") == 0) return 1;
    if (strcmp(side, "top") == 0) return 2;
    if (strcmp(side, "bottom") == 0) return 3;
    return -1;
}

static void fill_strut_arrays_for_side(int idx, int win_x, int win_y, int win_w, int win_h, long strut_partial[12], long strut4[4]) {
    memset(strut_partial, 0, sizeof(long)*12);
    memset(strut4, 0, sizeof(long)*4);

    if (idx == 2) {
        strut_partial[2] = win_h;
        strut4[2] = win_h;
        strut_partial[8] = win_x;
        strut_partial[9] = win_x + win_w - 1;
    } else if (idx == 3) {
        strut_partial[3] = win_h;
        strut4[3] = win_h;
        strut_partial[10] = win_x;
        strut_partial[11] = win_x + win_w - 1;
    } else if (idx == 0) {
        strut_partial[0] = win_w;
        strut4[0] = win_w;
        strut_partial[4] = win_y;
        strut_partial[5] = win_y + win_h - 1;
    } else if (idx == 1) {
        strut_partial[1] = win_w;
        strut4[1] = win_w;
        strut_partial[6] = win_y;
        strut_partial[7] = win_y + win_h - 1;
    }
}

/* --------------------------- helpers / storage --------------------------- */
/* Max counts for property arrays we keep (truncate if larger). */
#define MAX_ATOMS 8

typedef struct {
    Bool saved;

    /* originally-set atoms (if any) */
    int type_count;
    Atom type_atoms[MAX_ATOMS];

    int state_count;
    Atom state_atoms[MAX_ATOMS];

    Bool has_desktop;
    long desktop;

    Bool has_strut_partial;
    long strut_partial[12];

    Bool has_strut;
    long strut4[4];

    /* original geometry & map state */
    int x, y, w, h;
    Bool was_mapped;
} OrigWindowProps;

/* one per side */
static OrigWindowProps orig_props[4] = { {0} };

/* helper to clamp counts */
static int clamp_count(int v, int max) { return (v < 0) ? 0 : (v > max ? max : v); }

/* fetch atoms property (like _NET_WM_WINDOW_TYPE or _NET_WM_STATE) into dest array */
static void fetch_atoms(Display *dpy, Window win, Atom prop, Atom dest[], int *count_out) {
    if (!dpy || !prop) { *count_out = 0; return; }
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop_ret = NULL;
    int rc = XGetWindowProperty(dpy, win, prop, 0, (long) (MAX_ATOMS + 1), False,
                                AnyPropertyType,
                                &actual_type, &actual_format,
                                &nitems, &bytes_after, &prop_ret);
    if (rc != Success || prop_ret == NULL) {
        *count_out = 0;
        return;
    }
    int tocopy = clamp_count((int)nitems, MAX_ATOMS);
    if (actual_format == 32) {
        Atom *atoms = (Atom *)prop_ret;
        for (int i = 0; i < tocopy; ++i) dest[i] = atoms[i];
    } else {
        /* fallback: try to reinterpret as 32-bit chunks */
        long *ldata = (long *)prop_ret;
        for (int i = 0; i < tocopy; ++i) dest[i] = (Atom)ldata[i];
    }
    *count_out = tocopy;
    XFree(prop_ret);
}

/* fetch long-array style properties (cardinal) into dest. expects dest_len elements. */
static Bool fetch_longs(Display *dpy, Window win, Atom prop, long dest[], int dest_len, int *found) {
    if (!dpy || !prop) { *found = 0; return False; }
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop_ret = NULL;
    int rc = XGetWindowProperty(dpy, win, prop, 0, dest_len + 1, False,
                                AnyPropertyType,
                                &actual_type, &actual_format,
                                &nitems, &bytes_after, &prop_ret);
    if (rc != Success || prop_ret == NULL) {
        *found = 0;
        return False;
    }
    int tocopy = clamp_count((int)nitems, dest_len);
    if (actual_format == 32) {
        long *ldata = (long *)prop_ret;
        for (int i = 0; i < tocopy; ++i) dest[i] = ldata[i];
    } else {
        /* try interpret as bytes -> long */
        long *ldata = (long *)prop_ret;
        for (int i = 0; i < tocopy; ++i) dest[i] = ldata[i];
    }
    *found = tocopy;
    XFree(prop_ret);
    return True;
}

/* get window absolute geometry (x,y) on root and width/height */
static Bool get_window_geometry(Display *dpy, Window win, int *x, int *y, int *w, int *h) {
    if (!dpy) return False;
    Window root_return, child_return;
    int win_x, win_y;
    unsigned int width, height, border_width, depth;
    if (!XGetGeometry(dpy, win, &root_return, &win_x, &win_y, &width, &height, &border_width, &depth))
        return False;
    /* translate coordinates to root to get absolute x,y */
    int abs_x = 0, abs_y = 0;
    if (!XTranslateCoordinates(dpy, win, root_return, 0, 0, &abs_x, &abs_y, &child_return)) {
        *x = win_x; *y = win_y;
    } else {
        *x = abs_x; *y = abs_y;
    }
    *w = (int)width;
    *h = (int)height;
    return True;
}

/* save original properties for the given index (store in orig_props[idx]) */
static void save_original_props_for_index(int idx, Window win) {
    if (idx < 0 || idx > 3 || !dpy || win == 0) return;
    OrigWindowProps *op = &orig_props[idx];
    memset(op, 0, sizeof(*op));

    /* save atoms */
    fetch_atoms(dpy, win, _NET_WM_WINDOW_TYPE, op->type_atoms, &op->type_count);
    fetch_atoms(dpy, win, _NET_WM_STATE, op->state_atoms, &op->state_count);

    /* desktop */
    {
        Atom actual_type;
        int actual_format;
        unsigned long nitems, bytes_after;
        unsigned char *prop_ret = NULL;
        if (XGetWindowProperty(dpy, win, _NET_WM_DESKTOP, 0, 1, False, AnyPropertyType,
                               &actual_type, &actual_format,
                               &nitems, &bytes_after, &prop_ret) == Success && prop_ret) {
            if (nitems >= 1 && actual_format == 32) {
                long *ldata = (long *)prop_ret;
                op->desktop = ldata[0];
                op->has_desktop = True;
            }
            XFree(prop_ret);
        } else {
            op->has_desktop = False;
        }
    }

    /* strut_partial and strut if present */
    {
        int found = 0;
        if (fetch_longs(dpy, win, _NET_WM_STRUT_PARTIAL, op->strut_partial, 12, &found)) {
            op->has_strut_partial = found >= 1;
        } else op->has_strut_partial = False;

        if (fetch_longs(dpy, win, _NET_WM_STRUT, op->strut4, 4, &found)) {
            op->has_strut = found >= 1;
        } else op->has_strut = False;
    }

    /* geometry */
    {
        XWindowAttributes attr;
        if (XGetWindowAttributes(dpy, win, &attr)) {
            op->was_mapped = (attr.map_state == IsViewable);
        } else {
            op->was_mapped = False;
        }
        get_window_geometry(dpy, win, &op->x, &op->y, &op->w, &op->h);
    }

    op->saved = True;
}

/* restore original props stored for idx back to the window that was converted.
 * If there were no saved props, it will remove the strut properties.
 */
static void restore_original_props_for_index(int idx) {
    if (idx < 0 || idx > 3 || !dpy) return;
    OrigWindowProps *op = &orig_props[idx];
    Window win = struts[idx].win;
    if (win == 0) {
        /* nothing to restore to */
        op->saved = False;
        return;
    }

    if (op->saved) {
        /* restore type */
        if (op->type_count > 0) {
            XChangeProperty(dpy, win, _NET_WM_WINDOW_TYPE, XA_ATOM, 32, PropModeReplace,
                            (unsigned char *)op->type_atoms, op->type_count);
        } else {
            XDeleteProperty(dpy, win, _NET_WM_WINDOW_TYPE);
        }

        /* restore desktop */
        if (op->has_desktop) {
            long d = op->desktop;
            XChangeProperty(dpy, win, _NET_WM_DESKTOP, XA_CARDINAL, 32, PropModeReplace,
                            (unsigned char *)&d, 1);
        } else {
            XDeleteProperty(dpy, win, _NET_WM_DESKTOP);
        }

        /* restore struts */
        if (op->has_strut_partial) {
            XChangeProperty(dpy, win, _NET_WM_STRUT_PARTIAL, XA_CARDINAL, 32, PropModeReplace,
                            (unsigned char *)op->strut_partial, 12);
        } else {
            XDeleteProperty(dpy, win, _NET_WM_STRUT_PARTIAL);
        }

        if (op->has_strut) {
            XChangeProperty(dpy, win, _NET_WM_STRUT, XA_CARDINAL, 32, PropModeReplace,
                            (unsigned char *)op->strut4, 4);
        } else {
            XDeleteProperty(dpy, win, _NET_WM_STRUT);
        }

        /* restore state atoms */
        if (op->state_count > 0) {
            XChangeProperty(dpy, win, _NET_WM_STATE, XA_ATOM, 32, PropModeReplace,
                            (unsigned char *)op->state_atoms, op->state_count);
        } else {
            XDeleteProperty(dpy, win, _NET_WM_STATE);
        }

        /* restore geometry */
        XMoveResizeWindow(dpy, win, op->x, op->y, (unsigned int)op->w, (unsigned int)op->h);

        /* map/unmap to previous map state */
        if (op->was_mapped) XMapWindow(dpy, win);
        else XUnmapWindow(dpy, win);

    } else {
        /* nothing saved: clean strut properties */
        XDeleteProperty(dpy, win, _NET_WM_WINDOW_TYPE);
        XDeleteProperty(dpy, win, _NET_WM_DESKTOP);
        XDeleteProperty(dpy, win, _NET_WM_STRUT_PARTIAL);
        XDeleteProperty(dpy, win, _NET_WM_STRUT);
        XDeleteProperty(dpy, win, _NET_WM_STATE);
    }

    XFlush(dpy);

    /* clear saved struct */
    memset(op, 0, sizeof(*op));
}

/* find strut index that matches a given window */
static int find_strut_index_for_window(Window win) {
    for (int i = 0; i < 4; ++i) {
        if (struts[i].active && struts[i].win == win) return i;
    }
    return -1;
}

/* --------------------- conversion / creation / stopping ------------------ */

/* Convert an existing Window into a strut.
 * - Uses the window's width/height to determine strut thickness:
 *     - for top/bottom (idx 2/3) -> use window height as thickness
 *     - for left/right (idx 0/1) -> use window width as thickness
 * - X,Y is auto-generated to pin to the screen side (like your original create).
 * - Saves original properties (so stop_strut_for_window can restore them).
 * - Does NOT set the input-shape mask (per your request).
 */
static void convert_window_to_strut(Window win, int idx) {
    if (!dpy || win == 0 || idx < 0 || idx > 3) return;

    /* if a different strut occupies this index, stop it first */
    if (struts[idx].active && struts[idx].win != 0 && struts[idx].win != win) {
        /* if previously tracked strut had saved props, restore them */
        stop_strut_index(idx);
    }

    /* get window geometry to derive thickness */
    int w, h, cur_x, cur_y;
    if (!get_window_geometry(dpy, win, &cur_x, &cur_y, &w, &h)) {
        fprintf(stderr, "convert_window_to_strut: unable to fetch geometry of 0x%lx\n", (unsigned long)win);
        return;
    }

    /* decide width/height & position to pin to screen edge */
    int scr_w = DisplayWidth(dpy, screen);
    int scr_h = DisplayHeight(dpy, screen);

    int win_x = 0, win_y = 0, win_w = scr_w, win_h = scr_h;
    if (idx == 2) {           /* top: use window's height as thickness */
        win_h = h;
        /* center horizontally by using the window's width if available */
        win_w = w > 0 ? w : scr_w;
        /* position as original window x if that fits, else keep at 0 */
        win_x = (cur_x >= 0 && cur_x + win_w <= scr_w) ? cur_x : 0;
        win_y = 0;
    } else if (idx == 3) {    /* bottom */
        win_h = h;
        win_w = w > 0 ? w : scr_w;
        win_x = (cur_x >= 0 && cur_x + win_w <= scr_w) ? cur_x : 0;
        win_y = scr_h - win_h;
    } else if (idx == 0) {    /* left */
        win_w = w;
        win_h = h > 0 ? h : scr_h;
        win_x = 0;
        /* keep original y if fits */
        win_y = (cur_y >= 0 && cur_y + win_h <= scr_h) ? cur_y : 0;
    } else if (idx == 1) {    /* right */
        win_w = w;
        win_h = h > 0 ? h : scr_h;
        win_x = scr_w - win_w;
        win_y = (cur_y >= 0 && cur_y + win_h <= scr_h) ? cur_y : 0;
    }

    /* save original properties so we can restore later */
    save_original_props_for_index(idx, win);

    /* Set properties similar to a dock/strut */
    long strut_partial[12] = {0};
    long strut4[4] = {0};
    fill_strut_arrays_for_side(idx, win_x, win_y, win_w, win_h, strut_partial, strut4);

    Atom type_atom = _NET_WM_WINDOW_TYPE_DOCK;
    Atom states[2] = { _NET_WM_STATE_STICKY, _NET_WM_STATE_ABOVE };
    long desktop_all = -1;

    XMoveResizeWindow(dpy, win, win_x, win_y, (unsigned int)win_w, (unsigned int)win_h);

    XChangeProperty(dpy, win, _NET_WM_WINDOW_TYPE, XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)&type_atom, 1);
    XChangeProperty(dpy, win, _NET_WM_DESKTOP, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)&desktop_all, 1);
    XChangeProperty(dpy, win, _NET_WM_STRUT_PARTIAL, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)strut_partial, 12);
    XChangeProperty(dpy, win, _NET_WM_STRUT, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)strut4, 4);
    XChangeProperty(dpy, win, _NET_WM_STATE, XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)states, 2);

    /* map to ensure it's visible as a strut */
    XMapWindow(dpy, win);
    XFlush(dpy);

    /* record it in tracked struts */
    struts[idx].win = win;
    struts[idx].active = True;
    struts[idx].win_x = win_x;
    struts[idx].win_y = win_y;
    struts[idx].win_w = win_w;
    struts[idx].win_h = win_h;
    struts[idx].size = (idx == 2 || idx == 3) ? win_h : win_w;
    active_strut_count++;

    const char *names[4] = {"left","right","top","bottom"};
    printf("Converted window 0x%lx into strut on %s (geom %dx%d at %d,%d)\n",
           (unsigned long)win, names[idx], win_w, win_h, win_x, win_y);
}

/* convert_window_to_strut_with_size:
 *  - win: X window id to convert
 *  - idx: side index (0=left,1=right,2=top,3=bottom)
 *  - size_override: if >0 use that thickness; if <=0 use window's own width/height.
 *
 * Saves original properties into orig_props[idx] via save_original_props_for_index()
 * and records the result into struts[idx].
 */
static void convert_window_to_strut_with_size(Window win, int idx, int size_override) {
    if (!dpy || win == 0 || idx < 0 || idx > 3) return;

    /* if a different strut occupies this index, stop it first */
    if (struts[idx].active && struts[idx].win != 0 && struts[idx].win != win) {
        stop_strut_index(idx);
    }

    /* get window geometry to derive thickness (and fallback if we can't read) */
    int cur_x = 0, cur_y = 0, w = 0, h = 0;
    if (!get_window_geometry(dpy, win, &cur_x, &cur_y, &w, &h)) {
        /* if geometry fetch fails, fallback to screen-filling minimal values */
        int scr_w = DisplayWidth(dpy, screen);
        int scr_h = DisplayHeight(dpy, screen);
        cur_x = 0; cur_y = 0; w = scr_w; h = scr_h;
    }

    int scr_w = DisplayWidth(dpy, screen);
    int scr_h = DisplayHeight(dpy, screen);

    /* Determine final window geometry pinned to side.
     * If size_override > 0 use that as thickness (height for top/bottom, width for left/right).
     * Otherwise (size_override <= 0) use the window's own w/h as thickness.
     */
    int win_x = 0, win_y = 0, win_w = w, win_h = h;

    if (idx == 2) { /* top */
        win_h = (size_override > 0) ? size_override : (h > 0 ? h : scr_h);
        win_w = (w > 0) ? w : scr_w;
        /* try to preserve window's x if it fits, otherwise center/left */
        if (cur_x >= 0 && cur_x + win_w <= scr_w) win_x = cur_x;
        else {
            /* clamp width to screen if necessary */
            if (win_w > scr_w) win_w = scr_w;
            win_x = 0;
        }
        win_y = 0;
    } else if (idx == 3) { /* bottom */
        win_h = (size_override > 0) ? size_override : (h > 0 ? h : scr_h);
        win_w = (w > 0) ? w : scr_w;
        if (cur_x >= 0 && cur_x + win_w <= scr_w) win_x = cur_x;
        else {
            if (win_w > scr_w) win_w = scr_w;
            win_x = 0;
        }
        win_y = scr_h - win_h;
        if (win_y < 0) win_y = 0;
    } else if (idx == 0) { /* left */
        win_w = (size_override > 0) ? size_override : (w > 0 ? w : scr_w);
        win_h = (h > 0) ? h : scr_h;
        win_x = 0;
        if (cur_y >= 0 && cur_y + win_h <= scr_h) win_y = cur_y;
        else {
            if (win_h > scr_h) win_h = scr_h;
            win_y = 0;
        }
    } else if (idx == 1) { /* right */
        win_w = (size_override > 0) ? size_override : (w > 0 ? w : scr_w);
        win_h = (h > 0) ? h : scr_h;
        win_x = scr_w - win_w;
        if (win_x < 0) win_x = 0;
        if (cur_y >= 0 && cur_y + win_h <= scr_h) win_y = cur_y;
        else {
            if (win_h > scr_h) win_h = scr_h;
            win_y = 0;
        }
    }

    /* Save original props for this index (so restore later) */
    save_original_props_for_index(idx, win);

    /* Setup strut props like a dock */
    long strut_partial[12] = {0};
    long strut4[4] = {0};
    fill_strut_arrays_for_side(idx, win_x, win_y, win_w, win_h, strut_partial, strut4);

    Atom type_atom = _NET_WM_WINDOW_TYPE_DOCK;
    Atom states[2] = { _NET_WM_STATE_STICKY, _NET_WM_STATE_ABOVE };
    long desktop_all = -1;

    /* Move/resize and set properties. Note: we DO NOT set an input-shape mask here. */
    XMoveResizeWindow(dpy, win, win_x, win_y, (unsigned int)win_w, (unsigned int)win_h);

    XChangeProperty(dpy, win, _NET_WM_WINDOW_TYPE, XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)&type_atom, 1);
    XChangeProperty(dpy, win, _NET_WM_DESKTOP, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)&desktop_all, 1);
    XChangeProperty(dpy, win, _NET_WM_STRUT_PARTIAL, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)strut_partial, 12);
    XChangeProperty(dpy, win, _NET_WM_STRUT, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)strut4, 4);
    XChangeProperty(dpy, win, _NET_WM_STATE, XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)states, 2);

    XMapWindow(dpy, win);
    XFlush(dpy);

    /* record it in tracked struts */
    struts[idx].win = win;
    struts[idx].active = True;
    struts[idx].win_x = win_x;
    struts[idx].win_y = win_y;
    struts[idx].win_w = win_w;
    struts[idx].win_h = win_h;
    struts[idx].size = (idx == 2 || idx == 3) ? win_h : win_w;
    active_strut_count++;

    const char *names[4] = {"left","right","top","bottom"};
    printf("Converted window 0x%lx into strut on %s (geom %dx%d at %d,%d) override=%d\n",
           (unsigned long)win, names[idx], win_w, win_h, win_x, win_y, size_override);
}

/* Stop (revert) a strut for a particular Window.
 * If this window was converted via convert_window_to_strut, the original props
 * & geometry are restored. If no original saved props exist, strut props are removed.
 */
static void stop_strut_for_window(Window win) {
    if (!dpy || win == 0) return;

    int idx = find_strut_index_for_window(win);
    if (idx == -1) {
        /* not tracked: still try cleaning strut properties */
        XDeleteProperty(dpy, win, _NET_WM_WINDOW_TYPE);
        XDeleteProperty(dpy, win, _NET_WM_DESKTOP);
        XDeleteProperty(dpy, win, _NET_WM_STRUT_PARTIAL);
        XDeleteProperty(dpy, win, _NET_WM_STRUT);
        XDeleteProperty(dpy, win, _NET_WM_STATE);
        XFlush(dpy);
        printf("stop_strut_for_window: cleaned properties on 0x%lx (not tracked)\n", (unsigned long)win);
        return;
    }

    /* restore saved props for that index */
    restore_original_props_for_index(idx);

    /* reset struts[] tracking entry */
    struts[idx].win = 0;
    struts[idx].active = False;
    struts[idx].win_x = 0;
    struts[idx].win_y = 0;
    struts[idx].win_w = 0;
    struts[idx].win_h = 0;
    struts[idx].size = 0;
    active_strut_count = (active_strut_count > 0) ? active_strut_count - 1 : 0;

    const char *names[4] = {"left","right","top","bottom"};
    printf("Strut on %s reverted for window 0x%lx\n", names[idx], (unsigned long)win);
}

/* Updated create_or_replace_strut_index: updates existing strut window if present,
 * otherwise creates a new X window (original behavior). This function leaves the
 * possibility to create via new X window (create_strut) or convert an existing
 * window (convert_window_to_strut) as separate operations.
 */
static void create_or_replace_strut_index(int idx, int size) {
    if (idx < 0 || idx > 3) return;
    if (size <= 0) {
        fprintf(stderr, "create_strut: size must be > 0\n");
        return;
    }

    int scr_w = DisplayWidth(dpy, screen);
    int scr_h = DisplayHeight(dpy, screen);

    int win_x = 0, win_y = 0, win_w = scr_w, win_h = scr_h;
    if (idx == 2) {
        win_h = size;
    } else if (idx == 3) {
        win_y = scr_h - size; win_h = size;
    } else if (idx == 0) {
        win_w = size;
    } else if (idx == 1) {
        win_x = scr_w - size; win_w = size;
    }

    /* If there is already an active strut window for this index, update it in-place */
    if (struts[idx].active && struts[idx].win != 0) {
        Window win = struts[idx].win;

        /* move/resize */
        XMoveResizeWindow(dpy, win, win_x, win_y, (unsigned int)win_w, (unsigned int)win_h);

        /* update properties */
        long strut_partial[12];
        long strut4[4];
        fill_strut_arrays_for_side(idx, win_x, win_y, win_w, win_h, strut_partial, strut4);

        Atom type_atom = _NET_WM_WINDOW_TYPE_DOCK;
        Atom states[2] = { _NET_WM_STATE_STICKY, _NET_WM_STATE_ABOVE };
        long desktop_all = -1;

        XChangeProperty(dpy, win, _NET_WM_WINDOW_TYPE, XA_ATOM, 32, PropModeReplace,
                        (unsigned char *)&type_atom, 1);
        XChangeProperty(dpy, win, _NET_WM_DESKTOP, XA_CARDINAL, 32, PropModeReplace,
                        (unsigned char *)&desktop_all, 1);
        XChangeProperty(dpy, win, _NET_WM_STRUT_PARTIAL, XA_CARDINAL, 32, PropModeReplace,
                        (unsigned char *)strut_partial, 12);
        XChangeProperty(dpy, win, _NET_WM_STRUT, XA_CARDINAL, 32, PropModeReplace,
                        (unsigned char *)strut4, 4);
        XChangeProperty(dpy, win, _NET_WM_STATE, XA_ATOM, 32, PropModeReplace,
                        (unsigned char *)states, 2);

        XMapWindow(dpy, win);
        XFlush(dpy);

        /* update tracked geometry */
        struts[idx].win_x = win_x;
        struts[idx].win_y = win_y;
        struts[idx].win_w = win_w;
        struts[idx].win_h = win_h;
        struts[idx].size = size;

        const char *names[4] = {"left","right","top","bottom"};
        printf("Updated existing strut window 0x%lx on %s to %dpx\n", (unsigned long)win, names[idx], size);
        return;
    }

    /* No active window to update: create a new one (same as your original logic) */
    XSetWindowAttributes attr;
    attr.colormap = colormap;
    attr.border_pixel = black_pixel;
    attr.background_pixel = black_pixel;
    attr.override_redirect = False;
    unsigned long valuemask = CWBackPixel | CWBorderPixel | CWColormap | CWOverrideRedirect;

    Window win = XCreateWindow(dpy, root_window, win_x, win_y, (unsigned int)win_w, (unsigned int)win_h,
                               0, depth, InputOutput, visual, valuemask, &attr);
    if (!win) {
        fprintf(stderr, "create_strut: Failed to create window\n");
        return;
    }

    long strut_partial[12];
    long strut4[4];
    fill_strut_arrays_for_side(idx, win_x, win_y, win_w, win_h, strut_partial, strut4);

    Atom type = _NET_WM_WINDOW_TYPE_DOCK;
    Atom states[2] = { _NET_WM_STATE_STICKY, _NET_WM_STATE_ABOVE };
    long desktop_all = -1;

    XChangeProperty(dpy, win, _NET_WM_WINDOW_TYPE, XA_ATOM, 32, PropModeReplace, (unsigned char *)&type, 1);
    XChangeProperty(dpy, win, _NET_WM_DESKTOP, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&desktop_all, 1);
    XChangeProperty(dpy, win, _NET_WM_STRUT_PARTIAL, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)strut_partial, 12);
    XChangeProperty(dpy, win, _NET_WM_STRUT, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)strut4, 4);
    XChangeProperty(dpy, win, _NET_WM_STATE, XA_ATOM, 32, PropModeReplace, (unsigned char *)states, 2);

    XMapWindow(dpy, win);
    XMoveResizeWindow(dpy, win, win_x, win_y, (unsigned int)win_w, (unsigned int)win_h);

    XFlush(dpy);

    struts[idx].win = win;
    struts[idx].active = true;
    struts[idx].win_x = win_x;
    struts[idx].win_y = win_y;
    struts[idx].win_w = win_w;
    struts[idx].win_h = win_h;
    struts[idx].size = size;
    active_strut_count++;

    {
        const char *names[4] = {"left","right","top","bottom"};
        printf("Strut created on %s (%dpx)\n", names[idx], size);
    }
}

static Window get_active_window_id(void) {
    if (_NET_ACTIVE_WINDOW == None) return 0;

    Atom actual_type;
    int actual_format;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *prop = NULL;

    int rc = XGetWindowProperty(dpy, root_window, _NET_ACTIVE_WINDOW, 0, 1, False, XA_WINDOW, &actual_type, &actual_format,&nitems, &bytes_after, &prop);
    if (rc != Success || !prop || nitems == 0) {
        if (prop) XFree(prop);
        return 0;
    }

    Window w = 0;
    if (nitems >= 1) w = *((Window*)prop);
    XFree(prop);
    return w;
}

static bool window_has_wm_class(Window w) {
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop = NULL;
    if (XGetWindowProperty(dpy, w, WM_CLASS, 0, (~0L), False, AnyPropertyType, &actual_type, &actual_format, &nitems, &bytes_after, &prop) == Success && prop) {
        XFree(prop);
        return true;
    }
    if (prop) XFree(prop);
    return false;
}

static bool window_has_name(Window w) {
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop = NULL;
    if (XGetWindowProperty(dpy, w, _NET_WM_NAME, 0, (~0L), False, UTF8_STRING, &actual_type, &actual_format, &nitems, &bytes_after, &prop) == Success && prop) {
        XFree(prop);
        return true;
    }
    if (prop) XFree(prop);

    char *name = NULL;
    if (XFetchName(dpy, w, &name) > 0 && name) {
        XFree(name);
        return true;
    }
    return false;
}

static bool is_likely_client(Window w) {
    return window_has_wm_class(w) || window_has_name(w);
}

static Window find_client_window(Window w, int max_depth) {
    if (!dpy) return 0;
    if (max_depth < 0) max_depth = 3;

    if (is_likely_client(w)) return w;

    Window root_ret, parent_ret;
    Window *children = NULL;
    unsigned int nchildren = 0;

    typedef struct { Window w; int depth; } Item;
    size_t cap = 128;
    size_t size = 0;
    Item *q = malloc(sizeof(Item) * cap);
    if (!q) return 0;

    q[size++] = (Item){ .w = w, .depth = 0 };
    size_t idx = 0;

    while (idx < size) {
        Item it = q[idx++];
        if (it.depth >= max_depth) continue;

        if (XQueryTree(dpy, it.w, &root_ret, &parent_ret, &children, &nchildren)) {
            for (unsigned int i = 0; i < nchildren; ++i) {
                Window c = children[i];
                if (is_likely_client(c)) {
                    if (children) XFree(children);
                    Window found = c;
                    free(q);
                    return found;
                }
                if (size + 1 >= cap) {
                    size_t ncap = cap * 2;
                    Item *nq = realloc(q, sizeof(Item) * ncap);
                    if (!nq) {
                        if (children) XFree(children);
                        free(q);
                        return 0;
                    }
                    q = nq;
                    cap = ncap;
                }
                q[size++] = (Item){ .w = c, .depth = it.depth + 1 };
            }
            if (children) { XFree(children); children = NULL; nchildren = 0; }
        }
    }

    free(q);
    return 0;
}

static char* get_window_name(Window w) {
    if (!dpy) return NULL;
    XWindowAttributes attrs;
    if (XGetWindowAttributes(dpy, w, &attrs) == 0) return NULL;
    Atom name_atom = _NET_WM_NAME;
    Atom utf8 = UTF8_STRING;
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop = NULL;

    if (name_atom != None && utf8 != None) {
        int rc = XGetWindowProperty(dpy, w, name_atom, 0, (~0L), False, utf8, &actual_type, &actual_format, &nitems, &bytes_after, &prop);
        if (rc == Success && prop) {
            char *res = strdup((char*)prop);
            XFree(prop);
            return res;
        }
        if (prop) { XFree(prop); prop = NULL; }
    }

    char *name = NULL;
    if (XFetchName(dpy, w, &name) > 0 && name) {
        char *res = strdup(name);
        XFree(name);
        return res;
    }
    if (name) XFree(name);
    return NULL;
}

static bool set_decorations_for_xid(Window xwindow, bool decorate) {
    struct {
        unsigned long flags;
        unsigned long functions;
        unsigned long decorations;
        long inputMode;
        unsigned long status;
    } hints;

    memset(&hints, 0, sizeof(hints));
    const unsigned long MWM_HINTS_DECORATIONS = (1UL << 1);

    hints.flags = MWM_HINTS_DECORATIONS;
    hints.decorations = decorate ? 1 : 0;

    XChangeProperty(dpy, xwindow, _MOTIF_WM_HINTS, _MOTIF_WM_HINTS, 32, PropModeReplace, (unsigned char*)&hints, 5);

    XWindowAttributes attrs;
    if (XGetWindowAttributes(dpy, xwindow, &attrs) == 0) return false;
    Window root = RootWindowOfScreen(attrs.screen);

    XEvent xev;
    memset(&xev, 0, sizeof(xev));
    xev.xclient.type = ClientMessage;
    xev.xclient.serial = 0;
    xev.xclient.send_event = True;
    xev.xclient.display = dpy;
    xev.xclient.window = xwindow;
    xev.xclient.message_type = _NET_WM_STATE;
    xev.xclient.format = 32;

    const long _NET_WM_STATE_ADD = 1;
    xev.xclient.data.l[0] = _NET_WM_STATE_ADD;
    xev.xclient.data.l[1] = (long)_OB_WM_STATE_UNDECORATED;
    xev.xclient.data.l[2] = 0;
    xev.xclient.data.l[3] = 0;
    xev.xclient.data.l[4] = 0;

    XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &xev);

    XSync(dpy, False);
    return true;
}

static void handle_new_window(Window w) {
    if (!dpy) return;
    usleep(50 * 1000);
    XWindowAttributes attrs;
    if (XGetWindowAttributes(dpy, w, &attrs) == 0) return;
    Window client = find_client_window(w, 4);
    if (!client) {
        if (is_likely_client(w)) client = w;
    }
    if (!client) return;
    if (XGetWindowAttributes(dpy, client, &attrs) == 0) return;
    char *title = get_window_name(client);
    //printf("[xakard]: window opened xid=0x%lx title='%s'\n", (unsigned long)client, title ? title : "(no name)");
    if (title) free(title);
    //DO SOMETHING
}

static int get_geometry(Window active_id, Geometry *out) {
    if (!active_id) return 0;

    Window cur = active_id;
    Window parent, root_ret, *children = NULL;
    unsigned int nchildren;
    XWindowAttributes chain[MAX_CHAIN_DEPTH];
    int depth_chain = 0;
    XWindowAttributes attr;

    while (cur && depth_chain < MAX_CHAIN_DEPTH) {
        if (!XGetWindowAttributes(dpy, cur, &attr)) break;
        chain[depth_chain++] = attr;

        if (!XQueryTree(dpy, cur, &root_ret, &parent, &children, &nchildren)) break;
        if (children) { XFree(children); children = NULL; }
        if (!parent || parent == root_ret) break;
        cur = parent;
    }

    if (depth_chain == 0) return 0;

    out->object = active_id;
    out->width = chain[0].width;
    out->height = chain[0].height;
    out->ancestor_x = chain[depth_chain-1].x;
    out->ancestor_y = chain[depth_chain-1].y;
    out->ancestor_width = chain[depth_chain-1].width;
    out->ancestor_height = chain[depth_chain-1].height;
    return 1;
}

static void move_resize(Window w, int x, int y, int width, int height) {
    if (!w) return;
    XMoveResizeWindow(dpy, w, x, y, width, height);
    XFlush(dpy);
}

static int get_monitor_for_window(const Geometry *geom, int *mx, int *my, int *mw, int *mh) {
    if (!XineramaIsActive(dpy)) return 0;
    int num_screens = 0;
    XineramaScreenInfo *screens = XineramaQueryScreens(dpy, &num_screens);
    if (!screens) return 0;
    int i;
    for (i = 0; i < num_screens; i++) {
        XineramaScreenInfo s = screens[i];
        if (geom->ancestor_x >= s.x_org && geom->ancestor_x < s.x_org + s.width &&
            geom->ancestor_y >= s.y_org && geom->ancestor_y < s.y_org + s.height) {
            *mx = s.x_org;
            *my = s.y_org;
            *mw = s.width;
            *mh = s.height;
            XFree(screens);
            return 1;
        }
    }
    XFree(screens);
    return 0;
}

static int find_neighbor_monitor(int cur_mx, int cur_my, int cur_mw, int cur_mh, const char *direction, int *tx, int *ty, int *tw, int *th) {
    if (!XineramaIsActive(dpy)) return 0;
    int ns = 0;
    XineramaScreenInfo *screens = XineramaQueryScreens(dpy, &ns);
    if (!screens) return 0;

    double cur_cx = cur_mx + cur_mw / 2.0;
    double cur_cy = cur_my + cur_mh / 2.0;

    int best = -1;
    double best_metric = 0.0;
    int i;
    for (i = 0; i < ns; ++i) {
        XineramaScreenInfo s = screens[i];
        if (s.x_org == cur_mx && s.y_org == cur_my && s.width == cur_mw && s.height == cur_mh) continue;
        double cx = s.x_org + s.width / 2.0;
        double cy = s.y_org + s.height / 2.0;

        int valid = 0;
        double primary = 1e12, secondary = 1e12;

        if (strcmp(direction, "left") == 0) {
            if (cx < cur_cx) { valid = 1; primary = cur_cx - cx; secondary = fabs(cur_cy - cy); }
        } else if (strcmp(direction, "right") == 0) {
            if (cx > cur_cx) { valid = 1; primary = cx - cur_cx; secondary = fabs(cur_cy - cy); }
        } else if (strcmp(direction, "up") == 0) {
            if (cy < cur_cy) { valid = 1; primary = cur_cy - cy; secondary = fabs(cur_cx - cx); }
        } else if (strcmp(direction, "down") == 0) {
            if (cy > cur_cy) { valid = 1; primary = cy - cur_cy; secondary = fabs(cur_cx - cx); }
        }

        if (!valid) continue;
        double metric = primary * 1000.0 + secondary;
        if (best == -1 || metric < best_metric) {
            best = i;
            best_metric = metric;
        }
    }

    if (best == -1) {
        XFree(screens);
        return 0;
    }

    *tx = screens[best].x_org;
    *ty = screens[best].y_org;
    *tw = screens[best].width;
    *th = screens[best].height;
    XFree(screens);
    return 1;
}

static void generate_dimensions(const Geometry *geom, const char *action, int mx, int my, int mw, int mh, int *rx, int *ry, int *rw, int *rh) {
    int overhead_x = geom->ancestor_width - geom->width;
    int overhead_y = geom->ancestor_height - geom->height;

    int full_width = mw - overhead_x;
    int full_height = mh - overhead_y;
    int half_width = (mw / 2) - overhead_x;
    int half_height = (mh / 2) - overhead_y;

    int x_new = 0, y_new = 0, w_new = 0, h_new = 0;

    if (strcmp(action, "center") == 0) {
        w_new = (int)(0.75 * mw);
        h_new = (int)(0.75 * mh);
        x_new = (mw - w_new - overhead_x) / 2;
        y_new = (mh - h_new - overhead_y) / 2;
    } else if (strcmp(action, "fullscreen") == 0) {
        x_new = 0; y_new = 0; w_new = full_width; h_new = full_height;
    } else if (strcmp(action, "left") == 0 || strcmp(action, "right") == 0) {
        x_new = (strcmp(action, "left") == 0) ? 0 : (mw / 2);
        w_new = half_width;
        if ((geom->width <= half_width && geom->ancestor_x == (mx + x_new) && geom->height >= half_height)
            || geom->height == full_height) {
            y_new = 0; h_new = full_height;
        } else {
            int local_y = geom->ancestor_y - my;
            y_new = (local_y < (mh/2)) ? 0 : (mh/2);
            h_new = half_height;
        }
    } else if (strcmp(action, "up") == 0 || strcmp(action, "down") == 0) {
        y_new = (strcmp(action, "up") == 0) ? 0 : (mh / 2);
        h_new = half_height;
        if ((geom->height <= half_height && geom->ancestor_y == (my + y_new) && geom->width >= half_width)
            || geom->width == full_width) {
            x_new = 0; w_new = full_width;
        } else {
            int local_x = geom->ancestor_x - mx;
            x_new = (local_x < (mw/2)) ? 0 : (mw/2);
            w_new = half_width;
        }
    }

    *rx = x_new;
    *ry = y_new;
    *rw = w_new;
    *rh = h_new;
}

static void send_fullscreen_toggle(Window win) {
    if (_NET_WM_STATE == None || _NET_WM_STATE_FULLSCREEN == None) return;

    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop = NULL;
    int is_fs = 0;

    if (XGetWindowProperty(dpy, win, _NET_WM_STATE, 0, 1024, False, XA_ATOM, &actual_type, &actual_format, &nitems, &bytes_after, &prop) == Success && prop) {
        Atom *atoms = (Atom*)prop;
        unsigned long i;
        for (i = 0; i < nitems; ++i) if (atoms[i] == _NET_WM_STATE_FULLSCREEN) { is_fs = 1; break; }
        XFree(prop);
    }

    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xclient.type = ClientMessage;
    ev.xclient.window = win;
    ev.xclient.message_type = _NET_WM_STATE;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = is_fs ? 0 : 1;
    ev.xclient.data.l[1] = _NET_WM_STATE_FULLSCREEN;
    ev.xclient.data.l[2] = 0;
    ev.xclient.data.l[3] = 0;
    ev.xclient.data.l[4] = 0;

    XSendEvent(dpy, root_window, False, SubstructureNotifyMask | SubstructureRedirectMask, &ev);
    XFlush(dpy);
}

static TileMode detect_tiling_mode(const Geometry *geom, int mx, int my, int mw, int mh) {
    const int eps = 12;
    int overhead_x = geom->ancestor_width - geom->width;
    int overhead_y = geom->ancestor_height - geom->height;

    int full_w = mw - overhead_x;
    int full_h = mh - overhead_y;
    int half_w = (mw / 2) - overhead_x;
    int half_h = (mh / 2) - overhead_y;
    int center_w = (int)(0.75 * mw);
    int center_h = (int)(0.75 * mh);

    int local_x = geom->ancestor_x - mx;
    int local_y = geom->ancestor_y - my;

    if (abs(geom->width - full_w) <= eps && abs(geom->height - full_h) <= eps) return MODE_FULLSCREEN;
    if (abs(geom->width - center_w) <= eps && abs(geom->height - center_h) <= eps) return MODE_CENTER;

    if (abs(geom->width - half_w) <= eps && abs(geom->height - full_h) <= eps) {
        if (local_x < mw/2) return MODE_HALF_LEFT;
        else return MODE_HALF_RIGHT;
    }
    if (abs(geom->height - half_h) <= eps && abs(geom->width - full_w) <= eps) {
        if (local_y < mh/2) return MODE_HALF_UP;
        else return MODE_HALF_DOWN;
    }

    if (abs(geom->width - half_w) <= eps && abs(geom->height - half_h) <= eps) {
        if (local_x < mw/2 && local_y < mh/2) return MODE_QUARTER_TL;
        if (local_x >= mw/2 && local_y < mh/2) return MODE_QUARTER_TR;
        if (local_x < mw/2 && local_y >= mh/2) return MODE_QUARTER_BL;
        return MODE_QUARTER_BR;
    }
    return MODE_UNKNOWN;
}

static void preserve_mode_and_move(const Geometry *geom, TileMode mode, int tx, int ty, int tw, int th) {
    int overhead_x = geom->ancestor_width - geom->width;
    int overhead_y = geom->ancestor_height - geom->height;

    int t_full_w = tw - overhead_x;
    int t_full_h = th - overhead_y;
    int t_half_w = (tw / 2) - overhead_x;
    int t_half_h = (th / 2) - overhead_y;

    int nx = 0, ny = 0, nw = 0, nh = 0;

    switch (mode) {
        case MODE_FULLSCREEN:
            nx = 0; ny = 0; nw = t_full_w; nh = t_full_h;
            break;
        case MODE_CENTER:
            nw = (int)(0.75 * tw);
            nh = (int)(0.75 * th);
            nx = (tw - nw - overhead_x) / 2;
            ny = (th - nh - overhead_y) / 2;
            break;
        case MODE_HALF_LEFT:
            nx = 0; ny = 0; nw = t_half_w; nh = t_full_h;
            break;
        case MODE_HALF_RIGHT:
            nx = tw / 2; ny = 0; nw = t_half_w; nh = t_full_h;
            break;
        case MODE_HALF_UP:
            nx = 0; ny = 0; nw = t_full_w; nh = t_half_h;
            break;
        case MODE_HALF_DOWN:
            nx = 0; ny = th / 2; nw = t_full_w; nh = t_half_h;
            break;
        case MODE_QUARTER_TL:
            nx = 0; ny = 0; nw = t_half_w; nh = t_half_h;
            break;
        case MODE_QUARTER_TR:
            nx = tw / 2; ny = 0; nw = t_half_w; nh = t_half_h;
            break;
        case MODE_QUARTER_BL:
            nx = 0; ny = th / 2; nw = t_half_w; nh = t_half_h;
            break;
        case MODE_QUARTER_BR:
            nx = tw / 2; ny = th / 2; nw = t_half_w; nh = t_half_h;
            break;
        default:
            nw = (int)(0.75 * tw);
            nh = (int)(0.75 * th);
            nx = (tw - nw - overhead_x) / 2;
            ny = (th - nh - overhead_y) / 2;
            break;
    }

    move_resize(geom->object, tx + nx, ty + ny, nw, nh);
}

static void preserve_geom_and_move(Geometry *geom, const char *direction, int mx, int my, int tx, int ty, int tw, int th) {
    int rel_x = geom->ancestor_x - mx;
    int rel_y = geom->ancestor_y - my;
    int new_x = tx + rel_x;
    int new_y = ty + rel_y;
    if (new_x < tx) new_x = tx;
    if (new_y < ty) new_y = ty;
    if (new_x + geom->width > tx + tw) new_x = tx + tw - geom->width;
    if (new_y + geom->height > ty + th) new_y = ty + th - geom->height;
    move_resize(geom->object, new_x, new_y, geom->width, geom->height);
    return;
}

static void unpreserved_move(Geometry *geom, const char *direction, int mx, int my, int mw, int mh) {
    int tx, ty, tw, th;
    if (find_neighbor_monitor(mx, my, mw, mh, direction, &tx, &ty, &tw, &th)) {
        int overhead_x = geom->ancestor_width - geom->width;
        int overhead_y = geom->ancestor_height - geom->height;
        int t_full_w = tw - overhead_x;
        int t_full_h = th - overhead_y;
        int t_half_w = (tw / 2) - overhead_x;
        int t_half_h = (th / 2) - overhead_y;

        int tx_new = 0, ty_new = 0, tw_new = 0, th_new = 0;

        if (strcmp(direction, "left") == 0) {
            tx_new = tw / 2;
            tw_new = t_half_w;
            th_new = t_full_h;
        } else if (strcmp(direction, "right") == 0) {
            tw_new = t_half_w;
            th_new = t_full_h;
        } else if (strcmp(direction, "up") == 0) {
            ty_new = th / 2;
            th_new = t_half_h;
            tw_new = t_full_w;
        } else {
            th_new = t_half_h;
            tw_new = t_full_w;
        }
        move_resize(geom->object, tx + tx_new, ty + ty_new, tw_new, th_new);
        return;
    }
}

static void handle_action(const char *action_in) {
    if (!action_in) return;
    char action_buf[256];
    strncpy(action_buf, action_in, sizeof(action_buf)-1);
    action_buf[sizeof(action_buf)-1] = '\0';

    char *cmd = strtok(action_buf, " \t");
    if (!cmd) return;

    if (strcmp(cmd, "tile") == 0){
        char *direction = strtok(NULL, " \t");

        if (!direction ||
            (strcmp(direction, "center") != 0 &&
             strcmp(direction, "fullscreen") != 0 &&
             strcmp(direction, "left") != 0 &&
             strcmp(direction, "right") != 0 &&
             strcmp(direction, "up") != 0 &&
             strcmp(direction, "down") != 0 &&
             strcmp(direction, "wm_fullscreen") != 0)) return;

        Window active = get_active_window_id();
        if (!active) return;

        if (strcmp(direction, "wm_fullscreen") == 0) {
            send_fullscreen_toggle(active);
            return;
        }

        Geometry geom;
        if (!get_geometry(active, &geom)) return;

        int mx = 0, my = 0, mw = screen_width, mh = screen_height;
        int have_monitor = get_monitor_for_window(&geom, &mx, &my, &mw, &mh);

        char *preserve = strtok(NULL, " \t");

        if (preserve) {
            int tx, ty, tw, th;
            if (!find_neighbor_monitor(mx, my, mw, mh, direction, &tx, &ty, &tw, &th)) {
                char new_action[256];
                snprintf(new_action, sizeof(new_action), "tile %s", direction);
                handle_action(new_action);
                return;
            }

            TileMode mode = detect_tiling_mode(&geom, mx, my, mw, mh);

            if (strcmp(preserve, "preserve_geom") == 0 || mode == MODE_UNKNOWN) {
                preserve_geom_and_move(&geom, direction, mx, my, tx, ty, tw, th);
                return;
            }

            if (strcmp(preserve, "preserve_mode") == 0) {
                preserve_mode_and_move(&geom, mode, tx, ty, tw, th);
                return;
            }
        }

        int nx, ny, nw, nh;
        generate_dimensions(&geom, direction, mx, my, mw, mh, &nx, &ny, &nw, &nh);

        if (have_monitor && geom.ancestor_x == nx + mx && geom.ancestor_y == ny + my && geom.width == nw && geom.height == nh &&
            (strcmp(direction, "left") == 0 || strcmp(direction, "right") == 0 || strcmp(direction, "up") == 0 || strcmp(direction, "down") == 0)) {
            unpreserved_move(&geom, direction, mx, my, mw, mh);
            return;
        }
        move_resize(geom.object, nx + mx, ny + my, nw, nh);
    } else if (strcmp(cmd, "undecorate") == 0) {
        char *window_id_str = strtok(NULL, " \t");
        if (!window_id_str) return;
        unsigned long window_id = strtoul(window_id_str, NULL, 0);
        Window xid = (Window)window_id;
        Window client = find_client_window(xid, 4);
        if (!client) client = xid;
        bool result = set_decorations_for_xid(client, false);
        if (result) {
            printf("Window %lu undecorated successfully.\n", window_id);
        } else {
            fprintf(stderr, "Failed to undecorate window %lu.\n", window_id);
        }
    } else if (strcmp(cmd, "strut") == 0) {
        char *sub = strtok(NULL, " \t");
        if (!sub) return;
    
        /* helper to map side string to index if the token itself is a side */
        int parse_side_idx = -1;
        if (strcmp(sub, "left") == 0 || strcmp(sub, "right") == 0 ||
            strcmp(sub, "top") == 0 || strcmp(sub, "bottom") == 0) {
            parse_side_idx = side_to_index(sub);
        }
    
        if (strcmp(sub, "convert") == 0) {
            /* support:
            *   strut convert <xid> <side> <size>
            *   strut convert <xid> <side>        (use window's w/h)
            */
            char *xid_s = strtok(NULL, " \t");
            char *side = strtok(NULL, " \t");
            char *size_s = strtok(NULL, " \t"); /* optional */
    
            if (!xid_s || !side) {
                fprintf(stderr, "usage: strut convert <xid> <left|right|top|bottom> [size]\n");
                return;
            }
    
            unsigned long xid = strtoul(xid_s, NULL, 0);
            int idx = side_to_index(side);
            if (idx < 0) { fprintf(stderr, "invalid side: %s\n", side); return; }
    
            int size_override = 0; /* 0 -> use window w/h */
            if (size_s) {
                size_override = atoi(size_s);
                /* keep <=0 semantics: <=0 => use window size */
            }
    
            convert_window_to_strut_with_size((Window)xid, idx, size_override);
            return;
        }
        else if (strcmp(sub, "stop") == 0) {
            /* strut stop <xid>    -> revert by window id
            * strut stop <side>   -> revert by side (old behaviour)
            */
            char *arg = strtok(NULL, " \t");
            if (!arg) { fprintf(stderr, "usage: strut stop <xid|side>\n"); return; }
    
            /* detect numeric xid (supports 0x hex and decimal) */
            char *endptr = NULL;
            unsigned long maybe_xid = strtoul(arg, &endptr, 0);
            if (endptr != arg && *endptr == '\0') {
                stop_strut_for_window((Window)maybe_xid);
                return;
            } else {
                int idx = side_to_index(arg);
                if (idx < 0) { fprintf(stderr, "invalid side: %s\n", arg); return; }
                stop_strut_index(idx);
                return;
            }
        }
        else if (strcmp(sub, "new") == 0) {
            /* strut new <side> <size> */
            char *side = strtok(NULL, " \t");
            char *size_s = strtok(NULL, " \t");
            if (!side || !size_s) { fprintf(stderr, "usage: strut new <side> <size>\n"); return; }
            int idx = side_to_index(side);
            if (idx < 0) { fprintf(stderr, "invalid side: %s\n", side); return; }
            int size = atoi(size_s);
            if (size <= 0) { fprintf(stderr, "size must be > 0\n"); return; }
            create_or_replace_strut_index(idx, size);
            return;
        }
        else if (parse_side_idx >= 0) {
            /* backwards-compatible: strut <side> <size>    or  strut <side> stop */
            char *arg = strtok(NULL, " \t");
            if (!arg) { fprintf(stderr, "usage: strut <side> <size|stop>\n"); return; }
    
            if (strcmp(arg, "stop") == 0) {
                stop_strut_index(parse_side_idx);
                return;
            } else {
                int size = atoi(arg);
                if (size <= 0) { fprintf(stderr, "size must be > 0\n"); return; }
                create_or_replace_strut_index(parse_side_idx, size);
                return;
            }
        }
        else {
            fprintf(stderr, "unknown subcommand for 'strut': %s\n", sub);
            return;
        }
    } else if (strcmp(cmd, "brightness") == 0) {
        char *tok1 = strtok(NULL, " \t");
        char *tok2 = NULL;
        if (!tok1) {
            fprintf(stderr, "[xakard] set_brightness requires an argument (percentage or device + percentage)\n");
            return;
        }

        tok2 = strtok(NULL, " \t");

        const char *devname = NULL;
        char valuebuf[64];

        if (tok2) {
            devname = tok1;
            if (strlen(tok2) >= sizeof(valuebuf)) {
                fprintf(stderr, "[xakard] percentage value too long\n");
                return;
            }
            strncpy(valuebuf, tok2, sizeof(valuebuf)-1);
            valuebuf[sizeof(valuebuf)-1] = '\0';
        } else {
            if (strlen(tok1) >= sizeof(valuebuf)) {
                fprintf(stderr, "[xakard] percentage value too long\n");
                return;
            }
            strncpy(valuebuf, tok1, sizeof(valuebuf)-1);
            valuebuf[sizeof(valuebuf)-1] = '\0';
        }

        char *vptr = trim(valuebuf);
        if (!vptr || !*vptr) {
            fprintf(stderr, "[xakard] invalid percentage value\n");
            return;
        }

        bool is_delta = false;
        if (*vptr == '+' || *vptr == '-') is_delta = true;

        size_t vlen = strlen(vptr);
        bool has_percent_sign = (vlen > 0 && vptr[vlen-1] == '%');

        char tmp[64];
        strncpy(tmp, vptr, sizeof(tmp)-1);
        tmp[sizeof(tmp)-1] = '\0';
        if (has_percent_sign) tmp[vlen-1] = '\0';

        char *endptr = NULL;
        float parsed = strtof(tmp, &endptr);
        if (endptr == tmp) {
            fprintf(stderr, "[xakard] invalid numeric percentage: %s\n", vptr);
            return;
        }

        bool final_is_delta = is_delta;
        float value = parsed;
        bool ok = false;

        if (final_is_delta) {
            float delta = value;
            if (devname) {
                float curr_perc = 0.0f;
                if (!get_device_percent(devname, &curr_perc)) {
                    fprintf(stderr, "[xakard] set_brightness: could not read device '%s'\n", devname);
                    return;
                }
                float target_perc = curr_perc + delta;
                if (target_perc < 0.0f) target_perc = 0.0f;
                if (target_perc > 100.0f) target_perc = 100.0f;
                ok = set_brightness_for_device(devname, target_perc);
                if (ok) printf("[xakard] set_brightness: device '%s' -> %.2f%% (was %.2f%%)\n", devname, target_perc, curr_perc);
                else fprintf(stderr, "[xakard] set_brightness: failed for device '%s'\n", devname);
            } else {
                ok = adjust_brightness_all_delta(delta);
                if (ok) printf("[xakard] set_brightness: all backlights adjusted by %+g%%\n", delta);
                else fprintf(stderr, "[xakard] set_brightness: no backlights updated\n");
            }
        } else {
            if (value < 0.0f) value = 0.0f;
            if (value > 100.0f) value = 100.0f;
            if (devname) {
                ok = set_brightness_for_device(devname, value);
                if (ok) printf("[xakard] set_brightness: device '%s' -> %.2f%%\n", devname, value);
                else fprintf(stderr, "[xakard] set_brightness: failed for device '%s'\n", devname);
            } else {
                ok = set_brightness(value);
                if (ok) printf("[xakard] set_brightness: all backlights -> %.2f%%\n", value);
                else fprintf(stderr, "[xakard] set_brightness: no backlights updated\n");
            }
        }
        return;
    }

}

static Key_t *key_add_key (Key_t *keys, KeyCode key) {
    Key_t *rval = keys;
    Key_t *n = calloc(1, sizeof(Key_t));
    if (!n) return keys;
    n->key = key;
    n->refcount = 0;
    n->next = NULL;
    if (keys == NULL) {
        rval = n;
    } else {
        Key_t *p = keys;
        while (p->next != NULL) p = p->next;
        p->next = n;
    }
    return rval;
}

static void generated_add(KeyCode kc) {
    if (!g_hotkeys) return;
    if (g_hotkeys->generated_counts && kc >= g_hotkeys->keycode_min && kc <= g_hotkeys->keycode_max) {
        g_hotkeys->generated_counts[kc - g_hotkeys->keycode_min]++;
    }
}

static void generated_dec_and_remove(KeyCode kc) {
    if (!g_hotkeys) return;
    if (g_hotkeys->generated_counts && kc >= g_hotkeys->keycode_min && kc <= g_hotkeys->keycode_max) {
        int idx = kc - g_hotkeys->keycode_min;
        if (g_hotkeys->generated_counts[idx] > 0) g_hotkeys->generated_counts[idx]--;
    }
}

static void free_generated() {
    if (!g_hotkeys) return;
    if (g_hotkeys->generated_counts) {
        free(g_hotkeys->generated_counts);
        g_hotkeys->generated_counts = NULL;
    }
}

static Bool mods_are_pressed_with_map(Key_t *mods, unsigned char keys_return[32]) {
    if (!mods) return True;
    for (Key_t *m = mods; m; m = m->next) {
        KeyCode kc = m->key;
        if (kc == 0) return False;
        int byte = kc / 8;
        int bit = kc % 8;
        if (!(keys_return[byte] & (1 << bit))) return False;
    }
    return True;
}

static KeyCode parse_token_to_keycode(Display *dpy_arg, const char *token) {
    if (!token || !*token) return 0;
    if (token[0] == '#') {
        const char *num = token + 1;
        errno = 0;
        unsigned long parsed = strtoul(num, NULL, 0);
        if (errno == 0 && parsed <= 255) {
            KeyCode kc = (KeyCode)parsed;
            if (XkbKeycodeToKeysym(dpy_arg, kc, 0, 0) != NoSymbol) return kc;
            return 0;
        }
        return 0;
    } else {
        KeySym ks = XStringToKeysym(token);
        if (ks == NoSymbol) return 0;
        KeyCode kc = XKeysymToKeycode(dpy_arg, ks);
        return kc;
    }
}

static KeyCode token_modifier_keycode(Display *dpy_arg, const char *token) {
    if (!token) return 0;
    if (!strcasecmp(token, "Control_L") || !strcasecmp(token, "Control_R") || !strcasecmp(token, "Shift_L") || !strcasecmp(token, "Shift_R") || !strcasecmp(token, "Alt_L") || !strcasecmp(token, "Alt_R") || !strcasecmp(token, "Meta_L") || !strcasecmp(token, "Meta_R") || !strcasecmp(token, "Super_L") || !strcasecmp(token, "Super_R")) {
        return parse_token_to_keycode(dpy_arg, token);
    }
    return 0;
}

static int parse_key_sequence(Display *dpy_arg, const char *seq, Key_t **out_to_keys, Key_t **out_mods, KeySym *out_main_ks, KeyCode *out_main_kc, Bool *out_use_keycode) {
    if (!seq) return -1;
    char *copy = strdup(seq);
    if (!copy) return -1;
    Key_t *to_keys = NULL;
    Key_t *mods = NULL;
    KeySym main_ks = NoSymbol;
    KeyCode main_kc = 0;
    Bool use_kc = False;

    char *saveptr = NULL;
    char *tok = strtok_r(copy, "+", &saveptr);
    char *last_nonmod_tok = NULL;
    while (tok) {
        char *t = trim(tok);
        if (!t) { tok = strtok_r(NULL, "+", &saveptr); continue; }

        KeyCode mod_kc = token_modifier_keycode(dpy_arg, t);
        KeyCode token_kc = 0;
        if (mod_kc) {
            mods = key_add_key(mods, mod_kc);
            to_keys = key_add_key(to_keys, mod_kc);
        } else {
            token_kc = parse_token_to_keycode(dpy_arg, t);
            if (token_kc != 0) {
                to_keys = key_add_key(to_keys, token_kc);
                last_nonmod_tok = t;
            }
        }
        tok = strtok_r(NULL, "+", &saveptr);
    }

    if (last_nonmod_tok) {
        KeySym ks = XStringToKeysym(last_nonmod_tok);
        if (ks != NoSymbol) {
            main_ks = ks;
            main_kc = XKeysymToKeycode(dpy_arg, ks);
            use_kc = False;
        } else if (last_nonmod_tok[0] == '#') {
            errno = 0;
            unsigned long parsed = strtoul(last_nonmod_tok + 1, NULL, 0);
            if (errno == 0 && parsed <= 255) {
                main_kc = (KeyCode)parsed;
                main_ks = XkbKeycodeToKeysym(dpy_arg, main_kc, 0, 0);
                use_kc = True;
            }
        }
    } else {
        if (to_keys) {
            main_kc = to_keys->key;
            main_ks = XkbKeycodeToKeysym(dpy_arg, main_kc, 0, 0);
            use_kc = True;
        }
    }

    *out_to_keys = to_keys;
    *out_mods = mods;
    if (out_main_ks) *out_main_ks = main_ks;
    if (out_main_kc) *out_main_kc = main_kc;
    if (out_use_keycode) *out_use_keycode = use_kc;

    free(copy);
    return 0;
}

static void create_mappings_for_block(KeyMap_t **rval_ptr, KeyMap_t **km_ptr, const char *cur_Key, const char *cur_KeyAlt, const char *cur_KeyShortcut, const char *cur_KeyCommand) {
    if (!g_hotkeys || !cur_Key) return;

    Key_t *trigger_to = NULL;
    Key_t *trigger_mods = NULL;
    KeySym trigger_main_ks = NoSymbol;
    KeyCode trigger_main_kc = 0;
    Bool trigger_use_kc = False;

    if (parse_key_sequence(g_hotkeys->ctrl_conn, cur_Key, &trigger_to, &trigger_mods, &trigger_main_ks, &trigger_main_kc, &trigger_use_kc) != 0) {
        while (trigger_to) { Key_t *n = trigger_to->next; free(trigger_to); trigger_to = n; }
        if (trigger_mods) { while (trigger_mods) { Key_t *n = trigger_mods->next; free(trigger_mods); trigger_mods = n; } }
        return;
    }

    Bool trigger_is_single_modifier = False;
    if (cur_Key && !strchr(cur_Key, '+')) {
        if (token_modifier_keycode(g_hotkeys->ctrl_conn, cur_Key)) {
            trigger_is_single_modifier = True;
            while (trigger_mods) { Key_t *n = trigger_mods->next; free(trigger_mods); trigger_mods = n; }
        }
    }

    while (trigger_to) { Key_t *n = trigger_to->next; free(trigger_to); trigger_to = n; }

    KeyMap_t *create_action_entry(const char *action_seq, const char *command) {
        KeyMap_t *entry = calloc(1, sizeof(KeyMap_t));
        if (!entry) return NULL;
        entry->UseKeyCode = trigger_use_kc;
        entry->from_ks = trigger_main_ks;
        entry->from_kc = trigger_main_kc;
        entry->is_mod_trigger = trigger_is_single_modifier;
        entry->mods = NULL;
        for (Key_t *m = trigger_mods; m; m = m->next) {
            entry->mods = key_add_key(entry->mods, m->key);
        }
        entry->to_keys = NULL;
        entry->used = False;
        entry->pressed = False;
        entry->down_at.tv_sec = 0;
        entry->down_at.tv_usec = 0;
        entry->command = NULL;
        entry->internal_action = NULL;
        entry->next = NULL;
        entry->next_in_bucket = NULL;

        if (command) {
            const char *prefix = "xakar.action(";
            size_t prefix_len = strlen(prefix);
            size_t cmd_len = strlen(command);
            if (cmd_len > prefix_len && strncmp(command, prefix, prefix_len) == 0 && command[cmd_len - 1] == ')') {
                size_t payload_len = cmd_len - prefix_len - 1;
                char *payload = malloc(payload_len + 1);
                if (!payload) { free(entry); return NULL; }
                memcpy(payload, command + prefix_len, payload_len);
                payload[payload_len] = '\0';
                char *trimmed = trim(payload);
                entry->internal_action = strdup(trimmed);
                free(payload);
                return entry;
            } else {
                entry->command = strdup(command);
                return entry;
            }
        } else if (action_seq) {
            char *copy2 = strdup(action_seq);
            if (!copy2) { free(entry); return NULL; }
            char *savep2 = NULL;
            char *token2 = strtok_r(copy2, "+", &savep2);
            while (token2) {
                char *t2 = trim(token2);
                if (t2 && *t2) {
                    KeyCode code = parse_token_to_keycode(g_hotkeys->ctrl_conn, t2);
                    if (code) entry->to_keys = key_add_key(entry->to_keys, code);
                }
                token2 = strtok_r(NULL, "+", &savep2);
            }
            free(copy2);
            return entry;
        } else {
            free(entry);
            return NULL;
        }
    }

    KeyMap_t *head = NULL, *tail = NULL;

    if (cur_KeyCommand) {
        KeyMap_t *e = create_action_entry(NULL, cur_KeyCommand);
        if (e) { head = tail = e; }
    } else {
        if (cur_KeyShortcut) {
            KeyMap_t *e = create_action_entry(cur_KeyShortcut, NULL);
            if (e) { head = tail = e; }
        }
        if (cur_KeyAlt) {
            KeyMap_t *ea = create_action_entry(cur_KeyAlt, NULL);
            if (ea) {
                if (!head) head = tail = ea;
                else { tail->next = ea; tail = ea; }
            }
        }
    }

    for (KeyMap_t *it = head; it; ) {
        KeyMap_t *next_it = it->next;
        it->next = NULL;
        if (*rval_ptr == NULL) {
            *rval_ptr = it;
            *km_ptr = it;
        } else {
            (*km_ptr)->next = it;
            *km_ptr = it;
        }
        it = next_it;
    }

    while (trigger_mods) { Key_t *n = trigger_mods->next; free(trigger_mods); trigger_mods = n; }
}

static void build_keycache() {
    if (!g_hotkeys || !g_hotkeys->ctrl_conn || !g_hotkeys->map) return;

    int min_kc = 0, max_kc = 0;
    XDisplayKeycodes(g_hotkeys->ctrl_conn, &min_kc, &max_kc);
    if (min_kc <= 0) min_kc = 8;
    if (max_kc < min_kc) max_kc = min_kc;
    int range = max_kc - min_kc + 1;

    KeyMap_t **arr = calloc(range, sizeof(KeyMap_t*));
    if (!arr) return;

    for (KeyMap_t *km = g_hotkeys->map; km; km = km->next) {
        KeyCode trigger_kc = 0;
        if (km->UseKeyCode) trigger_kc = km->from_kc;
        else if (km->from_ks != NoSymbol) trigger_kc = XKeysymToKeycode(g_hotkeys->ctrl_conn, km->from_ks);
        if (trigger_kc == 0) continue;
        if (trigger_kc < (KeyCode)min_kc || trigger_kc > (KeyCode)max_kc) continue;
        int idx = (int)trigger_kc - min_kc;
        km->next_in_bucket = arr[idx];
        arr[idx] = km;
    }

    if (g_hotkeys->by_key) free(g_hotkeys->by_key);
    if (g_hotkeys->generated_counts) free(g_hotkeys->generated_counts);
    g_hotkeys->by_key = arr;
    g_hotkeys->keycode_min = min_kc;
    g_hotkeys->keycode_max = max_kc;
    g_hotkeys->generated_counts = calloc(range, sizeof(int));
}

static void process_km_chain(KeyMap_t *head, Bool use_bucket, int key_event, KeyCode key_code, unsigned char *keys_return, KeySym observed_ks, int *did_xfake) {

    KeyMap_t *best_km = NULL;
    int best_modcount = -1;

    for (KeyMap_t *km = head; km != NULL; km = (use_bucket ? km->next_in_bucket : km->next)) {
        Bool match = False;
        if ((km->UseKeyCode == False && observed_ks == km->from_ks) || (km->UseKeyCode == True && key_code == km->from_kc)) {
            if (mods_are_pressed_with_map(km->mods, keys_return)) match = True;
        }

        if (!match) {
            continue;
        }

        int modcount = 0;
        if (km->mods) {
            for (Key_t *m = km->mods; m; m = m->next) modcount++;
        } else if (km->is_mod_trigger) {
            modcount = 1;
        } else {
            modcount = 0;
        }

        if (modcount > best_modcount) {
            best_modcount = modcount;
            best_km = km;
        } else if (modcount == best_modcount && best_km != NULL) {
            int best_is_action = (best_km->internal_action || best_km->command) ? 1 : 0;
            int cur_is_action = (km->internal_action || km->command) ? 1 : 0;
            if (cur_is_action > best_is_action) {
                best_km = km;
            }
        }
    }

    for (KeyMap_t *km = head; km != NULL; km = (use_bucket ? km->next_in_bucket : km->next)) {
        Bool match = False;
        if ((km->UseKeyCode == False && observed_ks == km->from_ks) || (km->UseKeyCode == True && key_code == km->from_kc)) {
            if (mods_are_pressed_with_map(km->mods, keys_return)) match = True;
        }

        if (km == best_km && match) {
            Key_t *k;
            if (key_event == KeyPress) {
                km->pressed = True;
                gettimeofday(&km->down_at, NULL);
            } else {
                if (km->used == False) {
                    struct timeval now, elapsed;
                    gettimeofday(&now, NULL);
                    timersub(&now, &km->down_at, &elapsed);
                    if (timercmp(&elapsed, &g_hotkeys->timeout, <)) {
                        if (km->internal_action) {
                            fprintf(stdout, "[xakard] internal action: %s\n", km->internal_action);
                            handle_action(km->internal_action);
                        } else if (km->command) {
                            fprintf(stdout, "[xakard] Running command: %s\n", km->command);
                            run_command_async(km->command);
                        } else if (km->to_keys) {
                            fprintf(stdout, "[xakard] Generating keys: ");
                            for (k = km->to_keys; k != NULL; k = k->next) {
                                KeySym ks_tmp = XkbKeycodeToKeysym(g_hotkeys->ctrl_conn, k->key, 0, 0);
                                const char *name = XKeysymToString(ks_tmp);
                                fprintf(stdout, "%s ", name ? name : "(nil)");
                                XTestFakeKeyEvent(g_hotkeys->ctrl_conn, k->key, True, 0);
                                generated_add(k->key);
                                *did_xfake = 1;
                            }
                            fprintf(stdout, "\n");
                            for (k = km->to_keys; k != NULL; k = k->next) {
                                XTestFakeKeyEvent(g_hotkeys->ctrl_conn, k->key, False, 0);
                                generated_dec_and_remove(k->key);
                                *did_xfake = 1;
                            }
                        }
                    }
                }
                km->used = False;
                km->pressed = False;
            }
        } else {
            if (km->pressed && (key_event == KeyPress || key_event == ButtonPress)) {
                km->used = True;
            }
        }
    }
}

void intercept (XPointer user_data, XRecordInterceptData *data) {
    KeyMap_t *km;

    if (!g_hotkeys || !g_hotkeys->ctrl_conn) {
        if (data) XRecordFreeData (data);
        return;
    }

    XLockDisplay (g_hotkeys->ctrl_conn);
    if (data->category == XRecordFromServer) {
        int did_xfake = 0;
        static Bool mouse_pressed = False;
        unsigned char keys_return[32];
        KeyMap_t *iter_list = NULL;
        int     key_event = data->data[0];
        KeyCode key_code  = data->data[1];
        if (g_hotkeys->generated_counts && key_code >= g_hotkeys->keycode_min && key_code <= g_hotkeys->keycode_max) {
            if (g_hotkeys->generated_counts[key_code - g_hotkeys->keycode_min] > 0) {
                generated_dec_and_remove(key_code);
                goto exit;
            }
        }
        if (key_event == ButtonPress) mouse_pressed = True;
        else if (key_event == ButtonRelease) mouse_pressed = False;
        XQueryKeymap(g_hotkeys->ctrl_conn, (char*)keys_return);
        KeySym observed_ks = XkbKeycodeToKeysym(g_hotkeys->ctrl_conn, key_code, 0, 0);
        if (g_hotkeys->by_key && key_code >= g_hotkeys->keycode_min && key_code <= g_hotkeys->keycode_max) iter_list = g_hotkeys->by_key[key_code - g_hotkeys->keycode_min];
        if (iter_list == NULL) process_km_chain(g_hotkeys->map, False, key_event, key_code, keys_return, observed_ks, &did_xfake);
        else process_km_chain(iter_list, True, key_event, key_code, keys_return, observed_ks, &did_xfake);
        if (did_xfake) XFlush(g_hotkeys->ctrl_conn);
    }

exit:
    XUnlockDisplay (g_hotkeys->ctrl_conn);
    XRecordFreeData (data);
}

static unsigned int mask_from_modifier_keycode(Display *dpy_arg, KeyCode kc) {
    if (!dpy_arg || kc == 0) return 0;
    XModifierKeymap *mm = XGetModifierMapping(dpy_arg);
    if (!mm) return 0;
    unsigned int masks[8] = {
        ShiftMask, LockMask, ControlMask, Mod1Mask,
        Mod2Mask, Mod3Mask, Mod4Mask, Mod5Mask
    };
    unsigned int result = 0;
    int max = mm->max_keypermod;
    for (int mod = 0; mod < 8; ++mod) {
        for (int i = 0; i < max; ++i) {
            KeyCode m_kc = mm->modifiermap[mod * max + i];
            if (m_kc != 0 && m_kc == kc) {
                result |= masks[mod];
            }
        }
    }
    XFreeModifiermap(mm);
    return result;
}

static void find_lock_masks(Display *dpy_arg, unsigned int *numlock_mask, unsigned int *capslock_mask) {
    *numlock_mask = 0;
    *capslock_mask = 0;
    if (!dpy_arg) return;

    KeyCode kc_num = XKeysymToKeycode(dpy_arg, XStringToKeysym("Num_Lock"));
    KeyCode kc_caps = XKeysymToKeycode(dpy_arg, XStringToKeysym("Caps_Lock"));
    if (kc_num) *numlock_mask = mask_from_modifier_keycode(dpy_arg, kc_num);
    if (kc_caps) *capslock_mask = mask_from_modifier_keycode(dpy_arg, kc_caps);
}

static void grab_all_hotkeys() {
    if (!g_hotkeys || !g_hotkeys->ctrl_conn || !g_hotkeys->map) return;
    unsigned int numlock_mask = 0, capslock_mask = 0;
    find_lock_masks(g_hotkeys->ctrl_conn, &numlock_mask, &capslock_mask);
    int (*old_err_handler)(Display *, XErrorEvent *) = XSetErrorHandler(_x_global_error_handler);

    for (KeyMap_t *km = g_hotkeys->map; km; km = km->next) {
        if (km->is_mod_trigger) {
            KeySym ks = km->UseKeyCode ? XkbKeycodeToKeysym(g_hotkeys->ctrl_conn, km->from_kc, 0, 0) : km->from_ks;
            fprintf(stderr, "[xakard] Info: skipping XGrab for modifier trigger \"%s\" (will handle via XRecord)\n", XKeysymToString(ks));
            continue;
        }

        KeyCode trigger_kc = 0;
        if (km->UseKeyCode) trigger_kc = km->from_kc;
        else if (km->from_ks != NoSymbol) trigger_kc = XKeysymToKeycode(g_hotkeys->ctrl_conn, km->from_ks);
        if (trigger_kc == 0) continue;

        unsigned int base_mask = 0;
        for (Key_t *m = km->mods; m; m = m->next) {
            base_mask |= mask_from_modifier_keycode(g_hotkeys->ctrl_conn, m->key);
        }

        unsigned int combos[4];
        combos[0] = 0;
        combos[1] = numlock_mask;
        combos[2] = capslock_mask;
        combos[3] = numlock_mask | capslock_mask;

        for (int i = 0; i < 4; ++i) {
            unsigned int m = base_mask | combos[i];

            g_x_error_occurred = 0; g_x_error_code = 0;
            XGrabKey(g_hotkeys->ctrl_conn, trigger_kc, m, DefaultRootWindow(g_hotkeys->ctrl_conn), True, GrabModeAsync, GrabModeAsync);
            XSync(g_hotkeys->ctrl_conn, False);

            if (g_x_error_occurred) {
                if (g_x_error_code == BadAccess) {
                    /* In case some key combination does not work, try uncommenting these and debugging the key event
                    KeySym ks = XkbKeycodeToKeysym(g_hotkeys->ctrl_conn, trigger_kc, 0, 0);
                    const char *name = XKeysymToString(ks);
                    fprintf(stderr, "[xakard] INFO: grab failed (BadAccess) for trigger \"%s\" keycode %u mask 0x%x. Skipping this grab.\n", name ? name : "(nil)", (unsigned)trigger_kc, m); */
                } else {
                    fprintf(stderr, "[xakard] INFO: X error (code %d) during XGrabKey for keycode %u mask 0x%x. Skipping.\n", g_x_error_code, (unsigned)trigger_kc, m);
                }
                g_x_error_occurred = 0;
                g_x_error_code = 0;
                continue;
            }
        }
    }

    XSetErrorHandler(old_err_handler);
    XSync(g_hotkeys->ctrl_conn, False);
}

static void ungrab_all_hotkeys() {
    if (!g_hotkeys || !g_hotkeys->ctrl_conn || !g_hotkeys->map) return;
    unsigned int numlock_mask = 0, capslock_mask = 0;
    find_lock_masks(g_hotkeys->ctrl_conn, &numlock_mask, &capslock_mask);
    for (KeyMap_t *km = g_hotkeys->map; km; km = km->next) {
        KeyCode trigger_kc = 0;
        if (km->UseKeyCode) trigger_kc = km->from_kc;
        else if (km->from_ks != NoSymbol) trigger_kc = XKeysymToKeycode(g_hotkeys->ctrl_conn, km->from_ks);
        if (trigger_kc == 0) continue;

        unsigned int base_mask = 0;
        for (Key_t *m = km->mods; m; m = m->next) {
            base_mask |= mask_from_modifier_keycode(g_hotkeys->ctrl_conn, m->key);
        }

        unsigned int combos[4];
        combos[0] = 0;
        combos[1] = numlock_mask;
        combos[2] = capslock_mask;
        combos[3] = numlock_mask | capslock_mask;

        for (int i = 0; i < 4; ++i) {
            unsigned int mm = base_mask | combos[i];
            XUngrabKey(g_hotkeys->ctrl_conn, trigger_kc, mm, DefaultRootWindow(g_hotkeys->ctrl_conn));
        }
    }
    XSync(g_hotkeys->ctrl_conn, False);
}

static void cleanup() {
    stop_all_struts();
    pthread_mutex_destroy(&g_hotkeys->map_mutex);
    if (FIFO_PATH[0]) unlink(FIFO_PATH);
    if (!g_hotkeys) {
        if (dpy) { XCloseDisplay(dpy); dpy = NULL; }
        goto close_threads;
    }
    if (g_hotkeys->ctrl_conn && g_hotkeys->record_ctx) {
        XRecordDisableContext(g_hotkeys->ctrl_conn, g_hotkeys->record_ctx);
        XSync(g_hotkeys->ctrl_conn, False);
    }
    ungrab_all_hotkeys();

    fprintf(stdout, "[xakard] exiting\n");
    if (g_hotkeys->ctrl_conn && g_hotkeys->record_ctx) {
        XRecordFreeContext(g_hotkeys->ctrl_conn, g_hotkeys->record_ctx);
        g_hotkeys->record_ctx = 0;
    }

    if (g_hotkeys->rec_range) {
        XFree(g_hotkeys->rec_range);
        g_hotkeys->rec_range = NULL;
    }

    if (g_hotkeys->by_key) {
        free(g_hotkeys->by_key);
        g_hotkeys->by_key = NULL;
    }

    free_generated();

    if (g_hotkeys->ctrl_conn && g_hotkeys->ctrl_conn != dpy) {
        XCloseDisplay(g_hotkeys->ctrl_conn);
        g_hotkeys->ctrl_conn = NULL;
    }
    if (g_hotkeys->data_conn) {
        XCloseDisplay(g_hotkeys->data_conn);
        g_hotkeys->data_conn = NULL;
    }

    while (g_hotkeys->map != NULL) {
        KeyMap_t *nxt_map = g_hotkeys->map->next;
        while (g_hotkeys->map->to_keys != NULL) {
            Key_t *nxt_key = g_hotkeys->map->to_keys->next;
            free(g_hotkeys->map->to_keys);
            g_hotkeys->map->to_keys = nxt_key;
        }
        if (g_hotkeys->map->mods) {
            while (g_hotkeys->map->mods) {
                Key_t *n = g_hotkeys->map->mods->next;
                free(g_hotkeys->map->mods);
                g_hotkeys->map->mods = n;
            }
        }
        if (g_hotkeys->map->command) free(g_hotkeys->map->command);
        if (g_hotkeys->map->internal_action) free(g_hotkeys->map->internal_action);
        free(g_hotkeys->map);
        g_hotkeys->map = nxt_map;
    }

    if (dpy) { XCloseDisplay(dpy); dpy = NULL; }
    free(g_hotkeys);
    g_hotkeys = NULL;

close_threads:
    if (config_watcher_thread) pthread_join(config_watcher_thread, NULL);
    if (fifo_watcher_thread) pthread_join(fifo_watcher_thread, NULL);
    if (window_watcher_thread) pthread_join(window_watcher_thread, NULL);
    if (keyboard_watcher_thread) pthread_join(keyboard_watcher_thread, NULL);
}

static int init() {
    home = getenv("HOME");
    int w;
    int in_hotkey = 0;
    char path[4096];
    char *line = NULL;
    char *cur_Key = NULL, *cur_KeyAlt = NULL, *cur_KeyShortcut = NULL, *cur_KeyCommand = NULL;
    char *cur_Name = NULL, *cur_Description = NULL;
    FILE *fp = NULL;
    size_t len = 0;
    ssize_t linelen;
    KeyMap_t *rval = NULL, *km_tail = NULL;
    XRecordRange *rec_range = NULL;

    if (!home) fatal("HOME not set");
    if (snprintf(FIFO_PATH, sizeof(FIFO_PATH), "%s%s", home, LOCAL_FIFO_SUFFIX) >= (int)sizeof(FIFO_PATH)) fatal("FIFO path too long");

    unlink(FIFO_PATH);
    if (mkfifo(FIFO_PATH, 0666) < 0) {
        if (errno != EEXIST) perror("mkfifo");
    }

    dpy = XOpenDisplay(NULL);
    if (!dpy) fatal("Failed to open X display");
    g_hotkeys->ctrl_conn = dpy;

    g_hotkeys->data_conn = XOpenDisplay(NULL);
    if (!g_hotkeys->data_conn) {
        fprintf(stderr, "[xakard] XOpenDisplay(data) failed\n");
        cleanup();
        return EXIT_FAILURE;
    }

    screen = DefaultScreen(dpy);
    root_window = RootWindow(dpy, screen);
    screen_width = DisplayWidth(dpy, screen);
    screen_height = DisplayHeight(dpy, screen);
    black_pixel = BlackPixel(dpy, screen);
    visual = DefaultVisual(dpy, screen);
    depth = DefaultDepth(dpy, screen);
    half_screen_width = screen_width / 2;
    half_screen_height = screen_height / 2;

    XVisualInfo vinfo;
    if (XMatchVisualInfo(dpy, screen, 32, TrueColor, &vinfo) != 0) {
        visual = vinfo.visual;
        depth = vinfo.depth;
    }

    colormap = XCreateColormap(dpy, root_window, visual, AllocNone);

    _NET_ACTIVE_WINDOW = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", True);
    _NET_WM_STATE = XInternAtom(dpy, "_NET_WM_STATE", False);
    _NET_WM_STATE_FULLSCREEN = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
    _NET_WM_STRUT = XInternAtom(dpy, "_NET_WM_STRUT", False);
    _NET_WM_STRUT_PARTIAL = XInternAtom(dpy, "_NET_WM_STRUT_PARTIAL", False);
    _NET_WM_STATE = XInternAtom(dpy, "_NET_WM_STATE", False);
    _NET_WM_DESKTOP = XInternAtom(dpy, "_NET_WM_DESKTOP", False);
    _NET_WM_WINDOW_TYPE = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    _NET_WM_STATE_ABOVE = XInternAtom(dpy, "_NET_WM_STATE_ABOVE", False);
    _NET_WM_STATE_STICKY = XInternAtom(dpy, "_NET_WM_STATE_STICKY", False);
    _NET_WM_WINDOW_TYPE_DOCK = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    WM_CLASS = XInternAtom(dpy, "WM_CLASS", False);
    _NET_WM_NAME = XInternAtom(dpy, "_NET_WM_NAME", False);
    UTF8_STRING = XInternAtom(dpy, "UTF8_STRING", False);
    _MOTIF_WM_HINTS = XInternAtom(dpy, "_MOTIF_WM_HINTS", False);
    _OB_WM_STATE_UNDECORATED = XInternAtom(dpy, "_OB_WM_STATE_UNDECORATED", False);
    printf("[xakard] Listening on %s...\n", FIFO_PATH);

    g_hotkeys->timeout.tv_sec = 0;
    g_hotkeys->timeout.tv_usec = 500000;
    g_hotkeys->map = NULL;
    g_hotkeys->record_ctx = 0;
    g_hotkeys->rec_range = NULL;
    g_hotkeys->by_key = NULL;
    g_hotkeys->keycode_min = 0;
    g_hotkeys->keycode_max = 0;
    g_hotkeys->generated_counts = NULL;
    g_hotkeys->cached_numlock_mask = 0;
    g_hotkeys->cached_capslock_mask = 0;

    rec_range = XRecordAllocRange();
    if (!rec_range) {
        fprintf(stderr, "[xakard] XRecordAllocRange failed\n");
        cleanup();
        return EXIT_FAILURE;
    }
    g_hotkeys->rec_range = rec_range;

    rec_range->device_events.first = KeyPress;
    rec_range->device_events.last  = ButtonRelease;

    find_lock_masks(g_hotkeys->ctrl_conn, &g_hotkeys->cached_numlock_mask, &g_hotkeys->cached_capslock_mask);
    signal(SIGCHLD, SIG_IGN);

    w = snprintf(path, sizeof(path), "%s/.setuzuna/xakar/.config", home);
    if (w < 0 || (size_t)w >= sizeof(path)) {
        fprintf(stderr, "[xakard] config path too long\n");
        cleanup();
        return EXIT_FAILURE;
    }
    strncpy(CONFIG_PATH, path, sizeof(CONFIG_PATH)-1);
    CONFIG_PATH[sizeof(CONFIG_PATH)-1] = '\0';

    fp = fopen(path, "r");
    if (!fp) {
        perror("[xakard] fopen");
        cleanup();
        return EXIT_FAILURE;
    }

    while ((linelen = getline(&line, &len, fp)) != -1) {
        char *p = trim(line);
        if (p[0] == '[') {
            if (strcasecmp(p, "[Hotkey]") == 0) {
                if (in_hotkey && (cur_Key || cur_KeyAlt || cur_KeyShortcut || cur_KeyCommand)) {
                    create_mappings_for_block(&rval, &km_tail, cur_Key, cur_KeyAlt, cur_KeyShortcut, cur_KeyCommand);
                    free(cur_Key); cur_Key = NULL;
                    free(cur_KeyAlt); cur_KeyAlt = NULL;
                    free(cur_KeyShortcut); cur_KeyShortcut = NULL;
                    free(cur_KeyCommand); cur_KeyCommand = NULL;
                    free(cur_Name); cur_Name = NULL;
                    free(cur_Description); cur_Description = NULL;
                }
                in_hotkey = 1;
            } else {
                if (in_hotkey && (cur_Key || cur_KeyAlt || cur_KeyShortcut || cur_KeyCommand)) {
                    create_mappings_for_block(&rval, &km_tail, cur_Key, cur_KeyAlt, cur_KeyShortcut, cur_KeyCommand);
                    free(cur_Key); cur_Key = NULL;
                    free(cur_KeyAlt); cur_KeyAlt = NULL;
                    free(cur_KeyShortcut); cur_KeyShortcut = NULL;
                    free(cur_KeyCommand); cur_KeyCommand = NULL;
                    free(cur_Name); cur_Name = NULL;
                    free(cur_Description); cur_Description = NULL;
                }
                in_hotkey = 0;
            }
            continue;
        }

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = trim(p);
        char *v = trim(eq + 1);

        if (!k || !v) continue;
        if (!in_hotkey) continue;

        if (strcasecmp(k, "Key") == 0) {
            free(cur_Key);
            cur_Key = strdup(v);
        } else if (strcasecmp(k, "KeyAlt") == 0) {
            free(cur_KeyAlt);
            cur_KeyAlt = strdup(v);
        } else if (strcasecmp(k, "KeyShortcut") == 0) {
            free(cur_KeyShortcut);
            cur_KeyShortcut = strdup(v);
        } else if (strcasecmp(k, "KeyCommand") == 0) {
            free(cur_KeyCommand);
            cur_KeyCommand = strdup(v);
        } else if (strcasecmp(k, "Name") == 0) {
            free(cur_Name);
            cur_Name = strdup(v);
        } else if (strcasecmp(k, "Description") == 0) {
            free(cur_Description);
            cur_Description = strdup(v);
        }
    }

    if (in_hotkey && (cur_Key || cur_KeyAlt || cur_KeyShortcut || cur_KeyCommand)) {
        create_mappings_for_block(&rval, &km_tail, cur_Key, cur_KeyAlt, cur_KeyShortcut, cur_KeyCommand);
    }

    free(cur_Key);
    free(cur_KeyAlt);
    free(cur_KeyShortcut);
    free(cur_KeyCommand);
    free(cur_Name);
    free(cur_Description);
    free(line);
    fclose(fp);
    fp = NULL;

    if (!rval) {
        fprintf(stderr, "[xakard] No mappings found in config.\n");
        cleanup();
        return EXIT_FAILURE;
    }

    g_hotkeys->map = rval;
    build_keycache();
    grab_all_hotkeys();
    {
        XRecordClientSpec client_spec = XRecordAllClients;
        g_hotkeys->record_ctx = XRecordCreateContext(g_hotkeys->ctrl_conn, 0, &client_spec, 1, &rec_range, 1);
        if (!g_hotkeys->record_ctx) {
            fprintf(stderr, "[xakard] XRecordCreateContext failed\n");
            cleanup();
            return EXIT_FAILURE;
        }
    }

    if (pipe2(sigpipe_fds, O_CLOEXEC) == -1) {
        perror("[xakard] pipe2");
        cleanup();
        return EXIT_FAILURE;
    }

    return 0;
}

static void *config_watcher_loop(void *unused) {
    (void)unused;
    if (!CONFIG_PATH[0]) return NULL;
    char dirbuf[4096];
    char basebuf[256];
    strncpy(dirbuf, CONFIG_PATH, sizeof(dirbuf)-1); dirbuf[sizeof(dirbuf)-1] = '\0';
    char *slash = strrchr(dirbuf, '/');
    if (!slash) return NULL;
    strncpy(basebuf, slash + 1, sizeof(basebuf)-1); basebuf[sizeof(basebuf)-1] = '\0';
    *slash = '\0';

    inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotify_fd < 0) {
        perror("[xakard] inotify_init1");
        return NULL;
    }

    inotify_wd = inotify_add_watch(inotify_fd, dirbuf, IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE_SELF | IN_MOVE_SELF);
    if (inotify_wd < 0) {
        perror("[xakard] inotify_add_watch");
        close(inotify_fd);
        inotify_fd = -1;
        return NULL;
    }

    struct pollfd pfd;
    pfd.fd = inotify_fd;
    pfd.events = POLLIN;

    char buf[4096] __attribute__ ((aligned(__alignof__(struct inotify_event))));
    const int DEBOUNCE_MS = 250;

    while (!stop_requested) {
        int pol = poll(&pfd, 1, 1000);
        if (pol > 0 && (pfd.revents & POLLIN)) {
            ssize_t len = read(inotify_fd, buf, sizeof(buf));
            if (len <= 0) {
                if (errno == EINTR) continue;
            } else {
                for (char *ptr = buf; ptr < buf + len; ) {
                    struct inotify_event *ev = (struct inotify_event *)ptr;
                    if (ev->len && ev->name) {
                        if (strncmp(ev->name, basebuf, sizeof(basebuf)) == 0) {
                            restart_self();
                            usleep(DEBOUNCE_MS * 1000);
                        }
                    } else {
                        restart_self();
                        usleep(DEBOUNCE_MS * 1000);
                    }
                    ptr += sizeof(struct inotify_event) + ev->len;
                }
            }
        } else if (pol == 0) {
            continue;
        } else {
            if (errno == EINTR) continue;
            break;
        }
    }

    if (inotify_wd != -1) {
        inotify_rm_watch(inotify_fd, inotify_wd);
        inotify_wd = -1;
    }
    if (inotify_fd != -1) {
        close(inotify_fd);
        inotify_fd = -1;
    }
    return NULL;
}

void *fifo_watcher_loop(void *unused) {
    (void)unused;
    int fifo_fd = -1;
    int dummy_fd = -1;

    fifo_fd = open(FIFO_PATH, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fifo_fd < 0) {
        perror("open fifo read");
    }

    dummy_fd = open(FIFO_PATH, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (dummy_fd < 0) {
        dummy_fd = -1;
    }

    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        if (fifo_fd >= 0) FD_SET(fifo_fd, &rfds);
        int maxfd = fifo_fd >= 0 ? fifo_fd : 0;
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int rv = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (rv > 0 && fifo_fd >= 0 && FD_ISSET(fifo_fd, &rfds)) {
            char buf[256];
            ssize_t n = read(fifo_fd, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                char *nl = strpbrk(buf, "\r\n"); if (nl) *nl = '\0';
                handle_action(buf);
            } else if (n == 0) {
                close(fifo_fd);
                fifo_fd = open(FIFO_PATH, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
                if (fifo_fd < 0) {
                    perror("reopen fifo for read");
                    sleep(1);
                    continue;
                }
                if (dummy_fd < 0) {
                    dummy_fd = open(FIFO_PATH, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
                    if (dummy_fd < 0) dummy_fd = -1;
                }
            } else {
                if (errno != EAGAIN && errno != EINTR) {
                    perror("read fifo");
                }
            }
        } else if (rv < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        if (stop_requested) break;
    }

    if (fifo_fd >= 0) close(fifo_fd);
    if (dummy_fd >= 0) close(dummy_fd);
    return NULL;
}

static void *keyboard_watcher_loop() {
    if (!g_hotkeys) return NULL;
    if (!g_hotkeys->data_conn) return NULL;

    XSync(g_hotkeys->ctrl_conn, False);
    XRecordEnableContext(g_hotkeys->data_conn, g_hotkeys->record_ctx, intercept, (XPointer)g_hotkeys);
    return NULL;
}

static void *window_watcher_loop(void *unused) {
    (void)unused;

    XSetErrorHandler(_x_global_error_handler);
    int screen_count = ScreenCount(dpy);
    for (int i = 0; i < screen_count; ++i) {
        Window root = RootWindow(dpy, i);
        XSelectInput(dpy, root, SubstructureNotifyMask | StructureNotifyMask);
    }
    if (g_hotkeys) {
        printf("[xakard] listening for new windows on %d screens\n", screen_count);
    }

    while (!stop_requested) {
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            switch (ev.type) {
            case MapNotify:
                handle_new_window(ev.xmap.window);
                break;
            case ReparentNotify:
                handle_new_window(ev.xreparent.window);
                break;
            case ConfigureNotify:
                if (ev.xconfigure.event == ev.xconfigure.window) {
                    handle_new_window(ev.xconfigure.window);
                }
                break;
            default:
                break;
            }
        }
        usleep(100 * 1000);
    }

    if (g_hotkeys) printf("[xakard] thread exiting\n");
    return NULL;
}

int main(int argc, char **argv) {
    saved_argc = argc;
    saved_argv = argv;
    struct sigaction sa;
    
    if (!XInitThreads()) fprintf(stderr, "[xakard] Warning: XInitThreads failed (continuing)\n");

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    g_hotkeys = calloc(1, sizeof(Hotkeys_t));
    if (!g_hotkeys) {
        fprintf(stderr, "Failed to allocate g_hotkeys\n");
        return EXIT_FAILURE;
    }
    if (pthread_mutex_init(&g_hotkeys->map_mutex, NULL) != 0) {
        perror("pthread_mutex_init");
        cleanup();
        return EXIT_FAILURE;
    }

    if (init() != 0) {
        return EXIT_FAILURE;
    }

    if (pthread_create(&config_watcher_thread, NULL, config_watcher_loop, NULL) != 0) {
        perror("pthread_create(config_watcher_thread)");
        cleanup();
        return EXIT_FAILURE;
    }
    if (pthread_create(&fifo_watcher_thread, NULL, fifo_watcher_loop, NULL) != 0) {
        perror("pthread_create(fifo_watcher_thread)");
        stop_requested = 1;
        if (sigpipe_fds[1] != -1) {
            ssize_t rr; char c = 1;
            do { rr = write(sigpipe_fds[1], &c, 1); } while (rr == -1 && errno == EINTR);
        }
        cleanup();
        return EXIT_FAILURE;
    }
    if (pthread_create(&window_watcher_thread, NULL, window_watcher_loop, NULL) != 0) {
        perror("pthread_create(window_watcher_thread)");
        cleanup();
        return EXIT_FAILURE;
    }
    if (pthread_create(&keyboard_watcher_thread, NULL, keyboard_watcher_loop, NULL) != 0) {
        perror("pthread_create(keyboard_watcher_thread)");
        cleanup();
        return EXIT_FAILURE;
    }

    if (sigpipe_fds[0] != -1) {
        char buf;
        ssize_t r = read(sigpipe_fds[0], &buf, 1);
        (void)r;
    } else {
        while (!stop_requested) sleep(1);
    }

    if (g_hotkeys && g_hotkeys->ctrl_conn && g_hotkeys->record_ctx) {
        XRecordDisableContext(g_hotkeys->ctrl_conn, g_hotkeys->record_ctx);
        XSync(g_hotkeys->ctrl_conn, False);
    }

    cleanup();
    if (sigpipe_fds[0] != -1) close(sigpipe_fds[0]);
    if (sigpipe_fds[1] != -1) close(sigpipe_fds[1]);
    sigpipe_fds[0] = sigpipe_fds[1] = -1;

    return EXIT_SUCCESS;
}
