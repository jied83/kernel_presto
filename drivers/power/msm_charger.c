/* Copyright (c) 2010-2011, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/err.h>
#include <linux/mfd/pmic8058.h>
#include <linux/interrupt.h>
#include <linux/power_supply.h>
#include <linux/delay.h>
#include <linux/bitops.h>
#include <linux/debugfs.h>
#include <linux/msm-charger.h>
#include <linux/time.h>
#include <linux/slab.h>
#include <linux/wakelock.h>

#include <asm/atomic.h>

#include <mach/msm_hsusb.h>
//pz1946 20110904 battery remove issue
//#ifdef CONFIG_SKY_CHARGING  //kobj 110513
#if defined(CONFIG_SKY_CHARGING) || defined(CONFIG_SKY_SMB_CHARGER)
#include <linux/reboot.h>
#endif  //CONFIG_SKY_CHARGING
#define MSM_CHG_MAX_EVENTS		16
#define CHARGING_TEOC_MS		9000000
#define UPDATE_TIME_MS			60000
#define RESUME_CHECK_PERIOD_MS		60000

#ifdef CONFIG_SKY_SMB_CHARGER
#define DEFAULT_BATT_MAX_V		4350
#else
#define DEFAULT_BATT_MAX_V		4200
#endif
#define DEFAULT_BATT_MIN_V		3200

#define MSM_CHARGER_GAUGE_MISSING_VOLTS 3500
#define MSM_CHARGER_GAUGE_MISSING_TEMP  35

#define SKY_BATTERY_INFO //2011.09.15 leecy add for battery info

#ifdef CONFIG_SKY_CHARGING  //kobj 110513
#define MSM_CHARGER_TA_CURRENT_INCALL 700//700//800//400
#define MSM_CHARGER_TA_CURRENT_IDLE  700//700//800//700//900  // p14682 kobj 110711 change 
#define DEBUG
#define __DEBUG_KOBJ__
#endif  //CONFIG_SKY_CHARGING
/**
 * enum msm_battery_status
 * @BATT_STATUS_ABSENT: battery not present
 * @BATT_STATUS_ID_INVALID: battery present but the id is invalid
 * @BATT_STATUS_DISCHARGING: battery is present and is discharging
 * @BATT_STATUS_TRKL_CHARGING: battery is being trickle charged
 * @BATT_STATUS_FAST_CHARGING: battery is being fast charged
 * @BATT_STATUS_JUST_FINISHED_CHARGING: just finished charging,
 *		battery is fully charged. Do not begin charging untill the
 *		voltage falls below a threshold to avoid overcharging
 * @BATT_STATUS_TEMPERATURE_OUT_OF_RANGE: battery present,
					no charging, temp is hot/cold
 */
enum msm_battery_status {
	BATT_STATUS_ABSENT,
	BATT_STATUS_ID_INVALID,
	BATT_STATUS_DISCHARGING,
	BATT_STATUS_TRKL_CHARGING,
	BATT_STATUS_FAST_CHARGING,
	BATT_STATUS_JUST_FINISHED_CHARGING,
	BATT_STATUS_TEMPERATURE_OUT_OF_RANGE,
};

struct msm_hardware_charger_priv {
	struct list_head list;
	struct msm_hardware_charger *hw_chg;
	enum msm_hardware_charger_state hw_chg_state;
	unsigned int max_source_current;
	struct power_supply psy;
};

struct msm_charger_event {
	enum msm_hardware_charger_event event;
	struct msm_hardware_charger *hw_chg;
};

struct msm_charger_mux {
	int inited;
	struct list_head msm_hardware_chargers;
	int count_chargers;
	struct mutex msm_hardware_chargers_lock;

	struct device *dev;

	unsigned int max_voltage;
	unsigned int min_voltage;

	unsigned int safety_time;
	struct delayed_work teoc_work;

	unsigned int update_time;
	int stop_update;
	struct delayed_work update_heartbeat_work;

	struct mutex status_lock;
	enum msm_battery_status batt_status;
	struct msm_hardware_charger_priv *current_chg_priv;
	struct msm_hardware_charger_priv *current_mon_priv;

	unsigned int (*get_batt_capacity_percent) (void);

	struct msm_charger_event *queue;
	int tail;
	int head;
	spinlock_t queue_lock;
	int queue_count;
	struct work_struct queue_work;
	struct workqueue_struct *event_wq_thread;
	struct wake_lock wl;
//pz1946 20111108 carkit-charger
#ifdef CONFIG_SKY_CHARGING  //kobj 110513
    unsigned int charger_type;
	unsigned int chargerdone; 
	//unsigned int event_type;
#endif  //CONFIG_SKY_CHARGING
};
//pz1946 20110902 usb ac display
#ifdef CONFIG_SKY_SMB_CHARGER
extern int smb_charger_state;
//pz1946 20110914 factroy cable debug
extern int pm8058_is_factory_cable(void);
#endif

static struct msm_charger_mux msm_chg;

static struct msm_battery_gauge *msm_batt_gauge;
//[ 20120319 PZ1949 from KBJ 
#ifdef CONFIG_FB_MSM_MHL_SII9244 // MHL_KKCHO
//#define MHL_AUTH_TEST
//#define MHL_ON_TIMMING_CONTROL // Only for Auth
#ifndef MHL_AUTH_TEST
#define F_MHL_AUTO_CONNECT
#endif
#endif

#ifdef F_MHL_AUTO_CONNECT
#ifdef MHL_ON_TIMMING_CONTROL
extern void Auth_MHL_ON_Delay_Control(void);
#endif
extern void MHL_On(bool on);
extern int MHL_Cable_On(int on);
extern void MHL_Set_Cable_State(bool connect);

extern int mhl_power_ctrl(int on);
extern void MHL_En_Control(bool on);

#define MHL_CABLE_CONNCET			1
#define MHL_CABLE_DISCONNCET	       0
#endif
//] 20120319 PZ1949 from KBJ 

#ifdef CONFIG_SKY_BATTERY_MAX17040
extern int max17040_get_charge_state(void);
#endif
#ifdef CONFIG_SKY_CHARGING  //kobj 110513
extern void pm8058_chg_set_current_temperature(int chg_current);
extern int pmic8058_tz_get_temp_charging(unsigned long *temp);
extern void pm8058_chg_set_current_incall(int chg_current);

extern void pm8058_set_fast_charging_mode(void);
extern void pm8058_set_out_range_temp(void);
extern void pm8058_set_in_range_temp(void);
extern void test_rest_trickle_val(void);

static unsigned int old_status = 0;

#ifdef CONFIG_SKY_CHARGING  //kobj 110513
static unsigned int is_charger_done = 0;

unsigned int msm_charger_is_done(void)
{
        return is_charger_done;
}

void msm_charger_set_init_done(void)  //20120319 PZ1949 from KBJ
{
        is_charger_done = 0;
}
EXPORT_SYMBOL(msm_charger_set_init_done);

#endif 

unsigned int msm_charger_is_incall(void)
{
        return old_status;
}

void msm_charger_set_current_incall(unsigned int in_call)
{
        int chg_current;

        if(old_status == in_call)
                return;
        else
                old_status = in_call;

        if(in_call)
                chg_current = MSM_CHARGER_TA_CURRENT_INCALL;
        else
                chg_current = MSM_CHARGER_TA_CURRENT_IDLE;
            
	if (msm_chg.current_chg_priv
		&& (msm_chg.current_chg_priv->hw_chg_state
			== CHG_READY_STATE || msm_chg.current_chg_priv->hw_chg_state
			== CHG_CHARGING_STATE) && msm_chg.charger_type == CHG_TYPE_AC) {
			pm8058_chg_set_current_incall(chg_current);
	}
}
EXPORT_SYMBOL(msm_charger_set_current_incall);

void msm_charger_set_lcd_onoff(unsigned int onoff)
{
        int chg_current;

        if(msm_charger_is_incall())
                return;
            
        if(onoff)
                chg_current = 700;
        else
                chg_current = 900;
            
	if (msm_chg.current_chg_priv
		&& (msm_chg.current_chg_priv->hw_chg_state
			== CHG_READY_STATE || msm_chg.current_chg_priv->hw_chg_state
			== CHG_CHARGING_STATE) && msm_chg.charger_type == CHG_TYPE_AC) {
			pm8058_chg_set_current_temperature(chg_current);
	}
}
EXPORT_SYMBOL(msm_charger_set_lcd_onoff);
#endif  //CONFIG_SKY_CHARGING

static int is_chg_capable_of_charging(struct msm_hardware_charger_priv *priv)
{
	if (priv->hw_chg_state == CHG_READY_STATE
	    || priv->hw_chg_state == CHG_CHARGING_STATE)
		return 1;

	return 0;
}

static int is_batt_status_capable_of_charging(void)
{
	if (msm_chg.batt_status == BATT_STATUS_ABSENT
	    || msm_chg.batt_status == BATT_STATUS_TEMPERATURE_OUT_OF_RANGE
	    || msm_chg.batt_status == BATT_STATUS_ID_INVALID
	    || msm_chg.batt_status == BATT_STATUS_JUST_FINISHED_CHARGING)
		return 0;
	return 1;
}

