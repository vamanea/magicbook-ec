// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026 Valentin Manea
 */
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/thermal.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/input.h>
#include <linux/input/sparse-keymap.h>
#include <linux/leds.h>

#define EC_SCI_EVT_READ_CMD	0x05
#define EC_FW_VERSION_CMD	0x0e
#define EC_MODERN_STANDBY_CMD	0x23
#define EC_SCREEN_NOTIFY_CMD	0x0b
#define EC_FAN_DBG_CONTROL_CMD	0x30
#define EC_SCI_EVT_CONTROL_CMD	0x35
#define EC_THERMAL_CAP_CMD	0x42
#define EC_KEYBD_BACKLIGHT_CMD  0x53

#define EC_FAN_NAME_SIZE	20

#define EC_IRQ_REASON_REG 0x05
#define EC_SUSPEND_RESUME_REG 0x23
#define EC_IRQ_ENABLE_REG 0x35

#define EC_NOTIFY_SUSPEND_ENTER 0x01
#define EC_NOTIFY_SUSPEND_EXIT 0x00

#define EC_NOTIFY_SCREEN_REG 0x01
#define EC_NOTIFY_SCREEN_OFF 0x01
#define EC_NOTIFY_SCREEN_ON 0x00

enum qcom_ec_sci_events {
	EC_EVT_KEY_FN_F8 = 0x08,
	EC_EVT_KEY_FN_F7 = 0x07,
	EC_EVT_KEY_FN_F10 = 0x0a,
	EC_EVT_KEY_FN_F11 = 0x0b,
	EC_EVT_KEY_FN_F12 = 0x0c,
	EC_EVT_KEY_FN_F3_ON = 0x0e,
	EC_EVT_KEY_FN_F3_OFF = 0x0f,
	EC_EVT_KEY_FN_R = 0x10,
	EC_EVT_KEY_FN_P = 0x11,
	EC_EVT_KEY_FN_SPACE0 = 0x14,
	EC_EVT_KEY_FN_SPACE1 = 0x15,
	EC_EVT_KEY_FN_SPACE2 = 0x16,
	EC_EVT_KEY_FN_SPACE3 = 0x17,
	EC_FAN1_STATUS_CHANGE_EVT = 0x30,
	EC_FAN2_STATUS_CHANGE_EVT,
	EC_FAN1_SPEED_CHANGE_EVT,
	EC_FAN2_SPEED_CHANGE_EVT,
	EC_NEW_LUT_SET_EVT,
	EC_FAN_PROFILE_SWITCH_EVT,
	EC_THERMISTOR_1_THRESHOLD_CROSS_EVT,
	EC_THERMISTOR_2_THRESHOLD_CROSS_EVT,
	EC_THERMISTOR_3_THRESHOLD_CROSS_EVT,
	/* Reserved: 0x39 - 0x3c/0x3f */
	EC_RECOVERED_FROM_RESET_EVT = 0x3d,
	/* 0x40 -> 0x46 thermal table updates */
	EC_THERMAL_TABLE_NORMAL = 0x40,
	EC_THERMAL_TABLE_FAN_FAILURE2 = 0x41,
	EC_THERMAL_TABLE_FAN_FAILURE1 = 0x42,
	EC_THERMAL_TABLE_HOT_PROTECT = 0x43,
	EC_THERMAL_TABLE_NORMAL30C = 0x44,
	EC_THERMAL_TABLE_SAFETY = 0x45,
	EC_THERMAL_TABLE_MFG_MODE = 0x46,
	/* USB Camera in or out of it's box */
	EC_EVT_CAMERA_IN = 0x5a,
	EC_EVT_CAMERA_OUT = 0x5b,
	/* Forward or backward don't seem to be too reliable */
	EC_EVT_CAMERA_FORWARD = 0x5c,
	EC_EVT_CAMERA_BACKWARD = 0x5d,
};

