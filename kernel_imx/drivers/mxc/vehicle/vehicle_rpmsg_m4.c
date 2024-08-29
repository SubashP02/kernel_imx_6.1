/*
 * Copyright 2018,2021 NXP
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/imx_rpmsg.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_qos.h>
#include <linux/pm_domain.h>
#include <linux/reboot.h>
#include <linux/rpmsg.h>
#include <linux/delay.h>
#include <linux/extcon.h>
#include <linux/extcon-provider.h>
#include <linux/firmware/imx/sci.h>
#include <linux/slab.h>
#include <linux/of_platform.h>
#include <linux/of.h>
#include <linux/mutex.h>
#include "vehicle_core.h"

#define RPMSG_TIMEOUT 1000
#define REGISTER_PERIOD 50

/*command type which used between AP and M4 core*/
enum vehicle_cmd {
	VEHICLE_RPMSG_INIT = 0,
	VEHICLE_RPMSG_REGISTER,
	VEHICLE_RPMSG_UNREGISTER,
	VEHICLE_RPMSG_CONTROL,
	VEHICLE_RPMSG_PWR_REPORT,
	VEHICLE_RPMSG_GET_INFO,
	VEHICLE_RPMSG_BOOT_REASON,
	VEHICLE_RPMSG_PWR_CTRL,
	VEHICLE_RPMSG_VSTATE,
};

/*
 * There are three types for vehicle_cmd.
 * REQUEST/RESPONSE is paired.
 * NOTIFICATION have no response.
 */
enum vehicle_cmd_type {
	VEHICLE_RPMSG_REQUEST = 0,
	VEHICLE_RPMSG_RESPONSE,
	VEHICLE_RPMSG_NOTIFICATION,
};

/*mcu_state: mcuOperateMode in command REGISTER*/
enum mcu_state {
	RESOURCE_FREE = 0,
	REROURCE_BUSY,
	USER_ACTIVITY_BUSY,
};


struct rpmsg_vehicle_mcu_drvdata {
	struct rpmsg_device *rpdev;
	struct device *dev;
	struct device *vehicle_dev;
	struct vehicle_rpmsg_data *msg;
	struct pm_qos_request pm_qos_req;
	struct delayed_work vehicle_register_work;
	struct completion cmd_complete;
	/*
	 * register_ready means ap have been registered as client and been ready
	 * load camera/display modoules. When it get the driver signal first time
	 * and register_ready is true, it will loader related moduled.
	 */
	bool register_ready;
	/* register_ready means ap have been registered as client */
	bool vehicle_client_registered;
};

struct vehicle_rpmsg_data {
	struct imx_rpmsg_head header;
	u32 client;
	union {
		u8 reserved1;
		u8 retcode;
	};
	union {
		u8 reason;
		u8 partition_id;
		u16 control_id;
		u16 powerstate;
		u16 infoindex;
		u16 currentstate;
		u16 statetype;
		u16 devicestate;
		u16 result;
	};
	union {
		u8 length;
		u16 substate;
		u32 timeout;
		u32 statevalue;
		u32 time_postphone;
	};
	union {
		u32 controlparam;
		u32 reserved2;
	};

	union {
		u8 index;
		u8 reserved3;
	};
} __attribute__((packed));

#ifdef CONFIG_EXTCON
static const unsigned int imx_vehicle_rpmsg_extcon_register_cables[] = {
	EXTCON_VEHICLE_RPMSG_REGISTER,
	EXTCON_NONE,
};
static const unsigned int imx_vehicle_rpmsg_extcon_event_cables[] = {
	EXTCON_VEHICLE_RPMSG_EVENT,
	EXTCON_NONE,
};
struct extcon_dev *rg_edev;
struct extcon_dev *ev_edev;
#endif

static DEFINE_MUTEX(vehicle_send_rpmsg_lock);

extern void vehicle_hw_prop_ops_register(const struct hw_prop_ops* prop_ops);
extern void vehicle_hal_set_property(u16 prop, u8 index, u32 value, u32 param);
static struct rpmsg_vehicle_mcu_drvdata *vehicle_rpmsg;
static struct class* vehicle_rpmsg_class;
int state = 0;

/*send message to M4 core through rpmsg*/
static int vehicle_send_message(struct vehicle_rpmsg_data *msg,
			struct rpmsg_vehicle_mcu_drvdata *info, bool ack)
{
	int err;
        
