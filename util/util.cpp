#include "util.h"
#include "machine_const.h"
// #include "pmon_utils.h"
#include "skx_hash_utils.h"

// #define _GNU_SOURCE

#include <inttypes.h>
#include <stdlib.h>
#include <unistd.h>		// getpagesize, sysconf
#include <stdio.h>
#include <sched.h>		// sched_setaffinity
#include <stdbool.h>
#include <pthread.h>

/*
 * To be used to start a timing measurement.
 * It returns the current time.
 *
 * Inspired by:
 *  https://github.com/google/highwayhash/blob/master/highwayhash/tsc_timer.h
 */
uint64_t start_time(void)
{
	uint64_t t;
	asm volatile(
		"isb\n"
		"mrs %0, cntvct_el0\n"
		"isb" : "=r" (t));
	return t;
}

/*
 * To be used to end a timing measurement.
 * It returns the current time.
 *
 * Inspired by:
 *  https://github.com/google/highwayhash/blob/master/highwayhash/tsc_timer.h
 */
uint64_t stop_time(void)
{
	uint64_t t;
	asm volatile(
		"isb\n"
		"mrs %0, cntvct_el0\n"
		"isb" : "=r" (t));
	return t;
}

/*
 * Appends the given string to the linked list which is pointed to by the given head
 */
void append_string_to_linked_list(struct Node **head, void *addr)
{
	struct Node *current = *head;

	// Create the new node to append to the linked list
	struct Node *new_node = (struct Node *) malloc(sizeof(*new_node));
	new_node->address = addr;
	new_node->next = NULL;

	// If the linked list is empty, just make the head to be this new node
	if (current == NULL)
		*head = new_node;

	// Otherwise, go till the last node and append the new node after it
	else {
		while (current->next != NULL)
			current = current->next;

		current->next = new_node;
	}
}

/*
 * The argument addr should be the physical address, but in some cases it can be
 * the virtual address and this will still work. Here is why.
 *
 * Normal pages are 4KB (2^12) in size, meaning that the rightmost 12 bits of
 * the virtual address have to be the same in the physical address since they
 * are needed as offset for page table in the translation.
 * 
 * Huge pages have a size of 2MB (2^21), meaning that the rightmost 21 bits of
 * the virtual address have to be the same in the physical address since they
 * are needed as offset for page table in the translation. 
 * 
 * Since to find the set in the L1 we only need bits [11-6], then the virtual
 * address, either with normal or huge pages, is enough to get the set index.
 * 
 * Since to find the set in the L2 and LLC we only need bits [15-6], then the
 * virtual address, with huge pages, is enough to get the set index.
 * 
 * To visually understand why, see the presentations here:
 *  https://cs.adelaide.edu.au/~yval/Mastik/
 */
uint64_t get_cache_set_index(uint64_t addr, int cache_level)
{
	uint64_t index;

	if (cache_level == 1) {
		index = (addr)&L1_SET_INDEX_MASK;

	} else if (cache_level == 2) {
		index = (addr)&L2_SET_INDEX_MASK;

	} else if (cache_level == 3) {
		index = (addr)&LLC_SET_INDEX_PER_SLICE_MASK;

	} else {
		exit(EXIT_FAILURE);
	}

	return index >> CACHE_BLOCK_SIZE_LOG;
}

uint64_t find_next_address_on_slice_and_set(void *va, uint8_t desired_slice, uint32_t desired_set)
{
	uint64_t offset = 0;

	// Slice mapping will change for each cacheline which is 64 Bytes
	// NOTE: We are also ensuring that the addresses are on cache set 2
	// This is because otherwise the next time we run this program we might
	// get addresses on different sets and that might impact the latency regardless
	// of ring-bus contention.
	while (get_cache_set_index((uint64_t)va + offset, 3) != desired_set ||
		   desired_slice != get_cache_slice_index((void *)((uint64_t)va + offset))) {
		// printf("Cache slice: %ld\nLLC index: %ld\n", get_cache_slice_index((uint64_t)va + offset), get_cache_set_index((uint64_t)va + offset, 3));
		offset += CACHE_BLOCK_SIZE;
	}
	return offset;
}


/* 
 * Get the page frame number
 */