static char *sensor_labels[] = {
	"Battery",
	"CPU Fan",
	"GPU Fan",
	// The thermal zones are best effort guesses from the DSDT
	"CPU+GPU Temp",
	"GPU temp",
	"GPU temp",
	"Battery Temp",
	"CPU+GPU Temp",
	"GPU temp",
	"GPU temp",
};

/*
 * Add a virtual offset on all key event codes for sparse keymap handling,
 * since the sparse keymap infrastructure does not map some raw key event
 * codes used by the EC. For example 0x16 (EC_EVT_KEY_FN_F7) is mapped
 * to KEY_MUTE if no offset is applied.
 */
#define EC_KEY_EVT_OFFSET			0x1000
#define EC_KEY_ENTRY(key, value) \
	{ KE_KEY, EC_KEY_EVT_OFFSET + EC_EVT_KEY_##key, { value } }

static const struct key_entry t14s_keymap[] = {
	EC_KEY_ENTRY(FN_F7, KEY_MICMUTE),
	EC_KEY_ENTRY(FN_F8, KEY_CAMERA_ACCESS_TOGGLE),
	EC_KEY_ENTRY(FN_F10, KEY_VENDOR),
	EC_KEY_ENTRY(FN_F11, KEY_INFO),
	EC_KEY_ENTRY(FN_F12, KEY_PRINT),
	EC_KEY_ENTRY(FN_F3_ON, KEY_TOUCHPAD_ON),
	EC_KEY_ENTRY(FN_F3_OFF, KEY_TOUCHPAD_OFF),
	EC_KEY_ENTRY(FN_R, KEY_REFRESH_RATE_TOGGLE),
	EC_KEY_ENTRY(FN_P, KEY_PERFORMANCE),
	{ KE_END }
};

struct qcom_ec_cooling_dev {
	//struct thermal_cooling_device *cdev;
	struct device *cdev;
	struct device *parent_dev;

	char name[EC_FAN_NAME_SIZE];
	u8 fan_id;
	u32 state;
};

struct magicbook_ec {
	struct device *dev;
	struct i2c_client *client;
	struct input_dev *idev;
	struct led_classdev kbd_backlight;
	enum led_brightness kbd_brightness;
	struct qcom_ec_cooling_dev *ec_cdev;
	struct mutex lock;

	struct device *cdev;
	char name[EC_FAN_NAME_SIZE];
};


static int qcom_ec_read(struct magicbook_ec *ec, u8 reg, u8 resp_len, u8 resp[8])
{
	int ret;
	u8 cmd_buf[3] = {0xA1, reg, resp_len};
	struct i2c_msg msgs[] = {
		{
			.addr = ec->client->addr,
			.flags = 0,
			.len = sizeof(cmd_buf),
			.buf = cmd_buf,
		},
		{
			.addr = ec->client->addr,
			.flags = I2C_M_RD,
			.len = 8, /* This always needs to be 8 long */
			.buf = resp,
		},
	};

	ret = i2c_transfer(ec->client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret < 2) {
		dev_err(&ec->client->dev, "Failed to read register: %d, err = %d\n", reg, ret);
		return ret;
	}

	if (ret < 0)
		return ret;

	return 0;
}

static int qcom_ec_write(struct magicbook_ec *ec, u8 reg, u8 func, u8 value)
{
	int ret;
	u8 cmd_buf[4] = {0xA0, reg, func, value};
	struct i2c_msg msgs[] = {
		{
			.addr = ec->client->addr,
			.flags = 0,
			.len = sizeof(cmd_buf),
			.buf = cmd_buf,
		},
	};

	ret = i2c_transfer(ec->client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret != 1 || ret < 0) {
		dev_err(&ec->client->dev, "Failed to write register: %x, err = %d\n", reg, ret);
		return -ENXIO;
	}

	return 0;
}

static int ec_fan_get_cur_state(struct magicbook_ec *ec, u32 fan_reg_id, unsigned long *state, bool target)
{
	u8 resp[8];
	int ret;

	ret = qcom_ec_read(ec, fan_reg_id, 4, resp);
	if (ret < 0) {
		dev_err(ec->dev, "Failed to read response from register %x", fan_reg_id);
		return ret;
	}

	if (target) {
		*state = resp[1]<< 8 | resp[0];
	} else {
		*state = resp[3]<< 8 | resp[2];
	}
	dev_dbg(ec->dev, "Resp %x %x %x %x", resp[0], resp[1], resp[2], resp[3]);

	return 0;
}

static enum led_brightness ec_kbd_bl_get(struct led_classdev *led_cdev)
{
	struct magicbook_ec *ec = container_of(led_cdev, struct magicbook_ec,
					  kbd_backlight);
	u8 resp[8];
	u8 brightness;
	int ret;
	ret = qcom_ec_read(ec, EC_KEYBD_BACKLIGHT_CMD, 1, resp);
	if (ret < 0) {
		dev_err(ec->dev, "Failed to read response from keyboard backligth register");
		return ret;
	}
	brightness = resp[0];
	dev_dbg(ec->dev, "Reported brightnes: %u", brightness);
	if (brightness == 0xF) {
		return brightness;
	} else if (brightness <= 2) {
		return 0;
	} else if (brightness > 4) {
		return 2;
	} else {
		return brightness - 2;
	}
}

static int ec_kbd_bl_set(struct led_classdev *led_cdev,
			   enum led_brightness brightness)
{
	struct magicbook_ec *ec = container_of(led_cdev, struct magicbook_ec,
					  kbd_backlight);
	int ret;

	if (brightness >= 0 && brightness <= led_cdev->max_brightness) {
		brightness += 2;
	} else if (brightness != 0xF) {
		brightness = 2;
	}

	ret = qcom_ec_write(ec, EC_KEYBD_BACKLIGHT_CMD, 0x2, brightness);
	if (ret < 0) {
		dev_err(ec->dev, "Failed to write  keyboard backligth register");
		return ret;
	}
	return 0;
}

static void ec_kbd_bl_update(struct magicbook_ec *ec)
{
	ec->kbd_brightness = ec_kbd_bl_get(&ec->kbd_backlight);
	// The EC reports 0xF as auto backligth, in this case don't notify
	if (ec->kbd_brightness <= ec->kbd_backlight.max_brightness) {
		led_classdev_notify_brightness_hw_changed(&ec->kbd_backlight, ec->kbd_brightness);
	}
}

static irqreturn_t magicbook_ec_irq(int irq, void *data)
{
	struct magicbook_ec *ec = data;
	struct device *dev = &ec->client->dev;
	int val;

	guard(mutex)(&ec->lock);

	val = i2c_smbus_read_byte_data(ec->client, EC_IRQ_REASON_REG);
	if (val < 0) {
		dev_err(dev, "Failed to get EC IRQ reason: %d\n", val);
		return IRQ_HANDLED;
	}
	switch (val) {
	case EC_EVT_KEY_FN_SPACE0:
	case EC_EVT_KEY_FN_SPACE1:
	case EC_EVT_KEY_FN_SPACE2:
	case EC_EVT_KEY_FN_SPACE3:
		ec_kbd_bl_update(ec);
		break;
	case EC_EVT_KEY_FN_F7:
	case EC_EVT_KEY_FN_F8:
	case EC_EVT_KEY_FN_F10:
	case EC_EVT_KEY_FN_F11:
	case EC_EVT_KEY_FN_F12:
	case EC_EVT_KEY_FN_R:
	case EC_EVT_KEY_FN_F3_ON:
	case EC_EVT_KEY_FN_F3_OFF:
	case EC_EVT_CAMERA_IN:
	case EC_EVT_CAMERA_OUT:
	case EC_EVT_CAMERA_FORWARD:
	case EC_EVT_CAMERA_BACKWARD:
	case EC_EVT_KEY_FN_P:
		sparse_keymap_report_event(ec->idev,
				EC_KEY_EVT_OFFSET + val, 1, true);
		dev_dbg_ratelimited(dev, "Key EC event: 0x%x\n", val);
		break;
	case EC_FAN1_STATUS_CHANGE_EVT:
		dev_dbg_ratelimited(dev, "Fan1 status changed\n");
		break;
	case EC_FAN2_STATUS_CHANGE_EVT:
		dev_dbg_ratelimited(dev, "Fan2 status changed\n");
		break;
	case EC_FAN1_SPEED_CHANGE_EVT:
		dev_dbg_ratelimited(dev, "Fan1 speed crossed low/high trip point\n");
		break;
	case EC_FAN2_SPEED_CHANGE_EVT:
		dev_dbg_ratelimited(dev, "Fan2 speed crossed low/high trip point\n");
		break;
	case EC_NEW_LUT_SET_EVT:
		dev_dbg_ratelimited(dev, "New LUT set\n");
		break;
	case EC_FAN_PROFILE_SWITCH_EVT:
		dev_dbg_ratelimited(dev, "FAN Profile switched\n");
		break;
	case EC_THERMISTOR_1_THRESHOLD_CROSS_EVT:
		dev_dbg_ratelimited(dev, "Thermistor 1 threshold crossed\n");
		break;
	case EC_THERMISTOR_2_THRESHOLD_CROSS_EVT:
		dev_dbg_ratelimited(dev, "Thermistor 2 threshold crossed\n");
		break;
	case EC_THERMISTOR_3_THRESHOLD_CROSS_EVT:
		dev_dbg_ratelimited(dev, "Thermistor 3 threshold crossed\n");
		break;
	case EC_RECOVERED_FROM_RESET_EVT:
		dev_dbg_ratelimited(dev, "EC recovered from reset\n");
		break;
	case EC_THERMAL_TABLE_NORMAL:
	case EC_THERMAL_TABLE_FAN_FAILURE2:
	case EC_THERMAL_TABLE_FAN_FAILURE1:
	case EC_THERMAL_TABLE_HOT_PROTECT:
	case EC_THERMAL_TABLE_NORMAL30C:
	case EC_THERMAL_TABLE_SAFETY:
	case EC_THERMAL_TABLE_MFG_MODE:
		dev_dbg_ratelimited(dev, "Thermal table should be updated");
		break;
	default:
		dev_notice_ratelimited(dev, "Unknown EC event: 0x%x\n", val);
		break;
	}

	return IRQ_HANDLED;
}


static ssize_t fan1_input_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct magicbook_ec *ec = dev_get_drvdata(dev);
	struct sensor_device_attribute_2 *sattr = to_sensor_dev_attr_2(attr);

	long rpm = 0;
	int ret = ec_fan_get_cur_state(ec, sattr->nr, &rpm, sattr->index == 1);
	if (ret < 0) {
		return ret;
	}
	/* hwmon standard dictates RPM is printed in plain integer format */
	return sysfs_emit(buf, "%ld\n", rpm);
}