	if (!info->rpdev) {
		dev_err(info->dev,
			"rpmsg channel not ready, m4 image ready?\n");
		return -EINVAL;
	}

	mutex_lock(&vehicle_send_rpmsg_lock);
	cpu_latency_qos_add_request(&info->pm_qos_req, 0);

	if (ack) {
		reinit_completion(&info->cmd_complete);
	}

	err = rpmsg_send(info->rpdev->ept, (void *)msg,
			    sizeof(struct vehicle_rpmsg_data));
	if (err) {
		dev_err(&info->rpdev->dev, "rpmsg_send failed: %d\n", err);
		goto err_out;
	}

	if (ack) {
		err = wait_for_completion_timeout(&info->cmd_complete,
					msecs_to_jiffies(RPMSG_TIMEOUT));
		if (!err) {
			dev_err(&info->rpdev->dev, "rpmsg_send timeout!\n");
			err = -ETIMEDOUT;
			goto err_out;
		}

		if (info->msg->retcode != 0) {
			dev_err(&info->rpdev->dev, "rpmsg not ack %d!\n",
				info->msg->retcode);
			err = -EINVAL;
			goto err_out;
		}

		err = 0;
	}

err_out:
	cpu_latency_qos_remove_request(&info->pm_qos_req);
	mutex_unlock(&vehicle_send_rpmsg_lock);

	return err;
}

