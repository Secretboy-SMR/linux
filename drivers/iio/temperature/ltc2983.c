// SPDX-License-Identifier: GPL-2.0
/*
 * Analog Devices LTC2983 Multi-Sensor Digital Temperature Measurement System
 * driver
 *
 * Copyright 2019 Analog Devices Inc.
 */
#include <linux/bitfield.h>
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/iio/iio.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>

#include <asm/byteorder.h>
#include <linux/unaligned.h>

/* register map */
#define LTC2983_STATUS_REG			0x0000
#define LTC2983_TEMP_RES_START_REG		0x0010
#define LTC2983_TEMP_RES_END_REG		0x005F
#define LTC2983_EEPROM_KEY_REG			0x00B0
#define LTC2983_EEPROM_READ_STATUS_REG		0x00D0
#define LTC2983_GLOBAL_CONFIG_REG		0x00F0
#define LTC2983_MULT_CHANNEL_START_REG		0x00F4
#define LTC2983_MULT_CHANNEL_END_REG		0x00F7
#define LTC2986_EEPROM_STATUS_REG		0x00F9
#define LTC2983_MUX_CONFIG_REG			0x00FF
#define LTC2983_CHAN_ASSIGN_START_REG		0x0200
#define LTC2983_CHAN_ASSIGN_END_REG		0x024F
#define LTC2983_CUST_SENS_TBL_START_REG		0x0250
#define LTC2983_CUST_SENS_TBL_END_REG		0x03CF

#define LTC2983_DIFFERENTIAL_CHAN_MIN		2
#define LTC2983_MIN_CHANNELS_NR			1
#define LTC2983_SLEEP				0x97
#define LTC2983_CUSTOM_STEINHART_SIZE		24
#define LTC2983_CUSTOM_SENSOR_ENTRY_SZ		6
#define LTC2983_CUSTOM_STEINHART_ENTRY_SZ	4

#define LTC2983_EEPROM_KEY			0xA53C0F5A
#define LTC2983_EEPROM_WRITE_CMD		0x15
#define LTC2983_EEPROM_READ_CMD			0x16
#define LTC2983_EEPROM_STATUS_FAILURE_MASK	GENMASK(3, 1)
#define LTC2983_EEPROM_READ_FAILURE_MASK	GENMASK(7, 0)

#define LTC2983_EEPROM_WRITE_TIME_MS		2600
#define LTC2983_EEPROM_READ_TIME_MS		20

#define LTC2983_CHAN_START_ADDR(chan) \
			(((chan - 1) * 4) + LTC2983_CHAN_ASSIGN_START_REG)
#define LTC2983_CHAN_RES_ADDR(chan) \
			(((chan - 1) * 4) + LTC2983_TEMP_RES_START_REG)
#define LTC2983_THERMOCOUPLE_DIFF_MASK		BIT(3)
#define LTC2983_THERMOCOUPLE_SGL(x) \
				FIELD_PREP(LTC2983_THERMOCOUPLE_DIFF_MASK, x)
#define LTC2983_THERMOCOUPLE_OC_CURR_MASK	GENMASK(1, 0)
#define LTC2983_THERMOCOUPLE_OC_CURR(x) \
				FIELD_PREP(LTC2983_THERMOCOUPLE_OC_CURR_MASK, x)
#define LTC2983_THERMOCOUPLE_OC_CHECK_MASK	BIT(2)
#define LTC2983_THERMOCOUPLE_OC_CHECK(x) \
			FIELD_PREP(LTC2983_THERMOCOUPLE_OC_CHECK_MASK, x)

#define LTC2983_THERMISTOR_DIFF_MASK		BIT(2)
#define LTC2983_THERMISTOR_SGL(x) \
				FIELD_PREP(LTC2983_THERMISTOR_DIFF_MASK, x)
#define LTC2983_THERMISTOR_R_SHARE_MASK		BIT(1)
#define LTC2983_THERMISTOR_R_SHARE(x) \
				FIELD_PREP(LTC2983_THERMISTOR_R_SHARE_MASK, x)
#define LTC2983_THERMISTOR_C_ROTATE_MASK	BIT(0)
#define LTC2983_THERMISTOR_C_ROTATE(x) \
				FIELD_PREP(LTC2983_THERMISTOR_C_ROTATE_MASK, x)

#define LTC2983_DIODE_DIFF_MASK			BIT(2)
#define LTC2983_DIODE_SGL(x) \
			FIELD_PREP(LTC2983_DIODE_DIFF_MASK, x)
#define LTC2983_DIODE_3_CONV_CYCLE_MASK		BIT(1)
#define LTC2983_DIODE_3_CONV_CYCLE(x) \
				FIELD_PREP(LTC2983_DIODE_3_CONV_CYCLE_MASK, x)
#define LTC2983_DIODE_AVERAGE_ON_MASK		BIT(0)
#define LTC2983_DIODE_AVERAGE_ON(x) \
				FIELD_PREP(LTC2983_DIODE_AVERAGE_ON_MASK, x)

#define LTC2983_RTD_4_WIRE_MASK			BIT(3)
#define LTC2983_RTD_ROTATION_MASK		BIT(1)
#define LTC2983_RTD_C_ROTATE(x) \
			FIELD_PREP(LTC2983_RTD_ROTATION_MASK, x)
#define LTC2983_RTD_KELVIN_R_SENSE_MASK		GENMASK(3, 2)
#define LTC2983_RTD_N_WIRES_MASK		GENMASK(3, 2)
#define LTC2983_RTD_N_WIRES(x) \
			FIELD_PREP(LTC2983_RTD_N_WIRES_MASK, x)
#define LTC2983_RTD_R_SHARE_MASK		BIT(0)
#define LTC2983_RTD_R_SHARE(x) \
			FIELD_PREP(LTC2983_RTD_R_SHARE_MASK, 1)

#define LTC2983_COMMON_HARD_FAULT_MASK	GENMASK(31, 30)
#define LTC2983_COMMON_SOFT_FAULT_MASK	GENMASK(27, 25)

#define	LTC2983_STATUS_START_MASK	BIT(7)
#define	LTC2983_STATUS_START(x)		FIELD_PREP(LTC2983_STATUS_START_MASK, x)
#define	LTC2983_STATUS_UP_MASK		GENMASK(7, 6)
#define	LTC2983_STATUS_UP(reg)		FIELD_GET(LTC2983_STATUS_UP_MASK, reg)

#define	LTC2983_STATUS_CHAN_SEL_MASK	GENMASK(4, 0)
#define	LTC2983_STATUS_CHAN_SEL(x) \
				FIELD_PREP(LTC2983_STATUS_CHAN_SEL_MASK, x)

#define LTC2983_TEMP_UNITS_MASK		BIT(2)
#define LTC2983_TEMP_UNITS(x)		FIELD_PREP(LTC2983_TEMP_UNITS_MASK, x)

#define LTC2983_NOTCH_FREQ_MASK		GENMASK(1, 0)
#define LTC2983_NOTCH_FREQ(x)		FIELD_PREP(LTC2983_NOTCH_FREQ_MASK, x)

#define LTC2983_RES_VALID_MASK		BIT(24)
#define LTC2983_DATA_MASK		GENMASK(23, 0)
#define LTC2983_DATA_SIGN_BIT		23

#define LTC2983_CHAN_TYPE_MASK		GENMASK(31, 27)
#define LTC2983_CHAN_TYPE(x)		FIELD_PREP(LTC2983_CHAN_TYPE_MASK, x)

/* cold junction for thermocouples and rsense for rtd's and thermistor's */
#define LTC2983_CHAN_ASSIGN_MASK	GENMASK(26, 22)
#define LTC2983_CHAN_ASSIGN(x)		FIELD_PREP(LTC2983_CHAN_ASSIGN_MASK, x)

#define LTC2983_CUSTOM_LEN_MASK		GENMASK(5, 0)
#define LTC2983_CUSTOM_LEN(x)		FIELD_PREP(LTC2983_CUSTOM_LEN_MASK, x)

#define LTC2983_CUSTOM_ADDR_MASK	GENMASK(11, 6)
#define LTC2983_CUSTOM_ADDR(x)		FIELD_PREP(LTC2983_CUSTOM_ADDR_MASK, x)

