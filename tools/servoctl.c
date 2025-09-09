#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "include/servo_uapi.h"

int main(int argc, char **argv)
{
    int fd = open("/dev/servo0", O_RDWR);
    if (fd < 0) { perror("open"); return 1; }

    int en = 1;
    if (ioctl(fd, SERVO_IOCTL_ENABLE, &en) < 0) perror("ENABLE");

    int speed = 90; /* 90 deg/s */
    if (ioctl(fd, SERVO_IOCTL_SET_SPEED, &speed) < 0) perror("SET_SPEED");

    int angle = 45;
    if (ioctl(fd, SERVO_IOCTL_SET_ANGLE, &angle) < 0) perror("SET_ANGLE");

    sleep(2);

    angle = 135;
    if (ioctl(fd, SERVO_IOCTL_SET_ANGLE, &angle) < 0) perror("SET_ANGLE");

    if (ioctl(fd, SERVO_IOCTL_GET_ANGLE, &angle) == 0)
        printf("Current angle: %d\n", angle);

    en = 0;
    ioctl(fd, SERVO_IOCTL_ENABLE, &en);

    close(fd);
    return 0;
}