void mcu_set_control_commands(u32 prop, u32 area, u32 value)
{
	struct vehicle_rpmsg_data msg;

	msg.header.cate = IMX_RPMSG_VEHICLE;
	msg.header.major = IMX_RMPSG_MAJOR;
	msg.header.minor = IMX_RMPSG_MINOR;
	msg.header.type = 0;
	msg.header.cmd = VEHICLE_RPMSG_CONTROL;
	msg.client = 0;
	msg.control_id = (u16)VEHICLE_UNSUPPORTED;
	dev_dbg("%s: prop %d, area %d, value %d \n", __func__, prop, area, value);
	switch (prop) {
	case HVAC_FAN_SPEED:
		pr_info("set fan speed with value %d\n", value);
		msg.control_id = VEHICLE_FAN_SPEED;
		break;
	case HVAC_FAN_DIRECTION:
		pr_info("set fan direction with value %d\n", value);
		msg.control_id = VEHICLE_FAN_DIRECTION;
		break;
	case HVAC_AUTO_ON:
		pr_info("set fan auto on with value %d\n", value);
		msg.control_id = VEHICLE_AUTO_ON;
		break;
	case HVAC_AC_ON:
		pr_info("set fan ac on with value %d\n", value);
		msg.control_id = VEHICLE_AC;
		break;
	case HVAC_RECIRC_ON:
		pr_info("set fan recirc on with value %d\n", value);
		msg.control_id = VEHICLE_RECIRC_ON;
		break;
	case HVAC_DEFROSTER:
		pr_info("set defroster index %d with value %d\n", area, value);
		msg.control_id = VEHICLE_DEFROST;
		break;
	case HVAC_TEMPERATURE_SET:
		pr_info("set temp index %d with value %d\n", area, value);
		msg.control_id = VEHICLE_AC_TEMP;
		break;
	case HVAC_POWER_ON:
		pr_info("set hvac power on with value %d\n", value);
		msg.control_id = VEHICLE_HVAC_POWER_ON;
		break;
	case AP_POWER_STATE_REPORT:
		pr_info("receive power state report with value %d\n", value);
		break;
	case AP_POWER_STATE_REQ:
		// Proper action is TBD
		break;
	case WATCHDOG_ALIVE:
	case DISPLAY_BRIGHTNESS:
	case PERF_VEHICLE_SPEED:
		// Proper action is TBD
		return;
	case HVAC_SEAT_TEMPERATURE:
		pr_info("set seat temperature index %d with value %d\n", area, value);
		msg.control_id = VEHICLE_SEAT_TEMPERATURE;
		break;
	case GEAR_SELECTION:
	case TURN_SIGNAL_STATE:
	case CURRENT_GEAR:
		// GEAR is handled in mxc vehicle driver
		break;
	case INFO_MAKE:
	case INFO_MODEL:
	case POWER_POLICY_REQ:
	case FUEL_DOOR_OPEN:
	case EV_CHARGE_PORT_OPEN:
	case EV_CHARGE_PORT_CONNECTED:
	case PARKING_BRAKE_ON:
	case FUEL_LEVEL_LOW:
	case NIGHT_MODE:
	case ABS_ACTIVE:
	case TRACTION_CONTROL_ACTIVE:
	case SUPPORT_CUSTOMIZE_VENDOR_PERMISSION:
	case INFO_MODEL_YEAR:
	case INFO_FUEL_DOOR_LOCATION:
	case INFO_EV_PORT_LOCATION:
	case ENGINE_OIL_LEVEL:
	case IGNITION_STATE:
	case HVAC_STEERING_WHEEL_HEAT:
	case HVAC_TEMPERATURE_DISPLAY_UNITS:
	case DISTANCE_DISPLAY_UNITS:
	case TIRE_PRESSURE_DISPLAY_UNITS:
	case VEHICLE_SPEED_DISPLAY_UNITS:
	case HEADLIGHTS_STATE:
	case HIGH_BEAM_LIGHTS_STATE:
	case FOG_LIGHTS_STATE:
	case PARKING_BRAKE_AUTO_APPLY:
	case HAZARD_LIGHTS_STATE:
	case HEADLIGHTS_SWITCH:
	case HIGH_BEAM_LIGHTS_SWITCH:
	case FOG_LIGHTS_SWITCH:
	case HAZARD_LIGHTS_SWITCH:
	case CLUSTER_SWITCH_UI:
	case CLUSTER_REQUEST_DISPLAY:
	case POWER_POLICY_GROUP_REQ:
	case CURRENT_POWER_POLICY:
	case ELECTRONIC_TOLL_COLLECTION_CARD_TYPE:
	case ELECTRONIC_TOLL_COLLECTION_CARD_STATUS:
	case INFO_FUEL_TYPE:
	case INFO_EV_CONNECTOR_TYPE:
	case INFO_DRIVER_SEAT:
	case INFO_EXTERIOR_DIMENSIONS:
	case INFO_MULTI_EV_PORT_LOCATIONS:
	case HW_KEY_INPUT:
	case HW_ROTARY_INPUT:
	case HW_CUSTOM_INPUT:
	case EVS_SERVICE_REQUEST:
	case CLUSTER_DISPLAY_STATE:
	case EPOCH_TIME:
	case VHAL_HEARTBEAT:
	case WHEEL_TICK:
	case INFO_FUEL_CAPACITY:
	case INFO_EV_BATTERY_CAPACITY:
	case PERF_ODOMETER:
	case PERF_STEERING_ANGLE:
	case PERF_REAR_STEERING_ANGLE:
	case ENGINE_OIL_TEMP:
	case ENGINE_RPM:
	case FUEL_LEVEL:
	case RANGE_REMAINING:
	case EV_BATTERY_LEVEL:
	case EV_BATTERY_INSTANTANEOUS_CHARGE_RATE:
	case ENV_OUTSIDE_TEMPERATURE:
	case HVAC_TEMPERATURE_VALUE_SUGGESTION:
	case STORAGE_ENCRYPTION_BINDING_SEED:
	case CLUSTER_NAVIGATION_STATE:
	case INITIAL_USER_INFO:
	case SWITCH_USER:
	case CREATE_USER:
	case REMOVE_USER:
	case USER_IDENTIFICATION_ASSOCIATION:
	case WATCHDOG_TERMINATED_PROCESS:
	case CLUSTER_REPORT_STATE:
	case WINDOW_LOCK:
	case VEHICLE_MAP_SERVICE:
	case WINDOW_POS:
	case HVAC_MAX_AC_ON:
	case HVAC_MAX_DEFROST_ON:
	case HVAC_DUAL_ON:
	case HVAC_AUTO_RECIRC_ON:
	case HVAC_SEAT_VENTILATION:
	case HVAC_ELECTRIC_DEFROSTER_ON:
	case SEAT_OCCUPANCY:
	case HVAC_FAN_DIRECTION_AVAILABLE:
	case DOOR_LOCK:
	case DOOR_POS:
	case TIRE_PRESSURE:
	case CRITICALLY_LOW_TIRE_PRESSURE:
	case FRONT_FOG_LIGHTS_STATE:
	case FRONT_FOG_LIGHTS_SWITCH:
	case REAR_FOG_LIGHTS_STATE:
	case REAR_FOG_LIGHTS_SWITCH:
	case EV_CHARGE_CURRENT_DRAW_LIMIT:
	case EV_CHARGE_PERCENT_LIMIT:
	case EV_CHARGE_STATE:
	case EV_CHARGE_SWITCH:
	case EV_CHARGE_TIME_REMAINING:
	case EV_REGENERATIVE_BRAKING_STATE:
	case TRAILER_PRESENT:
	case VEHICLE_CURB_WEIGHT:
		// Proper action is TBD
		break;
	case VENDOR_EXTENSION_STRING_PROPERTY:
	case VENDOR_EXTENSION_BOOLEAN_PROPERTY:
	case VENDOR_EXTENSION_FLOAT_PROPERTY:
	case VENDOR_EXTENSION_INT_PROPERTY:
	case READING_LIGHTS_SWITCH:
	case SEAT_BELT_HEIGHT_POS:
	case WINDOW_MOVE:
	case MIRROR_AUTO_TILT_ENABLED:
	case SEAT_FORE_AFT_POS:
	case EV_BATTERY_DISPLAY_UNITS:
	case SEAT_HEIGHT_MOVE:
	case INFO_VIN:
	case SEAT_HEADREST_ANGLE_POS:
	case SEAT_HEADREST_ANGLE_MOVE:
	case ENGINE_COOLANT_TEMP:
	case MIRROR_LOCK:
	case HVAC_ACTUAL_FAN_SPEED_RPM:
	case SEAT_BELT_HEIGHT_MOVE:
	case SEAT_FORE_AFT_MOVE:
	case SEAT_BACKREST_ANGLE_2_MOVE:
	case SEAT_HEIGHT_POS:
	case SEAT_TILT_POS:
	case SEAT_DEPTH_MOVE:
	case SEAT_LUMBAR_FORE_AFT_POS:
	case SEAT_EASY_ACCESS_ENABLED:
	case SEAT_LUMBAR_FORE_AFT_MOVE:
	case SEAT_AIRBAG_ENABLED:
	case AUTOMATIC_EMERGENCY_BRAKING_ENABLED:
	case SEAT_LUMBAR_SIDE_SUPPORT_POS:
	case SEAT_LUMBAR_SIDE_SUPPORT_MOVE:
	case SEAT_MEMORY_SELECT:
	case HANDS_ON_DETECTION_DRIVER_STATE:
	case SEAT_HEADREST_HEIGHT_MOVE:
	case SEAT_HEADREST_FORE_AFT_POS:
	case MIRROR_FOLD:
	case SEAT_HEADREST_FORE_AFT_MOVE:
	case GLOVE_BOX_LOCKED:
	case SEAT_TILT_MOVE:
	case MIRROR_Z_POS:
	case HVAC_SIDE_MIRROR_HEAT:
	case SEAT_LUMBAR_VERTICAL_POS:
	case SEAT_FOOTWELL_LIGHTS_SWITCH:
	case WINDSHIELD_WIPERS_STATE:
	case SEAT_DEPTH_POS:
	case SEAT_HEADREST_HEIGHT_POS_V2:
	case EV_CURRENT_BATTERY_CAPACITY:
	case EV_BRAKE_REGENERATION_LEVEL:
	case SEAT_LUMBAR_VERTICAL_MOVE:
	case SEAT_WALK_IN_POS:
	case SEAT_BELT_BUCKLED:
	case SEAT_BACKREST_ANGLE_1_POS:
	case LANE_KEEP_ASSIST_STATE:
	case LANE_CENTERING_ASSIST_ENABLED:
	case SEAT_BACKREST_ANGLE_1_MOVE:
	case SEAT_BACKREST_ANGLE_2_POS:
	case MIRROR_AUTO_FOLD_ENABLED:
	case BLIND_SPOT_WARNING_ENABLED:
	case EV_STOPPING_MODE:
	case STEERING_WHEEL_LIGHTS_STATE:
	case MIRROR_Z_MOVE:
	case SEAT_CUSHION_SIDE_SUPPORT_POS:
	case MIRROR_Y_MOVE:
	case AUTOMATIC_EMERGENCY_BRAKING_STATE:
	case FORWARD_COLLISION_WARNING_ENABLED:
	case SEAT_MEMORY_SET:
	case HANDS_ON_DETECTION_WARNING:
	case WINDSHIELD_WIPERS_PERIOD:
	case SEAT_FOOTWELL_LIGHTS_STATE:
	case DOOR_CHILD_LOCK_ENABLED:
	case DOOR_MOVE:
	case WINDSHIELD_WIPERS_SWITCH:
	case STEERING_WHEEL_DEPTH_POS:
	case GLOVE_BOX_DOOR_POS:
	case STEERING_WHEEL_HEIGHT_POS:
	case STEERING_WHEEL_THEFT_LOCK_ENABLED:
	case SEAT_CUSHION_SIDE_SUPPORT_MOVE:
	case CABIN_LIGHTS_STATE:
	case STEERING_WHEEL_EASY_ACCESS_ENABLED:
	case CABIN_LIGHTS_SWITCH:
	case STEERING_WHEEL_HEIGHT_MOVE:
	case STEERING_WHEEL_LIGHTS_SWITCH:
	case FUEL_CONSUMPTION_UNITS_DISTANCE_OVER_VOLUME:
	case MIRROR_Y_POS:
	case LOCATION_CHARACTERIZATION:
	case EMERGENCY_LANE_KEEP_ASSIST_STATE:
	case CRUISE_CONTROL_TYPE:
	case CRUISE_CONTROL_STATE:
	case STEERING_WHEEL_LOCKED:
	case ADAPTIVE_CRUISE_CONTROL_TARGET_TIME_GAP:
	case ADAPTIVE_CRUISE_CONTROL_LEAD_VEHICLE_MEASURED_DISTANCE:
	case HANDS_ON_DETECTION_ENABLED:
	case FORWARD_COLLISION_WARNING_STATE:
	case STEERING_WHEEL_DEPTH_MOVE:
	case EMERGENCY_LANE_KEEP_ASSIST_ENABLED:
	case GENERAL_SAFETY_REGULATION_COMPLIANCE:
	case LANE_CENTERING_ASSIST_STATE:
	case CRUISE_CONTROL_ENABLED:
	case PERF_VEHICLE_SPEED_DISPLAY:
	case HVAC_TEMPERATURE_CURRENT:
	case LANE_KEEP_ASSIST_ENABLED:
	case READING_LIGHTS_STATE:
	case CRUISE_CONTROL_TARGET_SPEED:
	case ENGINE_IDLE_AUTO_STOP_ENABLED:
	case BLIND_SPOT_WARNING_STATE:
	case LANE_DEPARTURE_WARNING_ENABLED:
	case FUEL_VOLUME_DISPLAY_UNITS:
	case LANE_DEPARTURE_WARNING_STATE:
	case kMixedTypePropertyForTest:
		// Proper action is TBD
		break;
	default:
		pr_err("this type is not correct!\n");
	}
	msg.controlparam = value;
	msg.index = area;
	if (vehicle_send_message(&msg, vehicle_rpmsg, true)) {
		pr_err("send message failed!\n");
	}
}

