#include <errno.h>
#include <fcntl.h>
#include <libinput.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <libudev.h>
#ifdef WITH_WAYLAND
# include <wayland-client.h>
#endif

/* Defines */
#define MAXSLOTS 20
#define NOMOTION -999999
#define REINIT_RETRY_COUNT 5 // Number of retries for init_libinput
#define REINIT_RETRY_DELAY_US 200000 // Delay of 200ms for retries
#define REINIT_TIMEOUT_MS 10000 // Fallback reinitialization after 10s of no activity
#define PERIODIC_REINIT_MS 3000 // Periodic reinitialization every 3s if uninitialized

/* Types */
enum {
  SwipeDU,
  SwipeUD,
  SwipeLR,
  SwipeRL,
  SwipeDLUR,
  SwipeDRUL,
  SwipeURDL,
  SwipeULDR
};
typedef int Swipe;

enum {
    EdgeAny,
    EdgeNone,
    EdgeLeft,
    EdgeRight,
    EdgeTop,
    EdgeBottom,
    CornerTopLeft,
    CornerTopRight,
    CornerBottomLeft,
    CornerBottomRight,
};
typedef int Edge;

enum {
    DistanceAny,
    DistanceShort,
    DistanceMedium,
    DistanceLong,
};
typedef int Distance;

enum {
    ActModeReleased,
    ActModePressed,
};
typedef int ActMode;

typedef struct {
    int nfswipe;
    Swipe swipe;
    Edge edge;
    Distance distance;
    ActMode actmode;
    char *command;
} Gesture;

/* Config */
#include "config.h"

/* Globals */
Gesture *gestsarr;
int gestsarrlen;
int have_actmode_pressed = 0;
Swipe pendingswipe;
Edge pendingedge;
Distance pendingdistance;
double xstart[MAXSLOTS], xend[MAXSLOTS], ystart[MAXSLOTS], yend[MAXSLOTS];
unsigned nfdown = 0;
unsigned nfpendingswipe = 0;
struct timespec timedown;
#ifdef WITH_WAYLAND
struct wl_display *wl_display;
struct wl_registry *wl_registry;
struct wl_output *wl_output;
#endif
static int screenwidth = 0, screenheight = 0;
static int current_orientation = -1;
static struct libinput *li = NULL;
static int libinput_fd = -1;
static ino_t last_inode = 0; // Track inode for debugging
static bool last_init_success = false;
static int active_devices = 0; // Track number of active touchscreen devices

void
die(char *msg)
{
    fprintf(stderr, "%s\n", msg);
    exit(1);
}

int
gesturecalculateswipewithindegrees(double gestdegrees, double wantdegrees) {
    return (
        gestdegrees >= wantdegrees - degreesleniency &&
        gestdegrees <= wantdegrees + degreesleniency
    );
}

Swipe
gesturecalculateswipe(double x0, double y0, double x1, double y1, int mindistance) {
    double t, degrees, dist;
    t = atan2(x1 - x0, y0 - y1);
    degrees = 57.2957795130823209 * (t < 0 ? t + 6.2831853071795865 : t);
    dist = sqrt(pow(x1 - x0, 2) + pow(y1 - y0, 2));
    if (verbose)
        fprintf(stderr, "Swipe calc: x0=%.2f, y0=%.2f, x1=%.2f, y1=%.2f, distance=%.2f, degrees=%.2f, mindistance=%d\n",
            x0, y0, x1, y1, dist, degrees, mindistance);
    if (dist < mindistance) return -1;
    else if (gesturecalculateswipewithindegrees(degrees, 0))   return SwipeDU;
    else if (gesturecalculateswipewithindegrees(degrees, 45))  return SwipeDLUR;
    else if (gesturecalculateswipewithindegrees(degrees, 90))  return SwipeLR;
    else if (gesturecalculateswipewithindegrees(degrees, 135)) return SwipeULDR;
    else if (gesturecalculateswipewithindegrees(degrees, 180)) return SwipeUD;
    else if (gesturecalculateswipewithindegrees(degrees, 225)) return SwipeURDL;
    else if (gesturecalculateswipewithindegrees(degrees, 270)) return SwipeRL;
    else if (gesturecalculateswipewithindegrees(degrees, 315)) return SwipeDRUL;
    else if (gesturecalculateswipewithindegrees(degrees, 360)) return SwipeDU;
    return -1;
}