static ssize_t temp_input_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct magicbook_ec *ec = dev_get_drvdata(dev);
	struct sensor_device_attribute_2 *sattr = to_sensor_dev_attr_2(attr);
	u8 resp[8];
	int ret;

	ret = qcom_ec_read(ec, sattr->nr, 1, resp);
	if (ret < 0) {
		return ret;
	}
	return sysfs_emit(buf, "%d\n", resp[0] * 1000);
}

static ssize_t
label_show(struct device *dev, struct device_attribute *devattr, char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	if (attr->index >= ARRAY_SIZE(sensor_labels)) {
		return -EINVAL;
	}

	return sprintf(buf, "%s\n", sensor_labels[attr->index]);
}

static ssize_t bat_input_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct magicbook_ec *ec = dev_get_drvdata(dev);
	struct sensor_device_attribute_2 *sattr = to_sensor_dev_attr_2(attr);
	u8 resp[8];
	int ret;

	ret = qcom_ec_read(ec, sattr->nr, 4, resp);
	if (ret < 0) {
		return ret;
	}
	return sysfs_emit(buf, "%d\n", resp[1] << 8 | resp[0]);
}

/* Battery voltage, typical 15.36V for 4 cell LiPo */
static SENSOR_DEVICE_ATTR_2(in0_input, 0444, bat_input_show, NULL, 0x40, 0);
static SENSOR_DEVICE_ATTR_RO(in0_label, label, 0);
/* Define the sysfs attribute, first paramter is the register, second is high or low */
static SENSOR_DEVICE_ATTR_2(fan1_input, 0444, fan1_input_show, NULL, 0x30, 0);
static SENSOR_DEVICE_ATTR_2(fan1_target, 0444, fan1_input_show, NULL, 0x30, 1);
static SENSOR_DEVICE_ATTR_RO(fan1_label, label, 1);
static SENSOR_DEVICE_ATTR_2(fan2_input, 0444, fan1_input_show, NULL, 0x34, 0);
static SENSOR_DEVICE_ATTR_2(fan2_target, 0444, fan1_input_show, NULL, 0x34, 1);
static SENSOR_DEVICE_ATTR_RO(fan2_label, label, 2);
/* Thermal Sensors from the EC, seem to be the ones who drive the fans */
static SENSOR_DEVICE_ATTR_2(temp1_input, 0444, temp_input_show, NULL, 0x10, 0);
static SENSOR_DEVICE_ATTR_RO(temp1_label, label, 3);
static SENSOR_DEVICE_ATTR_2(temp2_input, 0444, temp_input_show, NULL, 0x12, 0);
static SENSOR_DEVICE_ATTR_RO(temp2_label, label, 4);
static SENSOR_DEVICE_ATTR_2(temp3_input, 0444, temp_input_show, NULL, 0x14, 0);
static SENSOR_DEVICE_ATTR_RO(temp3_label, label, 5);
static SENSOR_DEVICE_ATTR_2(temp4_input, 0444, temp_input_show, NULL, 0x16, 0);
static SENSOR_DEVICE_ATTR_RO(temp4_label, label, 6);
static SENSOR_DEVICE_ATTR_2(temp5_input, 0444, temp_input_show, NULL, 0x18, 0);
static SENSOR_DEVICE_ATTR_RO(temp5_label, label, 7);
static SENSOR_DEVICE_ATTR_2(temp6_input, 0444, temp_input_show, NULL, 0x1a, 0);
static SENSOR_DEVICE_ATTR_RO(temp6_label, label, 8);
static SENSOR_DEVICE_ATTR_2(temp7_input, 0444, temp_input_show, NULL, 0x1c, 0);
static SENSOR_DEVICE_ATTR_RO(temp7_label, label, 9);
/* Group the attributes */
static struct attribute *magicbook_ec_attrs[] = {
	&sensor_dev_attr_fan1_input.dev_attr.attr,
	&sensor_dev_attr_fan1_target.dev_attr.attr,
	&sensor_dev_attr_fan1_label.dev_attr.attr,
	&sensor_dev_attr_fan2_input.dev_attr.attr,
	&sensor_dev_attr_fan2_target.dev_attr.attr,
	&sensor_dev_attr_fan2_label.dev_attr.attr,