#define GET_BIT(X,Y) (X & ((uint64_t)1<<Y)) >> Y
static uint64_t get_page_frame_number_of_address(void *address)
{
	/* Open the pagemap file for the current process */
	FILE *pagemap = fopen("/proc/self/pagemap", "rb");

	if (pagemap == NULL) {
		perror("Failed to open /proc/self/pagemap");
		exit(1);
	}

	/* Seek to the page that the buffer is on */
	uint64_t offset = (uint64_t)((uint64_t)address >> PAGE_SHIFT) * (uint64_t)PAGEMAP_LENGTH;
	if (fseek(pagemap, (uint64_t)offset, SEEK_SET) != 0) {
		fprintf(stderr, "Failed to seek pagemap to proper location\n");
		exit(1);
	}

	/* The page frame number is in bits 0-54 so read the first 8 bytes and clear the upper bits */
	uint64_t page_frame_number = 0;
	if (fread(&page_frame_number, 1, PAGEMAP_LENGTH, pagemap) < 0) {
		fprintf(stderr, "fread failed\n");
		exit(EXIT_FAILURE);
	}

	fclose(pagemap);

	// Check the upper bits to ensure valid pagemap entry
	if (GET_BIT(page_frame_number, 63)) {
		return page_frame_number & 0x7FFFFFFFFFFFFF; // Mastik uses 0x3FFFFFFFFFFFFF
	} else {
		printf("pagemap: Page not present\n");
		if (GET_BIT(page_frame_number, 62)) {
			printf("pagemap: Page swapped\n");
		}
	}

	return 0;
}

/*
 * Get the physical address of a page
 */
uint64_t get_physical_address(void *address)
{
	/* Get page frame number */
	unsigned int page_frame_number = get_page_frame_number_of_address(address);

	/* Find the difference from the buffer to the page boundary */
	uint64_t distance_from_page_boundary = (uint64_t)address % getpagesize();

	/* Determine how far to seek into memory to find the buffer */
	uint64_t physical_address = (uint64_t)((uint64_t)page_frame_number << PAGE_SHIFT) + (uint64_t)distance_from_page_boundary;

	return physical_address;
}


struct job_info_timing {
    void *va;
    uint64_t repeat;
    uint64_t elapsed;
    int cpu_id;
};

struct job_info {
    void *va;
    int cpu_id;
};

volatile bool keep_running = true;
pthread_mutex_t timing_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t timing_cond = PTHREAD_COND_INITIALIZER;
bool load_thread_started = false; // Flag to indicate that the thread has started

void *probe_thread_timing(void *ptr) {
    struct job_info_timing* ji = (struct job_info_timing *)ptr;
    volatile uint64_t *p = (uint64_t *)ji->va;

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(ji->cpu_id, &cpuset);
    sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);

    // Wait for the thread to start
    pthread_mutex_lock(&timing_mutex);
    while (load_thread_started == 0) {
        pthread_cond_wait(&timing_cond, &timing_mutex); // Wait until thread sets the started flag
    }
    pthread_mutex_unlock(&timing_mutex);

    uint64_t start;
    asm volatile("mrs %0, cntvct_el0" : "=r" (start));  

    uint64_t res;
    for (int i = 0; i < ji->repeat; i ++) {
        res = *p;
    }

    uint64_t end;
    asm volatile("mrs %0, cntvct_el0" : "=r" (end));  

    ji->elapsed = end - start;
    return NULL;
}

void *load_thread_timing(void *ptr) {
    struct job_info* ji = (struct job_info *)ptr;
    volatile uint64_t *p = (uint64_t *)ji->va;

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(ji->cpu_id, &cpuset);
    sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);

    pthread_mutex_lock(&timing_mutex);
    load_thread_started = 1; // Set flag to indicate the thread has started
    pthread_cond_signal(&timing_cond); // Signal the condition variable
    pthread_mutex_unlock(&timing_mutex);

    uint64_t res;
    while (keep_running) {
         *p = *p + 1;
    }
    return NULL;
}

/*
 * Find the slice for a virtual address
 */
