#include "hash.h"
#include "page.h"
#include "frame.h"
#include <stdlib.h>
#include "bitmap.h"
#include "swap.h"

/**
 * Initializes the supplemental page table
 */
void spt_init(struct hash* spt) {
    hash_init(spt, spt_hash_func, spt_less_func, NULL);
}
 
/**
 * Hashing function for supplemental page table
 */
unsigned spt_hash_func (const struct hash_elem *e, void *aux) {
    const struct spte* entry = hash_entry (e, struct spte, hash_elem);
    return hash_int(((int) (entry->page)) >> 12);
}

/**
 * Comparison function for supplemental page table
 */
bool spt_less_func (const struct hash_elem *a, const struct hash_elem *b, 
        void *aux) {
    const struct spte *entry1 = hash_entry (a, struct spte, hash_elem);
    const struct spte *entry2 = hash_entry (b, struct spte, hash_elem);
    return (int) (entry1->page) < (int) (entry2->page);
}

/**
 * Destructor function for freeing supplemental page table
 */
void destructor(struct hash_elem *e, void *aux) {
    struct spte *temp = hash_entry(e, struct spte, hash_elem);
    if(temp->entry_loc == MEM){
        dealloc_frame_index(temp->frame_index);
    }
    if(temp->entry_loc == SWAP){
        dealloc_swap(temp->swap_index);
    }
    free(temp);
}