	&sensor_dev_attr_in0_input.dev_attr.attr,
	&sensor_dev_attr_in0_label.dev_attr.attr,

	&sensor_dev_attr_temp1_input.dev_attr.attr,
	&sensor_dev_attr_temp1_label.dev_attr.attr,
	&sensor_dev_attr_temp2_input.dev_attr.attr,
	&sensor_dev_attr_temp2_label.dev_attr.attr,
	&sensor_dev_attr_temp3_input.dev_attr.attr,
	&sensor_dev_attr_temp3_label.dev_attr.attr,
	&sensor_dev_attr_temp4_input.dev_attr.attr,
	&sensor_dev_attr_temp4_label.dev_attr.attr,
	&sensor_dev_attr_temp5_input.dev_attr.attr,
	&sensor_dev_attr_temp5_label.dev_attr.attr,
	&sensor_dev_attr_temp6_input.dev_attr.attr,
	&sensor_dev_attr_temp6_label.dev_attr.attr,
	&sensor_dev_attr_temp7_input.dev_attr.attr,
	&sensor_dev_attr_temp7_label.dev_attr.attr,
	NULL
};
ATTRIBUTE_GROUPS(magicbook_ec);

static ssize_t charge_control_thresholds_show(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct magicbook_ec *ec = i2c_get_clientdata(client);
	u8 resp[8];
	int ret, start = 0, end = 0;

	ret = qcom_ec_read(ec, 0x6e, 2, resp);
	if (ret < 0)
		return ret;

	start = resp[0];
	end = resp[1];
	return sysfs_emit(buf, "%d %d\n", start, end);
}