Distance
gesturecalculatedistance(double x0, double y0, double x1, double y1, Swipe swipe) {
    double dist = sqrt(pow(x1 - x0, 2) + pow(y1 - y0, 2));
    double diag = sqrt(pow(screenwidth, 2) + pow(screenheight, 2));
    if (verbose)
        fprintf(stderr, "Distance calc: dist=%.2f, screenwidth=%d, screenheight=%d, diag=%.2f\n",
            dist, screenwidth, screenheight, diag);
    switch (swipe) {
        case SwipeDU:
        case SwipeUD:
            if (dist >= screenheight * 0.66) return DistanceLong;
            else if (dist >= screenheight * 0.33) return DistanceMedium;
            else return DistanceShort;
        case SwipeLR:
        case SwipeRL:
            if (dist >= screenwidth * 0.66) return DistanceLong;
            else if (dist >= screenwidth * 0.33) return DistanceMedium;
            else return DistanceShort;
        case SwipeULDR:
        case SwipeDRUL:
        case SwipeDLUR:
        case SwipeURDL:
            if (dist >= diag * 0.66) return DistanceLong;
            else if (dist >= diag * 0.33) return DistanceMedium;
            else return DistanceShort;
    }
    return DistanceAny;
}

Edge
gesturecalculateedge(double x0, double y0, double x1, double y1) {
    Edge horizontal = EdgeNone;
    Edge vertical = EdgeNone;
    if (verbose) {
        fprintf(stderr, "Edge calc: x0=%.2f, y0=%.2f, x1=%.2f, y1=%.2f\n", x0, y0, x1, y1);
        fprintf(stderr, "Edge thresholds: left=%.2f, right=%.2f, top=%.2f, bottom=%.2f, scaling=%.2f\n",
            edgesizeleft * edgessizescaling, screenwidth - edgesizeright * edgessizescaling,
            edgesizetop * edgessizescaling, screenheight - edgesizebottom * edgessizescaling, edgessizescaling);
    }
    if (screenwidth == 0 || screenheight == 0) {
        if (verbose) fprintf(stderr, "Warning: screenwidth or screenheight is 0, edge detection may fail\n");
        return EdgeNone;
    }
    if (x0 <= edgesizeleft * edgessizescaling) horizontal = EdgeLeft;
    else if (x0 >= screenwidth - edgesizeright * edgessizescaling) horizontal = EdgeRight;
    else if (x1 <= edgesizeleft * edgessizescaling) horizontal = EdgeLeft;
    else if (x1 >= screenwidth - edgesizeright * edgessizescaling) horizontal = EdgeRight;
    if (y0 <= edgesizetop * edgessizescaling) vertical = EdgeTop;
    else if (y0 >= screenheight - edgesizebottom * edgessizescaling) vertical = EdgeBottom;
    else if (y1 <= edgesizetop * edgessizescaling) vertical = EdgeTop;
    else if (y1 >= screenheight - edgesizebottom * edgessizescaling) vertical = EdgeBottom;
    if (horizontal == EdgeLeft && vertical == EdgeTop) return CornerTopLeft;
    else if (horizontal == EdgeRight && vertical == EdgeTop) return CornerTopRight;
    else if (horizontal == EdgeLeft && vertical == EdgeBottom) return CornerBottomLeft;
    else if (horizontal == EdgeRight && vertical == EdgeBottom) return CornerBottomRight;
    else if (horizontal != EdgeNone) return horizontal;
    else return vertical;
}

int
gestureexecute(Swipe swipe, int nfingers, Edge edge, Distance distance, ActMode actmode) {
    int i, ret;
    for (i = 0; i < gestsarrlen; i++) {
        if (verbose) {
            fprintf(stderr,
                "[swipe]: Cfg(f=%d/s=%d,e=%d,d=%d) <=> Evt(f=%d/s=%d,e=%d,d=%d)\n",
                gestsarr[i].nfswipe, gestsarr[i].swipe, gestsarr[i].edge, gestsarr[i].distance,
                nfingers, swipe, edge, distance
            );
        }
        if (gestsarr[i].nfswipe == nfingers && gestsarr[i].swipe == swipe
            && gestsarr[i].distance <= distance
            && (gestsarr[i].edge == EdgeAny || gestsarr[i].edge == edge ||
                ((edge == CornerTopLeft || edge == CornerTopRight) && gestsarr[i].edge == EdgeTop) ||
                ((edge == CornerBottomLeft || edge == CornerBottomRight) && gestsarr[i].edge == EdgeBottom) ||
                ((edge == CornerTopLeft || edge == CornerBottomLeft) && gestsarr[i].edge == EdgeLeft) ||
                ((edge == CornerTopRight || edge == CornerBottomRight) && gestsarr[i].edge == EdgeRight)
               )
            && gestsarr[i].actmode == actmode
            ) {
            if (verbose) fprintf(stderr, "Execute %s\n", gestsarr[i].command);
            ret = system(gestsarr[i].command);
            if (verbose && ret != 0) fprintf(stderr, "Command '%s' returned %d\n", gestsarr[i].command, ret);
            return 1;
        }
    }
    return 0;
}

static int
libinputopenrestricted(const char *path, int flags, void *user_data __attribute__((unused)))
{
    int fd = open(path, flags | O_NONBLOCK);
    if (fd < 0 && verbose) fprintf(stderr, "Failed to open device %s: %s\n", path, strerror(errno));
    return fd < 0 ? -errno : fd;
}