static int update_dt_prop(struct device_node *dn, char* name, void* value, int len)
{
	struct property *new_prop;

	new_prop = kzalloc(sizeof(*new_prop), GFP_KERNEL);
	if (!new_prop)
		return -ENOMEM;
	new_prop->name = kstrdup(name, GFP_KERNEL);
	if (!new_prop->name) {
		kfree(new_prop);
		return -ENOMEM;
	}
	new_prop->length = len;
	new_prop->value = kzalloc(new_prop->length, GFP_KERNEL);
	if (!new_prop->value) {
		kfree(new_prop->name);
		kfree(new_prop);
		return -ENOMEM;
	}
	memcpy(new_prop->value, value, len);

	of_update_property(dn, new_prop);

	return 0;
}

const struct of_device_id of_shared_match_table[] = {
	{ .compatible = "simple-bus", },
	{} /* Empty terminated list */
};
static void notice_evs_released(struct rpmsg_device *rpdev)
{
	struct device_node *dev_node, *shared_node;
	int i, err;

	dev_node = vehicle_rpmsg->vehicle_dev->of_node;
	int count = of_get_available_child_count(dev_node);
	if (count) {
		if (of_platform_populate(dev_node,
			NULL, NULL, vehicle_rpmsg->vehicle_dev)) {
			dev_err(&rpdev->dev, "failed to populate child nodes\n");
		}
	}

	for (i=0; ;i++) {
		shared_node = of_parse_phandle(dev_node, "fsl,resources", i);
		if (!shared_node)
			break;
		err = update_dt_prop(shared_node, "status", "okay", sizeof("okay"));
		if (err)
			dev_err(&rpdev->dev, "failed to update %pOF status, err:%d\n",shared_node, err);

		of_node_clear_flag(shared_node, OF_POPULATED_BUS);
		of_platform_populate(shared_node, of_shared_match_table, NULL, vehicle_rpmsg->vehicle_dev);
	}
}

