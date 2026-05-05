# Pintos Virtual Memory
An extension of the Pintos operating system implementing demand paging, a frame eviction system, swap space management, and dynamic stack growth in C.

**Overview**
This project adds virtual memory support to Pintos, removing the constraint that all program data must reside in physical memory simultaneously. Three core subsystems work together: a supplemental page table (SPT) tracks the state of every virtual page, a frame table manages physical memory with a clock-based eviction policy, and a swap table handles paging pages out to disk.

**Key Components**
Supplemental Page Table (vm/page.c, vm/page.h)
A per-process hash table keyed on page-aligned virtual addresses. Each entry (spte) tracks where a page currently lives — physical memory (MEM), swap space (SWAP), or the file system (FSYS) — along with file metadata for lazy loading and a reference to the owning thread. The destructor walks the table on process exit to free all associated frames and swap slots.

Frame Table (vm/frame.c, vm/frame.h)
A global array of frame table entries (fte), indexed by physical address offset from kernel virtual memory base. Supports lazy allocation via palloc, with eviction triggered when physical memory is exhausted. Frames can be pinned to prevent eviction while kernel code is actively using them.

Clock Eviction Algorithm
Implements the clock (second-chance) page replacement algorithm. A global clock hand sweeps the frame table; frames with their accessed bit set get a second chance (bit cleared), while unaccessed frames are selected for eviction. Dirty pages or those not backed by a file are written to swap; clean file-backed pages are simply marked as evicted to FSYS and can be re-read on demand.

Swap Table (vm/swap.c, vm/swap.h)
A bitmap over the swap block device where each bit represents one page-sized slot (8 sectors). Uses bitmap_scan_and_flip for O(1) slot allocation. Provides write_to_swap and back_from_swap for moving pages between RAM and disk sector by sector.

Page Fault Handler (userprog/exception.c)
The central dispatch point. On a fault, it looks up the faulting address in the SPT. If found in FSYS, it calls obtain_page to load from the executable. If found in SWAP, it calls alloc_frame which restores the page from the swap block. If not found, it applies a stack growth heuristic: faults within 8 MB of PHYS_BASE and within 32 bytes below esp (to handle PUSHA) trigger a new zeroed page allocation and SPT insertion.

Synchronization
- frame_lock: global lock protecting frame table reads/writes and the clock hand
- swap_lock: protects bitmap and block device operations
- spt_lock (per-thread): protects each process's supplemental page table
- Frames are pinned during I/O to prevent eviction of in-use pages
