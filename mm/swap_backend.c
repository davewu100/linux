// SPDX-License-Identifier: GPL-2.0
#include <linux/swap.h>
#include <linux/export.h>

#include "swap.h"

/*
 * Bind a swap area to a virtual-swap backend when swapon targets a
 * device that registers one (for example zram).
 */
void swap_backend_setup(struct swap_info_struct *si)
{
#if IS_ENABLED(CONFIG_ZRAM)
	if (!zram_setup_swap_backend(si))
		return;
#endif
}
