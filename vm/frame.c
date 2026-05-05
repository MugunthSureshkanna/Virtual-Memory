#include "frame.h"
#include <debug.h>
#include <stdbool.h>
#include <stddef.h>
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "page.h"
#include "hash.h"
#include "userprog/syscall.h"
#include "userprog/pagedir.h"
#include "filesys/file.h"
#include "threads/thread.h"
#include "string.h"
#include "swap.h"
#include "devices/block.h"
#include "threads/synch.h"
#include "bitmap.h"

struct fte** frame_table;
uint32_t index_bias;
struct lock frame_lock;
int ft_size;
int clock_hand;

/* Initializes the frame table and the clock 
   user_pages passed in from palloc.c */
void frametable_init(size_t user_pages, uint32_t bias) {
    index_bias = bias;
    clock_hand = 1;
    frame_table = malloc(sizeof(struct fte*) * user_pages);
    for (int i = 0; i < user_pages; i++){
        frame_table[i] = NULL;
    }
    ft_size = user_pages;
    lock_init(&frame_lock);
}

/* Allocates a frame on the frame table and returns a kvaddr */
void* alloc_frame(struct spte* sup_entry, enum palloc_flags flags){
    void* kvaddr = palloc_get_page(flags);
    if(!kvaddr){
        // palloc returned null so no more space -> evict
        if(!lock_held_by_current_thread(&frame_lock)){
            lock_acquire(&frame_lock);
        }
        kvaddr = evict_frame(sup_entry);
        if(lock_held_by_current_thread(&frame_lock)){
            lock_release(&frame_lock);
        }
    }
    if(sup_entry->entry_loc == SWAP){
        // Copy over data on to the palloc page
        char *kcopy = kvaddr;
        back_from_swap(sup_entry->swap_index, kcopy);
        lock_acquire(&thread_current()->spt_lock);
        pagedir_set_page(sup_entry->thread->pagedir, pg_round_down(sup_entry->page), kvaddr, sup_entry->write);
        lock_release(&thread_current()->spt_lock);
    }

    // Determine the index into the frame table
    // Subtracting from index bias moves out of kernel memory
    // Bit shift 12 removes offset
    uint32_t index = ((uint32_t) kvaddr - index_bias) >> 12;
    sup_entry->frame_index = index;
    sup_entry->swap_index = -1;
    sup_entry->entry_loc = MEM;

    struct fte* frame_entry = malloc(sizeof(struct fte));
    frame_entry->kvaddr = kvaddr;
    frame_entry->sup_entry = sup_entry;
    frame_entry->pinned = true;

    // Add to the frame table
    if(!lock_held_by_current_thread(&frame_lock)){
        lock_acquire(&frame_lock);
    }
    frame_table[index] = frame_entry; 
    if(lock_held_by_current_thread(&frame_lock)){
        lock_release(&frame_lock);
    }
    return kvaddr;
}

