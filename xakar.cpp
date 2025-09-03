#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xinerama.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>

#define LOCAL_FIFO_SUFFIX "/.xakar.sock"
#define MAX_CHAIN_DEPTH 64

typedef struct {
    Window object;
    int width, height;
    int ancestor_x, ancestor_y;
    int ancestor_width, ancestor_height;
} Geometry;

static char FIFO_PATH[PATH_MAX];
static Display *dpy = NULL;
static Window root_window = 0;
static int screen_width = 0, screen_height = 0;
static int half_screen_width = 0, half_screen_height = 0;

static Atom atom_net_active_window = None;
static Atom atom_net_wm_state = None;
static Atom atom_net_wm_state_fullscreen = None;

static void fatal(const char *msg) {
    fprintf(stderr, "[xakard] Fatal: %s\n", msg);
    exit(1);
}

static void init() {
    const char *home = getenv("HOME");
    if (!home) fatal("HOME not set");
    if (snprintf(FIFO_PATH, sizeof(FIFO_PATH), "%s%s", home, LOCAL_FIFO_SUFFIX) >= (int)sizeof(FIFO_PATH))
        fatal("FIFO path too long");

    unlink(FIFO_PATH);
    if (mkfifo(FIFO_PATH, 0666) < 0) {
        if (errno != EEXIST) perror("mkfifo");
    }

    dpy = XOpenDisplay(NULL);
    if (!dpy) fatal("Failed to open X display");

    int scr = DefaultScreen(dpy);
    root_window = RootWindow(dpy, scr);
    screen_width = DisplayWidth(dpy, scr);
    screen_height = DisplayHeight(dpy, scr);
    half_screen_width = screen_width / 2;
    half_screen_height = screen_height / 2;

    atom_net_active_window = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", True);
    atom_net_wm_state = XInternAtom(dpy, "_NET_WM_STATE", False);
    atom_net_wm_state_fullscreen = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
}

static Window get_active_window_id(void) {
    if (atom_net_active_window == None) return 0;

    Atom actual_type;
    int actual_format;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *prop = NULL;

    int rc = XGetWindowProperty(dpy, root_window, atom_net_active_window, 0, 1, False,
                                XA_WINDOW, &actual_type, &actual_format,
                                &nitems, &bytes_after, &prop);
    if (rc != Success || !prop || nitems == 0) {
        if (prop) XFree(prop);
        return 0;
    }

    Window w = 0;
    if (nitems >= 1) w = *((Window*)prop);
    XFree(prop);
    return w;
}

static int get_geometry(Window active_id, Geometry *out) {
    if (!active_id) return 0;

    Window cur = active_id;
    Window parent, root_ret, *children = NULL;
    unsigned int nchildren;
    XWindowAttributes chain[MAX_CHAIN_DEPTH];
    int depth = 0;
    XWindowAttributes attr;

    int first = 1;

    while (cur && depth < MAX_CHAIN_DEPTH) {
        if (!XGetWindowAttributes(dpy, cur, &attr)) break;
        chain[depth++] = attr;

        if (!XQueryTree(dpy, cur, &root_ret, &parent, &children, &nchildren)) break;
        if (children) XFree(children);
        if (!parent || parent == root_ret) break;
        cur = parent;
    }

    if (depth == 0) return 0;

    out->object = active_id;
    out->width = chain[0].width;
    out->height = chain[0].height;
    out->ancestor_x = chain[depth-1].x;
    out->ancestor_y = chain[depth-1].y;
    out->ancestor_width = chain[depth-1].width;
    out->ancestor_height = chain[depth-1].height;
    return 1;
}

static void move_resize(Window w, int x, int y, int width, int height) {
    if (!w) return;
    XMoveResizeWindow(dpy, w, x, y, width, height);
    XFlush(dpy);
}

