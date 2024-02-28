/*
 * Copyright (c) 2024 Centro de Inovacao EDGE
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/drivers/adc.h>
#include <zephyr/kernel.h>

/* Macro for building an array of configurations. */
#define ADC_CHANNEL_CONFIG_DT_AND_COMMA(node_id) ADC_CHANNEL_CFG_DT(node_id),

/* ADC node from the devicetree. */
#define ADC_NODE DT_ALIAS(adc)

/* Get the ADC resolution from one of its childs. */
#define ADC_RESOLUTION DT_PROP(DT_CHILD(ADC_NODE, channel_0), zephyr_resolution)

/* Number of samples to be made on the sequence. */
#define SEQUENCE_SAMPLES 5

/* Enumerate channels for convenience. */
enum sequence_channels {
	CHANNEL_A = 0,
	CHANNEL_B,

	CHANNEL_COUNT,
};

/* Data of ADC device specified in devicetree. */
static const struct device *adc = DEVICE_DT_GET(ADC_NODE);

/* Data array of ADC channels for the specified ADC. */
const struct adc_channel_cfg channel_cfgs[CHANNEL_COUNT] = {
	DT_FOREACH_CHILD(ADC_NODE, ADC_CHANNEL_CONFIG_DT_AND_COMMA)};

/* Options for the sequence sampling. */
const struct adc_sequence_options options = {
	.extra_samplings = SEQUENCE_SAMPLES - 1,
	.interval_us = 0,
};

int main(void)
{
	int err;
	uint32_t count = 0;
	uint16_t channel_reading[SEQUENCE_SAMPLES][CHANNEL_COUNT];

	/* Configure the sampling sequence to be made. */
	struct adc_sequence sequence = {
		.buffer = &channel_reading,
		/* buffer size in bytes, not number of samples */
		.buffer_size = sizeof(channel_reading),
		.channels = BIT(channel_cfgs[CHANNEL_A].channel_id) |
			    BIT(channel_cfgs[CHANNEL_B].channel_id),
		.resolution = ADC_RESOLUTION,
		.options = &options,
	};

	if (!device_is_ready(adc)) {
		printk("ADC controller device %s not ready\n", adc->name);
		return 0;
	}

	/* Configure channels individually prior to sampling. */
	for (size_t i = 0U; i < CHANNEL_COUNT; i++) {
		err = adc_channel_setup(adc, &channel_cfgs[i]);
		if (err < 0) {
			printk("Could not setup channel #%d (%d)\n", i, err);
			return 0;
		}
	}

	while (1) {
		printk("ADC sequence reading [%u]:\n", count++);

		err = adc_read(adc, &sequence);
		if (err < 0) {
			printk("Could not read (%d)\n", err);
			continue;
		}

		for (size_t i = 0U; i < CHANNEL_COUNT; i++) {
			int32_t val_mv;

			printk("- %s, channel %"PRId32", %"PRId32" sequence samples:\n",
			       adc->name, channel_cfgs[i].channel_id, SEQUENCE_SAMPLES);
			for (size_t j = 0U; j < SEQUENCE_SAMPLES; j++) {

				val_mv = channel_reading[j][i];

				printk("- - %"PRId32, val_mv);
				err = adc_raw_to_millivolts(channel_cfgs[i].reference,
							    channel_cfgs[i].gain, ADC_RESOLUTION,
							    &val_mv);

				/* conversion to mV may not be supported, skip if not */
				if ((err < 0) || channel_cfgs[i].reference == 0) {
					printk(" (value in mV not available)\n");
				} else {
					printk(" = %"PRId32"mV\n", val_mv);
				}
			}
		}

		k_msleep(1000);
	}

	return 0;
}