#define LTC2983_THERMOCOUPLE_CFG_MASK	GENMASK(21, 18)
#define LTC2983_THERMOCOUPLE_CFG(x) \
				FIELD_PREP(LTC2983_THERMOCOUPLE_CFG_MASK, x)
#define LTC2983_THERMOCOUPLE_HARD_FAULT_MASK	GENMASK(31, 29)
#define LTC2983_THERMOCOUPLE_SOFT_FAULT_MASK	GENMASK(28, 25)

#define LTC2983_RTD_CFG_MASK		GENMASK(21, 18)
#define LTC2983_RTD_CFG(x)		FIELD_PREP(LTC2983_RTD_CFG_MASK, x)
#define LTC2983_RTD_EXC_CURRENT_MASK	GENMASK(17, 14)
#define LTC2983_RTD_EXC_CURRENT(x) \
				FIELD_PREP(LTC2983_RTD_EXC_CURRENT_MASK, x)
#define LTC2983_RTD_CURVE_MASK		GENMASK(13, 12)
#define LTC2983_RTD_CURVE(x)		FIELD_PREP(LTC2983_RTD_CURVE_MASK, x)

#define LTC2983_THERMISTOR_CFG_MASK	GENMASK(21, 19)
#define LTC2983_THERMISTOR_CFG(x) \
				FIELD_PREP(LTC2983_THERMISTOR_CFG_MASK, x)
#define LTC2983_THERMISTOR_EXC_CURRENT_MASK	GENMASK(18, 15)
#define LTC2983_THERMISTOR_EXC_CURRENT(x) \
			FIELD_PREP(LTC2983_THERMISTOR_EXC_CURRENT_MASK, x)

#define LTC2983_DIODE_CFG_MASK		GENMASK(26, 24)
#define LTC2983_DIODE_CFG(x)		FIELD_PREP(LTC2983_DIODE_CFG_MASK, x)
#define LTC2983_DIODE_EXC_CURRENT_MASK	GENMASK(23, 22)
#define LTC2983_DIODE_EXC_CURRENT(x) \
				FIELD_PREP(LTC2983_DIODE_EXC_CURRENT_MASK, x)
#define LTC2983_DIODE_IDEAL_FACTOR_MASK	GENMASK(21, 0)
#define LTC2983_DIODE_IDEAL_FACTOR(x) \
				FIELD_PREP(LTC2983_DIODE_IDEAL_FACTOR_MASK, x)

#define LTC2983_R_SENSE_VAL_MASK	GENMASK(26, 0)
#define LTC2983_R_SENSE_VAL(x)		FIELD_PREP(LTC2983_R_SENSE_VAL_MASK, x)

#define LTC2983_ADC_SINGLE_ENDED_MASK	BIT(26)
#define LTC2983_ADC_SINGLE_ENDED(x) \
				FIELD_PREP(LTC2983_ADC_SINGLE_ENDED_MASK, x)

enum {
	LTC2983_SENSOR_THERMOCOUPLE = 1,
	LTC2983_SENSOR_THERMOCOUPLE_CUSTOM = 9,
	LTC2983_SENSOR_RTD = 10,
	LTC2983_SENSOR_RTD_CUSTOM = 18,
	LTC2983_SENSOR_THERMISTOR = 19,
	LTC2983_SENSOR_THERMISTOR_STEINHART = 26,
	LTC2983_SENSOR_THERMISTOR_CUSTOM = 27,
	LTC2983_SENSOR_DIODE = 28,
	LTC2983_SENSOR_SENSE_RESISTOR = 29,
	LTC2983_SENSOR_DIRECT_ADC = 30,
	LTC2983_SENSOR_ACTIVE_TEMP = 31,
};

#define to_thermocouple(_sensor) \
		container_of(_sensor, struct ltc2983_thermocouple, sensor)

#define to_rtd(_sensor) \
		container_of(_sensor, struct ltc2983_rtd, sensor)

#define to_thermistor(_sensor) \
		container_of(_sensor, struct ltc2983_thermistor, sensor)

#define to_diode(_sensor) \
		container_of(_sensor, struct ltc2983_diode, sensor)

#define to_rsense(_sensor) \
		container_of(_sensor, struct ltc2983_rsense, sensor)

#define to_adc(_sensor) \
		container_of(_sensor, struct ltc2983_adc, sensor)

#define to_temp(_sensor) \
		container_of(_sensor, struct ltc2983_temp, sensor)

struct ltc2983_chip_info {
	const char *name;
	unsigned int max_channels_nr;
	bool has_temp;
	bool has_eeprom;
};

struct ltc2983_data {
	const struct ltc2983_chip_info *info;
	struct regmap *regmap;
	struct spi_device *spi;
	struct mutex lock;
	struct completion completion;
	struct iio_chan_spec *iio_chan;
	struct ltc2983_sensor **sensors;
	u32 mux_delay_config;
	u32 filter_notch_freq;
	u16 custom_table_size;
	u8 num_channels;
	u8 iio_channels;
	/*
	 * DMA (thus cache coherency maintenance) may require the
	 * transfer buffers to live in their own cache lines.
	 * Holds the converted temperature
	 */
	__be32 temp __aligned(IIO_DMA_MINALIGN);
	__be32 chan_val;
	__be32 eeprom_key;
};

struct ltc2983_sensor {
	int (*fault_handler)(const struct ltc2983_data *st, const u32 result);
	int (*assign_chan)(struct ltc2983_data *st,
			   const struct ltc2983_sensor *sensor);
	/* specifies the sensor channel */
	u32 chan;
	/* sensor type */
	u32 type;
};

struct ltc2983_custom_sensor {
	/* raw table sensor data */
	void *table;
	size_t size;
	/* address offset */
	s8 offset;
	bool is_steinhart;
};

struct ltc2983_thermocouple {
	struct ltc2983_sensor sensor;
	struct ltc2983_custom_sensor *custom;
	u32 sensor_config;
	u32 cold_junction_chan;
};

struct ltc2983_rtd {
	struct ltc2983_sensor sensor;
	struct ltc2983_custom_sensor *custom;
	u32 sensor_config;
	u32 r_sense_chan;
	u32 excitation_current;
	u32 rtd_curve;
};

struct ltc2983_thermistor {
	struct ltc2983_sensor sensor;
	struct ltc2983_custom_sensor *custom;
	u32 sensor_config;
	u32 r_sense_chan;
	u32 excitation_current;
};

struct ltc2983_diode {
	struct ltc2983_sensor sensor;
	u32 sensor_config;
	u32 excitation_current;
	u32 ideal_factor_value;
};

struct ltc2983_rsense {
	struct ltc2983_sensor sensor;
	u32 r_sense_val;
};

struct ltc2983_adc {
	struct ltc2983_sensor sensor;
	bool single_ended;
};

struct ltc2983_temp {
	struct ltc2983_sensor sensor;
	struct ltc2983_custom_sensor *custom;
	bool single_ended;
};

/*
 * Convert to Q format numbers. These number's are integers where
 * the number of integer and fractional bits are specified. The resolution
 * is given by 1/@resolution and tell us the number of fractional bits. For
 * instance a resolution of 2^-10 means we have 10 fractional bits.
 */
static u32 __convert_to_raw(const u64 val, const u32 resolution)
{
	u64 __res = val * resolution;

	/* all values are multiplied by 1000000 to remove the fraction */
	do_div(__res, 1000000);

	return __res;
}

static u32 __convert_to_raw_sign(const u64 val, const u32 resolution)
{
	s64 __res = -(s32)val;

	__res = __convert_to_raw(__res, resolution);

	return (u32)-__res;
}

static int __ltc2983_fault_handler(const struct ltc2983_data *st,
				   const u32 result, const u32 hard_mask,
				   const u32 soft_mask)
{
	const struct device *dev = &st->spi->dev;

	if (result & hard_mask) {
		dev_err(dev, "Invalid conversion: Sensor HARD fault\n");
		return -EIO;
	} else if (result & soft_mask) {
		/* just print a warning */
		dev_warn(dev, "Suspicious conversion: Sensor SOFT fault\n");
	}

	return 0;
}