static void
libinputcloserestricted(int fd, void *user_data __attribute__((unused)))
{
    close(fd);
}

Swipe
swipereorient(Swipe swipe, int orientation) {
    while (orientation > 0) {
        switch(swipe) {
            case SwipeDU:   swipe = SwipeLR; break;
            case SwipeDLUR: swipe = SwipeULDR; break;
            case SwipeLR:   swipe = SwipeUD; break;
            case SwipeULDR: swipe = SwipeURDL; break;
            case SwipeUD:   swipe = SwipeRL; break;
            case SwipeURDL: swipe = SwipeDRUL; break;
            case SwipeRL:   swipe = SwipeDU; break;
            case SwipeDRUL: swipe = SwipeDLUR; break;
        }
        orientation--;
    }
    return swipe;
}

Edge
edgereorient(Edge edge, int orientation) {
    while (orientation > 0) {
        switch(edge) {
            case EdgeLeft:   edge = EdgeTop; break;
            case EdgeRight:  edge = EdgeBottom; break;
            case EdgeTop:    edge = EdgeRight; break;
            case EdgeBottom: edge = EdgeLeft; break;
            case CornerTopLeft: edge = CornerTopRight; break;
            case CornerTopRight: edge = CornerBottomRight; break;
            case CornerBottomLeft: edge = CornerTopLeft; break;
            case CornerBottomRight: edge = CornerBottomLeft; break;
        }
        orientation--;
    }
    return edge;
}

void
reorientgestures(int new_orientation) {
    int i, delta;
    if (current_orientation == -1) delta = new_orientation;
    else delta = (new_orientation - current_orientation + 4) % 4;
    if (delta == 0) return;
    if (verbose) fprintf(stderr, "Reorienting gestures: %d -> %d (delta=%d)\n", current_orientation, new_orientation, delta);
    for (i = 0; i < gestsarrlen; i++) {
        gestsarr[i].swipe = swipereorient(gestsarr[i].swipe, delta);
        gestsarr[i].edge = edgereorient(gestsarr[i].edge, delta);
    }
    current_orientation = new_orientation;
}

void
touchdown(struct libinput_event *e)
{
    struct libinput_event_touch *tevent;
    int slot;
    tevent = libinput_event_get_touch_event(e);
    slot = libinput_event_touch_get_slot(tevent);
    xstart[slot] = libinput_event_touch_get_x_transformed(tevent, screenwidth);
    ystart[slot] = libinput_event_touch_get_y_transformed(tevent, screenheight);
    if (verbose) {
        double raw_x = libinput_event_touch_get_x(tevent);
        double raw_y = libinput_event_touch_get_y(tevent);
        fprintf(stderr, "Touch down: slot=%d, x=%.2f, y=%.2f, raw_x=%.2f, raw_y=%.2f\n",
            slot, xstart[slot], ystart[slot], raw_x, raw_y);
    }
    if (nfdown == 0) clock_gettime(CLOCK_MONOTONIC_RAW, &timedown);
    nfdown++;
}

void
resetslot(int slot) {
    xend[slot] = NOMOTION;
    yend[slot] = NOMOTION;
    xstart[slot] = NOMOTION;
    ystart[slot] = NOMOTION;
}

void
touchmotion(struct libinput_event *e)
{
    struct libinput_event_touch *tevent;
    struct timespec now;
    int slot;
    tevent = libinput_event_get_touch_event(e);
    slot = libinput_event_touch_get_slot(tevent);
    xend[slot] = libinput_event_touch_get_x_transformed(tevent, screenwidth);
    yend[slot] = libinput_event_touch_get_y_transformed(tevent, screenheight);
    if (verbose) {
        double raw_x = libinput_event_touch_get_x(tevent);
        double raw_y = libinput_event_touch_get_y(tevent);
        fprintf(stderr, "Touch motion: slot=%d, x=%.2f, y=%.2f, raw_x=%.2f, raw_y=%.2f\n",
            slot, xend[slot], yend[slot], raw_x, raw_y);
    }
    if (have_actmode_pressed) {
        Swipe swipe = gesturecalculateswipe(
            xstart[slot], ystart[slot], xend[slot], yend[slot], distancethreshold_pressed
        );
        if (swipe != -1) {
            Edge edge = gesturecalculateedge(
                xstart[slot], ystart[slot], xend[slot], yend[slot]
            );
            clock_gettime(CLOCK_MONOTONIC_RAW, &now);
            if (
                timeoutms >
                ((now.tv_sec - timedown.tv_sec) * 1000000 + (now.tv_nsec - timedown.tv_nsec) / 1000) / 1000
            ) {
                if (verbose) fprintf(stderr, "(Attempting to find matching pressed gesture)\n");
                Distance distance = gesturecalculatedistance(  
                    xstart[slot], ystart[slot], xend[slot], yend[slot], swipe  
                );  
                if (gestureexecute(swipe, nfdown, edge, distance, ActModePressed)) {
                    if (verbose) fprintf(stderr, "(Pressed gesture Executed)\n");
                    xstart[slot] = xend[slot];
                    ystart[slot] = yend[slot];
                    timedown = now;
                }
            }
        }
    }
}

