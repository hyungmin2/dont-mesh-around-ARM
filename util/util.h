#ifndef _UTIL_H
#define _UTIL_H

// #define _GNU_SOURCE

#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <sched.h>
#include "machine_const.h"

uint64_t get_cache_set_index(uint64_t addr, int cache_level);
uint64_t find_next_address_on_slice_and_set(void *va, uint8_t desired_slice, uint32_t desired_set);

/* 
 * Gets the value Time Stamp Counter 
 */
inline uint64_t get_time(void)
{
	uint64_t t;
	asm volatile(
		"isb\n"
		"mrs %0, cntvct_el0\n"
		"isb" : "=r" (t));
	return t;
}

uint64_t start_time(void);
uint64_t stop_time(void);

inline void wait_cycles(uint64_t delay)
{
	uint64_t cycles, end;
	cycles = get_time();
	end = cycles + delay;
	while (cycles < end) {
		cycles = get_time();
	}
}

inline void maccess(void *p)
{
	asm volatile("ldr x0, [%0]" : : "r"(p) : "x0");
}

struct Node {
	void *address;
	struct Node *next;
};

void append_string_to_linked_list(struct Node **head, void *addr);

uint64_t get_physical_address(void *address);
uint64_t get_cache_slice_index(void *va);

static void pin_cpu(size_t core_ID)
{
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(core_ID, &set);
	if (sched_setaffinity(0, sizeof(cpu_set_t), &set) < 0) {
		printf("Unable to Set Affinity\n");
		exit(EXIT_FAILURE);
	}
}

void flush_l1i(void);

#endif