static int is_batt_status_charging(void)
{
	if (msm_chg.batt_status == BATT_STATUS_TRKL_CHARGING
	    || msm_chg.batt_status == BATT_STATUS_FAST_CHARGING)
		return 1;
	return 0;
}

//[ PZ1949 for  
static char *chargingFlagStatus(unsigned int status)
{
    switch(status)
    {
          case  BATT_STATUS_ABSENT : return "ABSENT";
          case  BATT_STATUS_ID_INVALID : return "ID_INVALID";
          case  BATT_STATUS_DISCHARGING : return "DISCHARGING";
          case  BATT_STATUS_TRKL_CHARGING : return "TRKL_CHARGING";
          case  BATT_STATUS_FAST_CHARGING : return "FAST_CHARGING";
          case  BATT_STATUS_JUST_FINISHED_CHARGING : return "JUST_FINISHED_CHARGING";
          case  BATT_STATUS_TEMPERATURE_OUT_OF_RANGE : return "TEMPERATURE_OUT_OF_RANGE";        
          default : return "Wrong status ????";
      }
}

static char *chargingFlagCHGType(unsigned int status)
{
    switch(status)
    {
          case  CHG_TYPE_NONE  : return "NONE";
          case  CHG_TYPE_FACTORY : return "FACTORY";
          case  CHG_TYPE_USB :  return "USB";
          case  CHG_TYPE_AC :  return "AC";
          default : return "Wrong type of charser ????";
    }
}

static char *chargingFlagHwChgState(unsigned int status)
{
    switch(status)
    {
          case CHG_INSERTED_EVENT : return "CHG_INSERTED_EVENT";
          case CHG_ENUMERATED_EVENT :  return "CHG_ENUMERATED_EVENT";
          case CHG_REMOVED_EVENT :  return "CHG_REMOVED_EVENT";
          case CHG_DONE_EVENT :  return "CHG_DONE_EVENT";
          case CHG_BATT_BEGIN_FAST_CHARGING :  return "CHG_BATT_BEGIN_FAST_CHARGING";
          case CHG_BATT_CHG_RESUME :  return "CHG_BATT_CHG_RESUME";
          case CHG_BATT_TEMP_OUTOFRANGE :  return "CHG_BATT_TEMP_OUTOFRANGE";
          case CHG_BATT_TEMP_INRANGE :  return "CHG_BATT_TEMP_INRANGE";
          case CHG_BATT_INSERTED:  return "CHG_BATT_INSERTED";
          case CHG_BATT_REMOVED:  return "CHG_BATT_REMOVE";
          case CHG_BATT_STATUS_CHANGE :  return "CHG_BATT_STATUS_CHANGE";
          case CHG_BATT_NEEDS_RECHARGING :  return "CHG_BATT_NEEDS_RECHARGIN";
          default : return "Wrong HW charging state ????";
    }
}
//] PZ1949 


#ifdef CONFIG_SKY_BATTERY_MAX17040
int sky_get_plug_state(void)
{
        return is_batt_status_charging();
}
#endif  //CONFIG_SKY_BATTERY_MAX17040

static int is_battery_present(void)
{
	if (msm_batt_gauge && msm_batt_gauge->is_battery_present)
		return msm_batt_gauge->is_battery_present();
	else {
		pr_err("msm-charger: no batt gauge batt=absent\n");
		return 0;
	}
}
#if 1//ndef CONFIG_SKY_CHARGING			// p14682 kobj 110620	// kobj 110823 change used temp
static int is_battery_temp_within_range(void)
{
	if (msm_batt_gauge && msm_batt_gauge->is_battery_temp_within_range)
		return msm_batt_gauge->is_battery_temp_within_range();
	else {
		pr_err("msm-charger no batt gauge batt=out_of_temperatur\n");
		return 0;
	}
}
#endif //CONFIG_SKY_CHARGING
static int is_battery_id_valid(void)
{
	if (msm_batt_gauge && msm_batt_gauge->is_battery_id_valid)
		return msm_batt_gauge->is_battery_id_valid();
	else {
		pr_err("msm-charger no batt gauge batt=id_invalid\n");
		return 0;
	}
}

#ifdef F_MHL_AUTO_CONNECT  //20120319 PZ1949 from KBJ
static int is_mhl_cable(void)
{
	if (msm_batt_gauge && msm_batt_gauge->is_mhl_cable)
		return msm_batt_gauge->is_mhl_cable();
	else {
		pr_err("msm-charger no is_mhl_cable\n");
		return 0;
	}
}
#endif

#ifdef CONFIG_SKY_CHARGING  //kobj 110513
static int is_factory_cable(void)
{
	if (msm_batt_gauge && msm_batt_gauge->is_factory_cable)
		return msm_batt_gauge->is_factory_cable();
	else {
		pr_err("msm-charger no is_factory_cable\n");
		return 0;
	}
}
#endif  //CONFIG_SKY_CHARGING

static int get_prop_battery_mvolts(void)
{
	if (msm_batt_gauge && msm_batt_gauge->get_battery_mvolts)
		return msm_batt_gauge->get_battery_mvolts();
	else {
		pr_err("msm-charger no batt gauge assuming 3.5V\n");
		return MSM_CHARGER_GAUGE_MISSING_VOLTS;
	}
}
#ifdef CONFIG_SKY_CHARGING		//temp	// p14682 kobj 110816
static int get_battery_temperature(void)
{
	if (msm_batt_gauge && msm_batt_gauge->get_battery_temperature)
		return msm_batt_gauge->get_battery_temperature();
	else {
		pr_err("msm-charger no batt gauge assuming 35 deg G\n");
		return MSM_CHARGER_GAUGE_MISSING_TEMP;
	}
}
int max_for_fuel_temp = 35; //20120319 PZ1949 from KBJ
int Presto_get_batt_temp(void)				// p14682 kobj 110915
{
#if 0 //20120319 PZ1949 from KBJ return value changed 
	return get_battery_temperature();
#else
	if(max_for_fuel_temp>=0&&max_for_fuel_temp<80)
	{
		return max_for_fuel_temp;
	}
	else
	{
		return MSM_CHARGER_GAUGE_MISSING_TEMP;
	}
#endif 
}
EXPORT_SYMBOL(Presto_get_batt_temp);

#endif //CONFIG_SKY_CHARGING

#ifndef CONFIG_SKY_BATTERY_MAX17040
static int get_prop_batt_capacity(void)
{
	//int capacity;

	if (msm_batt_gauge && msm_batt_gauge->get_batt_remaining_capacity)
#if 1 //20120319 PZ1949 from KBJ changed 
		return msm_batt_gauge->get_batt_remaining_capacity();

	return msm_chg.get_batt_capacity_percent();	
#else
	capacity = msm_batt_gauge->get_batt_remaining_capacity();
	else
		capacity = msm_chg.get_batt_capacity_percent();

	if (capacity <= 10)
		pr_err("battery capacity very low = %d\n", capacity);

	return capacity;
#endif
}
#endif  //CONFIG_SKY_BATTERY_MAX17040

static int get_prop_batt_health(void)
{
	int status = 0;

	if (msm_chg.batt_status == BATT_STATUS_TEMPERATURE_OUT_OF_RANGE)
		status = POWER_SUPPLY_HEALTH_OVERHEAT;
	else
		status = POWER_SUPPLY_HEALTH_GOOD;

	return status;
}

static int get_prop_charge_type(void)
{
	int status = 0;

	if (msm_chg.batt_status == BATT_STATUS_TRKL_CHARGING)
		status = POWER_SUPPLY_CHARGE_TYPE_TRICKLE;
	else if (msm_chg.batt_status == BATT_STATUS_FAST_CHARGING)
		status = POWER_SUPPLY_CHARGE_TYPE_FAST;
	else
		status = POWER_SUPPLY_CHARGE_TYPE_NONE;

	return status;
}

static int get_prop_batt_status(void)
{
	int status = 0;
#ifdef CONFIG_SKY_BATTERY_MAX17040
	int state = 0;
#endif
	if (msm_batt_gauge && msm_batt_gauge->get_battery_status) {
		status = msm_batt_gauge->get_battery_status();
		if (status == POWER_SUPPLY_STATUS_CHARGING ||
			status == POWER_SUPPLY_STATUS_FULL ||
			status == POWER_SUPPLY_STATUS_DISCHARGING)
			return status;
	}

#ifdef CONFIG_SKY_BATTERY_MAX17040
        if (is_batt_status_charging())
        {
                state=max17040_get_charge_state();
                if(state)
                        status = POWER_SUPPLY_STATUS_FULL;
                else
                        status = POWER_SUPPLY_STATUS_CHARGING;				
        }
        else if (msm_chg.batt_status ==
                BATT_STATUS_JUST_FINISHED_CHARGING
                && msm_chg.current_chg_priv != NULL)
        {
                state=max17040_get_charge_state();
                if(state)
                        status = POWER_SUPPLY_STATUS_FULL;
                else
                        status = POWER_SUPPLY_STATUS_CHARGING;				
        }
        else
                status = POWER_SUPPLY_STATUS_DISCHARGING;
#else
	if (is_batt_status_charging())
		status = POWER_SUPPLY_STATUS_CHARGING;
	else if (msm_chg.batt_status ==
		 BATT_STATUS_JUST_FINISHED_CHARGING
			 && msm_chg.current_chg_priv != NULL)
		status = POWER_SUPPLY_STATUS_FULL;
	else
		status = POWER_SUPPLY_STATUS_DISCHARGING;
#endif

	return status;
}

 /* This function should only be called within handle_event or resume */
