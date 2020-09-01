//#ifdef VM_SWAP_H
#define VM_SWAP_H

#include "devices/block.h"
#include "lib/kernel/bitmap.h"
#include "threads/palloc.h"
#include "threads/vaddr.h"
#include "threads/thread.h"
#include "lib/kernel/hash.h"

void swap_init(void);
size_t find_empty_slot(void);

void* swap_out(void*, enum palloc_flags);
void* swap_out_only(void*, enum palloc_flags);
//swap in is declared in swap.c due to its parameter

void slot_set(size_t, bool);

void swap_dump(void);
bool swap_test(size_t);
//#endif