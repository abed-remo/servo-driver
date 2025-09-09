#ifndef SERVO_UAPI_H
#define SERVO_UAPI_H

#include <linux/ioctl.h>

#define SERVO_IOC_MAGIC   's'

/* Einheiten:
 * - Winkel: 0..180 Grad (int)
 * - Geschwindigkeit: Grad/Sek (int, 0 = sofort/sprung)
 * - Pulse in Nanosekunden
 */
struct servo_limits {
    int min_angle;          /* z.B. 0   */
    int max_angle;          /* z.B. 180 */
    unsigned int min_pulse_ns; /* z.B. 1000000 (1.0 ms) */
    unsigned int max_pulse_ns; /* z.B. 2000000 (2.0 ms) */
};

#define SERVO_IOCTL_SET_ANGLE     _IOW(SERVO_IOC_MAGIC, 0x01, int)
#define SERVO_IOCTL_GET_ANGLE     _IOR(SERVO_IOC_MAGIC, 0x02, int)
#define SERVO_IOCTL_SET_SPEED     _IOW(SERVO_IOC_MAGIC, 0x03, int)
#define SERVO_IOCTL_GET_SPEED     _IOR(SERVO_IOC_MAGIC, 0x04, int)
#define SERVO_IOCTL_SET_LIMITS    _IOW(SERVO_IOC_MAGIC, 0x05, struct servo_limits)
#define SERVO_IOCTL_GET_LIMITS    _IOR(SERVO_IOC_MAGIC, 0x06, struct servo_limits)
#define SERVO_IOCTL_ENABLE        _IOW(SERVO_IOC_MAGIC, 0x07, int) /* 0/1 */

#endif /* SERVO_UAPI_H */
