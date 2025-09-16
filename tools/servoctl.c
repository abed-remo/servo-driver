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
        "  %s [--device DEV] [--speed N] <cmd>\n"
        "\n"
        "Commands:\n"
        "  to45       : move to 45°\n"
        "  to90       : move to 90°\n"
        "  to135      : move to 135°\n"
        "  to180      : move to 180°\n"
        "  step+      : +1° from current (max 180)\n"
        "  step-      : -1° from current (min 0)\n"
        "  <number>   : set exact angle 0..180 (e.g. 73)\n"
        "\n"
        "Options:\n"
        "  --device DEV  (default: /dev/servo0)\n"
        "  --speed N     degrees per second (default: 90, 0 = immediate)\n",
        prog
    );
}

static int clamp(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

int main(int argc, char **argv)
{
    const char *dev = "/dev/servo0";
    int speed = 90; /* deg/s, 0 = immediate */
    const char *cmd = NULL;

    /* parse options */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--device") && i + 1 < argc) {
            dev = argv[++i];
        } else if (!strcmp(argv[i], "--speed") && i + 1 < argc) {
            speed = atoi(argv[++i]);
            if (speed < 0) speed = 0;
        } else if (argv[i][0] == '-' && strcmp(argv[i], "-") != 0) {
            usage(argv[0]);
            return 2;
        } else {
            cmd = argv[i];
            /* alles danach als ein Token betrachten */
            break;
        }
    }

    if (!cmd) {
        usage(argv[0]);
        return 2;
    }

    int fd = open(dev, O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }

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
        /* nicht fatal */
    }

    int angle = 0;
    int ret = ioctl(fd, SERVO_IOCTL_GET_ANGLE, &angle);
    if (ret < 0) {
        /* wenn Treiber noch keinen Winkel hat, starten wir bei 0 */
        angle = 0;
    }

    int target = -1;

    if (!strcmp(cmd, "to45"))       target = 45;
    else if (!strcmp(cmd, "to90"))  target = 90;
    else if (!strcmp(cmd, "to135")) target = 135;
    else if (!strcmp(cmd, "to180")) target = 180;
    else if (!strcmp(cmd, "step+")) target = clamp(angle + 1, 0, 180);
    else if (!strcmp(cmd, "step-")) target = clamp(angle - 1, 0, 180);
    else {
        /* try numeric */
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

    /* status anzeigen */
    int cur = target;
    if (ioctl(fd, SERVO_IOCTL_GET_ANGLE, &cur) == 0) {
        printf("Angle set to: %d°\n", cur);
    } else {
        printf("Angle set to: %d° (GET_ANGLE not available)\n", target);
    }

    /* optional: nicht automatisch deaktivieren, damit PWM aktiv bleibt
       Wenn du am Ende ausschalten willst, en=0 setzen: */
    // en = 0; ioctl(fd, SERVO_IOCTL_ENABLE, &en);

    close(fd);
    return 0;
}