static void update_batt_status(void)
{
	if (is_battery_present()) {
#ifdef CONFIG_SKY_CHARGING			// p14682 kobj 110621
        	if (msm_chg.batt_status == BATT_STATUS_ABSENT
        		|| msm_chg.batt_status
        			== BATT_STATUS_ID_INVALID) {
        		msm_chg.batt_status = BATT_STATUS_DISCHARGING;
                }
#else	//CONFIG_SKY_CHARGING	
		if (is_battery_id_valid()) {
			if (msm_chg.batt_status == BATT_STATUS_ABSENT
				|| msm_chg.batt_status
					== BATT_STATUS_ID_INVALID) {
				msm_chg.batt_status = BATT_STATUS_DISCHARGING;
			}
		} else
			msm_chg.batt_status = BATT_STATUS_ID_INVALID;
#endif 	//CONFIG_SKY_CHARGING	
	 } else
		msm_chg.batt_status = BATT_STATUS_ABSENT;
}

static enum power_supply_property msm_power_props[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
#ifdef CONFIG_SKY_CHARGING  //p14682 kobj 110816
	POWER_SUPPLY_PROP_TEMP,
#endif 	
};

static char *msm_power_supplied_to[] = {
	"battery",
};
//pz1946 20111108 position change here
static struct msm_hardware_charger_priv *usb_hw_chg_priv;

static int msm_power_get_property(struct power_supply *psy,
				  enum power_supply_property psp,
				  union power_supply_propval *val)
{
	struct msm_hardware_charger_priv *priv;

	priv = container_of(psy, struct msm_hardware_charger_priv, psy);
	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = !(priv->hw_chg_state == CHG_ABSENT_STATE);
		break;
	case POWER_SUPPLY_PROP_ONLINE:
#ifdef CONFIG_SKY_CHARGING  //kobj 110513
                if(msm_chg.charger_type == CHG_TYPE_USB)
                        val->intval = 1;
                else if(msm_chg.charger_type == CHG_TYPE_AC)
                        val->intval = 2;
                else if(msm_chg.charger_type == CHG_TYPE_FACTORY)
                        val->intval = 3;
                else
            	{
                        val->intval = 0;
					//	pr_info("[SKY CHG kobj] msm_power_get_property connect =%d\n", val->intval);

            	}

#else  //CONFIG_SKY_CHARGING
#if 0 //pz1946 debug
		val->intval = (priv->hw_chg_state == CHG_READY_STATE)
			|| (priv->hw_chg_state == CHG_CHARGING_STATE);
#else
	if(smb_charger_state == CHG_TYPE_USB) {
		val -> intval = 1;
	}
	else if(smb_charger_state == CHG_TYPE_AC) {
		val -> intval = 2;
	}
	else if(smb_charger_state == CHG_TYPE_FACTORY) {
		val -> intval = 3;
	}
	else {
		val -> intval = 0;
	}
#endif
#endif  //CONFIG_SKY_CHARGING
		break;
#ifdef CONFIG_SKY_CHARGING  //p14682 kobj 110816
	case POWER_SUPPLY_PROP_TEMP:
#if 0   //20120319 PZ1949 from KBJ  changed 
		val->intval = (get_battery_temperature()-0)*10;
#else
		val->intval = 0 ;
#endif 
		break;
#endif 
	default:
		return -EINVAL;
	}
	return 0;
}

static enum power_supply_property msm_batt_power_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN,
#ifndef CONFIG_SKY_BATTERY_MAX17040
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
#endif	//CONFIG_SKY_BATTERY_MAX17040
};

static int msm_batt_power_get_property(struct power_supply *psy,
				       enum power_supply_property psp,
				       union power_supply_propval *val)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = get_prop_batt_status();
		break;
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		val->intval = get_prop_charge_type();
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = get_prop_batt_health();
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = !(msm_chg.batt_status == BATT_STATUS_ABSENT);
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
#ifdef SKY_BATTERY_INFO	//20120319 PZ1949 from KBJ	
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
#else
		val->intval = POWER_SUPPLY_TECHNOLOGY_NiMH;
#endif
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN:
		val->intval = msm_chg.max_voltage * 1000;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN:
		val->intval = msm_chg.min_voltage * 1000;
		break;
#ifndef CONFIG_SKY_BATTERY_MAX17040
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = get_prop_battery_mvolts();
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = get_prop_batt_capacity();
		//20110323 choiseulkee chg for EF39S bring-up, please check Battery
		val->intval = 100;
		break;
#endif  //CONFIG_SKY_BATTERY_MAX17040
	default:
		return -EINVAL;
	}
	return 0;
}

static struct power_supply msm_psy_batt = {
	.name = "battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = msm_batt_power_props,
	.num_properties = ARRAY_SIZE(msm_batt_power_props),
	.get_property = msm_batt_power_get_property,
};

static int usb_chg_current;
static struct msm_hardware_charger_priv *usb_hw_chg_priv;
static void (*notify_vbus_state_func_ptr)(int);
static int usb_notified_of_insertion;

/* this is passed to the hsusb via platform_data msm_otg_pdata */
int msm_charger_register_vbus_sn(void (*callback)(int))
{
	pr_debug(KERN_INFO "%s\n", __func__);
	notify_vbus_state_func_ptr = callback;
	return 0;
}

/* this is passed to the hsusb via platform_data msm_otg_pdata */
void msm_charger_unregister_vbus_sn(void (*callback)(int))
{
	pr_debug(KERN_INFO "%s\n", __func__);
	notify_vbus_state_func_ptr = NULL;
}

#ifdef CONFIG_SKY_CHARGING  //kobj 110513
unsigned int hold_mA;

static void msm_charge_called_before_init(unsigned int mA)
{
        pr_info("[SKY CHG] msm_charge_called_before_init with mA =%d\n",hold_mA);

        if(hold_mA == 500)
                msm_chg.charger_type = CHG_TYPE_USB;
#ifdef CONFIG_SKY_CHARGING  // p14682 kobj 110816
        else if(hold_mA == 700/*900*/)  // p14682 kobj 110816
#else
        else if(hold_mA == 900)  
#endif 
                msm_chg.charger_type = CHG_TYPE_AC;
        else
                return;

        usb_hw_chg_priv->max_source_current = hold_mA;
        msm_charger_notify_event(usb_hw_chg_priv->hw_chg,
                CHG_ENUMERATED_EVENT);
}
#endif  //CONFIG_SKY_BATTERY_MAX17040

static void notify_usb_of_the_plugin_event(struct msm_hardware_charger_priv
					   *hw_chg, int plugin)
{
	plugin = !!plugin;
	if (plugin == 1 && usb_notified_of_insertion == 0) {
		usb_notified_of_insertion = 1;
		if (notify_vbus_state_func_ptr) {
			dev_dbg(msm_chg.dev, "%s notifying plugin\n", __func__);
			(*notify_vbus_state_func_ptr) (plugin);
		} else
			dev_dbg(msm_chg.dev, "%s unable to notify plugin\n",
				__func__);
		usb_hw_chg_priv = hw_chg;
	}
	if (plugin == 0 && usb_notified_of_insertion == 1) {
		if (notify_vbus_state_func_ptr) {
			dev_dbg(msm_chg.dev, "%s notifying unplugin\n",
				__func__);
			(*notify_vbus_state_func_ptr) (plugin);
		} else
			dev_dbg(msm_chg.dev, "%s unable to notify unplugin\n",
				__func__);
		usb_notified_of_insertion = 0;
		usb_hw_chg_priv = NULL;
	}

#ifdef CONFIG_SKY_CHARGING  //kobj 110513
        if(hold_mA && plugin)
        {
                msm_charge_called_before_init(hold_mA);
                hold_mA = 0;
        }
#endif    //CONFIG_SKY_BATTERY_MAX17040
}

static unsigned int msm_chg_get_batt_capacity_percent(void)
{
	unsigned int current_voltage = get_prop_battery_mvolts();
	unsigned int low_voltage = msm_chg.min_voltage;
	unsigned int high_voltage = msm_chg.max_voltage;

	if (current_voltage <= low_voltage)
		return 0;
	else if (current_voltage >= high_voltage)
		return 100;
	else
		return (current_voltage - low_voltage) * 100
		    / (high_voltage - low_voltage);
}

