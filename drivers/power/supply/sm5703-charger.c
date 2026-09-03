// SPDX-License-Identifier: GPL-2.0-only
/* Conservative charger support for the Silicon Mitus SM5703. */

#include <linux/bitfield.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

#define SM5703_REG_STATUS3		0x0a
#define SM5703_REG_CNTL			0x0c
#define SM5703_REG_VBUSCNTL		0x0d
#define SM5703_REG_CHGCNTL2		0x0f
#define SM5703_REG_CHGCNTL3		0x10
#define SM5703_REG_CHGCNTL4		0x11
#define SM5703_REG_CHGCNTL5		0x12
#define SM5703_REG_STATUS5		0x6b

#define SM5703_STATUS3_DONE_MASK	GENMASK(3, 2)
#define SM5703_STATUS5_VBUSOK		BIT(5)
#define SM5703_OPERATION_MODE_MASK	GENMASK(2, 0)
#define SM5703_OPERATION_MODE_CHARGING	5
#define SM5703_AUTOSTOP			BIT(7)
#define SM5703_TOPOFF_CURRENT_MASK	GENMASK(6, 3)
#define SM5703_AICLEN			BIT(7)

#define SM5703_USB_CURRENT_UA		450000
#define SM5703_FLOAT_VOLTAGE_UV		4300000
#define SM5703_POLL_INTERVAL_MS		5000

/* Register values follow the formulae in Samsung's SM5703 driver. */
#define SM5703_USB_CURRENT_REG		7
#define SM5703_FLOAT_VOLTAGE_REG	18
#define SM5703_TOPOFF_200MA_REG		4

struct sm5703_charger {
	struct regmap *regmap;
	struct gpio_desc *enable_gpio;
	struct power_supply *psy;
	struct delayed_work work;
	bool online;
};

static const enum power_supply_property sm5703_charger_properties[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE,
};

static int sm5703_charger_get_property(struct power_supply *psy,
				       enum power_supply_property property,
				       union power_supply_propval *val)
{
	struct sm5703_charger *charger = power_supply_get_drvdata(psy);
	unsigned int status;
	int ret;

	switch (property) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = charger->online;
		return 0;
	case POWER_SUPPLY_PROP_STATUS:
		if (!charger->online) {
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
			return 0;
		}

		ret = regmap_read(charger->regmap, SM5703_REG_STATUS3,
				  &status);
		if (ret)
			return ret;

		if (status & SM5703_STATUS3_DONE_MASK)
			val->intval = POWER_SUPPLY_STATUS_FULL;
		else
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
		return 0;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		val->intval = SM5703_USB_CURRENT_UA;
		return 0;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE:
		val->intval = SM5703_FLOAT_VOLTAGE_UV;
		return 0;
	default:
		return -EINVAL;
	}
}

static void sm5703_charger_update(struct work_struct *work)
{
	struct sm5703_charger *charger;
	unsigned int status;
	bool online;
	int ret;

	charger = container_of(to_delayed_work(work),
			       struct sm5703_charger, work);
	ret = regmap_read(charger->regmap, SM5703_REG_STATUS5, &status);
	if (ret)
		goto reschedule;

	online = status & SM5703_STATUS5_VBUSOK;
	if (online == charger->online)
		goto reschedule;

	charger->online = online;
	gpiod_set_value_cansleep(charger->enable_gpio, online);
	if (online)
		regmap_update_bits(charger->regmap, SM5703_REG_CNTL,
				   SM5703_OPERATION_MODE_MASK,
				   SM5703_OPERATION_MODE_CHARGING);
	power_supply_changed(charger->psy);

reschedule:
	schedule_delayed_work(&charger->work,
			      msecs_to_jiffies(SM5703_POLL_INTERVAL_MS));
}

static const struct power_supply_desc sm5703_charger_desc = {
	.name = "sm5703-charger",
	.type = POWER_SUPPLY_TYPE_USB,
	.properties = sm5703_charger_properties,
	.num_properties = ARRAY_SIZE(sm5703_charger_properties),
	.get_property = sm5703_charger_get_property,
};