void
touchup(struct libinput_event *e)
{
    int slot;
    struct libinput_event_touch *tevent;
    struct timespec now;
    tevent = libinput_event_get_touch_event(e);
    slot = libinput_event_touch_get_slot(tevent);
    nfdown--;
    clock_gettime(CLOCK_MONOTONIC_RAW, &now);
    if (
        xstart[slot] == NOMOTION || ystart[slot] == NOMOTION ||
        xend[slot] == NOMOTION || yend[slot] == NOMOTION
    ) return;
    Swipe swipe = gesturecalculateswipe(
        xstart[slot], ystart[slot], xend[slot], yend[slot], distancethreshold
    );
    Edge edge = gesturecalculateedge(
        xstart[slot], ystart[slot], xend[slot], yend[slot]
    );
    Distance distance = gesturecalculatedistance(
        xstart[slot], ystart[slot], xend[slot], yend[slot], swipe
    );
    if (nfpendingswipe == 0) {
        pendingswipe = swipe;
        pendingedge = edge;
        pendingdistance = distance;
    }
    if (pendingswipe == swipe) nfpendingswipe++;
    resetslot(slot);
    if (nfdown == 0) {
        if (
            timeoutms >
            ((now.tv_sec - timedown.tv_sec) * 1000000 + (now.tv_nsec - timedown.tv_nsec) / 1000) / 1000
        ) gestureexecute(swipe, nfpendingswipe, edge, distance, ActModeReleased);
        nfpendingswipe = 0;
    }
}

static int
init_libinput(void) {
    int retry_count = REINIT_RETRY_COUNT;
    struct udev *udev = NULL;

    // Clean up existing context
    if (li) {
        if (verbose) fprintf(stderr, "Cleaning up previous libinput context\n");
        if (libinput_fd >= 0) {
            close(libinput_fd);
            libinput_fd = -1;
        }
        libinput_unref(li);
        li = NULL;
        last_init_success = false;
        last_inode = 0;
        active_devices = 0;
    }

    static const struct libinput_interface interface = {
        .open_restricted = libinputopenrestricted,
        .close_restricted = libinputcloserestricted,
    };

    while (retry_count > 0) {
        udev = udev_new();
        if (!udev) {
            if (verbose) fprintf(stderr, "Failed to initialize udev context (retry %d/%d)\n", REINIT_RETRY_COUNT - retry_count, REINIT_RETRY_COUNT);
            usleep(REINIT_RETRY_DELAY_US);
            retry_count--;
            continue;
        }

        li = libinput_udev_create_context(&interface, NULL, udev);
        if (!li) {
            if (verbose) fprintf(stderr, "Failed to initialize libinput udev context: %s (retry %d/%d)\n", strerror(errno), REINIT_RETRY_COUNT - retry_count, REINIT_RETRY_COUNT);
            udev_unref(udev);
            udev = NULL;
            last_init_success = false;
            usleep(REINIT_RETRY_DELAY_US);
            retry_count--;
            continue;
        }

        if (libinput_udev_assign_seat(li, "seat0") < 0) {
            if (verbose) fprintf(stderr, "Failed to assign seat0: %s (retry %d/%d)\n", strerror(errno), REINIT_RETRY_COUNT - retry_count, REINIT_RETRY_COUNT);
            libinput_unref(li);
            li = NULL;
            udev_unref(udev);
            udev = NULL;
            last_init_success = false;
            usleep(REINIT_RETRY_DELAY_US);
            retry_count--;
            continue;
        }

        libinput_fd = libinput_get_fd(li);
        if (verbose) fprintf(stderr, "Libinput udev context initialized (fd: %d)\n", libinput_fd);

        // Check for touchscreen devices
        if (libinput_dispatch(li) < 0) {
            if (verbose) fprintf(stderr, "Initial libinput dispatch failed: %s\n", strerror(errno));
        }
        struct libinput_event *event;
        active_devices = 0;
        while ((event = libinput_get_event(li))) {
            if (libinput_event_get_type(event) == LIBINPUT_EVENT_DEVICE_ADDED) {
                struct libinput_device *dev = libinput_event_get_device(event);
                const char *name = libinput_device_get_name(dev);
                if (libinput_device_has_capability(dev, LIBINPUT_DEVICE_CAP_TOUCH)) {
                    active_devices++;
                    if (verbose) fprintf(stderr, "Device added: %s (touchscreen)\n", name ? name : "unknown");
                    if (name && strstr(name, "IPTSD Virtual Touchscreen")) {
                        if (verbose) fprintf(stderr, "Target touchscreen found: %s\n", name);
                    }
                }
                // Enable events
                if (libinput_device_config_send_events_set_mode(dev, LIBINPUT_CONFIG_SEND_EVENTS_ENABLED) != LIBINPUT_CONFIG_STATUS_SUCCESS) {
                    if (verbose) fprintf(stderr, "Failed to set event mode for %s\n", name ? name : "unknown");
                }
            }
            libinput_event_destroy(event);
        }

        if (active_devices > 0) {
            if (verbose) fprintf(stderr, "Found %d touchscreen device(s)\n", active_devices);
            last_init_success = true;
            udev_unref(udev);
            return libinput_fd;
        } else {
            if (verbose) fprintf(stderr, "No touchscreen devices found (retry %d/%d)\n", REINIT_RETRY_COUNT - retry_count, REINIT_RETRY_COUNT);
            libinput_unref(li);
            li = NULL;
            udev_unref(udev);
            udev = NULL;
            last_init_success = false;
            usleep(REINIT_RETRY_DELAY_US);
            retry_count--;
            continue;
        }
    }

    if (verbose) fprintf(stderr, "Failed to initialize libinput udev context after %d retries\n", REINIT_RETRY_COUNT);
    last_init_success = false;
    if (udev) udev_unref(udev);
    return -1;
}