#ifdef DEBUG
static inline void debug_print(const char *func,
			       struct msm_hardware_charger_priv *hw_chg_priv)
{
	dev_dbg(msm_chg.dev,
		"%s current=(%s)(s=%d)(r=%d) new=(%s)(s=%d)(r=%d) batt=%d En\n",
		func,
		msm_chg.current_chg_priv ? msm_chg.current_chg_priv->
		hw_chg->name : "none",
		msm_chg.current_chg_priv ? msm_chg.
		current_chg_priv->hw_chg_state : -1,
		msm_chg.current_chg_priv ? msm_chg.current_chg_priv->
		hw_chg->rating : -1,
		hw_chg_priv ? hw_chg_priv->hw_chg->name : "none",
		hw_chg_priv ? hw_chg_priv->hw_chg_state : -1,
		hw_chg_priv ? hw_chg_priv->hw_chg->rating : -1,
		msm_chg.batt_status);
}
#else
static inline void debug_print(const char *func,
			       struct msm_hardware_charger_priv *hw_chg_priv)
{
}
#endif

static struct msm_hardware_charger_priv *find_best_charger(void)
{
	struct msm_hardware_charger_priv *hw_chg_priv;
	struct msm_hardware_charger_priv *better;
	int rating;

	better = NULL;
	rating = 0;

	list_for_each_entry(hw_chg_priv, &msm_chg.msm_hardware_chargers, list) {
		if (is_chg_capable_of_charging(hw_chg_priv)) {
			if (hw_chg_priv->hw_chg->rating > rating) {
				rating = hw_chg_priv->hw_chg->rating;
				better = hw_chg_priv;
			}
		}
	}

	return better;
}

static int msm_charging_switched(struct msm_hardware_charger_priv *priv)
{
	int ret = 0;

	if (priv->hw_chg->charging_switched)
		ret = priv->hw_chg->charging_switched(priv->hw_chg);
	return ret;
}

static int msm_stop_charging(struct msm_hardware_charger_priv *priv)
{
	int ret;

	ret = priv->hw_chg->stop_charging(priv->hw_chg);
#ifndef CONFIG_SKY_CHARGING  	//20120319 PZ1949 from KBJ		
	if (!ret)
		wake_unlock(&msm_chg.wl);
#endif 	
	return ret;
}

static void msm_enable_system_current(struct msm_hardware_charger_priv *priv)
{
	if (priv->hw_chg->start_system_current)
		priv->hw_chg->start_system_current(priv->hw_chg,
					 priv->max_source_current);
}

static void msm_disable_system_current(struct msm_hardware_charger_priv *priv)
{
	if (priv->hw_chg->stop_system_current)
		priv->hw_chg->stop_system_current(priv->hw_chg);
}

/* the best charger has been selected -start charging from current_chg_priv */
static int msm_start_charging(void)
{
	int ret;
	struct msm_hardware_charger_priv *priv;

#ifdef CONFIG_SKY_CHARGING  //kobj 110513		
			//-- is_charger_done = 0;
			msm_charger_set_init_done(); //20120319 PZ1949 from KBJ
#endif

	priv = msm_chg.current_chg_priv;
#ifndef CONFIG_SKY_CHARGING  	//20120319 PZ1949 from KBJ	
	wake_lock(&msm_chg.wl);
#endif 
	ret = priv->hw_chg->start_charging(priv->hw_chg, msm_chg.max_voltage,
					 priv->max_source_current);
	if (ret) {
#ifndef CONFIG_SKY_CHARGING  				
		wake_unlock(&msm_chg.wl);
#endif 
		dev_err(msm_chg.dev, "%s couldnt start chg error = %d\n",
			priv->hw_chg->name, ret);
	} else
	{
#ifdef __DEBUG_KOBJ__			
		dev_err(msm_chg.dev, "[msm_charger] msm_start_charging():%d hw_chg_state:CHG_CHARGING_STATE\n",ret);	
#endif 
		priv->hw_chg_state = CHG_CHARGING_STATE;
	}//20120319 PZ1949 from KBJ
	return ret;
}

static void handle_charging_done(struct msm_hardware_charger_priv *priv)
{
	if (msm_chg.current_chg_priv == priv) {
		if (msm_chg.current_chg_priv->hw_chg_state ==
		    CHG_CHARGING_STATE)
			if (msm_stop_charging(msm_chg.current_chg_priv)) {
				dev_err(msm_chg.dev, "%s couldnt stop chg\n",
					msm_chg.current_chg_priv->hw_chg->name);
#ifdef __DEBUG_KOBJ__						
				dev_err(msm_chg.dev, "[msm_charger]handle_charging_done CHG_READY_STATE");				
#endif 
			}
		msm_chg.current_chg_priv->hw_chg_state = CHG_READY_STATE;

		msm_chg.batt_status = BATT_STATUS_JUST_FINISHED_CHARGING;
		dev_info(msm_chg.dev, "%s: stopping safety timer work\n",
				__func__);
		cancel_delayed_work(&msm_chg.teoc_work);

		if (msm_batt_gauge && msm_batt_gauge->monitor_for_recharging)
			msm_batt_gauge->monitor_for_recharging();
		else
			dev_err(msm_chg.dev,
			      "%s: no batt gauge recharge monitor\n", __func__);
	}
}

static void teoc(struct work_struct *work)
{
	/* we have been charging too long - stop charging */
	dev_info(msm_chg.dev, "%s: safety timer work expired\n", __func__);

	mutex_lock(&msm_chg.status_lock);
	if (msm_chg.current_chg_priv != NULL
	    && msm_chg.current_chg_priv->hw_chg_state == CHG_CHARGING_STATE) {
		handle_charging_done(msm_chg.current_chg_priv);
	}
	mutex_unlock(&msm_chg.status_lock);
}

static void handle_battery_inserted(void)
{
	/* if a charger is already present start charging */
	if (msm_chg.current_chg_priv != NULL &&
	    is_batt_status_capable_of_charging() &&
	    !is_batt_status_charging()) {
		if (msm_start_charging()) {
			dev_err(msm_chg.dev, "%s couldnt start chg\n",
				msm_chg.current_chg_priv->hw_chg->name);
			return;
		}
		msm_chg.batt_status = BATT_STATUS_TRKL_CHARGING;

		dev_info(msm_chg.dev, "%s: starting safety timer work\n",
				__func__);
		queue_delayed_work(msm_chg.event_wq_thread,
					&msm_chg.teoc_work,
				      round_jiffies_relative(msecs_to_jiffies
							     (msm_chg.
							      safety_time)));
	}
#ifdef CONFIG_SKY_CHARGING  //kobj 110513
	if (msm_chg.current_chg_priv == NULL) {
                msm_chg.batt_status = BATT_STATUS_DISCHARGING;
        }
#endif  //CONFIG_SKY_CHARGING
}

