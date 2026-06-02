/*
 * Copyright (c) 2016 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>

#include "app_main.h"

K_THREAD_DEFINE(app_main_tid,
		APP_MAIN_STACK_SIZE,
		(k_thread_entry_t)app_main_task,
		NULL,
		NULL,
		NULL,
		APP_MAIN_PRIORITY,
		0,
		0);


int main(void)
{
	return 0;
}
