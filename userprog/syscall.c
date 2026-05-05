#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "pagedir.h"
#include "devices/shutdown.h"
#include "userprog/process.h"
#include "lib/user/syscall.h"
#include "threads/synch.h"
#include "devices/input.h"
#include "threads/palloc.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "threads/thread.h"
#include "vm/page.h"
#include "vm/frame.h"
#include "threads/malloc.h"

struct lock filesys_lock; // lock for synchronization of syscalls

static void syscall_handler (struct intr_frame *);
void isPointer(int sys_call_number, int arg0, int arg1, int arg2, struct intr_frame *f);
void valid_pointer(void* ptr);
static int get_single_arg(uint32_t** sp);
static void get_two_args(uint32_t** sp, int* arg0, int* arg1);
static void get_three_args(uint32_t** sp, int* arg0, int* arg1, int* arg2);
void unpin_frames(void* arg1, uint32_t arg2);
bool create_entry (struct thread* t, void* fault_addr, void* esp);

    /**
     * initializes syscall handler & semaphore
     */
    void syscall_init (void)
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init(&filesys_lock);
}

/**
 * Helper to check if pointer parameters for system calls are valid
 * switch case to only check valid_pointer if the system call actually takes 
 * a pointer 
 */
void isPointer(int sys_call_number, int arg0, int arg1, int arg2, struct intr_frame *f) {
  switch(sys_call_number){
    case SYS_EXEC:
    case SYS_CREATE:
    case SYS_REMOVE:
    case SYS_OPEN:
      valid_pointer((void*)arg0);
      break;
    case SYS_READ:
    case SYS_WRITE:
      // Iterate through the buffer
      void* start = pg_round_down((void*)arg1);
      void* end = pg_round_up(((char*) arg1) + sizeof(char) * arg2);
      while(start != end){
        // Validate each page of the buffer
        valid_pointer(start);

        // Check if in SPT
        struct spte* temp = malloc (sizeof (struct spte));
        temp->page = start;
        lock_acquire(&thread_current ()->spt_lock);
        struct hash_elem* temp_elem = hash_find(thread_current()->spt, &temp->hash_elem);

        if(!temp_elem){
          lock_release(&thread_current ()->spt_lock);
          // If not in SPT check for stack growth and grow stack
          if(!create_entry(thread_current(), start, f->esp)){
            free(temp_elem);
            thread_current()->exit_status = -1;
            thread_exit();
          }
          lock_acquire(&thread_current ()->spt_lock);
          temp_elem = hash_find(thread_current()->spt, &temp->hash_elem);
        }
        struct spte *sup_entry = hash_entry(temp_elem, struct spte, hash_elem);
        lock_release(&thread_current ()->spt_lock);

        // In SPT, so check different cases
        if(sup_entry->entry_loc == FSYS) {
          obtain_page(sup_entry);

        } else if(sup_entry->entry_loc == SWAP) {
          // Allocate the frame
          void* kvaddr = alloc_frame(sup_entry, PAL_USER | PAL_ZERO);
          lock_acquire(&thread_current()->spt_lock);

          // Add to the page directory of the current process
          if(!pagedir_get_page(thread_current()->pagedir, sup_entry->page)){
             if(!pagedir_set_page(thread_current()->pagedir, sup_entry->page, kvaddr, sup_entry->write)){
              lock_release(&thread_current()->spt_lock);
              free(temp);
              palloc_free_page(kvaddr);
              thread_current()->exit_status = -1;
              thread_exit();
            }
          }
          lock_release(&thread_current ()->spt_lock);
          sup_entry->entry_loc = MEM;
        }

        // Pin the buffer
        lock_acquire(&frame_lock);
        frame_table[sup_entry->frame_index]->pinned = true;
        lock_release(&frame_lock);
        free(temp);
        start = (char*) start + PGSIZE;
      }
      break;
    default:
      break;
  }
}

/**
 * Helper that checks if a pointer is valid:
 * - not a null pointer
 * - pointer is in user virtual address space
 * - pointer is to mapped virtual memory
 * If not valid pointer, exit with error (-1)
 */
