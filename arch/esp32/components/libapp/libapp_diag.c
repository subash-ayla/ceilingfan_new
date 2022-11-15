/*
 * Copyright 2020 Ayla Networks, Inc.  All rights reserved.
 */
#include <string.h>
#include <stddef.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <esp_heap_task_info.h>
#include <ayla/log.h>
#include "libapp_conf_int.h"

/* heap tracing defines */
#define MAX_TASK_NUM	20
#define MAX_BLOCK_NUM	20

#if configUSE_TRACE_FACILITY == 1
static const char *taskStateString(eTaskState state)
{
	switch (state) {
	case eRunning:
		return "run";
	case eReady:
		return "rdy";
	case eBlocked:
		return "blk";
	case eSuspended:
		return "sus";
	case eDeleted:
		return "del";
	default:
		return "unk";
	}
}
#endif

/*
 * Print diagnostic info about tasks.
 *
 * Enable the FreeRTOS trace facility in menuconfig to enable this
 * feature.
 */
static void libapp_diag_tasks(void)
{
#if configUSE_TRACE_FACILITY == 1
	UBaseType_t count;
	TaskStatus_t *table;
	TaskStatus_t *tp;
	uint32_t total_time;
	unsigned int i;

	count = uxTaskGetNumberOfTasks() + 1;	/* allow for one extra */
	table = malloc(count * sizeof(*table));
	if (!table) {
		printcli("alloc for %u tasks failed", count);
		return;
	}
	count = uxTaskGetSystemState(table, count, &total_time);
	printcli("\ntasks:");
	printcli("   %20s %6s %6s %6s %6s %12s",
	    "",
	    "bprio", "cprio", "state", "unused", "handle");
	if (count) {
		tp = table;
		for (i = 0; i < count; i++) {
			printcli("   %20s %6u %6u %6s %6u %12p",
			    tp->pcTaskName,
			    tp->uxBasePriority,
			    tp->uxCurrentPriority,
			    taskStateString(tp->eCurrentState),
			    tp->usStackHighWaterMark,
			    tp->xHandle);
			tp++;
		}
	} else {
		printcli("Task list changing. Please try again.");
	}
	free(table);
#endif
}

/*
 * Print heap usage by task
 *
 * Enable heap task tracking in menuconfig to enable this featue.
 */
static void libapp_diag_mem_task(void)
{
#ifdef CONFIG_HEAP_TASK_TRACKING
	static size_t count;
	static heap_task_totals_t totals[MAX_TASK_NUM];
	static heap_task_block_t blocks[MAX_BLOCK_NUM];
	heap_task_info_params_t heap_info;
	size_t total_size = 0;
	size_t total_count = 0;
	char *task_name;
	char *p;
	char c;
	int i;
	int j;

	count = 0;
	memset(&heap_info, 0, sizeof(heap_info));

	heap_info.totals = totals;
	heap_info.num_totals = &count;
	heap_info.max_totals = MAX_TASK_NUM;
	heap_info.blocks = blocks;
	heap_info.max_blocks = MAX_BLOCK_NUM;

	heap_caps_get_per_task_info(&heap_info);

	printcli("\nheap usage by task:");
	printcli("   %20s %6s %6s %12s", "task", "size", "count", "handle");

	for (i = 0 ; i < count; i++) {
		task_name = "_none_";
		p = NULL;
		if (heap_info.totals[i].task) {
			task_name = "_unknown_";
			p = pcTaskGetTaskName(heap_info.totals[i].task);
			/*
			 * Only use returned name if it is valid ASCII.
			 * FreeRTOS assumes the task handle is good when it
			 * may not be.
			 */
			for (j = 0; j < 20; ++j) {
				c = p[j];
				if (c < ' ' || c > '~')  {
					break;
				}
			}
			if (j < 20 && c == '\0') {
				task_name = p;
			}
		}
		printcli("   %20s %6u %6u %12p",
		    task_name,
		    heap_info.totals[i].size[0],
		    heap_info.totals[i].count[0],
		    heap_info.totals[i].task);
		total_size += heap_info.totals[i].size[0];
		total_count += heap_info.totals[i].count[0];
	}
	printcli("   %20s %6u %6u", "TOTAL", total_size, total_count);
#endif
}

static void libapp_diag_mem(void)
{
	size_t ua_min = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
	size_t ua_free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
	size_t ua_large = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
#ifndef CONFIG_IDF_TARGET_ESP32C3
	size_t all_free = heap_caps_get_free_size(MALLOC_CAP_32BIT);
#endif

	printcli("\nheap summary:");
	printcli("   %20s %6s %6s %6s", "type", "min", "free", "lg-blk");
	printcli("   %20s %6u %6u %6u",
	    "any-access", ua_min, ua_free, ua_large);
#ifndef CONFIG_IDF_TARGET_ESP32C3 /* no aligned-only mem on C3 */
	printcli("   %20s %6s %6u", "aligned-only", "", all_free - ua_free);
	printcli("   %20s %6s %6u", "TOTAL", "", all_free);
#endif
}

const char libapp_diag_cli_help[] =
	"diag [mem|tasks]";

int libapp_diag_cli(int argc, char **argv)
{
	u8 display_mem = 0;
	u8 display_tasks = 0;

	if (argc == 2 && !strcmp(argv[1], "mem")) {
		display_mem = 1;
	}
	if (argc == 2 && !strcmp(argv[1], "tasks")) {
		display_tasks = 1;
	}

	if (!display_mem && !display_tasks) {
		/* display everything */
		display_mem = 1;
		display_tasks = 1;
	}
	if (display_mem) {
		libapp_diag_mem();
		libapp_diag_mem_task();
	}
	if (display_tasks) {
		libapp_diag_tasks();
	}
	return ESP_OK;
}