/*vehicle_rpmsg_cb is called once get rpmsg from M4*/
static int vehicle_rpmsg_cb(struct rpmsg_device *rpdev,
	void *data, int len, void *priv, u32 src)
{
	struct vehicle_rpmsg_data *msg = (struct vehicle_rpmsg_data *)data;

	vehicle_rpmsg->msg = msg;
	if (msg->header.cmd == VEHICLE_RPMSG_REGISTER) {
		complete(&vehicle_rpmsg->cmd_complete);
		if (msg->retcode == 0) {
			if (msg->devicestate == RESOURCE_FREE) {
				notice_evs_released(rpdev);
#ifdef CONFIG_EXTCON
				extcon_set_state_sync(rg_edev, EXTCON_VEHICLE_RPMSG_REGISTER, 1);
#endif
			} else
				vehicle_rpmsg->register_ready = true;
			vehicle_rpmsg->vehicle_client_registered = true;
		}
	} else if (msg->header.cmd == VEHICLE_RPMSG_UNREGISTER) {
		complete(&vehicle_rpmsg->cmd_complete);
		if (msg->retcode == 0) {
#ifdef CONFIG_EXTCON
			extcon_set_state_sync(rg_edev, EXTCON_VEHICLE_RPMSG_REGISTER, 0);
#endif
			vehicle_rpmsg->vehicle_client_registered = false;
		}
	} else if (msg->header.cmd == VEHICLE_RPMSG_CONTROL) {
		complete(&vehicle_rpmsg->cmd_complete);
		dev_dbg(&rpdev->dev, "get vehicle control signal %d", msg->result);
	} else if (msg->header.cmd == VEHICLE_RPMSG_PWR_REPORT) {
		complete(&vehicle_rpmsg->cmd_complete);
	} else if (msg->header.cmd == VEHICLE_RPMSG_GET_INFO) {
		complete(&vehicle_rpmsg->cmd_complete);
	} else if (msg->header.cmd == VEHICLE_RPMSG_BOOT_REASON) {
		msg->header.type = VEHICLE_RPMSG_RESPONSE;
		msg->retcode = 0;
		if (vehicle_send_message(msg, vehicle_rpmsg, false))
			dev_warn(&rpdev->dev, "vehicle_rpmsg_cb send message error \n");
	} else if (msg->header.cmd == VEHICLE_RPMSG_PWR_CTRL) {
		msg->header.type = VEHICLE_RPMSG_RESPONSE;
		msg->retcode = 0;
		if (vehicle_send_message(msg, vehicle_rpmsg, false))
			dev_warn(&rpdev->dev, "vehicle_rpmsg_cb send message error \n");
		vehicle_hal_set_property(VEHICLE_POWER_STATE_REQ, 0, msg->powerstate, msg->statevalue);
	} else if (msg->header.cmd == VEHICLE_RPMSG_VSTATE) {
		msg->header.type = VEHICLE_RPMSG_RESPONSE;
		msg->retcode = 0;
		if(msg->statetype == VEHICLE_GEAR) {
			if (msg->statevalue == VEHICLE_GEAR_DRIVE) {
				if (vehicle_rpmsg->register_ready) {
					notice_evs_released(rpdev);
#ifdef CONFIG_EXTCON
					extcon_set_state_sync(rg_edev, EXTCON_VEHICLE_RPMSG_REGISTER, 1);
#endif
					vehicle_rpmsg->register_ready = false;
				} else {
					extcon_set_state_sync(ev_edev, EXTCON_VEHICLE_RPMSG_EVENT, 0);
				}
			} else if (msg->statevalue == VEHICLE_GEAR_REVERSE) {
#ifdef CONFIG_EXTCON
				extcon_set_state_sync(ev_edev, EXTCON_VEHICLE_RPMSG_EVENT, 1);
#endif
			}
		}

		if (msg->statetype == VEHICLE_HVAC_POWER_ON){
			vehicle_hal_set_property(msg->statetype, VEHICLE_AREA_SEAT_ROW_1_LEFT ,  msg->statevalue, 0);
			vehicle_hal_set_property(msg->statetype, VEHICLE_AREA_SEAT_ROW_1_RIGHT ,  msg->statevalue, 0);
		}
		else {
			vehicle_hal_set_property(msg->statetype, msg->index,  msg->statevalue, 0);
		}
		if (vehicle_send_message(msg, vehicle_rpmsg, false))
			dev_warn(&rpdev->dev, "vehicle_rpmsg_cb send message error \n");
	}

	return 0;
}

