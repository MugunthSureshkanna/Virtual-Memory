#include "bitmap.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "devices/block.h"
#include "stddef.h"

extern struct bitmap* swap_table;

extern struct lock swap_lock;

#define SECTORS_PER_PAGE (PGSIZE / BLOCK_SECTOR_SIZE)

void swaptable_init();
void destroy_map(struct bitmap* swap_map);

size_t alloc_swap();
void dealloc_swap(size_t swap_index);
void write_to_swap(size_t swap_index, void* kvaddr);
void back_from_swap(size_t swap_index, void* kvaddr);