static int __ltc2983_chan_assign_common(struct ltc2983_data *st,
					const struct ltc2983_sensor *sensor,
					u32 chan_val)
{
	u32 reg = LTC2983_CHAN_START_ADDR(sensor->chan);

	chan_val |= LTC2983_CHAN_TYPE(sensor->type);
	dev_dbg(&st->spi->dev, "Assign reg:0x%04X, val:0x%08X\n", reg,
		chan_val);
	st->chan_val = cpu_to_be32(chan_val);
	return regmap_bulk_write(st->regmap, reg, &st->chan_val,
				 sizeof(st->chan_val));
}

static int __ltc2983_chan_custom_sensor_assign(struct ltc2983_data *st,
					  struct ltc2983_custom_sensor *custom,
					  u32 *chan_val)
{
	u32 reg;
	u8 mult = custom->is_steinhart ? LTC2983_CUSTOM_STEINHART_ENTRY_SZ :
		LTC2983_CUSTOM_SENSOR_ENTRY_SZ;
	const struct device *dev = &st->spi->dev;
	/*
	 * custom->size holds the raw size of the table. However, when
	 * configuring the sensor channel, we must write the number of
	 * entries of the table minus 1. For steinhart sensors 0 is written
	 * since the size is constant!
	 */
	const u8 len = custom->is_steinhart ? 0 :
		(custom->size / LTC2983_CUSTOM_SENSOR_ENTRY_SZ) - 1;
	/*
	 * Check if the offset was assigned already. It should be for steinhart
	 * sensors. When coming from sleep, it should be assigned for all.
	 */
	if (custom->offset < 0) {
		/*
		 * This needs to be done again here because, from the moment
		 * when this test was done (successfully) for this custom
		 * sensor, a steinhart sensor might have been added changing
		 * custom_table_size...
		 */
		if (st->custom_table_size + custom->size >
		    (LTC2983_CUST_SENS_TBL_END_REG -
		     LTC2983_CUST_SENS_TBL_START_REG) + 1) {
			dev_err(dev,
				"Not space left(%d) for new custom sensor(%zu)",
				st->custom_table_size,
				custom->size);
			return -EINVAL;
		}

		custom->offset = st->custom_table_size /
					LTC2983_CUSTOM_SENSOR_ENTRY_SZ;
		st->custom_table_size += custom->size;
	}

	reg = (custom->offset * mult) + LTC2983_CUST_SENS_TBL_START_REG;

	*chan_val |= LTC2983_CUSTOM_LEN(len);
	*chan_val |= LTC2983_CUSTOM_ADDR(custom->offset);
	dev_dbg(dev, "Assign custom sensor, reg:0x%04X, off:%d, sz:%zu",
		reg, custom->offset,
		custom->size);
	/* write custom sensor table */
	return regmap_bulk_write(st->regmap, reg, custom->table, custom->size);
}

static struct ltc2983_custom_sensor *
__ltc2983_custom_sensor_new(struct ltc2983_data *st, const struct fwnode_handle *fn,
			    const char *propname, const bool is_steinhart,
			    const u32 resolution, const bool has_signed)
{
	struct ltc2983_custom_sensor *new_custom;
	struct device *dev = &st->spi->dev;
	/*
	 * For custom steinhart, the full u32 is taken. For all the others
	 * the MSB is discarded.
	 */
	const u8 n_size = is_steinhart ? 4 : 3;
	u8 index, n_entries;
	int ret;

	if (is_steinhart)
		n_entries = fwnode_property_count_u32(fn, propname);
	else
		n_entries = fwnode_property_count_u64(fn, propname);
	/* n_entries must be an even number */
	if (!n_entries || (n_entries % 2) != 0)
		return dev_err_ptr_probe(dev, -EINVAL,
					 "Number of entries either 0 or not even\n");

	new_custom = devm_kzalloc(dev, sizeof(*new_custom), GFP_KERNEL);
	if (!new_custom)
		return ERR_PTR(-ENOMEM);

	new_custom->size = n_entries * n_size;
	/* check Steinhart size */
	if (is_steinhart && new_custom->size != LTC2983_CUSTOM_STEINHART_SIZE)
		return dev_err_ptr_probe(dev, -EINVAL,
					 "Steinhart sensors size(%zu) must be %u\n",
					 new_custom->size, LTC2983_CUSTOM_STEINHART_SIZE);

	/* Check space on the table. */
	if (st->custom_table_size + new_custom->size >
	    (LTC2983_CUST_SENS_TBL_END_REG - LTC2983_CUST_SENS_TBL_START_REG) + 1)
		return dev_err_ptr_probe(dev, -EINVAL,
					 "No space left(%d) for new custom sensor(%zu)\n",
					 st->custom_table_size, new_custom->size);

	/* allocate the table */
	if (is_steinhart)
		new_custom->table = devm_kcalloc(dev, n_entries, sizeof(u32), GFP_KERNEL);
	else
		new_custom->table = devm_kcalloc(dev, n_entries, sizeof(u64), GFP_KERNEL);
	if (!new_custom->table)
		return ERR_PTR(-ENOMEM);

	/*
	 * Steinhart sensors are configured with raw values in the firmware
	 * node. For the other sensors we must convert the value to raw.
	 * The odd index's correspond to temperatures and always have 1/1024
	 * of resolution. Temperatures also come in Kelvin, so signed values
	 * are not possible.
	 */
	if (is_steinhart) {
		ret = fwnode_property_read_u32_array(fn, propname, new_custom->table, n_entries);
		if (ret < 0)
			return ERR_PTR(ret);

		cpu_to_be32_array(new_custom->table, new_custom->table, n_entries);
	} else {
		ret = fwnode_property_read_u64_array(fn, propname, new_custom->table, n_entries);
		if (ret < 0)
			return ERR_PTR(ret);

		for (index = 0; index < n_entries; index++) {
			u64 temp = ((u64 *)new_custom->table)[index];

			if ((index % 2) != 0)
				temp = __convert_to_raw(temp, 1024);
			else if (has_signed && (s64)temp < 0)
				temp = __convert_to_raw_sign(temp, resolution);
			else
				temp = __convert_to_raw(temp, resolution);

			put_unaligned_be24(temp, new_custom->table + index * 3);
		}
	}

	new_custom->is_steinhart = is_steinhart;
	/*
	 * This is done to first add all the steinhart sensors to the table,
	 * in order to maximize the table usage. If we mix adding steinhart
	 * with the other sensors, we might have to do some roundup to make
	 * sure that sensor_addr - 0x250(start address) is a multiple of 4
	 * (for steinhart), and a multiple of 6 for all the other sensors.
	 * Since we have const 24 bytes for steinhart sensors and 24 is
	 * also a multiple of 6, we guarantee that the first non-steinhart
	 * sensor will sit in a correct address without the need of filling
	 * addresses.
	 */
	if (is_steinhart) {
		new_custom->offset = st->custom_table_size /
					LTC2983_CUSTOM_STEINHART_ENTRY_SZ;
		st->custom_table_size += new_custom->size;
	} else {
		/* mark as unset. This is checked later on the assign phase */
		new_custom->offset = -1;
	}

	return new_custom;
}

static int ltc2983_thermocouple_fault_handler(const struct ltc2983_data *st,
					      const u32 result)
{
	return __ltc2983_fault_handler(st, result,
				       LTC2983_THERMOCOUPLE_HARD_FAULT_MASK,
				       LTC2983_THERMOCOUPLE_SOFT_FAULT_MASK);
}

static int ltc2983_common_fault_handler(const struct ltc2983_data *st,
					const u32 result)
{
	return __ltc2983_fault_handler(st, result,
				       LTC2983_COMMON_HARD_FAULT_MASK,
				       LTC2983_COMMON_SOFT_FAULT_MASK);
}

static int ltc2983_thermocouple_assign_chan(struct ltc2983_data *st,
				const struct ltc2983_sensor *sensor)
{
	struct ltc2983_thermocouple *thermo = to_thermocouple(sensor);
	u32 chan_val;

	chan_val = LTC2983_CHAN_ASSIGN(thermo->cold_junction_chan);
	chan_val |= LTC2983_THERMOCOUPLE_CFG(thermo->sensor_config);

	if (thermo->custom) {
		int ret;

		ret = __ltc2983_chan_custom_sensor_assign(st, thermo->custom,
							  &chan_val);
		if (ret)
			return ret;
	}
	return __ltc2983_chan_assign_common(st, sensor, chan_val);
}