static int get_monitor_for_window(Display *dpy_local, const Geometry *geom, int *mx, int *my, int *mw, int *mh) {
    if (!XineramaIsActive(dpy_local)) return 0;
    int num_screens = 0;
    XineramaScreenInfo *screens = XineramaQueryScreens(dpy_local, &num_screens);
    if (!screens) return 0;
    for (int i = 0; i < num_screens; i++) {
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

static int find_neighbor_monitor(Display *dpy_local, int cur_mx, int cur_my, int cur_mw, int cur_mh, const char *direction, int *tx, int *ty, int *tw, int *th) {
    if (!XineramaIsActive(dpy_local)) return 0;
    int ns = 0;
    XineramaScreenInfo *screens = XineramaQueryScreens(dpy_local, &ns);
    if (!screens) return 0;

    double cur_cx = cur_mx + cur_mw / 2.0;
    double cur_cy = cur_my + cur_mh / 2.0;

    int best = -1;
    double best_metric = 0;

    for (int i = 0; i < ns; ++i) {
        XineramaScreenInfo s = screens[i];
        if (s.x_org == cur_mx && s.y_org == cur_my && s.width == cur_mw && s.height == cur_mh) continue;
        double cx = s.x_org + s.width / 2.0;
        double cy = s.y_org + s.height / 2.0;
        double dx = cx - cur_cx;
        double dy = cy - cur_cy;

        int valid = 0;
        double primary = 1e12, secondary = 1e12;

        if (strcmp(direction, "left") == 0) {
            if (cx < cur_cx) { valid = 1; primary = cur_cx - cx; secondary = fabs(dy); }
        } else if (strcmp(direction, "right") == 0) {
            if (cx > cur_cx) { valid = 1; primary = cx - cur_cx; secondary = fabs(dy); }
        } else if (strcmp(direction, "up") == 0) {
            if (cy < cur_cy) { valid = 1; primary = cur_cy - cy; secondary = fabs(dx); }
        } else if (strcmp(direction, "down") == 0) {
            if (cy > cur_cy) { valid = 1; primary = cy - cur_cy; secondary = fabs(dx); }
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
    if (atom_net_wm_state == None || atom_net_wm_state_fullscreen == None) return;

    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop = NULL;
    int is_fs = 0;

    if (XGetWindowProperty(dpy, win, atom_net_wm_state, 0, 1024, False, XA_ATOM,
                           &actual_type, &actual_format, &nitems, &bytes_after, &prop) == Success && prop) {
        Atom *atoms = (Atom*)prop;
        unsigned long i;
        for (i = 0; i < nitems; ++i) if (atoms[i] == atom_net_wm_state_fullscreen) { is_fs = 1; break; }
        XFree(prop);
    }

    XEvent ev;
    ev.xclient.type = ClientMessage;
    ev.xclient.window = win;
    ev.xclient.message_type = atom_net_wm_state;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = is_fs ? 0 : 1;
    ev.xclient.data.l[1] = atom_net_wm_state_fullscreen;
    ev.xclient.data.l[2] = 0;
    ev.xclient.data.l[3] = 0;
    ev.xclient.data.l[4] = 0;

    XSendEvent(dpy, root_window, False, SubstructureNotifyMask | SubstructureRedirectMask, &ev);
    XFlush(dpy);
}

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

static void apply_mode_to_target_and_move(const Geometry *geom, TileMode mode, int tx, int ty, int tw, int th) {
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

static void handle_action(const char *action_in) {
    if (!action_in) return;
    char action_buf[256];
    strncpy(action_buf, action_in, sizeof(action_buf)-1);
    action_buf[sizeof(action_buf)-1] = '\0';

    char *tok = strtok(action_buf, " \t");
    if (!tok) return;

    const char *first = tok;
    const char *second = NULL;
    char *sec = strtok(NULL, " \t");
    if (sec) second = sec;

    const char *cmd = first;
    const char *dir = second;

    int is_preserve_geom = (strcmp(cmd, "preserve_geom") == 0);
    int is_preserve_size = (strcmp(cmd, "preserve_size") == 0);

    if (is_preserve_geom || is_preserve_size) {
        if (!dir) return;
        if (strcmp(dir, "center") && strcmp(dir, "fullscreen") && strcmp(dir, "left")
            && strcmp(dir, "right") && strcmp(dir, "up") && strcmp(dir, "down")
            && strcmp(dir, "wm_fullscreen")) {
            return;
        }
    } else {
        if (strcmp(cmd, "center") && strcmp(cmd, "fullscreen") && strcmp(cmd, "left")
            && strcmp(cmd, "right") && strcmp(cmd, "up") && strcmp(cmd, "down")
            && strcmp(cmd, "wm_fullscreen")) {
            return;
        }
    }

    Window active = get_active_window_id();
    if (!active) return;

    if (strcmp(cmd, "wm_fullscreen") == 0) {
        send_fullscreen_toggle(active);
        return;
    }

    Geometry geom;
    if (!get_geometry(active, &geom)) return;

    int mx = 0, my = 0, mw = screen_width, mh = screen_height;
    int have_monitor = get_monitor_for_window(dpy, &geom, &mx, &my, &mw, &mh);
    if (!have_monitor) {
        mx = 0; my = 0; mw = screen_width; mh = screen_height;
    }

    if (is_preserve_geom || is_preserve_size) {
        int tx, ty, tw, th;
        if (!find_neighbor_monitor(dpy, mx, my, mw, mh, dir, &tx, &ty, &tw, &th)) {
            handle_action(dir);
            return;
        }

        TileMode mode = detect_tiling_mode(&geom, mx, my, mw, mh);

        if (is_preserve_geom || mode == MODE_UNKNOWN) {
            int rel_x = geom.ancestor_x - mx;
            int rel_y = geom.ancestor_y - my;
            int new_x = tx + rel_x;
            int new_y = ty + rel_y;
            if (new_x < tx) new_x = tx;
            if (new_y < ty) new_y = ty;
            if (new_x + geom.width > tx + tw) new_x = tx + tw - geom.width;
            if (new_y + geom.height > ty + th) new_y = ty + th - geom.height;
            if (geom.width <= tw && geom.height <= th) {
                move_resize(geom.object, new_x, new_y, geom.width, geom.height);
                return;
            }
        }
        apply_mode_to_target_and_move(&geom, mode, tx, ty, tw, th);
        return;
    }

    int nx, ny, nw, nh;
    generate_dimensions(&geom, cmd, mx, my, mw, mh, &nx, &ny, &nw, &nh);

    int target_abs_x = nx + mx;
    int target_abs_y = ny + my;

    if (have_monitor &&
        geom.ancestor_x == target_abs_x && geom.ancestor_y == target_abs_y &&
        geom.width == nw && geom.height == nh &&
        (strcmp(cmd, "left") == 0 || strcmp(cmd, "right") == 0 || strcmp(cmd, "up") == 0 || strcmp(cmd, "down") == 0)) {

        int tx, ty, tw, th;
        if (find_neighbor_monitor(dpy, mx, my, mw, mh, cmd, &tx, &ty, &tw, &th)) {
            int overhead_x = geom.ancestor_width - geom.width;
            int overhead_y = geom.ancestor_height - geom.height;
            int t_full_w = tw - overhead_x;
            int t_full_h = th - overhead_y;
            int t_half_w = (tw / 2) - overhead_x;
            int t_half_h = (th / 2) - overhead_y;

            int tx_new = 0, ty_new = 0, tw_new = 0, th_new = 0;

            if (strcmp(cmd, "left") == 0) {
                tx_new = tw / 2;
                tw_new = t_half_w;
                th_new = t_full_h;
            } else if (strcmp(cmd, "right") == 0) {
                tw_new = t_half_w;
                th_new = t_full_h;
            } else if (strcmp(cmd, "up") == 0) {
                ty_new = th / 2;
                th_new = t_half_h;
                tw_new = t_full_w;
            } else {
                th_new = t_half_h;
                tw_new = t_full_w;
            }

            move_resize(geom.object, tx + tx_new, ty + ty_new, tw_new, th_new);
            return;
        }

        return;
    }

    move_resize(geom.object, target_abs_x, target_abs_y, nw, nh);
}

int main(void) {
    init();
    printf("[xakard] Listening on %s...\n", FIFO_PATH);

    for (;;) {
        int fd = open(FIFO_PATH, O_RDONLY);
        if (fd < 0) { perror("open fifo"); break; }

        fd_set rfds;
        FD_ZERO(&rfds); FD_SET(fd, &rfds);
        struct timeval tv = {1, 0};
        int rv = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (rv > 0 && FD_ISSET(fd, &rfds)) {
            char buf[256];
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                char *nl = strpbrk(buf, "\r\n"); if (nl) *nl = '\0';
                handle_action(buf);
            }
        }
        close(fd);
    }

    if (dpy) XCloseDisplay(dpy);
    return 0;
}