static ssize_t charge_control_thresholds_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t size)
{
	int start, end;

	if (sscanf(buf, "%d %d", &start, &end) != 2)
		return -EINVAL;

	// err = huawei_wmi_battery_set(start, end);
	// if (err)
	// 	return err;

	return size;
}

static DEVICE_ATTR_RW(charge_control_thresholds);

static int ec_battery_setup(struct device *dev)
{
	return device_create_file(dev, &dev_attr_charge_control_thresholds);
}

static void ec_battery_exit(struct device *dev)
{
	device_remove_file(dev, &dev_attr_charge_control_thresholds);
}

static int ec_kbd_backlight_probe(struct magicbook_ec *ec)
{
	ec->kbd_backlight.name = "platform::kbd_backlight";
	ec->kbd_backlight.flags = LED_BRIGHT_HW_CHANGED;
	ec->kbd_backlight.max_brightness = 2;
	ec->kbd_backlight.brightness_get = ec_kbd_bl_get;
	ec->kbd_backlight.brightness_set_blocking = ec_kbd_bl_set;
	ec->kbd_brightness = 0x2; // Auto

	return devm_led_classdev_register(ec->dev, &ec->kbd_backlight);
}

static int ec_input_probe(struct magicbook_ec *ec)
{
	int ret;

	ec->idev = devm_input_allocate_device(ec->dev);
	if (!ec->idev)
		return -ENOMEM;

	ec->idev->name = "Magicbook Extra Buttons";
	ec->idev->phys = "magicbook/input0";
	ec->idev->id.bustype = BUS_HOST;
	ec->idev->dev.parent = ec->dev;

	ret = sparse_keymap_setup(ec->idev, t14s_keymap, NULL);
	if (ret)
		return ret;

	return input_register_device(ec->idev);
}