static int ltc2983_rtd_assign_chan(struct ltc2983_data *st,
				   const struct ltc2983_sensor *sensor)
{
	struct ltc2983_rtd *rtd = to_rtd(sensor);
	u32 chan_val;

	chan_val = LTC2983_CHAN_ASSIGN(rtd->r_sense_chan);
	chan_val |= LTC2983_RTD_CFG(rtd->sensor_config);
	chan_val |= LTC2983_RTD_EXC_CURRENT(rtd->excitation_current);
	chan_val |= LTC2983_RTD_CURVE(rtd->rtd_curve);

	if (rtd->custom) {
		int ret;

		ret = __ltc2983_chan_custom_sensor_assign(st, rtd->custom,
							  &chan_val);
		if (ret)
			return ret;
	}
	return __ltc2983_chan_assign_common(st, sensor, chan_val);
}

static int ltc2983_thermistor_assign_chan(struct ltc2983_data *st,
					  const struct ltc2983_sensor *sensor)
{
	struct ltc2983_thermistor *thermistor = to_thermistor(sensor);
	u32 chan_val;

	chan_val = LTC2983_CHAN_ASSIGN(thermistor->r_sense_chan);
	chan_val |= LTC2983_THERMISTOR_CFG(thermistor->sensor_config);
	chan_val |=
		LTC2983_THERMISTOR_EXC_CURRENT(thermistor->excitation_current);

	if (thermistor->custom) {
		int ret;

		ret = __ltc2983_chan_custom_sensor_assign(st,
							  thermistor->custom,
							  &chan_val);
		if (ret)
			return ret;
	}
	return __ltc2983_chan_assign_common(st, sensor, chan_val);
}

static int ltc2983_diode_assign_chan(struct ltc2983_data *st,
				     const struct ltc2983_sensor *sensor)
{
	struct ltc2983_diode *diode = to_diode(sensor);
	u32 chan_val;

	chan_val = LTC2983_DIODE_CFG(diode->sensor_config);
	chan_val |= LTC2983_DIODE_EXC_CURRENT(diode->excitation_current);
	chan_val |= LTC2983_DIODE_IDEAL_FACTOR(diode->ideal_factor_value);

	return __ltc2983_chan_assign_common(st, sensor, chan_val);
}

static int ltc2983_r_sense_assign_chan(struct ltc2983_data *st,
				       const struct ltc2983_sensor *sensor)
{
	struct ltc2983_rsense *rsense = to_rsense(sensor);
	u32 chan_val;

	chan_val = LTC2983_R_SENSE_VAL(rsense->r_sense_val);

	return __ltc2983_chan_assign_common(st, sensor, chan_val);
}

static int ltc2983_adc_assign_chan(struct ltc2983_data *st,
				   const struct ltc2983_sensor *sensor)
{
	struct ltc2983_adc *adc = to_adc(sensor);
	u32 chan_val;

	chan_val = LTC2983_ADC_SINGLE_ENDED(adc->single_ended);

	return __ltc2983_chan_assign_common(st, sensor, chan_val);
}

static int ltc2983_temp_assign_chan(struct ltc2983_data *st,
				    const struct ltc2983_sensor *sensor)
{
	struct ltc2983_temp *temp = to_temp(sensor);
	u32 chan_val;
	int ret;

	chan_val = LTC2983_ADC_SINGLE_ENDED(temp->single_ended);

	ret = __ltc2983_chan_custom_sensor_assign(st, temp->custom, &chan_val);
	if (ret)
		return ret;

	return __ltc2983_chan_assign_common(st, sensor, chan_val);
}

static struct ltc2983_sensor *
ltc2983_thermocouple_new(const struct fwnode_handle *child, struct ltc2983_data *st,
			 const struct ltc2983_sensor *sensor)
{
	struct ltc2983_thermocouple *thermo;
	u32 oc_current;
	int ret;

	thermo = devm_kzalloc(&st->spi->dev, sizeof(*thermo), GFP_KERNEL);
	if (!thermo)
		return ERR_PTR(-ENOMEM);

	if (fwnode_property_read_bool(child, "adi,single-ended"))
		thermo->sensor_config = LTC2983_THERMOCOUPLE_SGL(1);

	ret = fwnode_property_read_u32(child, "adi,sensor-oc-current-microamp", &oc_current);
	if (!ret) {
		switch (oc_current) {
		case 10:
			thermo->sensor_config |=
					LTC2983_THERMOCOUPLE_OC_CURR(0);
			break;
		case 100:
			thermo->sensor_config |=
					LTC2983_THERMOCOUPLE_OC_CURR(1);
			break;
		case 500:
			thermo->sensor_config |=
					LTC2983_THERMOCOUPLE_OC_CURR(2);
			break;
		case 1000:
			thermo->sensor_config |=
					LTC2983_THERMOCOUPLE_OC_CURR(3);
			break;
		default:
			return dev_err_ptr_probe(&st->spi->dev, -EINVAL,
						 "Invalid open circuit current:%u\n",
						 oc_current);
		}

		thermo->sensor_config |= LTC2983_THERMOCOUPLE_OC_CHECK(1);
	}
	/* validate channel index */
	if (!(thermo->sensor_config & LTC2983_THERMOCOUPLE_DIFF_MASK) &&
	    sensor->chan < LTC2983_DIFFERENTIAL_CHAN_MIN)
		return dev_err_ptr_probe(&st->spi->dev, -EINVAL,
					 "Invalid chann:%d for differential thermocouple\n",
					 sensor->chan);

	struct fwnode_handle *ref __free(fwnode_handle) =
		fwnode_find_reference(child, "adi,cold-junction-handle", 0);
	if (IS_ERR(ref)) {
		ref = NULL;
	} else {
		ret = fwnode_property_read_u32(ref, "reg", &thermo->cold_junction_chan);
		if (ret)
			/*
			 * This would be catched later but we can just return
			 * the error right away.
			 */
			return dev_err_ptr_probe(&st->spi->dev, ret,
						 "Property reg must be given\n");
	}

	/* check custom sensor */
	if (sensor->type == LTC2983_SENSOR_THERMOCOUPLE_CUSTOM) {
		const char *propname = "adi,custom-thermocouple";

		thermo->custom = __ltc2983_custom_sensor_new(st, child,
							     propname, false,
							     16384, true);
		if (IS_ERR(thermo->custom))
			return ERR_CAST(thermo->custom);
	}

	/* set common parameters */
	thermo->sensor.fault_handler = ltc2983_thermocouple_fault_handler;
	thermo->sensor.assign_chan = ltc2983_thermocouple_assign_chan;

	return &thermo->sensor;
}