void
run(void) {
    int i, max_fd;
    struct libinput_event *event;
    fd_set fdset;
    static struct timespec last_reinit = {0, 0}; // Track last reinitialization
    static struct timespec last_activity = {0, 0}; // Track last activity
    static struct timespec last_periodic_check = {0, 0}; // Track last periodic check

    if (init_libinput() < 0) {
        fprintf(stderr, "Initial libinput setup failed, retrying in loop\n");
    } else {
        clock_gettime(CLOCK_MONOTONIC, &last_activity);
        last_periodic_check = last_activity;
    }

    for (i = 0; i < MAXSLOTS; i++) {
        xend[i] = NOMOTION;
        yend[i] = NOMOTION;
        xstart[i] = NOMOTION;
        ystart[i] = NOMOTION;
    }

    for (;;) {
        FD_ZERO(&fdset);
        max_fd = -1;

        if (libinput_fd >= 0) {
            FD_SET(libinput_fd, &fdset);
            if (libinput_fd > max_fd) max_fd = libinput_fd;
        }
#ifdef WITH_WAYLAND
        if (wl_display) {
            int wl_fd = wl_display_get_fd(wl_display);
            FD_SET(wl_fd, &fdset);
            if (wl_fd > max_fd) max_fd = wl_fd;
        }
#endif

        struct timeval timeout;
        timeout.tv_sec = REINIT_TIMEOUT_MS / 1000;
        timeout.tv_usec = (REINIT_TIMEOUT_MS % 1000) * 1000;

        if (verbose) fprintf(stderr, "Select loop: libinput_fd=%d, max_fd=%d\n", libinput_fd, max_fd);

        int select_result = select(max_fd + 1, &fdset, NULL, NULL, &timeout);
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        if (select_result < 0 && errno != EINTR) {
            if (verbose) fprintf(stderr, "Select failed: %s\n", strerror(errno));
            long long ms_since_reinit = (now.tv_sec - last_reinit.tv_sec) * 1000 +
                                        (now.tv_nsec - last_reinit.tv_nsec) / 1000000;
            if (ms_since_reinit > 2000) {
                if (verbose) fprintf(stderr, "Reinitializing due to select failure\n");
                if (init_libinput() < 0) {
                    if (verbose) fprintf(stderr, "Reinitialization failed, retrying\n");
                    usleep(REINIT_RETRY_DELAY_US);
                } else {
                    if (verbose) fprintf(stderr, "Reinitialization successful\n");
                    last_reinit = now;
                    last_activity = now;
                    last_periodic_check = now;
                }
            }
            continue;
        } else if (select_result == 0) {
            // Timeout occurred, check for inactivity
            long long ms_since_activity = (now.tv_sec - last_activity.tv_sec) * 1000 +
                                          (now.tv_nsec - last_activity.tv_nsec) / 1000000;
            if (ms_since_activity > REINIT_TIMEOUT_MS && !last_init_success) {
                if (verbose) fprintf(stderr, "No activity for %dms, forcing reinitialization\n", REINIT_TIMEOUT_MS);
                if (init_libinput() < 0) {
                    if (verbose) fprintf(stderr, "Fallback reinitialization failed\n");
                } else {
                    if (verbose) fprintf(stderr, "Fallback reinitialization successful\n");
                    last_reinit = now;
                    last_activity = now;
                    last_periodic_check = now;
                }
            }
            continue;
        }

        last_activity = now; // Update activity timestamp

        // Periodic reinitialization check
        long long ms_since_periodic = (now.tv_sec - last_periodic_check.tv_sec) * 1000 +
                                      (now.tv_nsec - last_periodic_check.tv_nsec) / 1000000;
        if (ms_since_periodic >= PERIODIC_REINIT_MS && !last_init_success) {
            if (verbose) fprintf(stderr, "Periodic check: %lldms elapsed, libinput not initialized, reinitializing\n", ms_since_periodic);
            if (init_libinput() < 0) {
                if (verbose) fprintf(stderr, "Periodic reinitialization failed\n");
            } else {
                if (verbose) fprintf(stderr, "Periodic reinitialization successful\n");
                last_reinit = now;
                last_activity = now;
                last_periodic_check = now;
            }
        }
        last_periodic_check = now;

        if (libinput_fd >= 0 && FD_ISSET(libinput_fd, &fdset)) {
            if (verbose) fprintf(stderr, "Processing libinput event\n");
            if (libinput_dispatch(li) < 0) {
                if (verbose) fprintf(stderr, "Libinput dispatch failed, reinitializing\n");
                long long ms_since_reinit = (now.tv_sec - last_reinit.tv_sec) * 1000 +
                                            (now.tv_nsec - last_reinit.tv_nsec) / 1000000;
                if (ms_since_reinit > 2000) {
                    if (init_libinput() < 0) {
                        if (verbose) fprintf(stderr, "Reinitialization failed, retrying\n");
                        usleep(REINIT_RETRY_DELAY_US);
                    } else {
                        if (verbose) fprintf(stderr, "Reinitialization successful\n");
                        last_reinit = now;
                        last_activity = now;
                        last_periodic_check = now;
                    }
                }
            }
            while ((event = libinput_get_event(li)) != NULL) {
                switch (libinput_event_get_type(event)) {
                    case LIBINPUT_EVENT_DEVICE_ADDED: {
                        struct libinput_device *dev = libinput_event_get_device(event);
                        const char *name = libinput_device_get_name(dev);
                        if (libinput_device_has_capability(dev, LIBINPUT_DEVICE_CAP_TOUCH)) {
                            active_devices++;
                            if (verbose) fprintf(stderr, "Device added: %s (touchscreen), active_devices=%d\n", name ? name : "unknown", active_devices);
                            if (name && strstr(name, "IPTSD Virtual Touchscreen")) {
                                if (verbose) fprintf(stderr, "Target touchscreen found: %s\n", name);
                            }
                            // Enable events
                            if (libinput_device_config_send_events_set_mode(dev, LIBINPUT_CONFIG_SEND_EVENTS_ENABLED) != LIBINPUT_CONFIG_STATUS_SUCCESS) {
                                if (verbose) fprintf(stderr, "Failed to set event mode for %s\n", name ? name : "unknown");
                            }
                            last_init_success = true;
                        }
                        break;
                    }
                    case LIBINPUT_EVENT_DEVICE_REMOVED: {
                        struct libinput_device *dev = libinput_event_get_device(event);
                        const char *name = libinput_device_get_name(dev);
                        if (libinput_device_has_capability(dev, LIBINPUT_DEVICE_CAP_TOUCH)) {
                            active_devices--;
                            if (active_devices < 0) active_devices = 0;
                            if (verbose) fprintf(stderr, "Device removed: %s (touchscreen), active_devices=%d\n", name ? name : "unknown", active_devices);
                            if (active_devices == 0) {
                                last_init_success = false;
                            }
                        }
                        break;
                    }
                    case LIBINPUT_EVENT_TOUCH_DOWN: touchdown(event); break;
                    case LIBINPUT_EVENT_TOUCH_UP: touchup(event); break;
                    case LIBINPUT_EVENT_TOUCH_MOTION: touchmotion(event); break;
                    default: break; // Ignore non-touch events
                }
                libinput_event_destroy(event);
            }
        }

#ifdef WITH_WAYLAND
        if (wl_display && FD_ISSET(wl_display_get_fd(wl_display), &fdset)) {
            if (verbose) fprintf(stderr, "Processing Wayland event\n");
            if (wl_display_dispatch(wl_display) == -1) {
                if (verbose) fprintf(stderr, "Wayland dispatch failed: %s\n", strerror(errno));
            }
        }
#endif
    }

    if (li) libinput_unref(li);
}