static int magicbook_ec_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct magicbook_ec *ec;
	int ret;

	// Disable the IRQ first
	ret = i2c_smbus_write_byte_data(client, EC_IRQ_ENABLE_REG, 0x00);
	if (ret < 0)
		return ret;

	ec = devm_kzalloc(dev, sizeof(*ec), GFP_KERNEL);
	if (!ec)
		return -ENOMEM;

	ec->dev = dev;
	ec->client = client;
	mutex_init(&ec->lock);

	ret = ec_input_probe(ec);
	if (ret < 0)
		return ret;

	ret = ec_kbd_backlight_probe(ec);
	if (ret < 0)
		return ret;

	ret = devm_request_threaded_irq(dev, client->irq,
					NULL, magicbook_ec_irq,
					IRQF_ONESHOT, "magicbook_ec", ec);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Unable to request irq\n");
	i2c_set_clientdata(client, ec);

	scnprintf(ec->name, sizeof(ec->name), "qcom_ec");

	ec->cdev = devm_hwmon_device_register_with_groups(dev,
						   ec->name,
						   ec,
						   magicbook_ec_groups);

	if (IS_ERR(ec->cdev)) {
		return dev_err_probe(dev, PTR_ERR(ec->cdev),
				     "Failed to register the EC hwmon device\n");
	}

	ret = ec_battery_setup(dev);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to setup battery attributes\n");

	ret = i2c_smbus_write_byte_data(client, EC_IRQ_ENABLE_REG, 0x01);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to enable interrupts\n");

	return 0;
}