void valid_pointer(void* ptr){
  if(!ptr || !is_user_vaddr(ptr)){
    thread_current()->exit_status = -1;
    thread_exit();
  }
  ptr = (char*) ptr + sizeof(char) * 3;
  if(!is_user_vaddr(ptr)){
    thread_current ()->exit_status = -1;
    thread_exit ();
  }
}

/**
 * Helper to parse the arguments for system calls off the given stack pointer
 */
static int get_single_arg(uint32_t** sp) {
  *sp = *sp + 1;
  valid_pointer(*sp);
  return **sp;
}

/**
 * Helper for two parameter system calls (uses the functionality from 
 * get_single_arg)
 */
static void get_two_args(uint32_t** sp, int* arg0, int* arg1) {
  *arg0 = get_single_arg(sp);
  *arg1 = get_single_arg(sp);
}

/**
 * Helper for two parameter system calls (uses the functionality from
 * get_single_arg)
 */
static void get_three_args(uint32_t** sp, int* arg0, int* arg1, int* arg2) {
  *arg0 = get_single_arg(sp);
  *arg1 = get_single_arg(sp);
  *arg2 = get_single_arg(sp);
}

/**
 * Method to execute system calls based on syscall number (retrived from bottom 
 * of stack)
*/
static void syscall_handler (struct intr_frame *f)
{
  valid_pointer(f->esp); // check if stack pointer is valid
  int sys_call_number = *((uint32_t *) f->esp); // get system call number
  // duplicate stack pointer for getting system call paramters without moving esp
  uint32_t* dup_sp = f->esp; 
  int arg0;
  int arg1;
  int arg2;
  // pointer to file_table for cleanliness
  struct file** file_table = thread_current()->file_table; 
  
  switch(sys_call_number) { // switch case for system calls 
    case SYS_HALT:
      shutdown_power_off();
      break;
    case SYS_EXIT:
      // arg0 = int status
      arg0 = get_single_arg(&dup_sp);
      isPointer(sys_call_number, arg0, 0, 0, f);
      thread_current()->exit_status = arg0;
      thread_exit();
      break;
    case SYS_EXEC:
      // arg0 = const char *cmd_line
      arg0 = get_single_arg(&dup_sp);
      isPointer(sys_call_number, arg0, 0, 0, f);
      char *cmd_line = (char*)arg0;
      tid_t child_tid = process_execute(cmd_line);
      if (child_tid == TID_ERROR) {
        f->eax = -1;
        break;
      }

      // Find the child in the list
      struct thread* c = NULL;
      for (struct list_elem* e = list_begin(&thread_current()->children); 
            e != list_end(&thread_current()->children); e = list_next(e)) {
        struct thread *candidate = list_entry(e, struct thread, child_elem);
        if (candidate->tid == child_tid) {
          c = candidate;
          break;
        }
      }
      ASSERT(c);

      // wait for load to complete
      sema_down(&c->load);

      if (!c->load_status) {
        // If child loading failed, ensure it exits before we return
        sema_down(&c->start_exit);
        list_remove(&c->child_elem);
        sema_up(&c->free_pcb);
        f->eax = -1;
        break;
      }

      // Child loaded successfully
      f->eax = child_tid;
      break;
    case SYS_WAIT:
      // arg0 = pid_t pid
      arg0 = get_single_arg(&dup_sp);
      isPointer(sys_call_number, arg0, 0, 0, f);
      f->eax = process_wait(arg0);
      break;
    case SYS_REMOVE:
      // arg0 = const char *file
      arg0 = get_single_arg(&dup_sp);
      isPointer(sys_call_number, arg0, 0, 0, f);
      lock_acquire(&filesys_lock);
      f->eax = filesys_remove((char*)arg0); 
      lock_release(&filesys_lock);
      break;
    case SYS_OPEN:
      // arg0 = const char *file
      arg0 = get_single_arg(&dup_sp);
      isPointer(sys_call_number, arg0, 0, 0, f);
      lock_acquire(&filesys_lock);
      struct file* opened_file = filesys_open((char*)arg0);
      lock_release(&filesys_lock);
      f->eax = -1;      // find fd to allocate new file in the file_table
      if(opened_file){
        // if gap doesn't exit, allocate at num_files index of the file_table
        if(!thread_current()->gap_exists){ 
          f->eax = thread_current()->num_files;
          file_table[thread_current()->num_files++] = opened_file;
        }else{
          // Find the empty spot in the file table since the a gap exists 
          // and place the file there
          for (int i = 2; i < thread_current()->num_files; i++) {
            if (file_table[i] == NULL) {
              f->eax = i;
              file_table[i] = opened_file;
              if (i == thread_current()->num_files - 1) {
                thread_current()->gap_exists = false;
              }
              break;
            }
          }
        }
      }
      break;
    case SYS_FILESIZE:
      // arg0 = int fd
      arg0 = get_single_arg(&dup_sp);
      isPointer(sys_call_number, arg0, 0, 0, f);
      f->eax = -1;
      if(arg0 < thread_current()->num_files && arg0 >= 0 && file_table[arg0]){
        lock_acquire(&filesys_lock);
        f->eax = file_length(file_table[arg0]);
        lock_release(&filesys_lock);
      }
      break;
    case SYS_TELL:
      // arg0 = int fd
      arg0 = get_single_arg(&dup_sp);
      isPointer(sys_call_number, arg0, 0, 0, f);
      f->eax = -1;
      if(arg0 < thread_current()->num_files && arg0 >= 0 && file_table[arg0]){
        lock_acquire(&filesys_lock);
        f->eax = file_tell(file_table[arg0]);
        lock_release(&filesys_lock);
      }
      break;
    case SYS_CLOSE:
      // arg0 = int fd
      arg0 = get_single_arg(&dup_sp);
      isPointer(sys_call_number, arg0, 0, 0, f);
      if(arg0 < thread_current()->num_files && arg0 >= 2 && file_table[arg0]){
        lock_acquire(&filesys_lock);
        file_close(file_table[arg0]);
        lock_release(&filesys_lock);
        // Check if the file being closed is in middle or end of the file table
        if (arg0 != thread_current()->num_files - 1) {
          thread_current()->gap_exists = true;
        } else {
          thread_current()->num_files--;
        }
        file_table[arg0] = NULL;
      }
      break;
    case SYS_CREATE:
      // arg0 = const char *file
      // arg1 = unsigned initial_size
      get_two_args(&dup_sp, &arg0, &arg1);
      isPointer(sys_call_number, arg0, arg1, 0, f);
      lock_acquire(&filesys_lock);
      f->eax = filesys_create((char*)arg0, arg1);
      lock_release(&filesys_lock);
      break;
    case SYS_SEEK:
      // arg0 = int fd
      // arg1 = unsigned position
      get_two_args(&dup_sp, &arg0, &arg1);
      isPointer(sys_call_number, arg0, arg1, 0, f);
      if (arg0 < thread_current()->num_files && arg0 >= 0 && file_table[arg0]) {
        lock_acquire(&filesys_lock);
        file_seek(file_table[arg0], arg1);
        lock_release(&filesys_lock);
      }
      break;
    case SYS_READ:
      // arg0 = int fd
      // arg1 = void *buffer
      // arg2 = unsigned size
      get_three_args(&dup_sp, &arg0, &arg1, &arg2);
      isPointer(sys_call_number, arg0, arg1, arg2, f);
      if(arg0 == 0){
        // Case that we read from stdin and add to buffer
        lock_acquire(&filesys_lock);
        char* buffer_cpy = (char*)arg1;
        for(uint32_t i = 0; i < (uint32_t)arg2; i++) {
          *buffer_cpy = input_getc();
          buffer_cpy++;
        }
        lock_release(&filesys_lock);
        f->eax = f->eax == -1 ? -1 : arg2;
      } else if(arg0 == 1){
        // Invalid since fd being one is stdout
        f->eax = -1;
      } else {
        if(arg0 < thread_current()->num_files && arg0 >= 0 && file_table[arg0]){
          // Case that we have to read from a certain file
          lock_acquire(&filesys_lock);
          f->eax = file_read(file_table[arg0], (void*)arg1, arg2);
          if(lock_held_by_current_thread(&filesys_lock)) {
            lock_release(&filesys_lock);
          }

          // Unpin the frames after syscall is complete
          unpin_frames(arg1, arg1 + arg2);
        }
      }
      break;
    case SYS_WRITE:
      // arg0 = int fd
      // arg1 = const void *buffer
      // arg2 = unsigned size
      get_three_args(&dup_sp, &arg0, &arg1, &arg2);
      isPointer(sys_call_number, arg0, arg1, arg2, f);

      if(arg0 == 0){
        // Invalid case because fd being 0 is stdin
        f->eax = -1;
      } else if(arg0 == 1){
        /*Case that we need to write to the buffer
          Only writing 200 characters at a time since
          we do not want to write too much at one time*/
        int size_remaining = arg2;
        char* buf_cpy = (char*)arg1;
        lock_acquire(&filesys_lock);
        while (size_remaining > 0) {
          if(size_remaining < 200){
            putbuf(buf_cpy, size_remaining);
            size_remaining = 0;
          }else{
            putbuf(buf_cpy, 200);
            size_remaining -= 200;
            buf_cpy += 200;
          }
        }
        lock_release(&filesys_lock);
        f->eax = arg2;
      } else {
        // Case that we need to write to a file
        if(arg0 < thread_current()->num_files && arg0 >= 0 && file_table[arg0]){
          lock_acquire (&filesys_lock);
          f->eax = file_write(file_table[arg0], (void*)arg1, arg2);
          if(lock_held_by_current_thread(&filesys_lock)) {
            lock_release(&filesys_lock);
          }

          // Unpin the frames after syscall is complete
          unpin_frames(arg1, arg1 + arg2);
        }
      }
      break;
  }
}