/* send register event to M4 core
 * M4 will release camera/display once register successfully.
 */
static void vehicle_init_handler(struct work_struct *work)
{
	struct vehicle_rpmsg_data msg;
	struct imx_sc_ipc *ipc_handle;
	u8 os_part;
	int err;

	msg.header.cate = IMX_RPMSG_VEHICLE;
	msg.header.major = IMX_RMPSG_MAJOR;
	msg.header.minor = IMX_RMPSG_MINOR;
	msg.header.type = 0;
	msg.header.cmd = VEHICLE_RPMSG_REGISTER;
	msg.client = 0;
	msg.reserved1 = 0;

	imx_scu_get_handle(&ipc_handle);

	err = imx_sc_rm_get_partition(ipc_handle, &os_part);
	if (err != 0) {
		pr_err("sc_rm_get_partition failed!\n");
		msg.partition_id = 0xff;
	} else {
		msg.partition_id = os_part;
	}
	/* need check whether ap have been unregistered before register the ap*/
	if (!vehicle_rpmsg->vehicle_client_registered) {
		while (vehicle_send_message(&msg, vehicle_rpmsg, true)) {
			msleep(REGISTER_PERIOD);
		}
	}
}

/* This functions is on used now */
#if 0
/* send unregister event to M4 core
 * M4 will not send can event to ap once get the unregister signal successfully.
 */
