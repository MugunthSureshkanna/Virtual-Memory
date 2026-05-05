#include <hash.h>
#include <stddef.h>
#include "threads/thread.h"

enum location {
    MEM = 001, // in DRAM
    SWAP = 002, // in swap space
    FSYS = 004 // in file system
};
 
struct spte {
    uint32_t* page; // user virtual address
    size_t swap_index; // index in swap table
    int frame_index; // index in frame table
    enum location entry_loc; // where the page is located in memory
    struct hash_elem hash_elem; // hash table element
    bool write; // is page writable?
    struct file *file; // file pointer if loaded from file
    uint32_t read_bytes; // number of bytes to read from file
    uint32_t zero_bytes; // number of bytes to zero
    uint32_t offset; // offset in the file
    struct thread* thread; // owning thread
};

void spt_init(struct hash* spt);
unsigned spt_hash_func (const struct hash_elem *e, void *aux);
bool spt_less_func (const struct hash_elem *a, const struct hash_elem *b, void *aux);
void destructor(struct hash_elem *e, void *aux);