static struct ltc2983_sensor *
ltc2983_rtd_new(const struct fwnode_handle *child, struct ltc2983_data *st,
		const struct ltc2983_sensor *sensor)
{
	struct ltc2983_rtd *rtd;
	int ret = 0;
	struct device *dev = &st->spi->dev;
	u32 excitation_current = 0, n_wires = 0;

	rtd = devm_kzalloc(dev, sizeof(*rtd), GFP_KERNEL);
	if (!rtd)
		return ERR_PTR(-ENOMEM);

	struct fwnode_handle *ref __free(fwnode_handle) =
		fwnode_find_reference(child, "adi,rsense-handle", 0);
	if (IS_ERR(ref))
		return dev_err_cast_probe(dev, ref,
					  "Property adi,rsense-handle missing or invalid\n");

	ret = fwnode_property_read_u32(ref, "reg", &rtd->r_sense_chan);
	if (ret)
		return dev_err_ptr_probe(dev, ret,
					 "Property reg must be given\n");

	ret = fwnode_property_read_u32(child, "adi,number-of-wires", &n_wires);
	if (!ret) {
		switch (n_wires) {
		case 2:
			rtd->sensor_config = LTC2983_RTD_N_WIRES(0);
			break;
		case 3:
			rtd->sensor_config = LTC2983_RTD_N_WIRES(1);
			break;
		case 4:
			rtd->sensor_config = LTC2983_RTD_N_WIRES(2);
			break;
		case 5:
			/* 4 wires, Kelvin Rsense */
			rtd->sensor_config = LTC2983_RTD_N_WIRES(3);
			break;
		default:
			return dev_err_ptr_probe(dev, -EINVAL,
						 "Invalid number of wires:%u\n",
						 n_wires);
		}
	}

	if (fwnode_property_read_bool(child, "adi,rsense-share")) {
		/* Current rotation is only available with rsense sharing */
		if (fwnode_property_read_bool(child, "adi,current-rotate")) {
			if (n_wires == 2 || n_wires == 3)
				return dev_err_ptr_probe(dev, -EINVAL,
							 "Rotation not allowed for 2/3 Wire RTDs\n");

			rtd->sensor_config |= LTC2983_RTD_C_ROTATE(1);
		} else {
			rtd->sensor_config |= LTC2983_RTD_R_SHARE(1);
		}
	}
	/*
	 * rtd channel indexes are a bit more complicated to validate.
	 * For 4wire RTD with rotation, the channel selection cannot be
	 * >=19 since the chann + 1 is used in this configuration.
	 * For 4wire RTDs with kelvin rsense, the rsense channel cannot be
	 * <=1 since chanel - 1 and channel - 2 are used.
	 */
	if (rtd->sensor_config & LTC2983_RTD_4_WIRE_MASK) {
		/* 4-wire */
		u8 min = LTC2983_DIFFERENTIAL_CHAN_MIN,
			max = st->info->max_channels_nr;

		if (rtd->sensor_config & LTC2983_RTD_ROTATION_MASK)
			max = st->info->max_channels_nr - 1;

		if (((rtd->sensor_config & LTC2983_RTD_KELVIN_R_SENSE_MASK)
		     == LTC2983_RTD_KELVIN_R_SENSE_MASK) &&
		    (rtd->r_sense_chan <=  min))
			/* kelvin rsense*/
			return dev_err_ptr_probe(dev, -EINVAL,
						 "Invalid rsense chann:%d to use in kelvin rsense\n",
						 rtd->r_sense_chan);

		if (sensor->chan < min || sensor->chan > max)
			return dev_err_ptr_probe(dev, -EINVAL,
						 "Invalid chann:%d for the rtd config\n",
						 sensor->chan);
	} else {
		/* same as differential case */
		if (sensor->chan < LTC2983_DIFFERENTIAL_CHAN_MIN)
			return dev_err_ptr_probe(&st->spi->dev, -EINVAL,
						 "Invalid chann:%d for RTD\n",
						 sensor->chan);
	}

	/* check custom sensor */
	if (sensor->type == LTC2983_SENSOR_RTD_CUSTOM) {
		rtd->custom = __ltc2983_custom_sensor_new(st, child,
							  "adi,custom-rtd",
							  false, 2048, false);
		if (IS_ERR(rtd->custom))
			return ERR_CAST(rtd->custom);
	}

	/* set common parameters */
	rtd->sensor.fault_handler = ltc2983_common_fault_handler;
	rtd->sensor.assign_chan = ltc2983_rtd_assign_chan;

	ret = fwnode_property_read_u32(child, "adi,excitation-current-microamp",
				       &excitation_current);
	if (ret) {
		/* default to 5uA */
		rtd->excitation_current = 1;
	} else {
		switch (excitation_current) {
		case 5:
			rtd->excitation_current = 0x01;
			break;
		case 10:
			rtd->excitation_current = 0x02;
			break;
		case 25:
			rtd->excitation_current = 0x03;
			break;
		case 50:
			rtd->excitation_current = 0x04;
			break;
		case 100:
			rtd->excitation_current = 0x05;
			break;
		case 250:
			rtd->excitation_current = 0x06;
			break;
		case 500:
			rtd->excitation_current = 0x07;
			break;
		case 1000:
			rtd->excitation_current = 0x08;
			break;
		default:
			return dev_err_ptr_probe(&st->spi->dev, -EINVAL,
						 "Invalid value for excitation current(%u)\n",
						 excitation_current);
		}
	}

	fwnode_property_read_u32(child, "adi,rtd-curve", &rtd->rtd_curve);

	return &rtd->sensor;
}

static struct ltc2983_sensor *
ltc2983_thermistor_new(const struct fwnode_handle *child, struct ltc2983_data *st,
		       const struct ltc2983_sensor *sensor)
{
	struct ltc2983_thermistor *thermistor;
	struct device *dev = &st->spi->dev;
	u32 excitation_current = 0;
	int ret = 0;

	thermistor = devm_kzalloc(dev, sizeof(*thermistor), GFP_KERNEL);
	if (!thermistor)
		return ERR_PTR(-ENOMEM);

	struct fwnode_handle *ref __free(fwnode_handle) =
		fwnode_find_reference(child, "adi,rsense-handle", 0);
	if (IS_ERR(ref))
		return dev_err_cast_probe(dev, ref,
					  "Property adi,rsense-handle missing or invalid\n");

	ret = fwnode_property_read_u32(ref, "reg", &thermistor->r_sense_chan);
	if (ret)
		return dev_err_ptr_probe(dev, ret,
					 "rsense channel must be configured...\n");

	if (fwnode_property_read_bool(child, "adi,single-ended")) {
		thermistor->sensor_config = LTC2983_THERMISTOR_SGL(1);
	} else if (fwnode_property_read_bool(child, "adi,rsense-share")) {
		/* rotation is only possible if sharing rsense */
		if (fwnode_property_read_bool(child, "adi,current-rotate"))
			thermistor->sensor_config =
						LTC2983_THERMISTOR_C_ROTATE(1);
		else
			thermistor->sensor_config =
						LTC2983_THERMISTOR_R_SHARE(1);
	}
	/* validate channel index */
	if (!(thermistor->sensor_config & LTC2983_THERMISTOR_DIFF_MASK) &&
	    sensor->chan < LTC2983_DIFFERENTIAL_CHAN_MIN)
		return dev_err_ptr_probe(&st->spi->dev, -EINVAL,
					 "Invalid chann:%d for differential thermistor\n",
					 sensor->chan);

	/* check custom sensor */
	if (sensor->type >= LTC2983_SENSOR_THERMISTOR_STEINHART) {
		bool steinhart = false;
		const char *propname;

		if (sensor->type == LTC2983_SENSOR_THERMISTOR_STEINHART) {
			steinhart = true;
			propname = "adi,custom-steinhart";
		} else {
			propname = "adi,custom-thermistor";
		}

		thermistor->custom = __ltc2983_custom_sensor_new(st, child,
								 propname,
								 steinhart,
								 64, false);
		if (IS_ERR(thermistor->custom))
			return ERR_CAST(thermistor->custom);
	}
	/* set common parameters */
	thermistor->sensor.fault_handler = ltc2983_common_fault_handler;
	thermistor->sensor.assign_chan = ltc2983_thermistor_assign_chan;

	ret = fwnode_property_read_u32(child, "adi,excitation-current-nanoamp",
				       &excitation_current);
	if (ret) {
		/* Auto range is not allowed for custom sensors */
		if (sensor->type >= LTC2983_SENSOR_THERMISTOR_STEINHART)
			/* default to 1uA */
			thermistor->excitation_current = 0x03;
		else
			/* default to auto-range */
			thermistor->excitation_current = 0x0c;
	} else {
		switch (excitation_current) {
		case 0:
			/* auto range */
			if (sensor->type >= LTC2983_SENSOR_THERMISTOR_STEINHART)
				return dev_err_ptr_probe(&st->spi->dev, -EINVAL,
							 "Auto Range not allowed for custom sensors\n");

			thermistor->excitation_current = 0x0c;
			break;
		case 250:
			thermistor->excitation_current = 0x01;
			break;
		case 500:
			thermistor->excitation_current = 0x02;
			break;
		case 1000:
			thermistor->excitation_current = 0x03;
			break;
		case 5000:
			thermistor->excitation_current = 0x04;
			break;
		case 10000:
			thermistor->excitation_current = 0x05;
			break;
		case 25000:
			thermistor->excitation_current = 0x06;
			break;
		case 50000:
			thermistor->excitation_current = 0x07;
			break;
		case 100000:
			thermistor->excitation_current = 0x08;
			break;
		case 250000:
			thermistor->excitation_current = 0x09;
			break;
		case 500000:
			thermistor->excitation_current = 0x0a;
			break;
		case 1000000:
			thermistor->excitation_current = 0x0b;
			break;
		default:
			return dev_err_ptr_probe(&st->spi->dev, -EINVAL,
						 "Invalid value for excitation current(%u)\n",
						 excitation_current);
		}
	}

	return &thermistor->sensor;
}