/* Method that finds a frame to evict and evicts it*/
void* evict_frame(struct spte* entry) {
    // Clock algorithm
    while(true){
        // Reset clock hand if at end of frame table
        if(clock_hand >= ft_size){
            clock_hand = 1;
        }

        // Skip NULL or pinned frames
        if(!frame_table[clock_hand] || frame_table[clock_hand]->pinned){
            clock_hand++;
            continue;
        }

        // Check if the frame has been accessed and if has reset accessed bit
        struct spte* evict_entry = frame_table[clock_hand]->sup_entry;
        lock_acquire(&thread_current()->spt_lock);
        if(pagedir_is_accessed(evict_entry->thread->pagedir, evict_entry->page)){
            pagedir_set_accessed(evict_entry->thread->pagedir, evict_entry->page, 0);
            lock_release(&thread_current()->spt_lock);

        }else{
            // Found frame to evict
            lock_release(&thread_current()->spt_lock);
            break;
        }
        clock_hand++;
    }

    // Evict the frame at clock_hand
    int eviction_index = clock_hand;
    struct fte* frame = frame_table[eviction_index];
    frame->pinned = true;

    // Write to swap if dirty or not backed by file
    lock_acquire(&thread_current()->spt_lock);
    if(!frame->sup_entry->file || pagedir_is_dirty(frame->sup_entry->thread->pagedir, frame->kvaddr)) {

        pagedir_clear_page(frame->sup_entry->thread->pagedir, frame->sup_entry->page);
        lock_release(&thread_current()->spt_lock);
        size_t swap_index = alloc_swap();
        if(swap_index == BITMAP_ERROR){
            ASSERT(false);
        }
        frame->sup_entry->swap_index = swap_index;
        frame->sup_entry->frame_index = -1;
        frame->sup_entry->entry_loc = SWAP;
        char *kcopy = frame->kvaddr;
        write_to_swap(swap_index, kcopy);

    }else{
        // Remove from pagedir because in file system
        pagedir_clear_page(frame->sup_entry->thread->pagedir, frame->sup_entry->page);
        lock_release(&thread_current()->spt_lock);
        frame->sup_entry->swap_index = -1;
        frame->sup_entry->frame_index = -1;
        frame->sup_entry->entry_loc = FSYS;
    }
    void* kvaddr = frame->kvaddr;
    dealloc_frame_index(eviction_index);
    return kvaddr;

}

/* Obtains a page from the file system into memory 
   Old load segment code, but reformatted */
void obtain_page(struct spte* entry) {
    void* kpage = alloc_frame(entry, PAL_USER | PAL_ZERO);

    // Load this page from the file system.
    if(entry->file && entry->read_bytes > 0){
        if(!lock_held_by_current_thread(&filesys_lock)){
            lock_acquire(&filesys_lock);
        }
        file_seek(entry->file, entry->offset);
        int read = file_read(entry->file, kpage, entry->read_bytes);
        if(lock_held_by_current_thread(&filesys_lock)){
            lock_release(&filesys_lock);
        }
        if(read != (int) entry->read_bytes){
            lock_acquire(&thread_current()->spt_lock);
            hash_delete(thread_current()->spt, &entry->hash_elem);
            lock_release(&thread_current()->spt_lock);
            palloc_free_page(kpage);
            return;
        }
    }
    memset((void*)((int)kpage + entry->read_bytes), 0, entry->zero_bytes);

    // Add the page to the process' page table
    lock_acquire(&thread_current()->spt_lock);
    if(!(pagedir_get_page(thread_current()->pagedir, entry->page) == NULL && 
          pagedir_set_page(thread_current()->pagedir, pg_round_down(entry->page), kpage, entry->write))){
        hash_delete(thread_current()->spt, &entry->hash_elem);
        lock_release(&thread_current()->spt_lock);
        palloc_free_page(kpage);
        return;
    }

    // Set the spte entry location to memory
    lock_release(&thread_current()->spt_lock);
    entry->entry_loc = MEM;
    lock_acquire(&frame_lock);
    frame_table[entry->frame_index]->pinned = false;
    lock_release(&frame_lock);
}

/* Deallocates a frame given its kvaddr */
void dealloc_frame_addr(void* kvaddr){
    // Do the index calculation backwards to remove frame table index
    uint32_t index = ((uint32_t) kvaddr - index_bias) >> 12;
    lock_acquire(&frame_lock);
    free(frame_table[index]);
    frame_table[index] = NULL; 
    lock_release(&frame_lock);
}

/* Deallocates a frame given its index */
void dealloc_frame_index(int frame_index){
    if(!lock_held_by_current_thread(&frame_lock)){
        lock_acquire(&frame_lock);
    }
    free(frame_table[frame_index]);
    frame_table[frame_index] = NULL;
    if(lock_held_by_current_thread(&frame_lock)){
        lock_release(&frame_lock);
    }
}