/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CZ.NIC's Turris Omnia MCU I2C interface commands definitions
 *
 * 2024 by Marek Behún <kabel@kernel.org>
 */

#ifndef __TURRIS_OMNIA_MCU_INTERFACE_H
#define __TURRIS_OMNIA_MCU_INTERFACE_H

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/types.h>
#include <linux/unaligned.h>
#include <asm/byteorder.h>

enum omnia_commands_e {
	OMNIA_CMD_GET_STATUS_WORD		= 0x01, /* slave sends status word back */
	OMNIA_CMD_GENERAL_CONTROL		= 0x02,
	OMNIA_CMD_LED_MODE			= 0x03, /* default/user */
	OMNIA_CMD_LED_STATE			= 0x04, /* LED on/off */
	OMNIA_CMD_LED_COLOR			= 0x05, /* LED number + RED + GREEN + BLUE */
	OMNIA_CMD_USER_VOLTAGE			= 0x06,
	OMNIA_CMD_SET_BRIGHTNESS		= 0x07,
	OMNIA_CMD_GET_BRIGHTNESS		= 0x08,
	OMNIA_CMD_GET_RESET			= 0x09,
	OMNIA_CMD_GET_FW_VERSION_APP		= 0x0A, /* 20B git hash number */
	OMNIA_CMD_SET_WATCHDOG_STATE		= 0x0B, /* 0 - disable
							 * 1 - enable / ping
							 * after boot watchdog is started
							 * with 2 minutes timeout
							 */

	/* OMNIA_CMD_WATCHDOG_STATUS		= 0x0C, not implemented anymore */

	OMNIA_CMD_GET_WATCHDOG_STATE		= 0x0D,
	OMNIA_CMD_GET_FW_VERSION_BOOT		= 0x0E, /* 20B Git hash number */
	OMNIA_CMD_GET_FW_CHECKSUM		= 0x0F, /* 4B length, 4B checksum */

	/* available if FEATURES_SUPPORTED bit set in status word */
	OMNIA_CMD_GET_FEATURES			= 0x10,

	/* available if EXT_CMD bit set in features */
	OMNIA_CMD_GET_EXT_STATUS_DWORD		= 0x11,
	OMNIA_CMD_EXT_CONTROL			= 0x12,
	OMNIA_CMD_GET_EXT_CONTROL_STATUS	= 0x13,

	/* available if NEW_INT_API bit set in features */
	OMNIA_CMD_GET_INT_AND_CLEAR		= 0x14,
	OMNIA_CMD_GET_INT_MASK			= 0x15,
	OMNIA_CMD_SET_INT_MASK			= 0x16,

	/* available if FLASHING bit set in features */
	OMNIA_CMD_FLASH				= 0x19,

	/* available if WDT_PING bit set in features */
	OMNIA_CMD_SET_WDT_TIMEOUT		= 0x20,
	OMNIA_CMD_GET_WDT_TIMELEFT		= 0x21,

	/* available if POWEROFF_WAKEUP bit set in features */
	OMNIA_CMD_SET_WAKEUP			= 0x22,
	OMNIA_CMD_GET_UPTIME_AND_WAKEUP		= 0x23,
	OMNIA_CMD_POWER_OFF			= 0x24,

	/* available if USB_OVC_PROT_SETTING bit set in features */
	OMNIA_CMD_SET_USB_OVC_PROT		= 0x25,
	OMNIA_CMD_GET_USB_OVC_PROT		= 0x26,

	/* available if TRNG bit set in features */
	OMNIA_CMD_TRNG_COLLECT_ENTROPY		= 0x28,

	/* available if CRYPTO bit set in features */
	OMNIA_CMD_CRYPTO_GET_PUBLIC_KEY		= 0x29,
	OMNIA_CMD_CRYPTO_SIGN_MESSAGE		= 0x2A,
	OMNIA_CMD_CRYPTO_COLLECT_SIGNATURE	= 0x2B,

