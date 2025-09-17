#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "servo_uapi.h"

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s [--device DEV] [--speed N] [--step N] <cmd>\n"
        "\n"
        "Commands:\n"
        "  to45       : move to 45°\n"
        "  to90       : move to 90°\n"
        "  to135      : move to 135°\n"
        "  to180      : move to 180°\n"
        "  step+      : +step degrees (default 10°, max 180)\n"
        "  step-      : -step degrees (default 10°, min 0)\n"
        "  set-limits <min_us> <max_us> : set pulse limits in microseconds (e.g. 500 2500)\n"
        "  get-limits : read current limits\n"
        "  <number>   : set exact angle 0..180 (e.g. 73)\n"
        "\n"
        "Options:\n"
        "  --device DEV  (default: /dev/servo0)\n"
        "  --speed N     degrees per second (default: 90, 0 = immediate)\n"
        "  --step N      step size for step+/step- (default: 10)\n",
        prog
    );
}

static int clamp(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int open_dev(const char *dev) {
    int fd = open(dev, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "open(%s) failed: %s\n", dev, strerror(errno));
        return -1;
    }
    return fd;
}

static void print_limits(const struct servo_limits *L) {
    printf("Limits: angle [%d..%d], pulse [%u..%u] ns (%.3f..%.3f ms)\n",
           L->min_angle, L->max_angle,
           L->min_pulse_ns, L->max_pulse_ns,
           L->min_pulse_ns/1000000.0, L->max_pulse_ns/1000000.0);
}

int main(int argc, char **argv)
{
    const char *dev = "/dev/servo0";
    int speed = 90;            /* deg/s; 0 = immediate */
    int step = 10;             /* step size in degrees */
    const char *cmd = NULL;

    /* parse options */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--device") && i + 1 < argc) {
            dev = argv[++i];
        } else if (!strcmp(argv[i], "--speed") && i + 1 < argc) {
            speed = atoi(argv[++i]);
            if (speed < 0) speed = 0;
        } else if (!strcmp(argv[i], "--step") && i + 1 < argc) {
            step = atoi(argv[++i]);
            if (step < 1) step = 1;
        } else if (argv[i][0] == '-' && strcmp(argv[i], "-") != 0) {
            usage(argv[0]);
            return 2;
        } else {
            cmd = argv[i];
            /* restliche Tokens bleiben im argv für Kommandos mit Parametern */
            argc -= i; argv += i;
            break;
        }
    }

    if (!cmd) {
        usage(argv[0]);
        return 2;
    }

    int fd = open_dev(dev);
    if (fd < 0) return 1;

    /* enable device */
    int en = 1;
    if (ioctl(fd, SERVO_IOCTL_ENABLE, &en) < 0) {
        perror("ENABLE");
        close(fd);
        return 1;
    }

    /* optional speed */
    if (ioctl(fd, SERVO_IOCTL_SET_SPEED, &speed) < 0) {
        perror("SET_SPEED");
        /* not fatal */
    }

    /* handle set-limits / get-limits first */
    if (!strcmp(cmd, "get-limits")) {
        struct servo_limits L;
        if (ioctl(fd, SERVO_IOCTL_GET_LIMITS, &L) < 0) {
            perror("GET_LIMITS");
            close(fd);
            return 1;
        }
        print_limits(&L);
        close(fd);
        return 0;
    }

    if (!strcmp(cmd, "set-limits")) {
        if (argc < 3) {
            fprintf(stderr, "set-limits requires <min_us> <max_us>\n");
            usage(argv[0]);
            close(fd);
            return 2;
        }
        long min_us = strtol(argv[1], NULL, 10);
        long max_us = strtol(argv[2], NULL, 10);
        if (min_us <= 0 || max_us <= 0 || min_us >= max_us) {
            fprintf(stderr, "invalid limits: %ld..%ld us\n", min_us, max_us);
            close(fd);
            return 2;
        }

        struct servo_limits L = {
            .min_angle = 0,
            .max_angle = 180,
            .min_pulse_ns = (unsigned int)(min_us * 1000UL),
            .max_pulse_ns = (unsigned int)(max_us * 1000UL),
        };

        if (ioctl(fd, SERVO_IOCTL_SET_LIMITS, &L) < 0) {
            perror("SET_LIMITS");
            close(fd);
            return 1;
        }
        printf("SET_LIMITS ok: ");
        print_limits(&L);
        close(fd);
        return 0;
    }

    /* read current angle (for step operations) */
    int angle = 0;
    if (ioctl(fd, SERVO_IOCTL_GET_ANGLE, &angle) < 0) {
        /* if driver doesn't return a valid angle, assume 0 */
        angle = 0;
    }

    int target = -1;
    if (!strcmp(cmd, "to45"))       target = 45;
    else if (!strcmp(cmd, "to90"))  target = 90;
    else if (!strcmp(cmd, "to135")) target = 135;
    else if (!strcmp(cmd, "to180")) target = 180;
    else if (!strcmp(cmd, "step+")) target = clamp(angle + step, 0, 180);
    else if (!strcmp(cmd, "step-")) target = clamp(angle - step, 0, 180);
    else {
        /* numeric angle */
        char *endp = NULL;
        long v = strtol(cmd, &endp, 10);
        if (endp && *endp == '\0') {
            target = clamp((int)v, 0, 180);
        } else {
            fprintf(stderr, "Unknown command: %s\n\n", cmd);
            usage(argv[0]);
            close(fd);
            return 2;
        }
    }

    if (ioctl(fd, SERVO_IOCTL_SET_ANGLE, &target) < 0) {
        perror("SET_ANGLE");
        close(fd);
        return 1;
    }

    /* status */
    int cur = target;
    if (ioctl(fd, SERVO_IOCTL_GET_ANGLE, &cur) == 0) {
        printf("Angle set to: %d°\n", cur);
    } else {
        printf("Angle set to: %d° (GET_ANGLE not available)\n", target);
    }

    /* keep enabled; if you want to disable at end:
       en = 0; ioctl(fd, SERVO_IOCTL_ENABLE, &en); */

    close(fd);
    return 0;
}