#ifdef WITH_WAYLAND
static void
display_handle_geometry(void *data __attribute__((unused)), struct wl_output *wl_output __attribute__((unused)), int x __attribute__((unused)), int y __attribute__((unused)), int physical_width __attribute__((unused)), int physical_height __attribute__((unused)), int subpixel __attribute__((unused)), const char *make __attribute__((unused)), const char *model __attribute__((unused)), int transform)
{
    int new_orientation = transform;
    if (new_orientation == 1) new_orientation = 3;
    else if (new_orientation == 3) new_orientation = 1;
    if (verbose) {
        fprintf(stderr, "Geometry update: transform=%d, new_orientation=%d, current_orientation=%d, screenwidth=%d, screenheight=%d\n",
            transform, new_orientation, current_orientation, screenwidth, screenheight);
    }
    if (new_orientation != current_orientation) {
        reorientgestures(new_orientation);
    }
}

static void
display_handle_done(void *data __attribute__((unused)), struct wl_output *wl_output __attribute__((unused)))
{}

static void
display_handle_scale(void *data __attribute__((unused)), struct wl_output *wl_output __attribute__((unused)), int32_t scale __attribute__((unused)))
{}

static void
display_handle_mode(void *data __attribute__((unused)), struct wl_output *wl_output __attribute__((unused)), uint32_t flags __attribute__((unused)), int width, int height, int refresh __attribute__((unused)))
{
    screenwidth = width;
    screenheight = height;
    if (verbose) fprintf(stderr, "Screen dimensions: width=%d, height=%d\n", screenwidth, screenheight);
}

