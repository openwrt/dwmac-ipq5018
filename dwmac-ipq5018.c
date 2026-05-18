#include <linux/clk.h>
#include <linux/of_mdio.h>
#include <linux/pcs/pcs.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/stmmac.h>

#include <qca_uniphy.h>

#include "stmmac_platform.h"

enum {
	IPQ5018_GMAC_CLK_SYS,
	IPQ5018_GMAC_CLK_CFG,
	IPQ5018_GMAC_CLK_PTP,
	IPQ5018_GMAC_CLK_AHB,
	IPQ5018_GMAC_CLK_AXI,
	IPQ5018_GMAC_CLK_RX,
	IPQ5018_GMAC_CLK_TX,
};

static const struct clk_bulk_data ipq5018_gmac_clks[] = {
	[IPQ5018_GMAC_CLK_SYS]	= { .id = "stmmaceth" },
	[IPQ5018_GMAC_CLK_CFG]	= { .id = "pclk" },
	[IPQ5018_GMAC_CLK_PTP]	= { .id = "ptp_ref" },
	[IPQ5018_GMAC_CLK_AHB]	= { .id = "ahb" },
	[IPQ5018_GMAC_CLK_AXI]	= { .id = "axi" },
	[IPQ5018_GMAC_CLK_RX]	= { .id = "rx" },
	[IPQ5018_GMAC_CLK_TX]	= { .id = "tx" },
};

struct ipq5018_gmac {
	struct device *dev;
	struct clk_bulk_data clks[ARRAY_SIZE(ipq5018_gmac_clks)];
};

static void ipq5018_gmac_fix_speed(void *priv, unsigned int speed, unsigned int mode)
{
	struct ipq5018_gmac *gmac = priv;
	unsigned long rate;

	dev_info(gmac->dev, "%s: speed=%u, mode=0x%x\n", __func__, speed, mode);

	switch(speed) {
		case SPEED_10:
			rate = 2500000;
			break;
		case SPEED_100:
			rate = 25000000;
			break;
		case SPEED_1000:
			rate = 125000000;
			break;
		case SPEED_2500:
			rate = 312500000;
			break;
		default:
			dev_err(gmac->dev, "Unsupported speed: %d\n", speed);
			rate = 125000000;
			break;
	}

	clk_set_rate(gmac->clks[IPQ5018_GMAC_CLK_RX].clk, rate);
	clk_set_rate(gmac->clks[IPQ5018_GMAC_CLK_TX].clk, rate);
}

static int ipq5018_gmac_fill_available_pcs(struct phylink_config *config,
					   struct phylink_pcs **available_pcs,
					   unsigned int num_available_pcs)
{
	struct device *dev = config->dev;

	return fwnode_phylink_pcs_parse(dev_fwnode(dev), available_pcs,
				        &num_available_pcs);
}

static int ipq5018_gmac_pcs_init(struct stmmac_priv *priv)
{
	struct phylink_config *config = priv->phylink_config;
	int ret;

	ret = fwnode_phylink_pcs_parse(dev_fwnode(&priv->dev->dev), NULL,
				       &config->num_available_pcs);
	if (ret) {
		dev_err(&priv->dev->dev, "failed to parse PCS from fwnode\n");
		return;
	}

	config->fill_available_pcs = ipq5018_gmac_fill_available_pcs;

	return 0;
}

static void ipq5018_gmac_get_interfaces(struct stmmac_priv *priv, void *bsp_priv,
				    unsigned long *interfaces)
{
	struct mac_device_info *mac = priv->hw;

	__set_bit(PHY_INTERFACE_MODE_SGMII, interfaces);
	__set_bit(PHY_INTERFACE_MODE_2500BASEX, interfaces);

	phy_interface_copy(config->pcs_interfaces, interfaces);

	/* 
	 * Synopsys DWMAC is IP version 3.7 is limited to 1 Gpbs.
	 * This vendor specific implementation supports 2.5 Gbps, so override
	 * the default mac link capabilities.
	 */
	mac->link.caps |= MAC_2500FD;
}

static int ipq5018_gmac_probe(struct platform_device *pdev)
{
	struct plat_stmmacenet_data *plat_dat;
	struct stmmac_resources stmmac_res;
	struct device *dev = &pdev->dev;
	struct ipq5018_gmac *gmac;
	int ret;

	ret = stmmac_get_platform_resources(pdev, &stmmac_res);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to get stmmac platform resources\n");

	plat_dat = devm_stmmac_probe_config_dt(pdev, stmmac_res.mac);
	if (IS_ERR_OR_NULL(plat_dat))
		return dev_err_probe(dev, PTR_ERR(plat_dat),
				     "failed to parse stmmac dt parameters\n");

	gmac = devm_kzalloc(dev, sizeof(*gmac), GFP_KERNEL);
	if (!gmac)
		return dev_err_probe(dev, -ENOMEM,
				     "failed to allocate priv\n");

	gmac->dev = dev;

	memcpy(gmac->clks, ipq5018_gmac_clks, sizeof(gmac->clks));
	ret = devm_clk_bulk_get_optional(dev, ARRAY_SIZE(gmac->clks), gmac->clks);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to acquire clocks\n");

	ret = clk_bulk_prepare_enable(ARRAY_SIZE(gmac->clks), gmac->clks);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to enable clocks\n");

	plat_dat->bsp_priv = gmac;
	plat_dat->max_speed = 2500;
	plat_dat->get_interfaces = ipq5018_gmac_get_interfaces;
	plat_dat->fix_mac_speed = ipq5018_gmac_fix_speed;
	plat_dat->pcs_init = ipq5018_gmac_pcs_init;

	return stmmac_dvr_probe(dev, plat_dat, &stmmac_res);
}

static const struct of_device_id ipq5018_gmac_dwmac_match[] = {
	{ .compatible = "qcom,ipq5018-gmac" },
	{ }
};
MODULE_DEVICE_TABLE(of, ipq5018_gmac_dwmac_match);

static struct platform_driver ipq5018_gmac_dwmac_driver = {
	.probe = ipq5018_gmac_probe,
	.driver = {
		.name		= "ipq5018-gmac-dwmac",
		.of_match_table	= ipq5018_gmac_dwmac_match,
	},
};
module_platform_driver(ipq5018_gmac_dwmac_driver);

MODULE_DESCRIPTION("Qualcomm IPQ5018 DWMAC driver");
MODULE_AUTHOR("George Moussalem <george.moussalem@outlook.com>");