	/* available if BOARD_INFO it set in features */
	OMNIA_CMD_BOARD_INFO_GET		= 0x2C,
	OMNIA_CMD_BOARD_INFO_BURN		= 0x2D,

	/* available only at address 0x2b (LED-controller) */
	/* available only if LED_GAMMA_CORRECTION bit set in features */
	OMNIA_CMD_SET_GAMMA_CORRECTION		= 0x30,
	OMNIA_CMD_GET_GAMMA_CORRECTION		= 0x31,

	/* available only at address 0x2b (LED-controller) */
	/* available only if PER_LED_CORRECTION bit set in features */
	/* available only if FROM_BIT_16_INVALID bit NOT set in features */
	OMNIA_CMD_SET_LED_CORRECTIONS		= 0x32,
	OMNIA_CMD_GET_LED_CORRECTIONS		= 0x33,
};

enum omnia_flashing_commands_e {
	OMNIA_FLASH_CMD_UNLOCK		= 0x01,
	OMNIA_FLASH_CMD_SIZE_AND_CSUM	= 0x02,
	OMNIA_FLASH_CMD_PROGRAM		= 0x03,
	OMNIA_FLASH_CMD_RESET		= 0x04,
};

enum omnia_sts_word_e {
	OMNIA_STS_MCU_TYPE_MASK			= GENMASK(1, 0),
	OMNIA_STS_MCU_TYPE_STM32		= FIELD_PREP_CONST(OMNIA_STS_MCU_TYPE_MASK, 0),
	OMNIA_STS_MCU_TYPE_GD32			= FIELD_PREP_CONST(OMNIA_STS_MCU_TYPE_MASK, 1),
	OMNIA_STS_MCU_TYPE_MKL			= FIELD_PREP_CONST(OMNIA_STS_MCU_TYPE_MASK, 2),
	OMNIA_STS_FEATURES_SUPPORTED		= BIT(2),
	OMNIA_STS_USER_REGULATOR_NOT_SUPPORTED	= BIT(3),
	OMNIA_STS_CARD_DET			= BIT(4),
	OMNIA_STS_MSATA_IND			= BIT(5),
	OMNIA_STS_USB30_OVC			= BIT(6),
	OMNIA_STS_USB31_OVC			= BIT(7),
	OMNIA_STS_USB30_PWRON			= BIT(8),
	OMNIA_STS_USB31_PWRON			= BIT(9),
	OMNIA_STS_ENABLE_4V5			= BIT(10),
	OMNIA_STS_BUTTON_MODE			= BIT(11),
	OMNIA_STS_BUTTON_PRESSED		= BIT(12),
	OMNIA_STS_BUTTON_COUNTER_MASK		= GENMASK(15, 13),
};

enum omnia_ctl_byte_e {
	OMNIA_CTL_LIGHT_RST	= BIT(0),
	OMNIA_CTL_HARD_RST	= BIT(1),
	/* BIT(2) is currently reserved */
	OMNIA_CTL_USB30_PWRON	= BIT(3),
	OMNIA_CTL_USB31_PWRON	= BIT(4),
	OMNIA_CTL_ENABLE_4V5	= BIT(5),
	OMNIA_CTL_BUTTON_MODE	= BIT(6),
	OMNIA_CTL_BOOTLOADER	= BIT(7),
};

