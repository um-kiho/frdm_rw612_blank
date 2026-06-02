/*
 * Zephyr Multi-Threading Example Header
 */

#ifndef MULTITASK_EXAMPLE_H_
#define MULTITASK_EXAMPLE_H_

#include <zephyr/kernel.h>

/* 초기화 함수 */
int multitask_example_init(void);

/* 런타임 스레드 생성 */
void create_dynamic_task(void);

/* Delayed work 초기화 */
void init_delayed_work(void);

#endif /* MULTITASK_EXAMPLE_H_ */