static struct ltc2983_sensor *
ltc2983_diode_new(const struct fwnode_handle *child, const struct ltc2983_data *st,
		  const struct ltc2983_sensor *sensor)
{
	struct ltc2983_diode *diode;
	u32 temp = 0, excitation_current = 0;
	int ret;

	diode = devm_kzalloc(&st->spi->dev, sizeof(*diode), GFP_KERNEL);
	if (!diode)
		return ERR_PTR(-ENOMEM);

	if (fwnode_property_read_bool(child, "adi,single-ended"))
		diode->sensor_config = LTC2983_DIODE_SGL(1);

	if (fwnode_property_read_bool(child, "adi,three-conversion-cycles"))
		diode->sensor_config |= LTC2983_DIODE_3_CONV_CYCLE(1);

	if (fwnode_property_read_bool(child, "adi,average-on"))
		diode->sensor_config |= LTC2983_DIODE_AVERAGE_ON(1);

	/* validate channel index */
	if (!(diode->sensor_config & LTC2983_DIODE_DIFF_MASK) &&
	    sensor->chan < LTC2983_DIFFERENTIAL_CHAN_MIN)
		return dev_err_ptr_probe(&st->spi->dev, -EINVAL,
					 "Invalid chann:%d for differential thermistor\n",
					 sensor->chan);

	/* set common parameters */
	diode->sensor.fault_handler = ltc2983_common_fault_handler;
	diode->sensor.assign_chan = ltc2983_diode_assign_chan;

	ret = fwnode_property_read_u32(child, "adi,excitation-current-microamp",
				       &excitation_current);
	if (!ret) {
		switch (excitation_current) {
		case 10:
			diode->excitation_current = 0x00;
			break;
		case 20:
			diode->excitation_current = 0x01;
			break;
		case 40:
			diode->excitation_current = 0x02;
			break;
		case 80:
			diode->excitation_current = 0x03;
			break;
		default:
			return dev_err_ptr_probe(&st->spi->dev, -EINVAL,
						 "Invalid value for excitation current(%u)\n",
						 excitation_current);
		}
	}

	fwnode_property_read_u32(child, "adi,ideal-factor-value", &temp);

	/* 2^20 resolution */
	diode->ideal_factor_value = __convert_to_raw(temp, 1048576);

	return &diode->sensor;
}

static struct ltc2983_sensor *ltc2983_r_sense_new(struct fwnode_handle *child,
					struct ltc2983_data *st,
					const struct ltc2983_sensor *sensor)
{
	struct ltc2983_rsense *rsense;
	int ret;
	u32 temp;

	rsense = devm_kzalloc(&st->spi->dev, sizeof(*rsense), GFP_KERNEL);
	if (!rsense)
		return ERR_PTR(-ENOMEM);

	/* validate channel index */
	if (sensor->chan < LTC2983_DIFFERENTIAL_CHAN_MIN)
		return dev_err_ptr_probe(&st->spi->dev, -EINVAL,
					 "Invalid chann:%d for r_sense\n",
					 sensor->chan);

	ret = fwnode_property_read_u32(child, "adi,rsense-val-milli-ohms", &temp);
	if (ret)
		return dev_err_ptr_probe(&st->spi->dev, -EINVAL,
					 "Property adi,rsense-val-milli-ohms missing\n");
	/*
	 * Times 1000 because we have milli-ohms and __convert_to_raw
	 * expects scales of 1000000 which are used for all other
	 * properties.
	 * 2^10 resolution
	 */
	rsense->r_sense_val = __convert_to_raw((u64)temp * 1000, 1024);

	/* set common parameters */
	rsense->sensor.assign_chan = ltc2983_r_sense_assign_chan;

	return &rsense->sensor;
}

static struct ltc2983_sensor *ltc2983_adc_new(struct fwnode_handle *child,
					 struct ltc2983_data *st,
					 const struct ltc2983_sensor *sensor)
{
	struct ltc2983_adc *adc;

	adc = devm_kzalloc(&st->spi->dev, sizeof(*adc), GFP_KERNEL);
	if (!adc)
		return ERR_PTR(-ENOMEM);

	if (fwnode_property_read_bool(child, "adi,single-ended"))
		adc->single_ended = true;

	if (!adc->single_ended && sensor->chan < LTC2983_DIFFERENTIAL_CHAN_MIN)
		return dev_err_ptr_probe(&st->spi->dev, -EINVAL,
					 "Invalid chan:%d for differential adc\n",
					 sensor->chan);

	/* set common parameters */
	adc->sensor.assign_chan = ltc2983_adc_assign_chan;
	adc->sensor.fault_handler = ltc2983_common_fault_handler;

	return &adc->sensor;
}

static struct ltc2983_sensor *ltc2983_temp_new(struct fwnode_handle *child,
					       struct ltc2983_data *st,
					       const struct ltc2983_sensor *sensor)
{
	struct ltc2983_temp *temp;

	temp = devm_kzalloc(&st->spi->dev, sizeof(*temp), GFP_KERNEL);
	if (!temp)
		return ERR_PTR(-ENOMEM);

	if (fwnode_property_read_bool(child, "adi,single-ended"))
		temp->single_ended = true;

	if (!temp->single_ended && sensor->chan < LTC2983_DIFFERENTIAL_CHAN_MIN)
		return dev_err_ptr_probe(&st->spi->dev, -EINVAL,
					 "Invalid chan:%d for differential temp\n",
					 sensor->chan);

	temp->custom = __ltc2983_custom_sensor_new(st, child, "adi,custom-temp",
						   false, 4096, true);
	if (IS_ERR(temp->custom))
		return ERR_CAST(temp->custom);

	/* set common parameters */
	temp->sensor.assign_chan = ltc2983_temp_assign_chan;
	temp->sensor.fault_handler = ltc2983_common_fault_handler;

	return &temp->sensor;
}

static int ltc2983_chan_read(struct ltc2983_data *st,
			const struct ltc2983_sensor *sensor, int *val)
{
	u32 start_conversion = 0;
	int ret;
	unsigned long time;

	start_conversion = LTC2983_STATUS_START(true);
	start_conversion |= LTC2983_STATUS_CHAN_SEL(sensor->chan);
	dev_dbg(&st->spi->dev, "Start conversion on chan:%d, status:%02X\n",
		sensor->chan, start_conversion);
	/* start conversion */
	ret = regmap_write(st->regmap, LTC2983_STATUS_REG, start_conversion);
	if (ret)
		return ret;

	reinit_completion(&st->completion);
	/*
	 * wait for conversion to complete.
	 * 300 ms should be more than enough to complete the conversion.
	 * Depending on the sensor configuration, there are 2/3 conversions
	 * cycles of 82ms.
	 */
	time = wait_for_completion_timeout(&st->completion,
					   msecs_to_jiffies(300));
	if (!time) {
		dev_warn(&st->spi->dev, "Conversion timed out\n");
		return -ETIMEDOUT;
	}

	/* read the converted data */
	ret = regmap_bulk_read(st->regmap, LTC2983_CHAN_RES_ADDR(sensor->chan),
			       &st->temp, sizeof(st->temp));
	if (ret)
		return ret;

	*val = __be32_to_cpu(st->temp);

	if (!(LTC2983_RES_VALID_MASK & *val)) {
		dev_err(&st->spi->dev, "Invalid conversion detected\n");
		return -EIO;
	}

	ret = sensor->fault_handler(st, *val);
	if (ret)
		return ret;

	*val = sign_extend32((*val) & LTC2983_DATA_MASK, LTC2983_DATA_SIGN_BIT);
	return 0;
}

static int ltc2983_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int *val, int *val2, long mask)
{
	struct ltc2983_data *st = iio_priv(indio_dev);
	int ret;

	/* sanity check */
	if (chan->address >= st->num_channels) {
		dev_err(&st->spi->dev, "Invalid chan address:%ld",
			chan->address);
		return -EINVAL;
	}

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		mutex_lock(&st->lock);
		ret = ltc2983_chan_read(st, st->sensors[chan->address], val);
		mutex_unlock(&st->lock);
		return ret ?: IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		switch (chan->type) {
		case IIO_TEMP:
			/* value in milli degrees */
			*val = 1000;
			/* 2^10 */
			*val2 = 1024;
			return IIO_VAL_FRACTIONAL;
		case IIO_VOLTAGE:
			/* value in millivolt */
			*val = 1000;
			/* 2^21 */
			*val2 = 2097152;
			return IIO_VAL_FRACTIONAL;
		default:
			return -EINVAL;
		}
	}

	return -EINVAL;
}