enum omnia_features_e {
	OMNIA_FEAT_PERIPH_MCU		= BIT(0),
	OMNIA_FEAT_EXT_CMDS		= BIT(1),
	OMNIA_FEAT_WDT_PING		= BIT(2),
	OMNIA_FEAT_LED_STATE_EXT_MASK	= GENMASK(4, 3),
	OMNIA_FEAT_LED_STATE_EXT	= FIELD_PREP_CONST(OMNIA_FEAT_LED_STATE_EXT_MASK, 1),
	OMNIA_FEAT_LED_STATE_EXT_V32	= FIELD_PREP_CONST(OMNIA_FEAT_LED_STATE_EXT_MASK, 2),
	OMNIA_FEAT_LED_GAMMA_CORRECTION	= BIT(5),
	OMNIA_FEAT_NEW_INT_API		= BIT(6),
	OMNIA_FEAT_BOOTLOADER		= BIT(7),
	OMNIA_FEAT_FLASHING		= BIT(8),
	OMNIA_FEAT_NEW_MESSAGE_API	= BIT(9),
	OMNIA_FEAT_BRIGHTNESS_INT	= BIT(10),
	OMNIA_FEAT_POWEROFF_WAKEUP	= BIT(11),
	OMNIA_FEAT_CAN_OLD_MESSAGE_API	= BIT(12),
	OMNIA_FEAT_TRNG			= BIT(13),
	OMNIA_FEAT_CRYPTO		= BIT(14),
	OMNIA_FEAT_BOARD_INFO		= BIT(15),

	/*
	 * Orginally the features command replied only 16 bits. If more were
	 * read, either the I2C transaction failed or 0xff bytes were sent.
	 * Therefore to consider bits 16 - 31 valid, one bit (20) was reserved
	 * to be zero.
	 */

	/* Bits 16 - 19 correspond to bits 0 - 3 of status word */
	OMNIA_FEAT_MCU_TYPE_MASK		= GENMASK(17, 16),
	OMNIA_FEAT_MCU_TYPE_STM32		= FIELD_PREP_CONST(OMNIA_FEAT_MCU_TYPE_MASK, 0),
	OMNIA_FEAT_MCU_TYPE_GD32		= FIELD_PREP_CONST(OMNIA_FEAT_MCU_TYPE_MASK, 1),
	OMNIA_FEAT_MCU_TYPE_MKL			= FIELD_PREP_CONST(OMNIA_FEAT_MCU_TYPE_MASK, 2),
	OMNIA_FEAT_FEATURES_SUPPORTED		= BIT(18),
	OMNIA_FEAT_USER_REGULATOR_NOT_SUPPORTED	= BIT(19),

	/* must not be set */
	OMNIA_FEAT_FROM_BIT_16_INVALID	= BIT(20),

	OMNIA_FEAT_PER_LED_CORRECTION	= BIT(21),
	OMNIA_FEAT_USB_OVC_PROT_SETTING	= BIT(22),
};

enum omnia_ext_sts_dword_e {
	OMNIA_EXT_STS_SFP_nDET		= BIT(0),
	OMNIA_EXT_STS_LED_STATES_MASK	= GENMASK(31, 12),
	OMNIA_EXT_STS_WLAN0_MSATA_LED	= BIT(12),
	OMNIA_EXT_STS_WLAN1_LED		= BIT(13),
	OMNIA_EXT_STS_WLAN2_LED		= BIT(14),
	OMNIA_EXT_STS_WPAN0_LED		= BIT(15),
	OMNIA_EXT_STS_WPAN1_LED		= BIT(16),
	OMNIA_EXT_STS_WPAN2_LED		= BIT(17),
	OMNIA_EXT_STS_WAN_LED0		= BIT(18),
	OMNIA_EXT_STS_WAN_LED1		= BIT(19),
	OMNIA_EXT_STS_LAN0_LED0		= BIT(20),
	OMNIA_EXT_STS_LAN0_LED1		= BIT(21),
	OMNIA_EXT_STS_LAN1_LED0		= BIT(22),
	OMNIA_EXT_STS_LAN1_LED1		= BIT(23),
	OMNIA_EXT_STS_LAN2_LED0		= BIT(24),
	OMNIA_EXT_STS_LAN2_LED1		= BIT(25),
	OMNIA_EXT_STS_LAN3_LED0		= BIT(26),
	OMNIA_EXT_STS_LAN3_LED1		= BIT(27),
	OMNIA_EXT_STS_LAN4_LED0		= BIT(28),
	OMNIA_EXT_STS_LAN4_LED1		= BIT(29),
	OMNIA_EXT_STS_LAN5_LED0		= BIT(30),
	OMNIA_EXT_STS_LAN5_LED1		= BIT(31),
};