static void vehicle_deinit_handler(struct work_struct *work)
{
	struct vehicle_rpmsg_data msg;

	msg.header.cate = IMX_RPMSG_VEHICLE;
	msg.header.major = IMX_RMPSG_MAJOR;
	msg.header.minor = IMX_RMPSG_MINOR;
	msg.header.type = 0;
	msg.header.cmd = VEHICLE_RPMSG_UNREGISTER;
	msg.client = 0;
	msg.reserved1 = 0;
	msg.reason = 0;

	/* need check whether ap have been registered before deregister the ap*/
	if (vehicle_rpmsg->vehicle_client_registered) {
		if (vehicle_send_message(&msg, vehicle_rpmsg, true)) {
			pr_err("unregister vehicle device failed!\n");
		}
	}
}
#endif

static int vehicle_rpmsg_probe(struct rpmsg_device *rpdev)
{
#ifdef CONFIG_EXTCON
	int ret = 0;
	rg_edev = devm_extcon_dev_allocate(&rpdev->dev, imx_vehicle_rpmsg_extcon_register_cables);
	if (IS_ERR(rg_edev)) {
		dev_err(&rpdev->dev, "failed to allocate extcon device\n");
	}
	ret = devm_extcon_dev_register(&rpdev->dev,rg_edev);
	if (ret < 0) {
		dev_err(&rpdev->dev, "failed to register extcon device\n");
	}
	ev_edev = devm_extcon_dev_allocate(&rpdev->dev, imx_vehicle_rpmsg_extcon_event_cables);
	if (IS_ERR(ev_edev)) {
		dev_err(&rpdev->dev, "failed to allocate extcon device\n");
	}
	ret = devm_extcon_dev_register(&rpdev->dev,ev_edev);
	if (ret < 0) {
		dev_err(&rpdev->dev, "failed to register extcon device\n");
	}
#endif
	vehicle_rpmsg->rpdev = rpdev;
	init_completion(&vehicle_rpmsg->cmd_complete);
	dev_info(&rpdev->dev, "new channel: 0x%x -> 0x%x!\n",
			rpdev->src, rpdev->dst);

	struct vehicle_rpmsg_data msg;

	msg.header.cate = IMX_RPMSG_VEHICLE;
	msg.header.major = IMX_RMPSG_MAJOR;
	msg.header.minor = IMX_RMPSG_MINOR;
	msg.header.type = 0;
	msg.header.cmd = VEHICLE_RPMSG_INIT;
	msg.client = 0;
	msg.reserved1 = 0;

	if (vehicle_send_message(&msg, vehicle_rpmsg, false)) {
		pr_err("rpmsg init failed!\n");
	}

	return 0;
}