uint64_t find_closest_slice(void *va)
{
    uint64_t shortest_time = 1000000000;
    uint64_t shortest_cpu = -1;

    for (int probe_cpu = 0; probe_cpu < NUM_CORES; probe_cpu+=2)  {
        struct job_info_timing ji_p;
        struct job_info ji_l;
        ji_p.cpu_id = probe_cpu;
        ji_l.cpu_id = probe_cpu+1;

        ji_p.va = ji_l.va = va;

        ji_p.repeat = 100000;

        keep_running = true;
        load_thread_started = false; 

        pthread_t thread_p, thread_l;
        pthread_create(&thread_p, NULL, probe_thread_timing, &ji_p);
        pthread_create(&thread_l, NULL, load_thread_timing, &ji_l);
        
        pthread_join(thread_p, NULL);

        keep_running = false;
        pthread_join(thread_l, NULL);

        // printf("  cpu %d elapsed %ld\n",probe_cpu,ji_p.elapsed);
        if (ji_p.elapsed < shortest_time) {
            shortest_time = ji_p.elapsed;
            shortest_cpu = probe_cpu;
        }
    }

    return shortest_cpu;    
}

uint64_t get_cache_slice_index(void *va)
{
	while(1) {
		uint64_t t1 = find_closest_slice(va);
		uint64_t t2 = find_closest_slice(va);

		if(t1!= t2)
			printf("mismatch! address %p core %ld - %ld\n",va,t1,t2); 
		else return t1;
	}
}