static void handle_battery_removed(void)
{
	/* if a charger is charging the battery stop it */
	if (msm_chg.current_chg_priv != NULL
	    && msm_chg.current_chg_priv->hw_chg_state == CHG_CHARGING_STATE) {
		if (msm_stop_charging(msm_chg.current_chg_priv)) {
			dev_err(msm_chg.dev, "%s couldnt stop chg\n",
				msm_chg.current_chg_priv->hw_chg->name);
		}
#ifdef __DEBUG_KOBJ__				
		dev_err(msm_chg.dev, "[---------->]handle_battery_removed CHG_READY_STATE");			
#endif 
		msm_chg.current_chg_priv->hw_chg_state = CHG_READY_STATE;

		dev_info(msm_chg.dev, "%s: stopping safety timer work\n",
				__func__);
		cancel_delayed_work(&msm_chg.teoc_work);
	}
}
#ifdef CONFIG_SKY_CHARGING  //20120319 PZ1949 from KBJ
extern void pm8058_set_out_range_temp(void);
extern void pm8058_set_in_range_temp(void);
extern int get_udc_state(void);
extern int pm8058_chg_get_current(void);
#endif 
static void update_heartbeat(struct work_struct *work)
{
#ifdef CONFIG_SKY_CHARGING
	int temperature;
	static int set_current_check = 0; //20120319 PZ1949 from KBJ
	static int temp_cnt = 0; //20120319 PZ1949 from KBJ
#endif

	if (msm_chg.batt_status == BATT_STATUS_ABSENT
		|| msm_chg.batt_status == BATT_STATUS_ID_INVALID) {
		if (is_battery_present())
#ifndef CONFIG_SKY_CHARGING	// p14682 kobj 110620 			
			if (is_battery_id_valid()) 
#endif //CONFIG_SKY_CHARGING
			{
				msm_chg.batt_status = BATT_STATUS_DISCHARGING;
                pr_info("[update_heartbeat()] batt is valid so ......handle_battery_inserted()\n");//PZ1949 
				handle_battery_inserted();
			}
	} else {
		if (!is_battery_present()) {
			msm_chg.batt_status = BATT_STATUS_ABSENT;
            pr_info("[update_heartbeat()] batt no present so ......handle_battery_removed()\n");//PZ1949 
			handle_battery_removed();
		}
		/*
		 * check battery id because a good battery could be removed
		 * and replaced with a invalid battery.
		 */
		if (!is_battery_id_valid()) {
			msm_chg.batt_status = BATT_STATUS_ID_INVALID;
            pr_info("[update_heartbeat()] batt is invalid so ....handle_battery_removed()\n");//PZ1949 
			handle_battery_removed();
		}
	}
	pr_debug("msm-charger %s batt_status= %d\n",
				__func__, msm_chg.batt_status);
		
#ifdef CONFIG_SKY_CHARGING	//20120319 PZ1949 from KBJ  // p14682 kobj 110816	
	temperature = get_battery_temperature();
	max_for_fuel_temp = temperature; 
	
	pr_info("[SKY CHG][msm-charger] %s batt_status(%s)  charg_type(%s) batt_mvolts(%d) batt_temp(%d)\n",
					__func__, chargingFlagStatus(msm_chg.batt_status),chargingFlagCHGType(msm_chg.charger_type), 
                    get_prop_battery_mvolts(), temperature); 

	//pr_info("[SKY CHG]msm-charger %s charger_type= %d, get_battery_temperature = %d\n",
	//				__func__, msm_chg.charger_type, temperature);//get_battery_mvolts());	// kobj 110513
#ifdef __DEBUG_KOBJ__					
	if (msm_chg.current_chg_priv)
	{
		pr_info("[SKY CHG][msm-charger] %s hw_chg_state = %s\n",
						__func__, chargingFlagHwChgState(msm_chg.current_chg_priv->hw_chg_state));//get_battery_mvolts());	// kobj 110513

	}
#endif 
	if (msm_chg.current_chg_priv
		&& msm_chg.current_chg_priv->hw_chg_state
			== CHG_CHARGING_STATE) {
#if 0		
//		if(temperature >= 48)
//		{
//			temp_cnt++;
//			if(temp_cnt>3)
//			{
//				pm8058_set_out_range_temp();
//				temp_cnt = 0;
//			}
//		}
//		else 
//		{
//			temp_cnt = 0;
//		}
//		/* TODO implement JEITA SPEC*/
#else
				if(msm_chg.charger_type == CHG_TYPE_AC)			// ¹ß¿­·Î ÀÎÇÑ Ã³¸® 
				{
#ifdef __DEBUG_KOBJ__			
					pr_info("[SKY CHG][msm-charger] %s CHG_TYPE_AC pm8058_chg_get_current= %d mA, set_current_check = %d\n",
									__func__, pm8058_chg_get_current(), set_current_check);			
#endif
					if(temperature>=50&&temperature<=55)
					{
						if(pm8058_chg_get_current() == 700)
						{
							pm8058_chg_set_current_incall(500); 	
							set_current_check = 1;
						}
					}
					else if(temperature<=49)
					{
						if(set_current_check)
						{
							if(pm8058_chg_get_current() == 500)
							{
								pm8058_chg_set_current_incall(700); 	
								set_current_check = 0;
							}
						}
					}
#ifdef __DEBUG_KOBJ__					
					pr_info("[SKY CHG][msm-charger] %s CHG_TYPE_AC pm8058_chg_get_current= %d mA, set_current_check = %d\n",
									__func__, pm8058_chg_get_current(), set_current_check);	
#endif 

				}
				else if(msm_chg.charger_type == CHG_TYPE_USB)	// Â÷·®¿ë ÃæÀü±â·Î ÀÎÇ× Ã³¸® 
				{
#ifdef __DEBUG_KOBJ__				
					pr_info("[SKY CHG][msm-charger] %s CHG_TYPE_USB pm8058_chg_get_current= %d mA\n",
									__func__, pm8058_chg_get_current());				
#endif 
					if(!get_udc_state())
					{
#ifdef __DEBUG_KOBJ__					
					pr_info("[SKY CHG][msm-charger] %s CHG_TYPE_USB get_udc_state= %d\n",
									__func__, get_udc_state());									
#endif 					
						pm8058_chg_set_current_incall(700); 
						msm_chg.charger_type = CHG_TYPE_AC;
					}
#ifdef __DEBUG_KOBJ__					
					pr_info("[SKY CHG][msm-charger] %s CHG_TYPE_USB pm8058_chg_get_current= %d mA\n",
									__func__, pm8058_chg_get_current());									
#endif 
				}
#endif 
	}
	else
	{
		if(msm_chg.batt_status==BATT_STATUS_TEMPERATURE_OUT_OF_RANGE)
		{
#if 0
//			if(temperature <= 41)
#else
			if(temperature <= 51)
#endif 
			{
				temp_cnt++;
				if(temp_cnt>3)
				{
					pm8058_set_in_range_temp();
					temp_cnt = 0;
				}
			 }
			else
			{
				temp_cnt = 0;
			}
		}
	}
#endif
//] 20120319 PZ1949 from KBJ
	/* notify that the voltage has changed
	 * the read of the capacity will trigger a
	 * voltage read*/
	power_supply_changed(&msm_psy_batt);

	if (msm_chg.stop_update) {
		msm_chg.stop_update = 0;
		return;
	}
	queue_delayed_work(msm_chg.event_wq_thread,
				&msm_chg.update_heartbeat_work,
			      round_jiffies_relative(msecs_to_jiffies
						     (msm_chg.update_time)));
}

/* set the charger state to READY before calling this */
static void handle_charger_ready(struct msm_hardware_charger_priv *hw_chg_priv)
{
	struct msm_hardware_charger_priv *old_chg_priv = NULL;

	debug_print(__func__, hw_chg_priv);

	if (msm_chg.current_chg_priv != NULL
	    && hw_chg_priv->hw_chg->rating >
	    msm_chg.current_chg_priv->hw_chg->rating) {
		/*
		 * a better charger was found, ask the current charger
		 * to stop charging if it was charging
		 */
		if (msm_chg.current_chg_priv->hw_chg_state ==
		    CHG_CHARGING_STATE) {
			if (msm_stop_charging(msm_chg.current_chg_priv)) {
				dev_err(msm_chg.dev, "%s couldnt stop chg\n",
					msm_chg.current_chg_priv->hw_chg->name);
				return;
			}
			if (msm_charging_switched(msm_chg.current_chg_priv)) {
				dev_err(msm_chg.dev, "%s couldnt stop chg\n",
					msm_chg.current_chg_priv->hw_chg->name);
				return;
			}
		}
#ifdef __DEBUG_KOBJ__		
		dev_err(msm_chg.dev, "[------------------>]handle_charger_ready CHG_READY_STATE");			
#endif 
		msm_chg.current_chg_priv->hw_chg_state = CHG_READY_STATE;
		old_chg_priv = msm_chg.current_chg_priv;
		msm_chg.current_chg_priv = NULL;
	}

	if (msm_chg.current_chg_priv == NULL) {
		msm_chg.current_chg_priv = hw_chg_priv;
		dev_info(msm_chg.dev,
			 "%s: best charger = %s\n", __func__,
			 msm_chg.current_chg_priv->hw_chg->name);
#ifdef CONFIG_SKY_CHARGING		 
		if(get_prop_battery_mvolts()>MSM_CHARGER_GAUGE_MISSING_VOLTS)
		msm_enable_system_current(msm_chg.current_chg_priv);
#else
		msm_enable_system_current(msm_chg.current_chg_priv);
#endif 
		/*
		 * since a better charger was chosen, ask the old
		 * charger to stop providing system current
		 */
#ifdef CONFIG_SKY_CHARGING		 
		if ((old_chg_priv != NULL)&&(get_prop_battery_mvolts()>MSM_CHARGER_GAUGE_MISSING_VOLTS))
			msm_disable_system_current(old_chg_priv);
#else
		if (old_chg_priv != NULL)
			msm_disable_system_current(old_chg_priv);
#endif 
		if (!is_batt_status_capable_of_charging())
			return;

		/* start charging from the new charger */
		if (!msm_start_charging()) {
			/* if we simply switched chg continue with teoc timer
			 * else we update the batt state and set the teoc
			 * timer */
			if (!is_batt_status_charging()) {
				dev_info(msm_chg.dev,
				       "%s: starting safety timer\n", __func__);
				queue_delayed_work(msm_chg.event_wq_thread,
							&msm_chg.teoc_work,
						      round_jiffies_relative
						      (msecs_to_jiffies
						       (msm_chg.safety_time)));
				msm_chg.batt_status = BATT_STATUS_TRKL_CHARGING;
			}
		} else {
			/* we couldnt start charging from the new readied
			 * charger */
			if (is_batt_status_charging())
            {
				msm_chg.batt_status = BATT_STATUS_DISCHARGING;
            }
		}
	}
#ifdef CONFIG_SKY_CHARGING	//20120319 PZ1949 from KBJ
	else
	{
		if(msm_chg.current_chg_priv->hw_chg_state == CHG_READY_STATE)
		{
#ifdef __DEBUG_KOBJ__				
			dev_err(msm_chg.dev, "[-------->]handle_charger_ready CHG_CHARGING_STATE");	
#endif 
			msm_chg.current_chg_priv->hw_chg_state = CHG_CHARGING_STATE;
		}
	}
#endif
}