static int ltc2983_reg_access(struct iio_dev *indio_dev,
			      unsigned int reg,
			      unsigned int writeval,
			      unsigned int *readval)
{
	struct ltc2983_data *st = iio_priv(indio_dev);

	if (readval)
		return regmap_read(st->regmap, reg, readval);

	return regmap_write(st->regmap, reg, writeval);
}

static irqreturn_t ltc2983_irq_handler(int irq, void *data)
{
	struct ltc2983_data *st = data;

	complete(&st->completion);
	return IRQ_HANDLED;
}

#define LTC2983_CHAN(__type, index, __address) ({ \
	struct iio_chan_spec __chan = { \
		.type = __type, \
		.indexed = 1, \
		.channel = index, \
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW), \
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE), \
		.address = __address, \
	}; \
	__chan; \
})

static int ltc2983_parse_fw(struct ltc2983_data *st)
{
	struct device *dev = &st->spi->dev;
	int ret, chan = 0, channel_avail_mask = 0;

	device_property_read_u32(dev, "adi,mux-delay-config-us", &st->mux_delay_config);

	device_property_read_u32(dev, "adi,filter-notch-freq", &st->filter_notch_freq);

	st->num_channels = device_get_child_node_count(dev);
	if (!st->num_channels)
		return dev_err_probe(&st->spi->dev, -EINVAL,
				     "At least one channel must be given!\n");

	st->sensors = devm_kcalloc(dev, st->num_channels, sizeof(*st->sensors),
				   GFP_KERNEL);
	if (!st->sensors)
		return -ENOMEM;

	st->iio_channels = st->num_channels;
	device_for_each_child_node_scoped(dev, child) {
		struct ltc2983_sensor sensor;

		ret = fwnode_property_read_u32(child, "reg", &sensor.chan);
		if (ret)
			return dev_err_probe(dev, ret,
				"reg property must given for child nodes\n");

		/* check if we have a valid channel */
		if (sensor.chan < LTC2983_MIN_CHANNELS_NR ||
		    sensor.chan > st->info->max_channels_nr)
			return dev_err_probe(dev, -EINVAL,
					     "chan:%d must be from %u to %u\n",
					     sensor.chan,
					     LTC2983_MIN_CHANNELS_NR,
					     st->info->max_channels_nr);

		if (channel_avail_mask & BIT(sensor.chan))
			return dev_err_probe(dev, -EINVAL,
					     "chan:%d already in use\n",
					     sensor.chan);

		ret = fwnode_property_read_u32(child, "adi,sensor-type", &sensor.type);
		if (ret)
			return dev_err_probe(dev, ret,
				"adi,sensor-type property must given for child nodes\n");

		dev_dbg(dev, "Create new sensor, type %u, chann %u",
			sensor.type, sensor.chan);

		if (sensor.type >= LTC2983_SENSOR_THERMOCOUPLE &&
		    sensor.type <= LTC2983_SENSOR_THERMOCOUPLE_CUSTOM) {
			st->sensors[chan] = ltc2983_thermocouple_new(child, st,
								     &sensor);
		} else if (sensor.type >= LTC2983_SENSOR_RTD &&
			   sensor.type <= LTC2983_SENSOR_RTD_CUSTOM) {
			st->sensors[chan] = ltc2983_rtd_new(child, st, &sensor);
		} else if (sensor.type >= LTC2983_SENSOR_THERMISTOR &&
			   sensor.type <= LTC2983_SENSOR_THERMISTOR_CUSTOM) {
			st->sensors[chan] = ltc2983_thermistor_new(child, st,
								   &sensor);
		} else if (sensor.type == LTC2983_SENSOR_DIODE) {
			st->sensors[chan] = ltc2983_diode_new(child, st,
							      &sensor);
		} else if (sensor.type == LTC2983_SENSOR_SENSE_RESISTOR) {
			st->sensors[chan] = ltc2983_r_sense_new(child, st,
								&sensor);
			/* don't add rsense to iio */
			st->iio_channels--;
		} else if (sensor.type == LTC2983_SENSOR_DIRECT_ADC) {
			st->sensors[chan] = ltc2983_adc_new(child, st, &sensor);
		} else if (st->info->has_temp &&
			   sensor.type == LTC2983_SENSOR_ACTIVE_TEMP) {
			st->sensors[chan] = ltc2983_temp_new(child, st, &sensor);
		} else {
			return dev_err_probe(dev, -EINVAL,
					     "Unknown sensor type %d\n",
					     sensor.type);
		}

		if (IS_ERR(st->sensors[chan]))
			return dev_err_probe(dev, PTR_ERR(st->sensors[chan]),
					     "Failed to create sensor\n");

		/* set generic sensor parameters */
		st->sensors[chan]->chan = sensor.chan;
		st->sensors[chan]->type = sensor.type;

		channel_avail_mask |= BIT(sensor.chan);
		chan++;
	}

	return 0;
}

static int ltc2983_eeprom_cmd(struct ltc2983_data *st, unsigned int cmd,
			      unsigned int wait_time, unsigned int status_reg,
			      unsigned long status_fail_mask)
{
	unsigned long time;
	unsigned int val;
	int ret;

	ret = regmap_bulk_write(st->regmap, LTC2983_EEPROM_KEY_REG,
				&st->eeprom_key, sizeof(st->eeprom_key));
	if (ret)
		return ret;

	reinit_completion(&st->completion);

	ret = regmap_write(st->regmap, LTC2983_STATUS_REG,
			   LTC2983_STATUS_START(true) | cmd);
	if (ret)
		return ret;

	time = wait_for_completion_timeout(&st->completion,
					   msecs_to_jiffies(wait_time));
	if (!time)
		return dev_err_probe(&st->spi->dev, -ETIMEDOUT,
				     "EEPROM command timed out\n");

	ret = regmap_read(st->regmap, status_reg, &val);
	if (ret)
		return ret;

	if (val & status_fail_mask)
		return dev_err_probe(&st->spi->dev, -EINVAL,
				     "EEPROM command failed: 0x%02X\n", val);

	return 0;
}

static int ltc2983_setup(struct ltc2983_data *st, bool assign_iio)
{
	u32 iio_chan_t = 0, iio_chan_v = 0, chan, iio_idx = 0, status;
	int ret;

	/* make sure the device is up: start bit (7) is 0 and done bit (6) is 1 */
	ret = regmap_read_poll_timeout(st->regmap, LTC2983_STATUS_REG, status,
				       LTC2983_STATUS_UP(status) == 1, 25000,
				       25000 * 10);
	if (ret)
		return dev_err_probe(&st->spi->dev, ret,
				     "Device startup timed out\n");

	ret = regmap_update_bits(st->regmap, LTC2983_GLOBAL_CONFIG_REG,
				 LTC2983_NOTCH_FREQ_MASK,
				 LTC2983_NOTCH_FREQ(st->filter_notch_freq));
	if (ret)
		return ret;

	ret = regmap_write(st->regmap, LTC2983_MUX_CONFIG_REG,
			   st->mux_delay_config);
	if (ret)
		return ret;

	if (st->info->has_eeprom && !assign_iio) {
		ret = ltc2983_eeprom_cmd(st, LTC2983_EEPROM_READ_CMD,
					 LTC2983_EEPROM_READ_TIME_MS,
					 LTC2983_EEPROM_READ_STATUS_REG,
					 LTC2983_EEPROM_READ_FAILURE_MASK);
		if (!ret)
			return 0;
	}

	for (chan = 0; chan < st->num_channels; chan++) {
		u32 chan_type = 0, *iio_chan;

		ret = st->sensors[chan]->assign_chan(st, st->sensors[chan]);
		if (ret)
			return ret;
		/*
		 * The assign_iio flag is necessary for when the device is
		 * coming out of sleep. In that case, we just need to
		 * re-configure the device channels.
		 * We also don't assign iio channels for rsense.
		 */
		if (st->sensors[chan]->type == LTC2983_SENSOR_SENSE_RESISTOR ||
		    !assign_iio)
			continue;

		/* assign iio channel */
		if (st->sensors[chan]->type != LTC2983_SENSOR_DIRECT_ADC) {
			chan_type = IIO_TEMP;
			iio_chan = &iio_chan_t;
		} else {
			chan_type = IIO_VOLTAGE;
			iio_chan = &iio_chan_v;
		}

		/*
		 * add chan as the iio .address so that, we can directly
		 * reference the sensor given the iio_chan_spec
		 */
		st->iio_chan[iio_idx++] = LTC2983_CHAN(chan_type, (*iio_chan)++,
						       chan);
	}

	return 0;
}