/* Helper method that grows the stack.
   This is needed here because we are in the kernel context
   so page faulting is not allowed. This allows stack growth 
   from inside the kernel context */
bool create_entry(struct thread* t, void* fault_addr, void* esp){
  // Check stack growth conditions
  bool valid_stack_addr = (uint32_t) fault_addr >= ((uint32_t) PHYS_BASE - (1<<23));
  bool valid_relative_to_esp = ((uint32_t) fault_addr >= (uint32_t) esp) ||
                                ((uint32_t) esp - (uint32_t) fault_addr <= 32);
  if(!valid_stack_addr || !valid_relative_to_esp){
    return false;
  }
  // Define the meta-data for the new entry
  struct spte *new_entry = malloc (sizeof (struct spte));
  new_entry->page = pg_round_down(fault_addr);
  new_entry->entry_loc = MEM;
  new_entry->swap_index = -1;
  new_entry->frame_index = -1;
  new_entry->write = true;
  new_entry->thread = t;
  new_entry->file = NULL;
  new_entry->read_bytes = 0;
  new_entry->zero_bytes = PGSIZE;
  
  // Allocate the frame and add it to the process' page directory and SPT
  void *kvaddr = alloc_frame (new_entry, PAL_USER | PAL_ZERO);
  lock_acquire(&thread_current()->spt_lock);
  pagedir_set_page(thread_current()->pagedir, pg_round_down(fault_addr), kvaddr, new_entry->write);
  hash_insert(thread_current()->spt, &new_entry->hash_elem);
  lock_release(&thread_current ()->spt_lock);
  return true;
}

/* Helper method to unpin the buffer */
void unpin_frames(void* arg1, uint32_t arg2) {
  // Iterate the entire buffer
  void* start = pg_round_down((void*)arg1);
  void* end = pg_round_up(arg2);
  while(start != end){
    struct spte* temp = malloc(sizeof (struct spte));
    temp->page = start;
    lock_acquire(&thread_current()->spt_lock);
    struct hash_elem* temp_elem = hash_find(thread_current()->spt, &temp->hash_elem);
    lock_release(&thread_current()->spt_lock);

    // Check if the entry is valid and then unpin
    struct spte *sup_entry = temp_elem ? hash_entry(temp_elem, struct spte, hash_elem) :  NULL;
    if(sup_entry){ 
      lock_acquire(&frame_lock);
      frame_table[sup_entry->frame_index]->pinned = false;
      lock_release(&frame_lock);
    }
    free(temp);
    start = (char*) start + PGSIZE;
  }
}