static const struct wl_output_listener
output_listener = {
    .geometry = display_handle_geometry,
    .mode = display_handle_mode,
    .done = display_handle_done,
    .scale = display_handle_scale
};

static void
registry_global(void *data __attribute__((unused)), struct wl_registry *wl_registry,
        uint32_t name, const char *interface, uint32_t version __attribute__((unused)))
{
    if (strcmp(interface, "wl_output") == 0) {
        if (!wl_output) {
            wl_output = wl_registry_bind(wl_registry, name, &wl_output_interface, 3);
            wl_output_add_listener(wl_output, &output_listener, NULL);
            if (verbose) fprintf(stderr, "Bound wl_output interface\n");
        }
    }
}

static void
registry_global_remove(void *data __attribute__((unused)),
        struct wl_registry *wl_registry __attribute__((unused)), uint32_t name __attribute__((unused)))
{}

static const struct wl_registry_listener
wl_registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};
#endif

int
main(int argc, char *argv[])
{
    int i, j;
    char *gestpt;

    gestsarr = NULL;
    gestsarrlen = 0;
    prctl(PR_SET_NAME, "lisgd");
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    prctl(PR_SET_PDEATHSIG, SIGKILL);

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) {
            verbose = 1;
        } else if (!strcmp(argv[i], "-d")) {
            if (i == argc - 1) die("option -d expects a value");
            device = argv[++i];
        } else if (!strcmp(argv[i], "-t")) {
            if (i == argc - 1) die("option -t expects a value");
            distancethreshold = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-T")) {
            if (i == argc - 1) die("option -T expects a value");
            distancethreshold_pressed = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-r")) {
            if (i == argc - 1) die("option -r expects a value");
            degreesleniency = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-m")) {
            if (i == argc - 1) die("option -m expects a value");
            timeoutms = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-o")) {
            if (i == argc - 1) die("option -o expects a value");
            orientation = atoi(argv[++i]);
            current_orientation = orientation;
        } else if (!strcmp(argv[i], "-h")) {
            if (i == argc - 1) die("option -h expects a value");
            screenheight = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-w")) {
            if (i == argc - 1) die("option -w expects a value");
            screenwidth = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-s")) {
            if (i == argc - 1) die("option -s expects a value");
            edgessizescaling = atof(argv[++i]);
        } else if (!strcmp(argv[i], "-g")) {
            if (i == argc - 1) die("option -g expects a value");
            gestsarrlen++;
            gestsarr = realloc(gestsarr, (gestsarrlen * sizeof(Gesture)));
            if (gestsarr == NULL) {
                perror("Could not allocate memory");
                exit(EXIT_FAILURE);
            }
            gestpt = strtok(argv[++i], ",");
            for (j = 0; gestpt != NULL && j < 6; j++, gestpt = strtok(NULL, ",")) {
                switch(j) {
                    case 0: gestsarr[gestsarrlen - 1].nfswipe = atoi(gestpt); break;
                    case 1:
                        if (!strcmp(gestpt, "LR")) gestsarr[gestsarrlen-1].swipe = SwipeLR;
                        else if (!strcmp(gestpt, "RL")) gestsarr[gestsarrlen-1].swipe = SwipeRL;
                        else if (!strcmp(gestpt, "DU")) gestsarr[gestsarrlen-1].swipe = SwipeDU;
                        else if (!strcmp(gestpt, "UD")) gestsarr[gestsarrlen-1].swipe = SwipeUD;
                        else if (!strcmp(gestpt, "DLUR")) gestsarr[gestsarrlen-1].swipe = SwipeDLUR;
                        else if (!strcmp(gestpt, "URDL")) gestsarr[gestsarrlen-1].swipe = SwipeURDL;
                        else if (!strcmp(gestpt, "ULDR")) gestsarr[gestsarrlen-1].swipe = SwipeULDR;
                        else if (!strcmp(gestpt, "DRUL")) gestsarr[gestsarrlen-1].swipe = SwipeDRUL;
                        break;
                    case 2:
                        if (!strcmp(gestpt, "L")) gestsarr[gestsarrlen - 1].edge = EdgeLeft;
                        else if (!strcmp(gestpt, "R")) gestsarr[gestsarrlen - 1].edge = EdgeRight;
                        else if (!strcmp(gestpt, "T")) gestsarr[gestsarrlen - 1].edge = EdgeTop;
                        else if (!strcmp(gestpt, "B")) gestsarr[gestsarrlen - 1].edge = EdgeBottom;
                        else if (!strcmp(gestpt, "TL")) gestsarr[gestsarrlen - 1].edge = CornerTopLeft;
                        else if (!strcmp(gestpt, "TR")) gestsarr[gestsarrlen - 1].edge = CornerTopRight;
                        else if (!strcmp(gestpt, "BL")) gestsarr[gestsarrlen - 1].edge = CornerBottomLeft;
                        else if (!strcmp(gestpt, "BR")) gestsarr[gestsarrlen - 1].edge = CornerBottomRight;
                        else if (!strcmp(gestpt, "N")) gestsarr[gestsarrlen - 1].edge = EdgeNone;
                        else if (!strcmp(gestpt, "*")) gestsarr[gestsarrlen - 1].edge = EdgeAny;
                        break;
                    case 3:
                        if (!strcmp(gestpt, "L")) gestsarr[gestsarrlen - 1].distance = DistanceLong;
                        else if (!strcmp(gestpt, "M")) gestsarr[gestsarrlen - 1].distance = DistanceMedium;
                        else if (!strcmp(gestpt, "S")) gestsarr[gestsarrlen - 1].distance = DistanceShort;
                        else if (!strcmp(gestpt, "*")) gestsarr[gestsarrlen - 1].distance = DistanceAny;
                        break;
                    case 4:
                        if (!strcmp(gestpt, "P")) {
                            gestsarr[gestsarrlen - 1].actmode = ActModePressed;
                        } else {
                            gestsarr[gestsarrlen - 1].actmode = ActModeReleased;
                            if (strcmp(gestpt, "R") != 0) {
                                gestsarr[gestsarrlen - 1].command = gestpt;
                            }
                        }
                        break;
                    case 5: gestsarr[gestsarrlen - 1].command = gestpt;
                }
            }
        } else {
            fprintf(stderr, "lisgd usage: [-v] [-d /dev/input/touchscreen] [-o 0] [-t 200] [-r 20] [-m 400] [-g '1,LR,L,*,R,notify-send swiped left to right']\n");
            exit(1);
        }
    }

    if (!device) device = "/dev/input/touchscreen";

    if (screenwidth == 0 && screenheight == 0) {
        if (getenv("WAYLAND_DISPLAY")) {
#ifdef WITH_WAYLAND
            wl_display = wl_display_connect(NULL);
            if (!wl_display) {
                if (verbose) fprintf(stderr, "Failed to connect to Wayland display\n");
                screenwidth = 1920;
                screenheight = 1080;
            } else {
                wl_registry = wl_display_get_registry(wl_display);
                wl_registry_add_listener(wl_registry, &wl_registry_listener, NULL);
                for (i = 0; i < 2 && (screenwidth == 0 || screenheight == 0); i++) {
                    if (wl_display_roundtrip(wl_display) == -1) {
                        if (verbose) fprintf(stderr, "Wayland roundtrip failed: %s\n", strerror(errno));
                    }
                }
            }
#else
            die("Wayland environment detected but support not enabled");
#endif
        } else {
            if (verbose) fprintf(stderr, "No display environment, using fallback dimensions\n");
            screenwidth = 1920;
            screenheight = 1080;
        }
    }
    if (screenwidth == 0 || screenheight == 0) {
        if (verbose) fprintf(stderr, "Screen dimensions not set, using defaults: width=1920, height=1080\n");
        screenwidth = 1920;
        screenheight = 1080;
    }

    if (gestsarrlen == 0) {
        gestsarr = malloc(sizeof(gestures));
        if (gestsarr == NULL) {
            perror("Could not allocate memory");
            exit(EXIT_FAILURE);
        }
        gestsarrlen = sizeof(gestures) / sizeof(Gesture);
        memcpy(gestsarr, gestures, sizeof(gestures));
    }

    for (i = 0; i < gestsarrlen; i++) {
        gestsarr[i].swipe = swipereorient(gestsarr[i].swipe, orientation);
        gestsarr[i].edge = edgereorient(gestsarr[i].edge, orientation);
        if (gestsarr[i].actmode == ActModePressed) have_actmode_pressed++;
    }

    if (verbose) {
        fprintf(stderr, "Initial config: screenwidth=%d, screenheight=%d, orientation=%d, device=%s\n",
            screenwidth, screenheight, orientation, device);
        for (i = 0; i < gestsarrlen; i++) {
            fprintf(stderr, "Gesture %d: nf=%d, sw=%d, edge=%d, dist=%d, actmode=%d, cmd=%s\n",
                    i, gestsarr[i].nfswipe, gestsarr[i].swipe, gestsarr[i].edge, gestsarr[i].distance,
                    gestsarr[i].actmode, gestsarr[i].command);
        }
    }

    run();

    // Cleanup
    if (gestsarr) free(gestsarr);
    return 0;
}
