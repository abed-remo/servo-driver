// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/pwm.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>

#include "servo_uapi.h"

#define SERVO_DEVICE_NAME  "servo0"
#define SERVO_CLASS_NAME   "servo_class"

#define SERVO_DEFAULT_PERIOD_NS  20000000U   /* 20 ms -> 50 Hz */
#define SERVO_DEFAULT_MIN_NS      1000000U   /* 1.0 ms */
#define SERVO_DEFAULT_MAX_NS      2000000U   /* 2.0 ms */

struct servo_dev {
    struct device       *dev;
    struct pwm_device   *pwm;
    unsigned int         period_ns;

    struct mutex         lock;

    /* Char device */
    dev_t                devt;
    struct cdev          cdev;
    struct class        *cls;
    struct device       *cdev_dev;

    /* State */
    int                  enabled;        /* 0/1 */
    int                  cur_angle;      /* 0..180 (gerundet) */
    int                  target_angle;   /* 0..180 */
    int                  speed_dps;      /* degrees per second; 0 = jump */

    struct servo_limits  limits;

    /* Motion */
    struct delayed_work  motion_work;
    unsigned int         tick_ms;        /* control loop period (e.g. 20 ms) */
};

static inline unsigned int map_angle_to_pulse_ns(struct servo_dev *sd, int angle)
{
    unsigned int min_ns = sd->limits.min_pulse_ns;
    unsigned int max_ns = sd->limits.max_pulse_ns;
    unsigned int span   = max_ns - min_ns;

    if (angle < sd->limits.min_angle) angle = sd->limits.min_angle;
    if (angle > sd->limits.max_angle) angle = sd->limits.max_angle;

    return min_ns + (span * (unsigned int)(angle - sd->limits.min_angle)) /
                    (unsigned int)(sd->limits.max_angle - sd->limits.min_angle);
}

static int servo_apply_angle(struct servo_dev *sd, int angle)
{
    int ret;
    unsigned int duty_ns;

    if (!sd->enabled)
        return 0;

    duty_ns = map_angle_to_pulse_ns(sd, angle);

    /* Apply PWM state */
    ret = pwm_config(sd->pwm, duty_ns, sd->period_ns);
    if (ret)
        return ret;

    sd->cur_angle = angle;
    return 0;
}

/* Motion control loop: moves cur_angle -> target_angle with speed */
static void servo_motion_tick(struct work_struct *work)
{
    struct servo_dev *sd = container_of(to_delayed_work(work), struct servo_dev, motion_work);
    int step_deg, delta, next_angle;

    mutex_lock(&sd->lock);

    if (!sd->enabled || sd->speed_dps == 0 || sd->cur_angle == sd->target_angle)
        goto out_resched_if_needed;

    /* degrees per tick */
    step_deg = (sd->speed_dps * sd->tick_ms + 500) / 1000; /* round */

    if (step_deg <= 0)
        step_deg = 1;

    delta = sd->target_angle - sd->cur_angle;
    if (delta > 0) {
        next_angle = sd->cur_angle + min(step_deg, delta);
    } else {
        next_angle = sd->cur_angle - min(step_deg, -delta);
    }

    servo_apply_angle(sd, next_angle);

out_resched_if_needed:
    /* Keep ticking while enabled and not at target or speed>0 */
    if (sd->enabled && (sd->speed_dps > 0) && (sd->cur_angle != sd->target_angle))
        schedule_delayed_work(&sd->motion_work, msecs_to_jiffies(sd->tick_ms));

    mutex_unlock(&sd->lock);
}

/* ---------- Char device ---------- */

static long servo_unlocked_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct servo_dev *sd = filp->private_data;
    int val, ret = 0;

    switch (cmd) {
    case SERVO_IOCTL_ENABLE:
        if (copy_from_user(&val, (void __user *)arg, sizeof(int)))
            return -EFAULT;
        mutex_lock(&sd->lock);
        if (val && !sd->enabled) {
            ret = pwm_enable(sd->pwm);
            if (!ret) {
                sd->enabled = 1;
                /* apply current angle immediately */
                servo_apply_angle(sd, sd->cur_angle);
                /* kick motion loop if speed>0 */
                if (sd->speed_dps > 0)
                    schedule_delayed_work(&sd->motion_work, 0);
            }
        } else if (!val && sd->enabled) {
            cancel_delayed_work_sync(&sd->motion_work);
            pwm_disable(sd->pwm);
            sd->enabled = 0;
        }
        mutex_unlock(&sd->lock);
        break;

    case SERVO_IOCTL_SET_ANGLE:
        if (copy_from_user(&val, (void __user *)arg, sizeof(int)))
            return -EFAULT;
        mutex_lock(&sd->lock);
        if (val < sd->limits.min_angle) val = sd->limits.min_angle;
        if (val > sd->limits.max_angle) val = sd->limits.max_angle;
        sd->target_angle = val;

        if (!sd->enabled) {
            mutex_unlock(&sd->lock);
            return 0;
        }

        if (sd->speed_dps == 0) {
            ret = servo_apply_angle(sd, sd->target_angle);
        } else {
            /* start motion loop */
            schedule_delayed_work(&sd->motion_work, 0);
        }
        mutex_unlock(&sd->lock);
        break;

    case SERVO_IOCTL_GET_ANGLE:
        mutex_lock(&sd->lock);
        val = sd->cur_angle;
        mutex_unlock(&sd->lock);
        if (copy_to_user((void __user *)arg, &val, sizeof(int)))
            return -EFAULT;
        break;

    case SERVO_IOCTL_SET_SPEED:
        if (copy_from_user(&val, (void __user *)arg, sizeof(int)))
            return -EFAULT;
        mutex_lock(&sd->lock);
        sd->speed_dps = (val < 0) ? 0 : val;
        if (sd->enabled && sd->speed_dps > 0 && sd->cur_angle != sd->target_angle)
            schedule_delayed_work(&sd->motion_work, 0);
        mutex_unlock(&sd->lock);
        break;

    case SERVO_IOCTL_GET_SPEED:
        mutex_lock(&sd->lock);
        val = sd->speed_dps;
        mutex_unlock(&sd->lock);
        if (copy_to_user((void __user *)arg, &val, sizeof(int)))
            return -EFAULT;
        break;

    case SERVO_IOCTL_SET_LIMITS: {
        struct servo_limits lims;
        if (copy_from_user(&lims, (void __user *)arg, sizeof(lims)))
            return -EFAULT;
        if (lims.max_angle <= lims.min_angle)
            return -EINVAL;
        if (lims.max_pulse_ns <= lims.min_pulse_ns)
            return -EINVAL;
        mutex_lock(&sd->lock);
        sd->limits = lims;
        /* re-apply current */
        if (sd->enabled)
            ret = servo_apply_angle(sd, sd->cur_angle);
        mutex_unlock(&sd->lock);
        break;
    }

    case SERVO_IOCTL_GET_LIMITS: {
        struct servo_limits lims;
        mutex_lock(&sd->lock);
        lims = sd->limits;
        mutex_unlock(&sd->lock);
        if (copy_to_user((void __user *)arg, &lims, sizeof(lims)))
            return -EFAULT;
        break;
    }

    default:
        ret = -ENOTTY;
    }

    return ret;
}