static void handle_charger_removed(struct msm_hardware_charger_priv
				   *hw_chg_removed, int new_state)
{
	struct msm_hardware_charger_priv *hw_chg_priv;

	debug_print(__func__, hw_chg_removed);

	if (msm_chg.current_chg_priv == hw_chg_removed) {
#ifdef CONFIG_SKY_CHARGING	
		if(get_prop_battery_mvolts()>MSM_CHARGER_GAUGE_MISSING_VOLTS)
			msm_disable_system_current(hw_chg_removed);
#else
		msm_disable_system_current(hw_chg_removed);
#endif 
		if (msm_chg.current_chg_priv->hw_chg_state
						== CHG_CHARGING_STATE) {
			if (msm_stop_charging(hw_chg_removed)) {
				dev_err(msm_chg.dev, "%s couldnt stop chg\n",
					msm_chg.current_chg_priv->hw_chg->name);
			}
		}
		msm_chg.current_chg_priv = NULL;
	}

	hw_chg_removed->hw_chg_state = new_state;

	if (msm_chg.current_chg_priv == NULL) {
		hw_chg_priv = find_best_charger();
		if (hw_chg_priv == NULL) {
			dev_info(msm_chg.dev, "%s: no chargers\n", __func__);
			/* if the battery was Just finished charging
			 * we keep that state as is so that we dont rush
			 * in to charging the battery when a charger is
			 * plugged in shortly. */
			if (is_batt_status_charging())
				msm_chg.batt_status = BATT_STATUS_DISCHARGING;
		} else {
			msm_chg.current_chg_priv = hw_chg_priv;
#ifdef CONFIG_SKY_CHARGING	
			if(get_prop_battery_mvolts()>MSM_CHARGER_GAUGE_MISSING_VOLTS)
				msm_enable_system_current(hw_chg_priv);
#else
			msm_enable_system_current(hw_chg_priv);
#endif 
			dev_info(msm_chg.dev,
				 "%s: best charger = %s\n", __func__,
				 msm_chg.current_chg_priv->hw_chg->name);

			if (!is_batt_status_capable_of_charging())
				return;

			if (msm_start_charging()) {
				/* we couldnt start charging for some reason */
				msm_chg.batt_status = BATT_STATUS_DISCHARGING;
			}
		}
	}

	/* if we arent charging stop the safety timer */
	if (!is_batt_status_charging()) {
		dev_info(msm_chg.dev, "%s: stopping safety timer work\n",
				__func__);
		cancel_delayed_work(&msm_chg.teoc_work);
	}
}

static void handle_event(struct msm_hardware_charger *hw_chg, int event)
{
	struct msm_hardware_charger_priv *priv = NULL;

	/*
	 * if hw_chg is NULL then this event comes from non-charger
	 * parties like battery gauge
	 */
	if (hw_chg)
		priv = hw_chg->charger_private;
#ifdef CONFIG_SKY_CHARGING  //kobj 110513
	//msm_chg.event_type = event;
	if(hw_chg)				// p14682 kobj add  110726
		dev_info(msm_chg.dev, "[SKY CHG]%s %d from %s\n", __func__, event, hw_chg->name);
#else  //CONFIG_SKY_CHARGING
	if(hw_chg)				// p14682 kobj add 110726
		dev_info(msm_chg.dev, "%s %d from %s\n", __func__, event, hw_chg->name);
#endif  //CONFIG_SKY_CHARGING
	mutex_lock(&msm_chg.status_lock);

	switch (event) {
	case CHG_INSERTED_EVENT:
#ifdef CONFIG_SKY_CHARGING //20120319 PZ1949 from KBJ
		wake_lock(&msm_chg.wl);
#endif 
		if (priv->hw_chg_state != CHG_ABSENT_STATE) {
			dev_info(msm_chg.dev,
				 "%s insertion detected when cbl present",
				 hw_chg->name);
			break;
		}
		update_batt_status();
	//pr_info("[~~~~pz1946~~~~] CHG_INSERTED_EVENT hw_chg->type =%d\n", hw_chg->type);
		if (hw_chg->type == CHG_TYPE_USB) {
#ifdef CONFIG_SKY_CHARGING /* default setting */  //kobj 110513
                        msm_chg.charger_type = CHG_TYPE_USB;
#endif //CONFIG_SKY_CHARGING
//pz1946 20111114 
                        //msm_chg.charger_type = CHG_TYPE_USB;
			priv->hw_chg_state = CHG_PRESENT_STATE;
			notify_usb_of_the_plugin_event(priv, 1);
			if (usb_chg_current) {
				priv->max_source_current = usb_chg_current;
				usb_chg_current = 0;
				/* usb has already indicated us to charge */
#ifdef __DEBUG_KOBJ__						
				dev_err(msm_chg.dev, "[------->]CHG_INSERTED_EVENT CHG_READY_STATE");					
#endif 
				priv->hw_chg_state = CHG_READY_STATE;
				handle_charger_ready(priv);
			}
		} else {
#ifdef __DEBUG_KOBJ__				
			dev_err(msm_chg.dev, "[------->]CHG_INSERTED_EVENT CHG_READY_STATE");			
#endif 
			priv->hw_chg_state = CHG_READY_STATE;
			handle_charger_ready(priv);
		}
		break;
	case CHG_ENUMERATED_EVENT:	/* only in USB types */
		if (priv->hw_chg_state == CHG_ABSENT_STATE) {
			dev_info(msm_chg.dev, "%s enum withuot presence\n",
				 hw_chg->name);
			break;
		}
		
	//pr_info("[~~~~pz1946~~~~] time check CHG_ENUMERATED_EVENT =%d\n", CHG_ENUMERATED_EVENT);
#ifdef F_MHL_AUTO_CONNECT //20120319 PZ1949 from KBJ
		if(is_mhl_cable())
		{
			mhl_power_ctrl(1);
#ifdef MHL_ON_TIMMING_CONTROL
			Auth_MHL_ON_Delay_Control();
#else			
			//MHL_En_Control(1) ;// switch-MHL
			MHL_On(1);
			MHL_En_Control(1) ;// switch-MHL
			MHL_Set_Cable_State(MHL_CABLE_CONNCET);
			//MHL_Cable_On(MHL_CABLE_CONNCET);			
			//MHL_On(1);
#endif			
			printk(KERN_ERR "[SKY_MHL]%s MHL cable Connect \n",__func__);			
		}
#endif
		
#ifdef CONFIG_SKY_CHARGING  //kobj 110513
                if(is_factory_cable())
                    msm_chg.charger_type = CHG_TYPE_FACTORY;
#endif //CONFIG_SKY_CHARGING  
		update_batt_status();
		dev_dbg(msm_chg.dev, "%s enum with %dmA to draw\n",
			 hw_chg->name, priv->max_source_current);
	//pr_info("[~~~~pz1946~~~~]  priv->max_source_current = %d\n", priv->max_source_current);
		if (priv->max_source_current == 0) {
			/* usb subsystem doesnt want us to draw
			 * charging current */
			/* act as if the charge is removed */
	//pr_info("[~~~~pz1946~~~~]  priv->hw_chg_state = %d\n", priv->hw_chg_state);
			if (priv->hw_chg_state != CHG_PRESENT_STATE)
				handle_charger_removed(priv, CHG_PRESENT_STATE);
		} else {
			if (priv->hw_chg_state != CHG_READY_STATE) {
#ifdef __DEBUG_KOBJ__						
				dev_err(msm_chg.dev, "[--------> CHG_ENUMERATED_EVENT CHG_READY_STATE");					
#endif 
				priv->hw_chg_state = CHG_READY_STATE;
				handle_charger_ready(priv);
			}
		}
		break;
	case CHG_REMOVED_EVENT:
		if (priv->hw_chg_state == CHG_ABSENT_STATE) {
			dev_info(msm_chg.dev, "%s cable already removed\n",
				 hw_chg->name);
			break;
		}
		update_batt_status();
	//pr_info("[~~~~pz1946~~~~] CHG_REMOVED_EVENT hw_chg->type=%d\n", hw_chg->type);
		if (hw_chg->type == CHG_TYPE_USB) {
			usb_chg_current = 0;
	//pr_info("[~~~~pz1946~~~~]  notify_usb_of_the_plugin_event\n");
			notify_usb_of_the_plugin_event(priv, 0);
		}
		handle_charger_removed(priv, CHG_ABSENT_STATE);
#ifdef CONFIG_SKY_CHARGING /* default setting */  //kobj 110513
                msm_chg.charger_type = CHG_TYPE_NONE;
		wake_unlock(&msm_chg.wl); //20120319 PZ1949 from KBJ
#endif  //CONFIG_SKY_CHARGING
	//pr_info("[~~~~pz1946~~~~] CHG_REMOVED_EVENT msm_chg.charger_type= %d\n", msm_chg.charger_type);
//pz1946 20111114 
               // msm_chg.charger_type = CHG_TYPE_NONE;
		break;
	case CHG_DONE_EVENT:
#ifdef CONFIG_SKY_CHARGING  //kobj 110513		
		is_charger_done = 1;
#endif
		if (priv->hw_chg_state == CHG_CHARGING_STATE)
			handle_charging_done(priv);
		break;
	case CHG_BATT_BEGIN_FAST_CHARGING:
		/* only update if we are TRKL charging */
		if (msm_chg.batt_status == BATT_STATUS_TRKL_CHARGING)
			msm_chg.batt_status = BATT_STATUS_FAST_CHARGING;
		break;
	case CHG_BATT_NEEDS_RECHARGING:
		msm_chg.batt_status = BATT_STATUS_DISCHARGING;
		handle_battery_inserted();
		priv = msm_chg.current_chg_priv;
		break;
	case CHG_BATT_TEMP_OUTOFRANGE:
		/* the batt_temp out of range can trigger
		 * when the battery is absent */
            //20120319 PZ1949 from KBJ
		dev_info(msm_chg.dev, "%s CHG_BATT_TEMP_OUTOFRANGE\n",
		hw_chg->name);
		if (!is_battery_present()
		    && msm_chg.batt_status != BATT_STATUS_ABSENT) {
			msm_chg.batt_status = BATT_STATUS_ABSENT;
			handle_battery_removed();
			break;
		}
		if (msm_chg.batt_status == BATT_STATUS_TEMPERATURE_OUT_OF_RANGE)
			break;
		msm_chg.batt_status = BATT_STATUS_TEMPERATURE_OUT_OF_RANGE;
		handle_battery_removed();
		break;
	case CHG_BATT_TEMP_INRANGE:
             //20120319 PZ1949 from KBJ
		dev_info(msm_chg.dev, "%s CHG_BATT_TEMP_INRANGE\n",
		hw_chg->name);		
		if (msm_chg.batt_status != BATT_STATUS_TEMPERATURE_OUT_OF_RANGE)
			break;
		msm_chg.batt_status = BATT_STATUS_ID_INVALID;
		/* check id */
		if (!is_battery_id_valid())
			break;
		/* assume that we are discharging from the battery
		 * and act as if the battery was inserted
		 * if a charger is present charging will be resumed */
		msm_chg.batt_status = BATT_STATUS_DISCHARGING;
		handle_battery_inserted();
		break;
	case CHG_BATT_INSERTED:
		if (msm_chg.batt_status != BATT_STATUS_ABSENT)
			break;
		/* debounce */
		if (!is_battery_present())
			break;
		msm_chg.batt_status = BATT_STATUS_ID_INVALID;
		if (!is_battery_id_valid())
			break;
		/* assume that we are discharging from the battery */
		msm_chg.batt_status = BATT_STATUS_DISCHARGING;
		/* check if a charger is present */
		handle_battery_inserted();
		break;
	case CHG_BATT_REMOVED:
//pz1946 20110904 battery remove issue
//#ifdef CONFIG_SKY_SMB_CHARGER
//pz1946 20110920 battery remove position change
#if 0//def CONFIG_SKY_SMB_CHARGER
		kernel_power_off();
		mdelay(500);
#endif
		if (msm_chg.batt_status == BATT_STATUS_ABSENT)
			break;
		/* debounce */
		if (is_battery_present())
			break;
#ifdef CONFIG_SKY_CHARGING  //kobj 110513
		if (msm_chg.batt_status == BATT_STATUS_DISCHARGING)
			break;
                mdelay(10);
		if (is_battery_present())
			break;
                mdelay(10);
		if (is_battery_present())
			break;
        	machine_power_off();
#endif  //CONFIG_SKY_CHARGING
		msm_chg.batt_status = BATT_STATUS_ABSENT;
		handle_battery_removed();
		break;
	case CHG_BATT_STATUS_CHANGE:
		/* TODO  battery SOC like battery-alarm/charging-full features
		can be added here for future improvement */
		break;
	}
	dev_dbg(msm_chg.dev, "%s %d done batt_status=%d\n", __func__,
		event, msm_chg.batt_status);

	/* update userspace */
	if (msm_batt_gauge)
		power_supply_changed(&msm_psy_batt);
	if (priv)
		power_supply_changed(&priv->psy);

	mutex_unlock(&msm_chg.status_lock);
}

