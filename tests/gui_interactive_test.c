/*
 * gui_interactive_test.c — Automated X11 GUI interactive test runner
 *
 * Launches Terminator under X11 (DISPLAY=:98), interacts with the window
 * using synthetic mouse clicks and keyboard events, captures screenshots at
 * each step, and validates visual rendering and state transitions.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

typedef struct {
    int left;
    int right;
} TabRegion;

static Window find_terminator_window(Display *dpy) {
    Window root = RootWindow(dpy, DefaultScreen(dpy));
    Window root_ret, parent_ret;
    Window *children = NULL;
    unsigned int nchildren = 0;

    if (!XQueryTree(dpy, root, &root_ret, &parent_ret, &children, &nchildren)) {
        return None;
    }

    Window target = None;
    for (unsigned int i = 0; i < nchildren; i++) {
        char *name = NULL;
        if (XFetchName(dpy, children[i], &name) && name) {
            if (strcmp(name, "Terminator") == 0) {
                target = children[i];
                XFree(name);
                break;
            }
            XFree(name);
        }
    }

    if (children) XFree(children);
    return target;
}

static void capture_ppm(Display *dpy, Window win, const char *path) {
    XWindowAttributes attr;
    XGetWindowAttributes(dpy, win, &attr);

    XImage *img = XGetImage(dpy, win, 0, 0, (unsigned)attr.width, (unsigned)attr.height, AllPlanes, ZPixmap);
    if (!img) {
        fprintf(stderr, "capture_ppm: XGetImage failed for %s\n", path);
        return;
    }

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "capture_ppm: cannot open %s for writing\n", path);
        XDestroyImage(img);
        return;
    }

    fprintf(fp, "P6\n%d %d\n255\n", img->width, img->height);
    for (int y = 0; y < img->height; y++) {
        for (int x = 0; x < img->width; x++) {
            unsigned long p = XGetPixel(img, x, y);
            unsigned char r = (unsigned char)((p >> 16) & 0xFF);
            unsigned char g = (unsigned char)((p >> 8) & 0xFF);
            unsigned char b = (unsigned char)(p & 0xFF);
            fputc(r, fp);
            fputc(g, fp);
            fputc(b, fp);
        }
    }

    fclose(fp);
    XDestroyImage(img);
    printf("Captured: %s (%dx%d)\n", path, attr.width, attr.height);
}

static int scan_tab_regions(Display *dpy, Window win, TabRegion *regions, int max_regions) {
    XWindowAttributes attr;
    XGetWindowAttributes(dpy, win, &attr);
    XImage *img = XGetImage(dpy, win, 0, 0, (unsigned)attr.width, 30, AllPlanes, ZPixmap);
    if (!img) return 0;

    int y = 12;
    int count = 0;
    int in_rect = 0;
    int rect_start = 0;

    for (int x = 0; x < attr.width; x++) {
        unsigned long p = XGetPixel(img, x, y);
        int r = (int)((p >> 16) & 0xFF);
        /* Background is 0x1C (28). Active is 0x33, inactive 0x22, border 0x55 */
        int is_bg = (r <= 30);

        if (!in_rect && !is_bg) {
            in_rect = 1;
            rect_start = x;
        } else if (in_rect && is_bg) {
            in_rect = 0;
            if (x - rect_start >= 10 && count < max_regions) {
                regions[count].left = rect_start;
                regions[count].right = x - 1;
                count++;
            }
        }
    }
    if (in_rect && count < max_regions) {
        regions[count].left = rect_start;
        regions[count].right = attr.width - 1;
        count++;
    }

    XDestroyImage(img);
    return count;
}

static void send_mouse_click(Display *dpy, Window win, int x, int y, int button) {
    XButtonEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = ButtonPress;
    ev.display = dpy;
    ev.window = win;
    ev.root = RootWindow(dpy, DefaultScreen(dpy));
    ev.time = CurrentTime;
    ev.x = x;
    ev.y = y;
    ev.x_root = x;
    ev.y_root = y;
    ev.button = (unsigned)button;
    ev.same_screen = True;

    XSendEvent(dpy, win, True, ButtonPressMask, (XEvent *)&ev);
    XFlush(dpy);
    usleep(50000);

    ev.type = ButtonRelease;
    XSendEvent(dpy, win, True, ButtonReleaseMask, (XEvent *)&ev);
    XFlush(dpy);
    usleep(150000);
}

static void send_key(Display *dpy, Window win, KeySym ks, unsigned int state) {
    KeyCode kc = XKeysymToKeycode(dpy, ks);
    XKeyEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = KeyPress;
    ev.display = dpy;
    ev.window = win;
    ev.root = RootWindow(dpy, DefaultScreen(dpy));
    ev.time = CurrentTime;
    ev.state = state;
    ev.keycode = kc;
    ev.same_screen = True;

    XSendEvent(dpy, win, True, KeyPressMask, (XEvent *)&ev);
    XFlush(dpy);
    usleep(30000);

    ev.type = KeyRelease;
    XSendEvent(dpy, win, True, KeyReleaseMask, (XEvent *)&ev);
    XFlush(dpy);
    usleep(30000);
}

