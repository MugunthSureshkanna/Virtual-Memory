#include "threads/vaddr.h"
#include <stddef.h>
#include "threads/palloc.h"
#include "threads/synch.h"

extern struct fte** frame_table;

extern struct lock frame_lock;

struct fte {
    void* kvaddr; // kernel virtual address (functionally physical address)
    struct spte* sup_entry; // supplemental page table entry
    bool pinned; // is the frame pinned?
};

void frametable_init(size_t user_pages, uint32_t bias);
void* alloc_frame(struct spte* sup_entry, enum palloc_flags flags);
void* evict_frame(struct spte* entry);
void obtain_page(struct spte* entry);
void dealloc_frame_addr(void* kvaddr);
void dealloc_frame_index(int frame_index);
