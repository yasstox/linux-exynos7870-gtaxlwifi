// SPDX-License-Identifier: GPL-2.0
/*
 * Read-only power supply driver for the Silicon Mitus SM5703 fuel gauge.
 *
 * The conversion formulas and register layout are derived from the Samsung
 * vendor driver used on the Galaxy Tab A 10.1 (2016).
 */

#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/mod_devicetable.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/units.h>

#define SM5703_REG_DEVICE_ID	0x00
#define SM5703_REG_SOC		0x05
#define SM5703_REG_VOLTAGE	0x07
#define SM5703_REG_CURRENT	0x08
#define SM5703_REG_TEMPERATURE	0x09

struct sm5703_fg {
	struct device *dev;
	struct regmap *regmap;
	struct power_supply *psy;
};

static const struct regmap_config sm5703_fg_regmap_config = {
	.reg_bits = 8,
	.val_bits = 16,
	.val_format_endian = REGMAP_ENDIAN_LITTLE,
};

static int sm5703_fg_read(struct sm5703_fg *fg, unsigned int reg,
			  unsigned int *val)
{
	int ret = regmap_read(fg->regmap, reg, val);

	if (ret)
		dev_err_ratelimited(fg->dev, "failed to read register %#x: %d\n",
				    reg, ret);

	return ret;
}

static int sm5703_fg_voltage(struct sm5703_fg *fg, int *value)
{
	unsigned int val;
	int ret;

	ret = sm5703_fg_read(fg, SM5703_REG_VOLTAGE, &val);
	if (ret)
		return ret;

	*value = (((val & 0x700) >> 8) * 1000 +
		  (val & 0xff) * 1000 / 256) * MILLI;
	return 0;
}

static int sm5703_fg_current(struct sm5703_fg *fg, int *value)
{
	unsigned int val;
	int measured_current;
	int ret;

	ret = sm5703_fg_read(fg, SM5703_REG_CURRENT, &val);
	if (ret)
		return ret;

	measured_current = ((val & 0x700) >> 8) * 1000 +
			   (val & 0xff) * 1000 / 256;
	if (val & BIT(15))
		measured_current = -measured_current;
	*value = measured_current * MILLI;
	return 0;
}

static int sm5703_fg_capacity(struct sm5703_fg *fg, int *value)
{
	unsigned int val;
	int ret;

	ret = sm5703_fg_read(fg, SM5703_REG_SOC, &val);
	if (ret)
		return ret;

	*value = min_t(int, DIV_ROUND_CLOSEST(val, 256), 100);
	return 0;
}

static int sm5703_fg_temperature(struct sm5703_fg *fg, int *value)
{
	unsigned int val;
	int temperature;
	int ret;

	ret = sm5703_fg_read(fg, SM5703_REG_TEMPERATURE, &val);
	if (ret)
		return ret;

	temperature = ((val & 0x7f00) >> 8) * 10 + (val & 0xff) * 10 / 256;
	if (val & BIT(15))
		temperature = -temperature;
	*value = temperature;
	return 0;
}

static int sm5703_fg_get_property(struct power_supply *psy,
				  enum power_supply_property property,
				  union power_supply_propval *val)
{
	struct sm5703_fg *fg = power_supply_get_drvdata(psy);
	int measured_current;
	int ret;

	switch (property) {
	case POWER_SUPPLY_PROP_STATUS:
		ret = sm5703_fg_current(fg, &measured_current);
		if (ret)
			return ret;
		val->intval = measured_current > 0 ? POWER_SUPPLY_STATUS_CHARGING :
			measured_current < 0 ? POWER_SUPPLY_STATUS_DISCHARGING :
			POWER_SUPPLY_STATUS_NOT_CHARGING;
		return 0;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		return 0;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		return 0;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		return sm5703_fg_voltage(fg, &val->intval);
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		return sm5703_fg_current(fg, &val->intval);
	case POWER_SUPPLY_PROP_CAPACITY:
		return sm5703_fg_capacity(fg, &val->intval);
	case POWER_SUPPLY_PROP_TEMP:
		return sm5703_fg_temperature(fg, &val->intval);
	default:
		return -EINVAL;
	}
}

static const enum power_supply_property sm5703_fg_properties[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_TEMP,
};

static const struct power_supply_desc sm5703_fg_desc = {
	.name = "sm5703-fuel-gauge",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = sm5703_fg_properties,
	.num_properties = ARRAY_SIZE(sm5703_fg_properties),
	.get_property = sm5703_fg_get_property,
};

static irqreturn_t sm5703_fg_irq(int irq, void *data)
{
	struct sm5703_fg *fg = data;

	power_supply_changed(fg->psy);
	return IRQ_HANDLED;
}

static int sm5703_fg_probe(struct i2c_client *client)
{
	struct power_supply_config psy_cfg = {};
	struct device *dev = &client->dev;
	struct sm5703_fg *fg;
	unsigned int device_id;
	int ret;

	fg = devm_kzalloc(dev, sizeof(*fg), GFP_KERNEL);
	if (!fg)
		return -ENOMEM;

	fg->dev = dev;
	fg->regmap = devm_regmap_init_i2c(client, &sm5703_fg_regmap_config);
	if (IS_ERR(fg->regmap))
		return dev_err_probe(dev, PTR_ERR(fg->regmap),
				     "failed to initialize regmap\n");

	ret = sm5703_fg_read(fg, SM5703_REG_DEVICE_ID, &device_id);
	if (ret)
		return dev_err_probe(dev, ret, "fuel gauge not responding\n");

	psy_cfg.drv_data = fg;
	psy_cfg.fwnode = dev_fwnode(dev);
	fg->psy = devm_power_supply_register(dev, &sm5703_fg_desc, &psy_cfg);
	if (IS_ERR(fg->psy))
		return dev_err_probe(dev, PTR_ERR(fg->psy),
				     "failed to register power supply\n");

	if (client->irq > 0) {
		ret = devm_request_threaded_irq(dev, client->irq, NULL,
						sm5703_fg_irq, IRQF_ONESHOT,
						sm5703_fg_desc.name, fg);
		if (ret)
			return dev_err_probe(dev, ret, "failed to request IRQ\n");
	}

	dev_info(dev, "SM5703 fuel gauge detected (ID %#x)\n", device_id);
	return 0;
}

static const struct of_device_id sm5703_fg_of_match[] = {
	{ .compatible = "samsung,sm5703-fuelgauge" },
	{ }
};
MODULE_DEVICE_TABLE(of, sm5703_fg_of_match);

static struct i2c_driver sm5703_fg_driver = {
	.probe = sm5703_fg_probe,
	.driver = {
		.name = "sm5703-fuel-gauge",
		.of_match_table = sm5703_fg_of_match,
	},
};
module_i2c_driver(sm5703_fg_driver);

MODULE_DESCRIPTION("Silicon Mitus SM5703 fuel gauge driver");
MODULE_LICENSE("GPL");