static void magicbook_ec_remove(struct i2c_client *client)
{
	struct device *dev = &client->dev;

	int ret;
	ret = i2c_smbus_write_byte_data(client, EC_IRQ_ENABLE_REG, 0x00);
	if (ret < 0)
		dev_err(dev, "Failed to disable interrupts: %d\n", ret);

	ec_battery_exit(dev);
}

static int magicbook_ec_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct magicbook_ec *ec = i2c_get_clientdata(client);
	int ret;

	led_classdev_suspend(&ec->kbd_backlight);

	// screen off
	ret = qcom_ec_write(ec, EC_SCREEN_NOTIFY_CMD, EC_NOTIFY_SCREEN_REG, EC_NOTIFY_SCREEN_OFF);
	if (ret) {
		dev_err(dev, "Failed to notify screen on %d\n", ret);
	}

	ret = i2c_smbus_write_byte_data(client, EC_SUSPEND_RESUME_REG, EC_NOTIFY_SUSPEND_ENTER);
	if (ret)
		return ret;

	//ret = i2c_smbus_write_byte_data(client, EC_IRQ_ENABLE_REG, 0x0);
	if (ret < 0)
		dev_err_probe(dev, ret, "Failed to disable interrupts %d\n", ret);


	return 0;
}

static int magicbook_ec_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct magicbook_ec *ec = i2c_get_clientdata(client);
	int ret;


	ret = i2c_smbus_write_byte_data(client, EC_IRQ_ENABLE_REG, 0x01);
	if (ret < 0)
		dev_err(dev, "Failed to enable interrupts %d\n", ret);



	ret = i2c_smbus_write_byte_data(client, EC_SUSPEND_RESUME_REG, EC_NOTIFY_SUSPEND_EXIT);
	if (ret)
		return ret;

	// screen on
	ret = qcom_ec_write(ec, EC_SCREEN_NOTIFY_CMD, EC_NOTIFY_SCREEN_REG, EC_NOTIFY_SCREEN_ON);
	if (ret) {
		dev_err(dev, "Failed to notify screen on %d\n", ret);
	}

	led_classdev_resume(&ec->kbd_backlight);
	return 0;
}

static const struct of_device_id magicbook_ec_of_match[] = {
	{ .compatible = "honor,magicbook-ec" },
	{ .compatible = "honor,magicbook-art-14-ec" },
	{}
};
MODULE_DEVICE_TABLE(of, magicbook_ec_of_match);

static const struct i2c_device_id magicbook_ec_i2c_id_table[] = {
	{ "magicbook-art-14", },
	{}
};
MODULE_DEVICE_TABLE(i2c, magicbook_ec_i2c_id_table);

static DEFINE_SIMPLE_DEV_PM_OPS(magicbook_ec_pm_ops,
		magicbook_ec_suspend,
		magicbook_ec_resume);

static struct i2c_driver magicbook_ec_i2c_driver = {
	.driver = {
		.name = "magicbook-art-14-ec",
		.of_match_table = magicbook_ec_of_match,
		.pm = &magicbook_ec_pm_ops
	},
	.probe = magicbook_ec_probe,
	.remove = magicbook_ec_remove,
	.id_table = magicbook_ec_i2c_id_table,
};
module_i2c_driver(magicbook_ec_i2c_driver);

MODULE_DESCRIPTION("Lenovo Yoga Slim 7x Embedded Controller");
MODULE_LICENSE("GPL");
