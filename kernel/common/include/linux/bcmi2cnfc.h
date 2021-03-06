/*
 * Copyright (C) 2010 Broadcom Corporation.
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
 #include <plat/bcm_i2c.h>
#define BCMNFC_MAGIC	0xFA

/*
 * BCMNFC power control via ioctl
 * BCMNFC_POWER_CTL(0): power off
 * BCMNFC_POWER_CTL(1): power on
 * BCMNFC_POWER_CTL(2): Sleep off
 * BCMNFC_POWER_CTL(3): Sleep on
 */
#define BCMNFC_POWER_CTL	_IO(BCMNFC_MAGIC, 0x01)
#define BCMNFC_CHANGE_ADDR  _IO(BCMNFC_MAGIC, 0x02)

#define BCMNFC_POWER_OFF   0
#define BCMNFC_POWER_ON    1
#define BCMNFC_WAKE_OFF    2
#define BCMNFC_WAKE_ON     3


struct bcmi2cnfc_i2c_platform_data {
	unsigned int irq_gpio;
	unsigned int en_gpio;
	unsigned int wake_gpio;
	int (*init)(void);
	int (*reset)(void);
	struct i2c_slave_platform_data i2c_pdata;
};
