// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2019-2021 NXP
 *	Dong Aisheng <aisheng.dong@nxp.com>
 */

#include <dt-bindings/firmware/imx/rsrc.h>

#include "clk-scu.h"

/* Keep sorted in the ascending order */
static const u32 imx8qxp_clk_scu_rsrc_table[] = {
#ifndef CONFIG_VEHICLE_POST_INIT
	IMX_SC_R_DC_0_VIDEO0,
	IMX_SC_R_DC_0_VIDEO1,
	IMX_SC_R_DC_0,
	IMX_SC_R_DC_0_PLL_0,
	IMX_SC_R_DC_0_PLL_1,
#endif
	IMX_SC_R_SPI_0,
	IMX_SC_R_SPI_1,
	IMX_SC_R_SPI_2,
	IMX_SC_R_SPI_3,
	IMX_SC_R_UART_0,
	IMX_SC_R_UART_1,
	IMX_SC_R_UART_2,
	IMX_SC_R_UART_3,
	IMX_SC_R_I2C_0,
	IMX_SC_R_I2C_1,
	IMX_SC_R_I2C_2,
	IMX_SC_R_I2C_3,
	IMX_SC_R_ADC_0,
	IMX_SC_R_FTM_0,
	IMX_SC_R_FTM_1,
	IMX_SC_R_CAN_0,
	IMX_SC_R_GPU_0_PID0,
	IMX_SC_R_LCD_0,
	IMX_SC_R_LCD_0_PWM_0,
	IMX_SC_R_PWM_0,
	IMX_SC_R_PWM_1,
	IMX_SC_R_PWM_2,
	IMX_SC_R_PWM_3,
	IMX_SC_R_PWM_4,
	IMX_SC_R_PWM_5,
	IMX_SC_R_PWM_6,
	IMX_SC_R_PWM_7,
	IMX_SC_R_GPT_0,
	IMX_SC_R_GPT_1,
	IMX_SC_R_GPT_2,
	IMX_SC_R_GPT_3,
	IMX_SC_R_GPT_4,
	IMX_SC_R_FSPI_0,
	IMX_SC_R_FSPI_1,
	IMX_SC_R_SDHC_0,
	IMX_SC_R_SDHC_1,
	IMX_SC_R_SDHC_2,
	IMX_SC_R_ENET_0,
	IMX_SC_R_ENET_1,
	IMX_SC_R_USB_2,
	IMX_SC_R_NAND,
#ifndef CONFIG_VEHICLE_POST_INIT
	IMX_SC_R_LVDS_0,
#endif
	IMX_SC_R_LVDS_1,
	IMX_SC_R_M4_0_UART,
#ifndef CONFIG_VEHICLE_POST_INIT
	IMX_SC_R_M4_0_I2C,
#endif
	IMX_SC_R_ELCDIF_PLL,
	IMX_SC_R_AUDIO_PLL_0,
	IMX_SC_R_PI_0,
	IMX_SC_R_PI_0_PWM_0,
	IMX_SC_R_PI_0_I2C_0,
	IMX_SC_R_PI_0_PLL,
#ifndef CONFIG_VEHICLE_POST_INIT
	IMX_SC_R_MIPI_0,
	IMX_SC_R_MIPI_0_PWM_0,
	IMX_SC_R_MIPI_0_I2C_0,
	IMX_SC_R_MIPI_0_I2C_1,
#endif
	IMX_SC_R_MIPI_1,
	IMX_SC_R_MIPI_1_PWM_0,
	IMX_SC_R_MIPI_1_I2C_0,
	IMX_SC_R_MIPI_1_I2C_1,
#ifndef CONFIG_VEHICLE_POST_INIT
	IMX_SC_R_CSI_0,
	IMX_SC_R_CSI_0_PWM_0,
	IMX_SC_R_CSI_0_I2C_0,
#endif
	IMX_SC_R_AUDIO_PLL_1,
	IMX_SC_R_AUDIO_CLK_0,
	IMX_SC_R_AUDIO_CLK_1,
	IMX_SC_R_A35,
	IMX_SC_R_VPU_DEC_0,
	IMX_SC_R_VPU_ENC_0,
};

const struct imx_clk_scu_rsrc_table imx_clk_scu_rsrc_imx8qxp = {
	.rsrc = imx8qxp_clk_scu_rsrc_table,
	.num = ARRAY_SIZE(imx8qxp_clk_scu_rsrc_table),
};

#ifdef CONFIG_VEHICLE_POST_INIT
/* Keep sorted in the ascending order */
static u32 imx8qxp_clk_post_scu_rsrc_table[] = {
	IMX_SC_R_DC_0_VIDEO0,
	IMX_SC_R_DC_0_VIDEO1,
	IMX_SC_R_DC_0,
	IMX_SC_R_DC_0_PLL_0,
	IMX_SC_R_DC_0_PLL_1,
	IMX_SC_R_LVDS_0,
	IMX_SC_R_M4_0_I2C,
	IMX_SC_R_MIPI_0,
	IMX_SC_R_MIPI_0_PWM_0,
	IMX_SC_R_MIPI_0_I2C_0,
	IMX_SC_R_MIPI_0_I2C_1,
	IMX_SC_R_CSI_0,
	IMX_SC_R_CSI_0_PWM_0,
	IMX_SC_R_CSI_0_I2C_0,
};

const struct imx_clk_scu_rsrc_table imx_clk_post_scu_rsrc_imx8qxp = {
	.rsrc = imx8qxp_clk_post_scu_rsrc_table,
	.num = ARRAY_SIZE(imx8qxp_clk_post_scu_rsrc_table),
};
#endif