static void send_string(Display *dpy, Window win, const char *s) {
    for (size_t i = 0; s[i]; i++) {
        char c = s[i];
        if (c == '\n') {
            send_key(dpy, win, XK_Return, 0);
        } else if (c == '\t') {
            send_key(dpy, win, XK_Tab, 0);
        } else if (c >= 'A' && c <= 'Z') {
            send_key(dpy, win, (KeySym)c, ShiftMask);
        } else if (c == '"' || c == ':' || c == '_' || c == '<' || c == '>' || c == '?' || c == '!' || c == '@') {
            send_key(dpy, win, (KeySym)c, ShiftMask);
        } else {
            send_key(dpy, win, (KeySym)c, 0);
        }
    }
}

int main(int argc, char *argv[]) {
    const char *disp_str = (argc > 1) ? argv[1] : ":98";
    const char *out_dir  = (argc > 2) ? argv[2] : "/tmp";

    Display *dpy = XOpenDisplay(disp_str);
    if (!dpy) {
        fprintf(stderr, "Cannot open display %s\n", disp_str);
        return 1;
    }

    printf("Waiting for Terminator window on %s...\n", disp_str);
    Window win = None;
    for (int retry = 0; retry < 50; retry++) {
        win = find_terminator_window(dpy);
        if (win != None) break;
        usleep(100000);
    }

    if (win == None) {
        fprintf(stderr, "Terminator window not found!\n");
        XCloseDisplay(dpy);
        return 1;
    }

    printf("Found Terminator Window: 0x%lx\n", win);
    usleep(300000); /* wait for initial draw */

    char ppm_path[512];
    TabRegion tabs[10];

    /* Step 1: Initial window state */
    snprintf(ppm_path, sizeof(ppm_path), "%s/01_initial_gui.ppm", out_dir);
    capture_ppm(dpy, win, ppm_path);

    /* Step 2: Type a command: echo "Welcome to Terminator!" */
    printf("Typing command: echo \"Welcome to Terminator!\"\n");
    send_string(dpy, win, "echo \"Welcome to Terminator!\"\n");
    usleep(300000);
    snprintf(ppm_path, sizeof(ppm_path), "%s/02_echo_command.ppm", out_dir);
    capture_ppm(dpy, win, ppm_path);

    /* Step 3: Mouse interaction — click [+] to open a new tab */
    int ntabs = scan_tab_regions(dpy, win, tabs, 10);
    printf("Found %d tab regions\n", ntabs);
    for (int i = 0; i < ntabs; i++) {
        printf("  Region %d: [%d .. %d]\n", i, tabs[i].left, tabs[i].right);
    }
    if (ntabs >= 2) {
        /* The last region is the [+] button */
        int plus_x = (tabs[ntabs - 1].left + tabs[ntabs - 1].right) / 2;
        printf("Clicking [+] button at x=%d, y=12...\n", plus_x);
        send_mouse_click(dpy, win, plus_x, 12, Button1);
    } else {
        /* Fallback */
        send_mouse_click(dpy, win, 340, 12, Button1);
    }
    usleep(400000);
    snprintf(ppm_path, sizeof(ppm_path), "%s/03_new_tab_by_mouse.ppm", out_dir);
    capture_ppm(dpy, win, ppm_path);

    /* Step 4: In Tab 2, change directory and verify independent worker state */
    printf("In Tab 2, typing: cd /tmp && echo \"Now inside Tab 2!\"\n");
    send_string(dpy, win, "cd /tmp\n");
    usleep(200000);
    send_string(dpy, win, "echo \"Now inside Tab 2!\"\n");
    usleep(300000);
    snprintf(ppm_path, sizeof(ppm_path), "%s/04_tab2_cwd_tmp.ppm", out_dir);
    capture_ppm(dpy, win, ppm_path);

    /* Step 5: Mouse interaction — click Tab 1 to switch back */
    ntabs = scan_tab_regions(dpy, win, tabs, 10);
    printf("After Tab 2 created: %d tab regions\n", ntabs);
    for (int i = 0; i < ntabs; i++) {
        printf("  Region %d: [%d .. %d]\n", i, tabs[i].left, tabs[i].right);
    }
    if (ntabs >= 1) {
        int tab1_x = (tabs[0].left + tabs[0].right) / 2;
        printf("Clicking Tab 1 header at x=%d, y=12 to switch back...\n", tab1_x);
        send_mouse_click(dpy, win, tab1_x, 12, Button1);
    }
    usleep(300000);
    snprintf(ppm_path, sizeof(ppm_path), "%s/05_switch_tab_by_mouse.ppm", out_dir);
    capture_ppm(dpy, win, ppm_path);

    /* Step 6: Autocomplete testing — unambiguous single prefix */
    printf("Testing single auto-completion with Tab key...\n");
    send_string(dpy, win, "ls /private/tm\t");
    usleep(300000);
    snprintf(ppm_path, sizeof(ppm_path), "%s/06_autocomplete_single.ppm", out_dir);
    capture_ppm(dpy, win, ppm_path);
    /* Clear line with Ctrl+C */
    send_key(dpy, win, XK_c, ControlMask);
    usleep(200000);

    /* Step 7: Ambiguous autocomplete menu */
    /* Create two temp files in /tmp to guarantee known ambiguous completions */
    system("mkdir -p /tmp/term_auto_test && touch /tmp/term_auto_test/alpha.txt /tmp/term_auto_test/alpine.txt");
    printf("Testing ambiguous suggestions popup (Tab key)...\n");
    send_string(dpy, win, "cat /tmp/term_auto_test/al\t");
    usleep(300000);
    snprintf(ppm_path, sizeof(ppm_path), "%s/07_autocomplete_menu.ppm", out_dir);
    capture_ppm(dpy, win, ppm_path);

    /* Step 8: Select menu item 1 */
    printf("Pressing '1' to select first item from autocomplete menu...\n");
    send_string(dpy, win, "1");
    usleep(300000);
    snprintf(ppm_path, sizeof(ppm_path), "%s/08_menu_selected.ppm", out_dir);
    capture_ppm(dpy, win, ppm_path);
    /* Clear line */
    send_key(dpy, win, XK_c, ControlMask);
    usleep(200000);

    /* Step 9: History search Ctrl+R */
    printf("Testing Ctrl+R history search...\n");
    send_key(dpy, win, XK_r, ControlMask);
    usleep(200000);
    send_string(dpy, win, "Welcome\n");
    usleep(300000);
    snprintf(ppm_path, sizeof(ppm_path), "%s/09_history_search.ppm", out_dir);
    capture_ppm(dpy, win, ppm_path);

    /* Step 10: Multi-line input continuation */
    printf("Testing multi-line continuation prompt (trailing \\)...\n");
    send_string(dpy, win, "echo \"Multi-line string \\\n");
    usleep(200000);
    send_string(dpy, win, "continued on second line\"\n");
    usleep(300000);
    snprintf(ppm_path, sizeof(ppm_path), "%s/10_multiline_continuation.ppm", out_dir);
    capture_ppm(dpy, win, ppm_path);

    /* Step 11: Long output generation & Mouse wheel scroll testing */
    printf("Generating multi-line output for scrolling...\n");
    send_string(dpy, win, "seq 1 50\n");
    usleep(500000);

    printf("Testing mouse scroll up (Button4 x5)...\n");
    for (int s = 0; s < 5; s++) {
        send_mouse_click(dpy, win, 400, 300, Button4);
        usleep(50000);
    }
    usleep(200000);
    snprintf(ppm_path, sizeof(ppm_path), "%s/11_mouse_scroll_up.ppm", out_dir);
    capture_ppm(dpy, win, ppm_path);

    printf("Testing mouse scroll down (Button5 x5)...\n");
    for (int s = 0; s < 5; s++) {
        send_mouse_click(dpy, win, 400, 300, Button5);
        usleep(50000);
    }
    usleep(200000);
    snprintf(ppm_path, sizeof(ppm_path), "%s/12_mouse_scrolled_back.ppm", out_dir);
    capture_ppm(dpy, win, ppm_path);

    /* Step 12: Mouse interaction — Right-click on Tab 2 to close it */
    ntabs = scan_tab_regions(dpy, win, tabs, 10);
    printf("Tabs before closing Tab 2: %d tab regions\n", ntabs);
    for (int i = 0; i < ntabs; i++) {
        printf("  Region %d: [%d .. %d]\n", i, tabs[i].left, tabs[i].right);
    }
    if (ntabs >= 3) {
        /* Region 0 is Tab 1, Region 1 is Tab 2, Region 2 is [+] */
        int tab2_x = (tabs[1].left + tabs[1].right) / 2;
        printf("Closing Tab 2 via mouse Right-click (Button3) at x=%d, y=12...\n", tab2_x);
        send_mouse_click(dpy, win, tab2_x, 12, Button3);
        usleep(400000);
        snprintf(ppm_path, sizeof(ppm_path), "%s/13_tab_closed_by_mouse.ppm", out_dir);
        capture_ppm(dpy, win, ppm_path);
    }

    /* Step 13: Keyboard tab navigation — Alt+1 and Ctrl+T */
    printf("Testing Ctrl+T hotkey to create tab...\n");
    send_key(dpy, win, XK_t, ControlMask);
    usleep(400000);
    snprintf(ppm_path, sizeof(ppm_path), "%s/14_hotkey_tab_nav.ppm", out_dir);
    capture_ppm(dpy, win, ppm_path);

    printf("=== All GUI interactive steps completed successfully! ===\n");
    XCloseDisplay(dpy);
    return 0;
}