static struct rpmsg_device_id vehicle_rpmsg_id_table[] = {
	{ .name	= "rpmsg-vehicle-channel" },
	{ },
};

static struct rpmsg_driver vehicle_rpmsg_driver = {
	.drv.name       = "vehicle_rpmsg",
	.drv.owner      = THIS_MODULE,
	.id_table       = vehicle_rpmsg_id_table,
	.probe          = vehicle_rpmsg_probe,
	.callback       = vehicle_rpmsg_cb,
};

static ssize_t register_show(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	return sprintf(buf, "%u\n", state);
}

/*register ap to M4 once echo 1 > /sys/devices/platform/vehicle_rpmsg/register*/
static ssize_t register_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf,
		size_t size)
{
	if (!size)
		return -EINVAL;

	unsigned long state_set = simple_strtoul(buf, NULL, 10);
	if (state_set == 1 && state_set != state) {
		INIT_DELAYED_WORK(&vehicle_rpmsg->vehicle_register_work,
				vehicle_init_handler);
		schedule_delayed_work(&vehicle_rpmsg->vehicle_register_work, 0);
		state = state_set;
	}
	return size;
}

static DEVICE_ATTR(register, 0664, register_show, register_store);

static struct rpmsg_vehicle_mcu_drvdata *
rpmsg_vehicle_get_devtree_pdata(struct device *dev)
{
	struct rpmsg_vehicle_mcu_drvdata *ddata;

	ddata = devm_kzalloc(dev,
			     sizeof(*ddata),
			     GFP_KERNEL);
	if (!ddata)
		return ERR_PTR(-ENOMEM);

	return ddata;
}

static const struct hw_prop_ops hw_prop_mcu_ops = {
	.set_control_commands = mcu_set_control_commands,
};

static int vehicle_mcu_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rpmsg_vehicle_mcu_drvdata *ddata;
	int err;
        
	ddata = rpmsg_vehicle_get_devtree_pdata(dev);
	if (IS_ERR(ddata))
		return PTR_ERR(ddata);

	vehicle_rpmsg = ddata;
	vehicle_rpmsg->vehicle_dev = dev;
	vehicle_rpmsg->register_ready = false;
	vehicle_rpmsg->vehicle_client_registered = false;
	platform_set_drvdata(pdev, ddata);

	vehicle_rpmsg_class = class_create(THIS_MODULE, "vehicle_rpmsg");
	if (IS_ERR(vehicle_rpmsg_class)) {
		dev_err(dev, "failed to create class.\n");
		goto out_free_mem;
	}
	err = device_create_file(dev, &dev_attr_register);
	if (err)
		goto out_free_mem;

	vehicle_hw_prop_ops_register(&hw_prop_mcu_ops);

	return 0;
out_free_mem:
	return -ENOMEM;
}

static int vehicle_mcu_remove(struct platform_device *pdev)
{
	class_destroy(vehicle_rpmsg_class);
	unregister_rpmsg_driver(&vehicle_rpmsg_driver);
	return 0;
}

static const struct of_device_id imx_vehicle_mcu_id[] = {
	{ .compatible = "nxp,imx-vehicle-m4", },
	{},
};

static struct platform_driver vehicle_device_driver = {
	.probe          = vehicle_mcu_probe,
	.remove         = vehicle_mcu_remove,
	.driver         =  {
		.name   = "rpmsg-vehicle-m4",
		.of_match_table = imx_vehicle_mcu_id,
	}
};

static int vehicle_mcu_init(void)
{
	int err;

	err = platform_driver_register(&vehicle_device_driver);
	if (err) {
		pr_err("Failed to register rpmsg vehicle driver\n");
		return err;
	}

	err = register_rpmsg_driver(&vehicle_rpmsg_driver);
	if (err < 0) {
		pr_err("register rpmsg driver failed\n");
		return -EINVAL;
	}

	return 0;
}

static void __exit vehicle_mcu_exit(void)
{
	platform_driver_unregister(&vehicle_device_driver);
}

postcore_initcall_sync(vehicle_mcu_init);
module_exit(vehicle_mcu_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("NXP Semiconductor");
MODULE_DESCRIPTION("VEHICLE M4 image");