static const struct regmap_range ltc2983_reg_ranges[] = {
	regmap_reg_range(LTC2983_STATUS_REG, LTC2983_STATUS_REG),
	regmap_reg_range(LTC2983_TEMP_RES_START_REG, LTC2983_TEMP_RES_END_REG),
	regmap_reg_range(LTC2983_EEPROM_KEY_REG, LTC2983_EEPROM_KEY_REG),
	regmap_reg_range(LTC2983_EEPROM_READ_STATUS_REG,
			 LTC2983_EEPROM_READ_STATUS_REG),
	regmap_reg_range(LTC2983_GLOBAL_CONFIG_REG, LTC2983_GLOBAL_CONFIG_REG),
	regmap_reg_range(LTC2983_MULT_CHANNEL_START_REG,
			 LTC2983_MULT_CHANNEL_END_REG),
	regmap_reg_range(LTC2986_EEPROM_STATUS_REG, LTC2986_EEPROM_STATUS_REG),
	regmap_reg_range(LTC2983_MUX_CONFIG_REG, LTC2983_MUX_CONFIG_REG),
	regmap_reg_range(LTC2983_CHAN_ASSIGN_START_REG,
			 LTC2983_CHAN_ASSIGN_END_REG),
	regmap_reg_range(LTC2983_CUST_SENS_TBL_START_REG,
			 LTC2983_CUST_SENS_TBL_END_REG),
};

static const struct regmap_access_table ltc2983_reg_table = {
	.yes_ranges = ltc2983_reg_ranges,
	.n_yes_ranges = ARRAY_SIZE(ltc2983_reg_ranges),
};

/*
 *  The reg_bits are actually 12 but the device needs the first *complete*
 *  byte for the command (R/W).
 */
static const struct regmap_config ltc2983_regmap_config = {
	.reg_bits = 24,
	.val_bits = 8,
	.wr_table = &ltc2983_reg_table,
	.rd_table = &ltc2983_reg_table,
	.read_flag_mask = GENMASK(1, 0),
	.write_flag_mask = BIT(1),
};

static const struct  iio_info ltc2983_iio_info = {
	.read_raw = ltc2983_read_raw,
	.debugfs_reg_access = ltc2983_reg_access,
};

static int ltc2983_probe(struct spi_device *spi)
{
	struct ltc2983_data *st;
	struct iio_dev *indio_dev;
	struct gpio_desc *gpio;
	int ret;

	indio_dev = devm_iio_device_alloc(&spi->dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;

	st = iio_priv(indio_dev);

	st->info = spi_get_device_match_data(spi);
	if (!st->info)
		return -ENODEV;

	st->regmap = devm_regmap_init_spi(spi, &ltc2983_regmap_config);
	if (IS_ERR(st->regmap))
		return dev_err_probe(&spi->dev, PTR_ERR(st->regmap),
				     "Failed to initialize regmap\n");

	mutex_init(&st->lock);
	init_completion(&st->completion);
	st->spi = spi;
	st->eeprom_key = cpu_to_be32(LTC2983_EEPROM_KEY);
	spi_set_drvdata(spi, st);

	ret = ltc2983_parse_fw(st);
	if (ret)
		return ret;

	ret = devm_regulator_get_enable(&spi->dev, "vdd");
	if (ret)
		return ret;

	gpio = devm_gpiod_get_optional(&st->spi->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(gpio))
		return PTR_ERR(gpio);

	if (gpio) {
		/* bring the device out of reset */
		usleep_range(1000, 1200);
		gpiod_set_value_cansleep(gpio, 0);
	}

	st->iio_chan = devm_kzalloc(&spi->dev,
				    st->iio_channels * sizeof(*st->iio_chan),
				    GFP_KERNEL);
	if (!st->iio_chan)
		return -ENOMEM;

	ret = ltc2983_setup(st, true);
	if (ret)
		return ret;

	ret = devm_request_irq(&spi->dev, spi->irq, ltc2983_irq_handler,
			       IRQF_TRIGGER_RISING, st->info->name, st);
	if (ret)
		return dev_err_probe(&spi->dev, ret,
				     "failed to request an irq\n");

	if (st->info->has_eeprom) {
		ret = ltc2983_eeprom_cmd(st, LTC2983_EEPROM_WRITE_CMD,
					 LTC2983_EEPROM_WRITE_TIME_MS,
					 LTC2986_EEPROM_STATUS_REG,
					 LTC2983_EEPROM_STATUS_FAILURE_MASK);
		if (ret)
			return ret;
	}

	indio_dev->name = st->info->name;
	indio_dev->num_channels = st->iio_channels;
	indio_dev->channels = st->iio_chan;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->info = &ltc2983_iio_info;

	return devm_iio_device_register(&spi->dev, indio_dev);
}

static int ltc2983_resume(struct device *dev)
{
	struct ltc2983_data *st = spi_get_drvdata(to_spi_device(dev));
	int dummy;

	/* dummy read to bring the device out of sleep */
	regmap_read(st->regmap, LTC2983_STATUS_REG, &dummy);
	/* we need to re-assign the channels */
	return ltc2983_setup(st, false);
}

static int ltc2983_suspend(struct device *dev)
{
	struct ltc2983_data *st = spi_get_drvdata(to_spi_device(dev));

	return regmap_write(st->regmap, LTC2983_STATUS_REG, LTC2983_SLEEP);
}

static DEFINE_SIMPLE_DEV_PM_OPS(ltc2983_pm_ops, ltc2983_suspend,
				ltc2983_resume);

static const struct ltc2983_chip_info ltc2983_chip_info_data = {
	.name = "ltc2983",
	.max_channels_nr = 20,
};

static const struct ltc2983_chip_info ltc2984_chip_info_data = {
	.name = "ltc2984",
	.max_channels_nr = 20,
	.has_eeprom = true,
};

static const struct ltc2983_chip_info ltc2986_chip_info_data = {
	.name = "ltc2986",
	.max_channels_nr = 10,
	.has_temp = true,
	.has_eeprom = true,
};

static const struct ltc2983_chip_info ltm2985_chip_info_data = {
	.name = "ltm2985",
	.max_channels_nr = 10,
	.has_temp = true,
	.has_eeprom = true,
};

static const struct spi_device_id ltc2983_id_table[] = {
	{ "ltc2983", (kernel_ulong_t)&ltc2983_chip_info_data },
	{ "ltc2984", (kernel_ulong_t)&ltc2984_chip_info_data },
	{ "ltc2986", (kernel_ulong_t)&ltc2986_chip_info_data },
	{ "ltm2985", (kernel_ulong_t)&ltm2985_chip_info_data },
	{ }
};
MODULE_DEVICE_TABLE(spi, ltc2983_id_table);

static const struct of_device_id ltc2983_of_match[] = {
	{ .compatible = "adi,ltc2983", .data = &ltc2983_chip_info_data },
	{ .compatible = "adi,ltc2984", .data = &ltc2984_chip_info_data },
	{ .compatible = "adi,ltc2986", .data = &ltc2986_chip_info_data },
	{ .compatible = "adi,ltm2985", .data = &ltm2985_chip_info_data },
	{ }
};
MODULE_DEVICE_TABLE(of, ltc2983_of_match);

static struct spi_driver ltc2983_driver = {
	.driver = {
		.name = "ltc2983",
		.of_match_table = ltc2983_of_match,
		.pm = pm_sleep_ptr(&ltc2983_pm_ops),
	},
	.probe = ltc2983_probe,
	.id_table = ltc2983_id_table,
};

module_spi_driver(ltc2983_driver);

MODULE_AUTHOR("Nuno Sa <nuno.sa@analog.com>");
MODULE_DESCRIPTION("Analog Devices LTC2983 SPI Temperature sensors");
MODULE_LICENSE("GPL");