enum omnia_ext_ctl_e {
	OMNIA_EXT_CTL_nRES_MMC		= BIT(0),
	OMNIA_EXT_CTL_nRES_LAN		= BIT(1),
	OMNIA_EXT_CTL_nRES_PHY		= BIT(2),
	OMNIA_EXT_CTL_nPERST0		= BIT(3),
	OMNIA_EXT_CTL_nPERST1		= BIT(4),
	OMNIA_EXT_CTL_nPERST2		= BIT(5),
	OMNIA_EXT_CTL_PHY_SFP		= BIT(6),
	OMNIA_EXT_CTL_PHY_SFP_AUTO	= BIT(7),
	OMNIA_EXT_CTL_nVHV_CTRL		= BIT(8),
};

enum omnia_int_e {
	OMNIA_INT_CARD_DET		= BIT(0),
	OMNIA_INT_MSATA_IND		= BIT(1),
	OMNIA_INT_USB30_OVC		= BIT(2),
	OMNIA_INT_USB31_OVC		= BIT(3),
	OMNIA_INT_BUTTON_PRESSED	= BIT(4),
	OMNIA_INT_SFP_nDET		= BIT(5),
	OMNIA_INT_BRIGHTNESS_CHANGED	= BIT(6),
	OMNIA_INT_TRNG			= BIT(7),
	OMNIA_INT_MESSAGE_SIGNED	= BIT(8),

	OMNIA_INT_LED_STATES_MASK	= GENMASK(31, 12),
	OMNIA_INT_WLAN0_MSATA_LED	= BIT(12),
	OMNIA_INT_WLAN1_LED		= BIT(13),
	OMNIA_INT_WLAN2_LED		= BIT(14),
	OMNIA_INT_WPAN0_LED		= BIT(15),
	OMNIA_INT_WPAN1_LED		= BIT(16),
	OMNIA_INT_WPAN2_LED		= BIT(17),
	OMNIA_INT_WAN_LED0		= BIT(18),
	OMNIA_INT_WAN_LED1		= BIT(19),
	OMNIA_INT_LAN0_LED0		= BIT(20),
	OMNIA_INT_LAN0_LED1		= BIT(21),
	OMNIA_INT_LAN1_LED0		= BIT(22),
	OMNIA_INT_LAN1_LED1		= BIT(23),
	OMNIA_INT_LAN2_LED0		= BIT(24),
	OMNIA_INT_LAN2_LED1		= BIT(25),
	OMNIA_INT_LAN3_LED0		= BIT(26),
	OMNIA_INT_LAN3_LED1		= BIT(27),
	OMNIA_INT_LAN4_LED0		= BIT(28),
	OMNIA_INT_LAN4_LED1		= BIT(29),
	OMNIA_INT_LAN5_LED0		= BIT(30),
	OMNIA_INT_LAN5_LED1		= BIT(31),
};

enum omnia_cmd_led_mode_e {
	OMNIA_CMD_LED_MODE_LED_MASK	= GENMASK(3, 0),
	OMNIA_CMD_LED_MODE_USER		= BIT(4),
};

#define OMNIA_CMD_LED_MODE_LED(_l)	FIELD_PREP(OMNIA_CMD_LED_MODE_LED_MASK, _l)

enum omnia_cmd_led_state_e {
	OMNIA_CMD_LED_STATE_LED_MASK	= GENMASK(3, 0),
	OMNIA_CMD_LED_STATE_ON		= BIT(4),
};

#define OMNIA_CMD_LED_STATE_LED(_l)	FIELD_PREP(OMNIA_CMD_LED_STATE_LED_MASK, _l)