void flush_l1i(void)
{
	asm volatile(
		R"(
		.balign 64
		label1: b label2
		.balign 64
		label2: b label3
		.balign 64
		label3: b label4
		.balign 64
		label4: b label5
		.balign 64
		label5: b label6
		.balign 64
		label6: b label7
		.balign 64
		label7: b label8
		.balign 64
		label8: b label9
		.balign 64
		label9: b label10
		.balign 64
		label10: b label11
		.balign 64
		label11: b label12
		.balign 64
		label12: b label13
		.balign 64
		label13: b label14
		.balign 64
		label14: b label15
		.balign 64
		label15: b label16
		.balign 64
		label16: b label17
		.balign 64
		label17: b label18
		.balign 64
		label18: b label19
		.balign 64
		label19: b label20
		.balign 64
		label20: b label21
		.balign 64
		label21: b label22
		.balign 64
		label22: b label23
		.balign 64
		label23: b label24
		.balign 64
		label24: b label25
		.balign 64
		label25: b label26
		.balign 64
		label26: b label27
		.balign 64
		label27: b label28
		.balign 64
		label28: b label29
		.balign 64
		label29: b label30
		.balign 64
		label30: b label31
		.balign 64
		label31: b label32
		.balign 64
		label32: b label33
		.balign 64
		label33: b label34
		.balign 64
		label34: b label35
		.balign 64
		label35: b label36
		.balign 64
		label36: b label37
		.balign 64
		label37: b label38
		.balign 64
		label38: b label39
		.balign 64
		label39: b label40
		.balign 64
		label40: b label41
		.balign 64
		label41: b label42
		.balign 64
		label42: b label43
		.balign 64
		label43: b label44
		.balign 64
		label44: b label45
		.balign 64
		label45: b label46
		.balign 64
		label46: b label47
		.balign 64
		label47: b label48
		.balign 64
		label48: b label49
		.balign 64
		label49: b label50
		.balign 64
		label50: b label51
		.balign 64
		label51: b label52
		.balign 64
		label52: b label53
		.balign 64
		label53: b label54
		.balign 64
		label54: b label55
		.balign 64
		label55: b label56
		.balign 64
		label56: b label57
		.balign 64
		label57: b label58
		.balign 64
		label58: b label59
		.balign 64
		label59: b label60
		.balign 64
		label60: b label61
		.balign 64
		label61: b label62
		.balign 64
		label62: b label63
		.balign 64
		label63: b label64
		.balign 64
		label64: b label65
		.balign 64
		label65: b label66
		.balign 64
		label66: b label67
		.balign 64
		label67: b label68
		.balign 64
		label68: b label69
		.balign 64
		label69: b label70
		.balign 64
		label70: b label71
		.balign 64
		label71: b label72
		.balign 64
		label72: b label73
		.balign 64
		label73: b label74
		.balign 64
		label74: b label75
		.balign 64
		label75: b label76
		.balign 64
		label76: b label77
		.balign 64
		label77: b label78
		.balign 64
		label78: b label79
		.balign 64
		label79: b label80
		.balign 64
		label80: b label81
		.balign 64
		label81: b label82
		.balign 64
		label82: b label83
		.balign 64
		label83: b label84
		.balign 64
		label84: b label85
		.balign 64
		label85: b label86
		.balign 64
		label86: b label87
		.balign 64
		label87: b label88
		.balign 64
		label88: b label89
		.balign 64
		label89: b label90
		.balign 64
		label90: b label91
		.balign 64
		label91: b label92
		.balign 64
		label92: b label93
		.balign 64
		label93: b label94
		.balign 64
		label94: b label95
		.balign 64
		label95: b label96
		.balign 64
		label96: b label97
		.balign 64
		label97: b label98
		.balign 64
		label98: b label99
		.balign 64
		label99: b label100
		.balign 64
		label100: b label101
		.balign 64
		label101: b label102
		.balign 64
		label102: b label103
		.balign 64
		label103: b label104
		.balign 64
		label104: b label105
		.balign 64
		label105: b label106
		.balign 64
		label106: b label107
		.balign 64
		label107: b label108
		.balign 64
		label108: b label109
		.balign 64
		label109: b label110
		.balign 64
		label110: b label111
		.balign 64
		label111: b label112
		.balign 64
		label112: b label113
		.balign 64
		label113: b label114
		.balign 64
		label114: b label115
		.balign 64
		label115: b label116
		.balign 64
		label116: b label117
		.balign 64
		label117: b label118
		.balign 64
		label118: b label119
		.balign 64
		label119: b label120
		.balign 64
		label120: b label121
		.balign 64
		label121: b label122
		.balign 64
		label122: b label123
		.balign 64
		label123: b label124
		.balign 64
		label124: b label125
		.balign 64
		label125: b label126
		.balign 64
		label126: b label127
		.balign 64
		label127: b label128
		.balign 64
		label128: b label129
		.balign 64
		label129: b label130
		.balign 64
		label130: b label131
		.balign 64
		label131: b label132
		.balign 64
		label132: b label133
		.balign 64
		label133: b label134
		.balign 64
		label134: b label135
		.balign 64
		label135: b label136
		.balign 64
		label136: b label137
		.balign 64
		label137: b label138
		.balign 64
		label138: b label139
		.balign 64
		label139: b label140
		.balign 64
		label140: b label141
		.balign 64
		label141: b label142
		.balign 64
		label142: b label143
		.balign 64
		label143: b label144
		.balign 64
		label144: b label145
		.balign 64
		label145: b label146
		.balign 64
		label146: b label147
		.balign 64
		label147: b label148
		.balign 64
		label148: b label149
		.balign 64
		label149: b label150
		.balign 64
		label150: b label151
		.balign 64
		label151: b label152
		.balign 64
		label152: b label153
		.balign 64
		label153: b label154
		.balign 64
		label154: b label155
		.balign 64
		label155: b label156
		.balign 64
		label156: b label157
		.balign 64
		label157: b label158
		.balign 64
		label158: b label159
		.balign 64
		label159: b label160
		.balign 64
		label160: b label161
		.balign 64
		label161: b label162
		.balign 64
		label162: b label163
		.balign 64
		label163: b label164
		.balign 64
		label164: b label165
		.balign 64
		label165: b label166
		.balign 64
		label166: b label167
		.balign 64
		label167: b label168
		.balign 64
		label168: b label169
		.balign 64
		label169: b label170
		.balign 64
		label170: b label171
		.balign 64
		label171: b label172
		.balign 64
		label172: b label173
		.balign 64
		label173: b label174
		.balign 64
		label174: b label175
		.balign 64
		label175: b label176
		.balign 64
		label176: b label177
		.balign 64
		label177: b label178
		.balign 64
		label178: b label179
		.balign 64
		label179: b label180
		.balign 64
		label180: b label181
		.balign 64
		label181: b label182
		.balign 64
		label182: b label183
		.balign 64
		label183: b label184
		.balign 64
		label184: b label185
		.balign 64
		label185: b label186
		.balign 64
		label186: b label187
		.balign 64
		label187: b label188
		.balign 64
		label188: b label189
		.balign 64
		label189: b label190
		.balign 64
		label190: b label191
		.balign 64
		label191: b label192
		.balign 64
		label192: b label193
		.balign 64
		label193: b label194
		.balign 64
		label194: b label195
		.balign 64
		label195: b label196
		.balign 64
		label196: b label197
		.balign 64
		label197: b label198
		.balign 64
		label198: b label199
		.balign 64
		label199: b label200
		.balign 64
		label200: b label201
		.balign 64
		label201: b label202
		.balign 64
		label202: b label203
		.balign 64
		label203: b label204
		.balign 64
		label204: b label205
		.balign 64
		label205: b label206
		.balign 64
		label206: b label207
		.balign 64
		label207: b label208
		.balign 64
		label208: b label209
		.balign 64
		label209: b label210
		.balign 64
		label210: b label211
		.balign 64
		label211: b label212
		.balign 64
		label212: b label213
		.balign 64
		label213: b label214
		.balign 64
		label214: b label215
		.balign 64
		label215: b label216
		.balign 64
		label216: b label217
		.balign 64
		label217: b label218
		.balign 64
		label218: b label219
		.balign 64
		label219: b label220
		.balign 64
		label220: b label221
		.balign 64
		label221: b label222
		.balign 64
		label222: b label223
		.balign 64
		label223: b label224
		.balign 64
		label224: b label225
		.balign 64
		label225: b label226
		.balign 64
		label226: b label227
		.balign 64
		label227: b label228
		.balign 64
		label228: b label229
		.balign 64
		label229: b label230
		.balign 64
		label230: b label231
		.balign 64
		label231: b label232
		.balign 64
		label232: b label233
		.balign 64
		label233: b label234
		.balign 64
		label234: b label235
		.balign 64
		label235: b label236
		.balign 64
		label236: b label237
		.balign 64
		label237: b label238
		.balign 64
		label238: b label239
		.balign 64
		label239: b label240
		.balign 64
		label240: b label241
		.balign 64
		label241: b label242
		.balign 64
		label242: b label243
		.balign 64
		label243: b label244
		.balign 64
		label244: b label245
		.balign 64
		label245: b label246
		.balign 64
		label246: b label247
		.balign 64
		label247: b label248
		.balign 64
		label248: b label249
		.balign 64
		label249: b label250
		.balign 64
		label250: b label251
		.balign 64
		label251: b label252
		.balign 64
		label252: b label253
		.balign 64
		label253: b label254
		.balign 64
		label254: b label255
		.balign 64
		label255: b label256
		.balign 64
		label256: b label257
		.balign 64
		label257: b label258
		.balign 64
		label258: b label259
		.balign 64
		label259: b label260
		.balign 64
		label260: b label261
		.balign 64
		label261: b label262
		.balign 64
		label262: b label263
		.balign 64
		label263: b label264
		.balign 64
		label264: b label265
		.balign 64
		label265: b label266
		.balign 64
		label266: b label267
		.balign 64
		label267: b label268
		.balign 64
		label268: b label269
		.balign 64
		label269: b label270
		.balign 64
		label270: b label271
		.balign 64
		label271: b label272
		.balign 64
		label272: b label273
		.balign 64
		label273: b label274
		.balign 64
		label274: b label275
		.balign 64
		label275: b label276
		.balign 64
		label276: b label277
		.balign 64
		label277: b label278
		.balign 64
		label278: b label279
		.balign 64
		label279: b label280
		.balign 64
		label280: b label281
		.balign 64
		label281: b label282
		.balign 64
		label282: b label283
		.balign 64
		label283: b label284
		.balign 64
		label284: b label285
		.balign 64
		label285: b label286
		.balign 64
		label286: b label287
		.balign 64
		label287: b label288
		.balign 64
		label288: b label289
		.balign 64
		label289: b label290
		.balign 64
		label290: b label291
		.balign 64
		label291: b label292
		.balign 64
		label292: b label293
		.balign 64
		label293: b label294
		.balign 64
		label294: b label295
		.balign 64
		label295: b label296
		.balign 64
		label296: b label297
		.balign 64
		label297: b label298
		.balign 64
		label298: b label299
		.balign 64
		label299: b label300
		.balign 64
		label300: b label301
		.balign 64
		label301: b label302
		.balign 64
		label302: b label303
		.balign 64
		label303: b label304
		.balign 64
		label304: b label305
		.balign 64
		label305: b label306
		.balign 64
		label306: b label307
		.balign 64
		label307: b label308
		.balign 64
		label308: b label309
		.balign 64
		label309: b label310
		.balign 64
		label310: b label311
		.balign 64
		label311: b label312
		.balign 64
		label312: b label313
		.balign 64
		label313: b label314
		.balign 64
		label314: b label315
		.balign 64
		label315: b label316
		.balign 64
		label316: b label317
		.balign 64
		label317: b label318
		.balign 64
		label318: b label319
		.balign 64
		label319: b label320
		.balign 64
		label320: b label321
		.balign 64
		label321: b label322
		.balign 64
		label322: b label323
		.balign 64
		label323: b label324
		.balign 64
		label324: b label325
		.balign 64
		label325: b label326
		.balign 64
		label326: b label327
		.balign 64
		label327: b label328
		.balign 64
		label328: b label329
		.balign 64
		label329: b label330
		.balign 64
		label330: b label331
		.balign 64
		label331: b label332
		.balign 64
		label332: b label333
		.balign 64
		label333: b label334
		.balign 64
		label334: b label335
		.balign 64
		label335: b label336
		.balign 64
		label336: b label337
		.balign 64
		label337: b label338
		.balign 64
		label338: b label339
		.balign 64
		label339: b label340
		.balign 64
		label340: b label341
		.balign 64
		label341: b label342
		.balign 64
		label342: b label343
		.balign 64
		label343: b label344
		.balign 64
		label344: b label345
		.balign 64
		label345: b label346
		.balign 64
		label346: b label347
		.balign 64
		label347: b label348
		.balign 64
		label348: b label349
		.balign 64
		label349: b label350
		.balign 64
		label350: b label351
		.balign 64
		label351: b label352
		.balign 64
		label352: b label353
		.balign 64
		label353: b label354
		.balign 64
		label354: b label355
		.balign 64
		label355: b label356
		.balign 64
		label356: b label357
		.balign 64
		label357: b label358
		.balign 64
		label358: b label359
		.balign 64
		label359: b label360
		.balign 64
		label360: b label361
		.balign 64
		label361: b label362
		.balign 64
		label362: b label363
		.balign 64
		label363: b label364
		.balign 64
		label364: b label365
		.balign 64
		label365: b label366
		.balign 64
		label366: b label367
		.balign 64
		label367: b label368
		.balign 64
		label368: b label369
		.balign 64
		label369: b label370
		.balign 64
		label370: b label371
		.balign 64
		label371: b label372
		.balign 64
		label372: b label373
		.balign 64
		label373: b label374
		.balign 64
		label374: b label375
		.balign 64
		label375: b label376
		.balign 64
		label376: b label377
		.balign 64
		label377: b label378
		.balign 64
		label378: b label379
		.balign 64
		label379: b label380
		.balign 64
		label380: b label381
		.balign 64
		label381: b label382
		.balign 64
		label382: b label383
		.balign 64
		label383: b label384
		.balign 64
		label384: b label385
		.balign 64
		label385: b label386
		.balign 64
		label386: b label387
		.balign 64
		label387: b label388
		.balign 64
		label388: b label389
		.balign 64
		label389: b label390
		.balign 64
		label390: b label391
		.balign 64
		label391: b label392
		.balign 64
		label392: b label393
		.balign 64
		label393: b label394
		.balign 64
		label394: b label395
		.balign 64
		label395: b label396
		.balign 64
		label396: b label397
		.balign 64
		label397: b label398
		.balign 64
		label398: b label399
		.balign 64
		label399: b label400
		.balign 64
		label400: b label401
		.balign 64
		label401: b label402
		.balign 64
		label402: b label403
		.balign 64
		label403: b label404
		.balign 64
		label404: b label405
		.balign 64
		label405: b label406
		.balign 64
		label406: b label407
		.balign 64
		label407: b label408
		.balign 64
		label408: b label409
		.balign 64
		label409: b label410
		.balign 64
		label410: b label411
		.balign 64
		label411: b label412
		.balign 64
		label412: b label413
		.balign 64
		label413: b label414
		.balign 64
		label414: b label415
		.balign 64
		label415: b label416
		.balign 64
		label416: b label417
		.balign 64
		label417: b label418
		.balign 64
		label418: b label419
		.balign 64
		label419: b label420
		.balign 64
		label420: b label421
		.balign 64
		label421: b label422
		.balign 64
		label422: b label423
		.balign 64
		label423: b label424
		.balign 64
		label424: b label425
		.balign 64
		label425: b label426
		.balign 64
		label426: b label427
		.balign 64
		label427: b label428
		.balign 64
		label428: b label429
		.balign 64
		label429: b label430
		.balign 64
		label430: b label431
		.balign 64
		label431: b label432
		.balign 64
		label432: b label433
		.balign 64
		label433: b label434
		.balign 64
		label434: b label435
		.balign 64
		label435: b label436
		.balign 64
		label436: b label437
		.balign 64
		label437: b label438
		.balign 64
		label438: b label439
		.balign 64
		label439: b label440
		.balign 64
		label440: b label441
		.balign 64
		label441: b label442
		.balign 64
		label442: b label443
		.balign 64
		label443: b label444
		.balign 64
		label444: b label445
		.balign 64
		label445: b label446
		.balign 64
		label446: b label447
		.balign 64
		label447: b label448
		.balign 64
		label448: b label449
		.balign 64
		label449: b label450
		.balign 64
		label450: b label451
		.balign 64
		label451: b label452
		.balign 64
		label452: b label453
		.balign 64
		label453: b label454
		.balign 64
		label454: b label455
		.balign 64
		label455: b label456
		.balign 64
		label456: b label457
		.balign 64
		label457: b label458
		.balign 64
		label458: b label459
		.balign 64
		label459: b label460
		.balign 64
		label460: b label461
		.balign 64
		label461: b label462
		.balign 64
		label462: b label463
		.balign 64
		label463: b label464
		.balign 64
		label464: b label465
		.balign 64
		label465: b label466
		.balign 64
		label466: b label467
		.balign 64
		label467: b label468
		.balign 64
		label468: b label469
		.balign 64
		label469: b label470
		.balign 64
		label470: b label471
		.balign 64
		label471: b label472
		.balign 64
		label472: b label473
		.balign 64
		label473: b label474
		.balign 64
		label474: b label475
		.balign 64
		label475: b label476
		.balign 64
		label476: b label477
		.balign 64
		label477: b label478
		.balign 64
		label478: b label479
		.balign 64
		label479: b label480
		.balign 64
		label480: b label481
		.balign 64
		label481: b label482
		.balign 64
		label482: b label483
		.balign 64
		label483: b label484
		.balign 64
		label484: b label485
		.balign 64
		label485: b label486
		.balign 64
		label486: b label487
		.balign 64
		label487: b label488
		.balign 64
		label488: b label489
		.balign 64
		label489: b label490
		.balign 64
		label490: b label491
		.balign 64
		label491: b label492
		.balign 64
		label492: b label493
		.balign 64
		label493: b label494
		.balign 64
		label494: b label495
		.balign 64
		label495: b label496
		.balign 64
		label496: b label497
		.balign 64
		label497: b label498
		.balign 64
		label498: b label499
		.balign 64
		label499: b label500
		.balign 64
		label500: b label501
		.balign 64
		label501: b label502
		.balign 64
		label502: b label503
		.balign 64
		label503: b label504
		.balign 64
		label504: b label505
		.balign 64
		label505: b label506
		.balign 64
		label506: b label507
		.balign 64
		label507: b label508
		.balign 64
		label508: b label509
		.balign 64
		label509: b label510
		.balign 64
		label510: b label511
		.balign 64
		label511: b label512
		.balign 64
		label512: mov x0, xzr)"
		:
		:
		: "x0", "memory");
}