static int msm_chg_dequeue_event(struct msm_charger_event **event)
{
	unsigned long flags;

	spin_lock_irqsave(&msm_chg.queue_lock, flags);
	if (msm_chg.queue_count == 0) {
		spin_unlock_irqrestore(&msm_chg.queue_lock, flags);
		return -EINVAL;
	}
	*event = &msm_chg.queue[msm_chg.head];
	msm_chg.head = (msm_chg.head + 1) % MSM_CHG_MAX_EVENTS;
	pr_debug("%s dequeueing %d\n", __func__, (*event)->event);
	msm_chg.queue_count--;
	spin_unlock_irqrestore(&msm_chg.queue_lock, flags);
	return 0;
}

static int msm_chg_enqueue_event(struct msm_hardware_charger *hw_chg,
			enum msm_hardware_charger_event event)
{
	unsigned long flags;

	spin_lock_irqsave(&msm_chg.queue_lock, flags);
	if (msm_chg.queue_count == MSM_CHG_MAX_EVENTS) {
		spin_unlock_irqrestore(&msm_chg.queue_lock, flags);
		pr_err("%s: queue full cannot enqueue %d\n",
				__func__, event);
		return -EAGAIN;
	}
	pr_debug("%s queueing %d\n", __func__, event);
	msm_chg.queue[msm_chg.tail].event = event;
	msm_chg.queue[msm_chg.tail].hw_chg = hw_chg;
	msm_chg.tail = (msm_chg.tail + 1)%MSM_CHG_MAX_EVENTS;
	msm_chg.queue_count++;
	spin_unlock_irqrestore(&msm_chg.queue_lock, flags);
	return 0;
}

static void process_events(struct work_struct *work)
{
	struct msm_charger_event *event;
	int rc;

	do {
		rc = msm_chg_dequeue_event(&event);
		if (!rc)
			handle_event(event->hw_chg, event->event);
	} while (!rc);
}

/* USB calls these to tell us how much charging current we should draw */
void msm_charger_vbus_draw(unsigned int mA)
{
	if (usb_hw_chg_priv) {
#ifdef CONFIG_SKY_CHARGING  //kobj 110513
                if(mA == 500)
                    msm_chg.charger_type = CHG_TYPE_USB;
                else
                    msm_chg.charger_type = CHG_TYPE_AC;
#endif //CONFIG_SKY_CHARGING                   
		usb_hw_chg_priv->max_source_current = mA;
		msm_charger_notify_event(usb_hw_chg_priv->hw_chg,
						CHG_ENUMERATED_EVENT);
	} else
		/* remember the current, to be used when charger is ready */
		usb_chg_current = mA;

#ifdef CONFIG_SKY_CHARGING  //kobj 110513
        if(!usb_hw_chg_priv && mA)
        {
                hold_mA = mA;
        }
#endif  //CONFIG_SKY_CHARGING
}

static int __init determine_initial_batt_status(void)
{
	int rc;
#if 0//def CONFIG_SKY_CHARGING			// p14682 kobj 110620	// kobj 110823 change used temp
	if (is_battery_present())
			msm_chg.batt_status = BATT_STATUS_DISCHARGING;
#else  //CONFIG_SKY_CHARGING
	if (is_battery_present())
    {
		if (is_battery_id_valid())
        {
			if (is_battery_temp_within_range())
            {
				msm_chg.batt_status = BATT_STATUS_DISCHARGING;
            }    
			else
            {
				msm_chg.batt_status
				    = BATT_STATUS_TEMPERATURE_OUT_OF_RANGE;
            }        
        }    
		else
			msm_chg.batt_status = BATT_STATUS_ID_INVALID;
#endif //CONFIG_SKY_CHARGING		
    }

	else
    {
		msm_chg.batt_status = BATT_STATUS_ABSENT;
    }    
	if (is_batt_status_capable_of_charging())
		handle_battery_inserted();
      //[ 20120319 PZ1949 from KBJ
	rc = power_supply_register(msm_chg.dev, &msm_psy_batt);
	if (rc < 0) {
		dev_err(msm_chg.dev, "%s: power_supply_register failed"
			" rc=%d\n", __func__, rc);
		return rc;
	}
      //] 20120319 PZ1949 from KBJ
	/* start updaing the battery powersupply every msm_chg.update_time
	 * milliseconds */
	queue_delayed_work(msm_chg.event_wq_thread,
				&msm_chg.update_heartbeat_work,
			      round_jiffies_relative(msecs_to_jiffies
						     (msm_chg.update_time)));

	pr_debug("%s:OK batt_status=%d\n", __func__, msm_chg.batt_status);
	return 0;
}

