// SPDX-License-Identifier: ISC
/*
 * Copyright (c) 2014 Broadcom Corporation
 */
#include <linux/init.h>
#include <linux/of.h>
#include <linux/of_irq.h>

#include <defs.h>
#include "debug.h"
#include "core.h"
#include "common.h"
#include "of.h"

/* TODO: FIXME: Use DT */
static void brcmf_of_probe_cc(struct device *dev,
			      struct brcmf_mp_device *settings)
{
	static struct brcmfmac_pd_cc_entry netgear_r8000_cc_ent[] = {
		{ "JP", "JP", 78 },
		{ "US", "Q2", 86 },
	};
	struct brcmfmac_pd_cc_entry *cc_ent = NULL;
	int table_size = 0;

	if (of_machine_is_compatible("netgear,r8000")) {
		cc_ent = netgear_r8000_cc_ent;
		table_size = ARRAY_SIZE(netgear_r8000_cc_ent);
	}

	if (cc_ent && table_size) {
		struct brcmfmac_pd_cc *cc;
		size_t memsize;

		memsize = table_size * sizeof(struct brcmfmac_pd_cc_entry);
		cc = devm_kzalloc(dev, sizeof(*cc) + memsize, GFP_KERNEL);
		if (!cc)
			return;
		cc->table_size = table_size;
		memcpy(cc->table, cc_ent, memsize);
		settings->country_codes = cc;
	}
}

void brcmf_of_probe(struct device *dev, enum brcmf_bus_type bus_type,
		    struct brcmf_mp_device *settings)
{
	struct brcmfmac_sdio_pd *sdio = &settings->bus.sdio;
	struct device_node *root, *np = dev->of_node;
	struct property *prop;
	int irq;
	u32 irqf;
	u32 val;

	/* Set board-type to the first string of the machine compatible prop */
	root = of_find_node_by_path("/");
	if (root) {
		prop = of_find_property(root, "compatible", NULL);
		settings->board_type = of_prop_next_string(prop, NULL);
		of_node_put(root);
	}

	brcmf_of_probe_cc(dev, settings);

	if (!np || bus_type != BRCMF_BUSTYPE_SDIO ||
	    !of_device_is_compatible(np, "brcm,bcm4329-fmac"))
		return;

	if (of_property_read_u32(np, "brcm,drive-strength", &val) == 0)
		sdio->drive_strength = val;

	/* make sure there are interrupts defined in the node */
	if (!of_find_property(np, "interrupts", NULL))
		return;

	irq = irq_of_parse_and_map(np, 0);
	if (!irq) {
		brcmf_err("interrupt could not be mapped\n");
		return;
	}
	irqf = irqd_get_trigger_type(irq_get_irq_data(irq));

	sdio->oob_irq_supported = true;
	sdio->oob_irq_nr = irq;
	sdio->oob_irq_flags = irqf;
}