static int servo_open(struct inode *inode, struct file *filp)
{
    struct servo_dev *sd = container_of(inode->i_cdev, struct servo_dev, cdev);
    filp->private_data = sd;
    return 0;
}

static int servo_release(struct inode *inode, struct file *filp)
{
    return 0;
}

static const struct file_operations servo_fops = {
    .owner          = THIS_MODULE,
    .open           = servo_open,
    .release        = servo_release,
    .unlocked_ioctl = servo_unlocked_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl   = servo_unlocked_ioctl,
#endif
};

/* ---------- Platform driver ---------- */

static int servo_probe(struct platform_device *pdev)
{
    struct servo_dev *sd;
    int ret;

    sd = devm_kzalloc(&pdev->dev, sizeof(*sd), GFP_KERNEL);
    if (!sd)
        return -ENOMEM;

    sd->dev = &pdev->dev;
    mutex_init(&sd->lock);

    /* PWM handle aus DT: pwms = <&pwm 0 20000000>; period 20ms */
    sd->pwm = devm_pwm_get(&pdev->dev, "servo");
    if (IS_ERR(sd->pwm)) {
        dev_err(&pdev->dev, "failed to get PWM\n");
        return PTR_ERR(sd->pwm);
    }

    /* Default-Parameter */
    sd->period_ns = SERVO_DEFAULT_PERIOD_NS;
    sd->limits.min_angle    = 0;
    sd->limits.max_angle    = 180;
    sd->limits.min_pulse_ns = SERVO_DEFAULT_MIN_NS;
    sd->limits.max_pulse_ns = SERVO_DEFAULT_MAX_NS;
    sd->cur_angle = 90;
    sd->target_angle = 90;
    sd->speed_dps = 0;
    sd->enabled = 0;
    sd->tick_ms = 20; /* 50Hz update */

    INIT_DELAYED_WORK(&sd->motion_work, servo_motion_tick);

    /* Vorkonfigurieren */
    ret = pwm_config(sd->pwm,
                     map_angle_to_pulse_ns(sd, sd->cur_angle),
                     sd->period_ns);
    if (ret)
        return ret;

    /* Char device anlegen */
    ret = alloc_chrdev_region(&sd->devt, 0, 1, SERVO_DEVICE_NAME);
    if (ret)
        return ret;

    cdev_init(&sd->cdev, &servo_fops);
    ret = cdev_add(&sd->cdev, sd->devt, 1);
    if (ret)
        goto err_unregister;

    sd->cls = class_create(SERVO_CLASS_NAME);
    if (IS_ERR(sd->cls)) {
        ret = PTR_ERR(sd->cls);
        goto err_cdev;
    }

    sd->cdev_dev = device_create(sd->cls, NULL, sd->devt, NULL, SERVO_DEVICE_NAME);
    if (IS_ERR(sd->cdev_dev)) {
        ret = PTR_ERR(sd->cdev_dev);
        goto err_class;
    }

    platform_set_drvdata(pdev, sd);
    dev_info(&pdev->dev, "servo driver ready (/dev/%s)\n", SERVO_DEVICE_NAME);
    return 0;

err_class:
    class_destroy(sd->cls);
err_cdev:
    cdev_del(&sd->cdev);
err_unregister:
    unregister_chrdev_region(sd->devt, 1);
    return ret;
}

static int servo_remove(struct platform_device *pdev)
{
    struct servo_dev *sd = platform_get_drvdata(pdev);

    cancel_delayed_work_sync(&sd->motion_work);
    if (sd->enabled)
        pwm_disable(sd->pwm);

    device_destroy(sd->cls, sd->devt);
    class_destroy(sd->cls);
    cdev_del(&sd->cdev);
    unregister_chrdev_region(sd->devt, 1);

    return 0;
}

static const struct of_device_id servo_of_match[] = {
    { .compatible = "remo,servo" },
    { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, servo_of_match);

static struct platform_driver servo_driver = {
    .probe  = servo_probe,
    .remove = servo_remove,
    .driver = {
        .name           = "remo_servo",
        .of_match_table = servo_of_match,
    },
};
module_platform_driver(servo_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Remo");
MODULE_DESCRIPTION("PWM Servo Driver with IOCTL control");
