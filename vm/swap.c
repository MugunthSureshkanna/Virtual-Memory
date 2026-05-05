#include "swap.h"
#include "bitmap.h"
#include "devices/block.h"
#include "debug.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "stddef.h"

struct bitmap* swap_table;
struct lock swap_lock;
struct block* swap_block;

/* Initialize the swap table and lock*/
void swaptable_init() {
    swap_block = block_get_role(BLOCK_SWAP);
    swap_table = bitmap_create(block_size(swap_block) / SECTORS_PER_PAGE);
    bitmap_set_all(swap_table, 0);
    lock_init(&swap_lock);
}

/* Helper the set an index in swap table as allocated */
size_t alloc_swap(){
    lock_acquire(&swap_lock);
    size_t swap_index = bitmap_scan_and_flip(swap_table, 0, 1, 0);
    lock_release(&swap_lock);
    return swap_index;
}

/* Helper that sets an index in the swap table as empty */
void dealloc_swap(size_t swap_index){
    if(!lock_held_by_current_thread(&swap_lock)){
        lock_acquire(&swap_lock);
    }
    bitmap_set(swap_table, swap_index, 0);
    if(lock_held_by_current_thread(&swap_lock)){
        lock_release(&swap_lock);
    }
}

/* Helper to write to swap space */
void write_to_swap(size_t swap_index, void* kvaddr){
    lock_acquire(&swap_lock);
    for(int i = 0; i < SECTORS_PER_PAGE; i++){
        block_write(swap_block, swap_index * SECTORS_PER_PAGE + i, kvaddr);
        kvaddr = (char*) kvaddr + BLOCK_SECTOR_SIZE;
    }
    lock_release(&swap_lock);
}

/* Helper to write back from swap space */
void back_from_swap(size_t swap_index, void* kvaddr){
    // Write back
    lock_acquire(&swap_lock);
    for(int i = 0; i < SECTORS_PER_PAGE; i++){
        block_read(swap_block, swap_index * SECTORS_PER_PAGE + i, kvaddr);
        kvaddr = (char*) kvaddr + BLOCK_SECTOR_SIZE;
    }
    // Set the index in bit map to unallocated
    dealloc_swap(swap_index);
    if(lock_held_by_current_thread(&swap_lock)){
        lock_release(&swap_lock);
    }
}

/* Helper to destroy the swap_table - UNUSED */
void destroy_map(struct bitmap* swap_map) {
    bitmap_destroy(swap_map);
}