enum omnia_cmd_poweroff_e {
	OMNIA_CMD_POWER_OFF_POWERON_BUTTON	= BIT(0),
	OMNIA_CMD_POWER_OFF_MAGIC		= 0xdead,
};

enum omnia_cmd_usb_ovc_prot_e {
	OMNIA_CMD_xET_USB_OVC_PROT_PORT_MASK	= GENMASK(3, 0),
	OMNIA_CMD_xET_USB_OVC_PROT_ENABLE	= BIT(4),
};

/* Command execution functions */

struct i2c_client;

int omnia_cmd_write_read(const struct i2c_client *client,
			 void *cmd, unsigned int cmd_len,
			 void *reply, unsigned int reply_len);

static inline int omnia_cmd_write(const struct i2c_client *client, void *cmd,
				  unsigned int len)
{
	return omnia_cmd_write_read(client, cmd, len, NULL, 0);
}

static inline int omnia_cmd_write_u8(const struct i2c_client *client, u8 cmd,
				     u8 val)
{
	u8 buf[2] = { cmd, val };

	return omnia_cmd_write(client, buf, sizeof(buf));
}

static inline int omnia_cmd_write_u16(const struct i2c_client *client, u8 cmd,
				      u16 val)
{
	u8 buf[3];

	buf[0] = cmd;
	put_unaligned_le16(val, &buf[1]);

	return omnia_cmd_write(client, buf, sizeof(buf));
}

static inline int omnia_cmd_write_u32(const struct i2c_client *client, u8 cmd,
				      u32 val)
{
	u8 buf[5];

	buf[0] = cmd;
	put_unaligned_le32(val, &buf[1]);

	return omnia_cmd_write(client, buf, sizeof(buf));
}

static inline int omnia_cmd_read(const struct i2c_client *client, u8 cmd,
				 void *reply, unsigned int len)
{
	return omnia_cmd_write_read(client, &cmd, 1, reply, len);
}

static inline unsigned int
omnia_compute_reply_length(unsigned long mask, bool interleaved,
			   unsigned int offset)
{
	if (!mask)
		return 0;

	return ((__fls(mask) >> 3) << interleaved) + 1 + offset;
}

/* Returns 0 on success */
static inline int omnia_cmd_read_bits(const struct i2c_client *client, u8 cmd,
				      unsigned long bits, unsigned long *dst)
{
	__le32 reply;
	int err;

	if (!bits) {
		*dst = 0;
		return 0;
	}

	err = omnia_cmd_read(client, cmd, &reply,
			     omnia_compute_reply_length(bits, false, 0));
	if (err)
		return err;

	*dst = le32_to_cpu(reply) & bits;

	return 0;
}

static inline int omnia_cmd_read_bit(const struct i2c_client *client, u8 cmd,
				     unsigned long bit)
{
	unsigned long reply;
	int err;

	err = omnia_cmd_read_bits(client, cmd, bit, &reply);
	if (err)
		return err;

	return !!reply;
}

static inline int omnia_cmd_read_u32(const struct i2c_client *client, u8 cmd,
				     u32 *dst)
{
	__le32 reply;
	int err;

	err = omnia_cmd_read(client, cmd, &reply, sizeof(reply));
	if (err)
		return err;

	*dst = le32_to_cpu(reply);

	return 0;
}

static inline int omnia_cmd_read_u16(const struct i2c_client *client, u8 cmd,
				     u16 *dst)
{
	__le16 reply;
	int err;

	err = omnia_cmd_read(client, cmd, &reply, sizeof(reply));
	if (err)
		return err;

	*dst = le16_to_cpu(reply);

	return 0;
}

static inline int omnia_cmd_read_u8(const struct i2c_client *client, u8 cmd,
				    u8 *reply)
{
	return omnia_cmd_read(client, cmd, reply, sizeof(*reply));
}

#endif /* __TURRIS_OMNIA_MCU_INTERFACE_H */