static int __devinit msm_charger_probe(struct platform_device *pdev)
{
	msm_chg.dev = &pdev->dev;
	if (pdev->dev.platform_data) {
		unsigned int milli_secs;

		struct msm_charger_platform_data *pdata
		    =
		    (struct msm_charger_platform_data *)pdev->dev.platform_data;

		milli_secs = pdata->safety_time * 60 * MSEC_PER_SEC;
		if (milli_secs > jiffies_to_msecs(MAX_JIFFY_OFFSET)) {
			dev_warn(&pdev->dev, "%s: safety time too large"
				 "%dms\n", __func__, milli_secs);
			milli_secs = jiffies_to_msecs(MAX_JIFFY_OFFSET);
		}
		msm_chg.safety_time = milli_secs;

		milli_secs = pdata->update_time * 60 * MSEC_PER_SEC;
		if (milli_secs > jiffies_to_msecs(MAX_JIFFY_OFFSET)) {
			dev_warn(&pdev->dev, "%s: safety time too large"
				 "%dms\n", __func__, milli_secs);
			milli_secs = jiffies_to_msecs(MAX_JIFFY_OFFSET);
		}
		msm_chg.update_time = milli_secs;

		msm_chg.max_voltage = pdata->max_voltage;
		msm_chg.min_voltage = pdata->min_voltage;
		msm_chg.get_batt_capacity_percent =
		    pdata->get_batt_capacity_percent;
	}
	if (msm_chg.safety_time == 0)
		msm_chg.safety_time = CHARGING_TEOC_MS;
	if (msm_chg.update_time == 0)
		msm_chg.update_time = UPDATE_TIME_MS;
	if (msm_chg.max_voltage == 0)
		msm_chg.max_voltage = DEFAULT_BATT_MAX_V;
	if (msm_chg.min_voltage == 0)
		msm_chg.min_voltage = DEFAULT_BATT_MIN_V;
	if (msm_chg.get_batt_capacity_percent == NULL)
		msm_chg.get_batt_capacity_percent =
		    msm_chg_get_batt_capacity_percent;

	mutex_init(&msm_chg.status_lock);
	INIT_DELAYED_WORK(&msm_chg.teoc_work, teoc);
	INIT_DELAYED_WORK(&msm_chg.update_heartbeat_work, update_heartbeat);

	wake_lock_init(&msm_chg.wl, WAKE_LOCK_SUSPEND, "msm_charger");
	return 0;
}

static int __devexit msm_charger_remove(struct platform_device *pdev)
{
	wake_lock_destroy(&msm_chg.wl);
	mutex_destroy(&msm_chg.status_lock);
	power_supply_unregister(&msm_psy_batt);
	return 0;
}

int msm_charger_notify_event(struct msm_hardware_charger *hw_chg,
			     enum msm_hardware_charger_event event)
{
	msm_chg_enqueue_event(hw_chg, event);
	queue_work(msm_chg.event_wq_thread, &msm_chg.queue_work);
	return 0;
}
EXPORT_SYMBOL(msm_charger_notify_event);

int msm_charger_register(struct msm_hardware_charger *hw_chg)
{
	struct msm_hardware_charger_priv *priv;
	int rc = 0;

	if (!msm_chg.inited) {
		pr_err("%s: msm_chg is NULL,Too early to register\n", __func__);
		return -EAGAIN;
	}

	if (hw_chg->start_charging == NULL
		|| hw_chg->stop_charging == NULL
		|| hw_chg->name == NULL
		|| hw_chg->rating == 0) {
		pr_err("%s: invalid hw_chg\n", __func__);
		return -EINVAL;
	}

	priv = kzalloc(sizeof *priv, GFP_KERNEL);
	if (priv == NULL) {
		dev_err(msm_chg.dev, "%s kzalloc failed\n", __func__);
		return -ENOMEM;
	}

	priv->psy.name = hw_chg->name;
	if (hw_chg->type == CHG_TYPE_USB)
		priv->psy.type = POWER_SUPPLY_TYPE_USB;
	else
		priv->psy.type = POWER_SUPPLY_TYPE_MAINS;

	priv->psy.supplied_to = msm_power_supplied_to;
	priv->psy.num_supplicants = ARRAY_SIZE(msm_power_supplied_to);
	priv->psy.properties = msm_power_props;
	priv->psy.num_properties = ARRAY_SIZE(msm_power_props);
	priv->psy.get_property = msm_power_get_property;

	rc = power_supply_register(NULL, &priv->psy);
	if (rc) {
		dev_err(msm_chg.dev, "%s power_supply_register failed\n",
			__func__);
		goto out;
	}

	priv->hw_chg = hw_chg;
	priv->hw_chg_state = CHG_ABSENT_STATE;
	INIT_LIST_HEAD(&priv->list);
	mutex_lock(&msm_chg.msm_hardware_chargers_lock);
	list_add_tail(&priv->list, &msm_chg.msm_hardware_chargers);
	mutex_unlock(&msm_chg.msm_hardware_chargers_lock);
	hw_chg->charger_private = (void *)priv;
	return 0;

out:
	kfree(priv);
	return rc;
}
EXPORT_SYMBOL(msm_charger_register);

void msm_battery_gauge_register(struct msm_battery_gauge *batt_gauge)
{
	//- int rc;
	if (msm_batt_gauge) {
		msm_batt_gauge = batt_gauge;
		pr_err("msm-charger %s multiple battery gauge called\n",
								__func__);
	} else {
       #if 0
		rc = power_supply_register(msm_chg.dev, &msm_psy_batt);
		if (rc < 0) {
			dev_err(msm_chg.dev, "%s: power_supply_register failed"
					" rc=%d\n", __func__, rc);
			return;
		}
       #endif 
		msm_batt_gauge = batt_gauge;
		determine_initial_batt_status();
	}
}
EXPORT_SYMBOL(msm_battery_gauge_register);

void msm_battery_gauge_unregister(struct msm_battery_gauge *batt_gauge)
{
	msm_batt_gauge = NULL;
}
EXPORT_SYMBOL(msm_battery_gauge_unregister);

int msm_charger_unregister(struct msm_hardware_charger *hw_chg)
{
	struct msm_hardware_charger_priv *priv;

	priv = (struct msm_hardware_charger_priv *)(hw_chg->charger_private);
	mutex_lock(&msm_chg.msm_hardware_chargers_lock);
	list_del(&priv->list);
	mutex_unlock(&msm_chg.msm_hardware_chargers_lock);
	power_supply_unregister(&priv->psy);
	kfree(priv);
	return 0;
}
EXPORT_SYMBOL(msm_charger_unregister);

static int msm_charger_suspend(struct device *dev)
{
	dev_dbg(msm_chg.dev, "%s suspended\n", __func__);
	msm_chg.stop_update = 1;
	cancel_delayed_work(&msm_chg.update_heartbeat_work);
	mutex_lock(&msm_chg.status_lock);
	handle_battery_removed();
	mutex_unlock(&msm_chg.status_lock);
	return 0;
}

static int msm_charger_resume(struct device *dev)
{
	dev_dbg(msm_chg.dev, "%s resumed\n", __func__);
	msm_chg.stop_update = 0;
	/* start updaing the battery powersupply every msm_chg.update_time
	 * milliseconds */
	queue_delayed_work(msm_chg.event_wq_thread,
				&msm_chg.update_heartbeat_work,
			      round_jiffies_relative(msecs_to_jiffies
						     (msm_chg.update_time)));
	mutex_lock(&msm_chg.status_lock);
	handle_battery_inserted();
	mutex_unlock(&msm_chg.status_lock);
	return 0;
}

static SIMPLE_DEV_PM_OPS(msm_charger_pm_ops,
		msm_charger_suspend, msm_charger_resume);

static struct platform_driver msm_charger_driver = {
	.probe = msm_charger_probe,
	.remove = __devexit_p(msm_charger_remove),
	.driver = {
		   .name = "msm-charger",
		   .owner = THIS_MODULE,
		   .pm = &msm_charger_pm_ops,
	},
};

static int __init msm_charger_init(void)
{
	int rc;

	INIT_LIST_HEAD(&msm_chg.msm_hardware_chargers);
	msm_chg.count_chargers = 0;
	mutex_init(&msm_chg.msm_hardware_chargers_lock);

	msm_chg.queue = kzalloc(sizeof(struct msm_charger_event)
				* MSM_CHG_MAX_EVENTS,
				GFP_KERNEL);
	if (!msm_chg.queue) {
		rc = -ENOMEM;
		goto out;
	}
	msm_chg.tail = 0;
	msm_chg.head = 0;
	spin_lock_init(&msm_chg.queue_lock);
	msm_chg.queue_count = 0;
	INIT_WORK(&msm_chg.queue_work, process_events);
	msm_chg.event_wq_thread = create_workqueue("msm_charger_eventd");
	if (!msm_chg.event_wq_thread) {
		rc = -ENOMEM;
		goto free_queue;
	}
	rc = platform_driver_register(&msm_charger_driver);
	if (rc < 0) {
		pr_err("%s: FAIL: platform_driver_register. rc = %d\n",
		       __func__, rc);
		goto destroy_wq_thread;
	}
	msm_chg.inited = 1;
	return 0;

destroy_wq_thread:
	destroy_workqueue(msm_chg.event_wq_thread);
free_queue:
	kfree(msm_chg.queue);
out:
	return rc;
}

static void __exit msm_charger_exit(void)
{
	flush_workqueue(msm_chg.event_wq_thread);
	destroy_workqueue(msm_chg.event_wq_thread);
	kfree(msm_chg.queue);
	platform_driver_unregister(&msm_charger_driver);
}

module_init(msm_charger_init);
module_exit(msm_charger_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Abhijeet Dharmapurikar <adharmap@codeaurora.org>");
MODULE_DESCRIPTION("Battery driver for Qualcomm MSM chipsets.");
MODULE_VERSION("1.0");