static const struct regmap_config sm5703_charger_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = SM5703_REG_STATUS5,
};

static int sm5703_charger_configure(struct sm5703_charger *charger)
{
	int ret;

	ret = regmap_write(charger->regmap, SM5703_REG_VBUSCNTL,
			   SM5703_USB_CURRENT_REG);
	if (ret)
		return ret;

	ret = regmap_write(charger->regmap, SM5703_REG_CHGCNTL2,
			   SM5703_USB_CURRENT_REG);
	if (ret)
		return ret;

	ret = regmap_write(charger->regmap, SM5703_REG_CHGCNTL3,
			   SM5703_FLOAT_VOLTAGE_REG);
	if (ret)
		return ret;

	ret = regmap_update_bits(charger->regmap, SM5703_REG_CHGCNTL4,
				 SM5703_AUTOSTOP |
				 SM5703_TOPOFF_CURRENT_MASK,
				 SM5703_AUTOSTOP |
				 FIELD_PREP(SM5703_TOPOFF_CURRENT_MASK,
					    SM5703_TOPOFF_200MA_REG));
	if (ret)
		return ret;

	/* Samsung disables AICL for the 450 mA USB profile (CHGCNTL5=0x7a). */
	return regmap_update_bits(charger->regmap, SM5703_REG_CHGCNTL5,
				  SM5703_AICLEN, 0);
}

static int sm5703_charger_probe(struct i2c_client *client)
{
	struct power_supply_config psy_cfg = {};
	struct device *dev = &client->dev;
	struct sm5703_charger *charger;
	int ret;

	charger = devm_kzalloc(dev, sizeof(*charger), GFP_KERNEL);
	if (!charger)
		return -ENOMEM;

	charger->regmap = devm_regmap_init_i2c(
		client, &sm5703_charger_regmap_config);
	if (IS_ERR(charger->regmap))
		return PTR_ERR(charger->regmap);

	charger->enable_gpio = devm_gpiod_get(dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(charger->enable_gpio))
		return dev_err_probe(dev, PTR_ERR(charger->enable_gpio),
				     "failed to get enable GPIO\n");

	ret = sm5703_charger_configure(charger);
	if (ret)
		return dev_err_probe(dev, ret, "failed to configure charger\n");

	psy_cfg.drv_data = charger;
	psy_cfg.fwnode = dev_fwnode(dev);
	charger->psy = devm_power_supply_register(dev, &sm5703_charger_desc,
						  &psy_cfg);
	if (IS_ERR(charger->psy))
		return PTR_ERR(charger->psy);

	INIT_DELAYED_WORK(&charger->work, sm5703_charger_update);
	schedule_delayed_work(&charger->work, 0);
	i2c_set_clientdata(client, charger);

	dev_info(dev, "SM5703 charger registered with 450 mA USB limit\n");
	return 0;
}

static void sm5703_charger_remove(struct i2c_client *client)
{
	struct sm5703_charger *charger = i2c_get_clientdata(client);

	cancel_delayed_work_sync(&charger->work);
	gpiod_set_value_cansleep(charger->enable_gpio, 0);
}

static const struct of_device_id sm5703_charger_of_match[] = {
	{ .compatible = "siliconmitus,sm5703-charger" },
	{ }
};
MODULE_DEVICE_TABLE(of, sm5703_charger_of_match);

static struct i2c_driver sm5703_charger_driver = {
	.driver = {
		.name = "sm5703-charger",
		.of_match_table = sm5703_charger_of_match,
	},
	.probe = sm5703_charger_probe,
	.remove = sm5703_charger_remove,
};
module_i2c_driver(sm5703_charger_driver);

MODULE_AUTHOR("SM-T580 Linux port contributors");
MODULE_DESCRIPTION("Silicon Mitus SM5703 battery charger driver");
MODULE_LICENSE("GPL");
