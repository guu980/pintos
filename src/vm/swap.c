#include "vm/swap.h"
#include "vm/frame.h"
#include "vm/page.h"
#include "threads/palloc.h"
#include "threads/interrupt.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include <stdio.h>
#include <string.h>
#include <debug.h>

struct bitmap *swap_table;

struct block *swap_disk_block;

struct lock swap_lock;

/* Number of sectors to store one page */
block_sector_t sector_for_page = PGSIZE/BLOCK_SECTOR_SIZE;

/* initializing swap table, the bitmap, and swap block, the swapping block. */
void swap_init(void)
{
    swap_disk_block = block_get_role(BLOCK_SWAP);
    if(swap_disk_block == NULL)
    {
        ASSERT("Fail allocating block");
    }
    /* Number of available slots for allocating pages */
    block_sector_t slot_nums = block_size(swap_disk_block) / sector_for_page;
    swap_table = bitmap_create(slot_nums);
    bitmap_set_all(swap_table, false); //false means that slot is empty

    lock_init(&swap_lock);
}

size_t find_empty_slot(void)
{
    return bitmap_scan(swap_table, 0, 1, false);
}

/* Swapping out. Copy data from page to Swapping block, and
mark it at bitmap(swap_table)to true

This is not locked. It is locked when it is called inside swap_out_only

Return the index of bitmap where page will be allocated
*/
void*
swap_out(void* upage, enum palloc_flags flags)
{
    void* kpage = palloc_get_page (flags);
    if(kpage != NULL){
        frame_update(kpage, upage);
        return kpage;
    }

    //palloc_get_page is failed so there should be no empty frame. We have to conduct swapping out
    //Choose frame to be evicted and copy data from frame to swap_disk
    struct fte *f = choose_victim();
    if(f == NULL)
    {
        PANIC("Victim is not chosen.");
    }
    void *victim_page = f->kpage;
    struct thread *owner_thread = f->owner;
    
    size_t slot_idx = find_empty_slot();
    if(slot_idx == BITMAP_ERROR)
    {
        PANIC("Swap disk is full.");
    }
    block_sector_t sector_idx = slot_idx*sector_for_page;
    for(block_sector_t i=0; i<sector_for_page; i++)
    {
        block_write(swap_disk_block, sector_idx+i, victim_page+i*BLOCK_SECTOR_SIZE);
    }
    slot_set(slot_idx, true);

    //pagedir clear (set to 0)
    pagedir_clear_page(owner_thread->pagedir, f->upage);
    
    //update the SPT. Change status to SWAPPED_OUT and store slot index information
    spte_swap_out(f->upage, owner_thread, slot_idx);
    
    //Remove victim page's fte in the frame table, and get page again
    frame_remove(victim_page);
    victim_page = palloc_get_page(flags);

    frame_update(victim_page, upage);
    
    //return the swapped frame (evicted frame)
    return victim_page;
}

/* This function is used when we only use swap_out in loading */
void *
swap_out_only(void* upage, enum palloc_flags flags)
{
    frame_acquire();
    void *kpage = swap_out(upage, flags);
    frame_release();
    return kpage;
}


/* Swapping in. Copy data from Swapping block to page, and
    mark it at bitmap(swap_table) to false */
void* swap_in(struct spte *, struct thread*);

void*
swap_in(struct spte* spte, struct thread* t)
{
    //Copy data from swap disk to new allocated frame
    if(spte==NULL || spte->status != SWAPPED_OUT)
    {
        PANIC("Unswapped frame trying to swap in");
    }
    //Allocate new kpage to store data from swap disk, which is swapped out before at faulted upage
    void *new_kpage = swap_out(spte->upage, PAL_USER);

    size_t slot_idx = spte->slot_idx;
    block_sector_t sector_idx = slot_idx*sector_for_page;
    for(block_sector_t i=0; i<sector_for_page; i++)
    {
        block_read(swap_disk_block, sector_idx+i, new_kpage+i*BLOCK_SECTOR_SIZE);
    }
    slot_set(slot_idx, false);

    //pagedir set (set to present, and update the kpage information)
    pagedir_set_page (t->pagedir, spte->upage, new_kpage, true);

    //update the SPT. change status to IN_FRAME and update kpage inforamtion
    spte->status = IN_FRAME;
    spte->kpage = new_kpage;
    
    unpin_fte(new_kpage);
    return new_kpage;
}

void* swap_in_only(struct spte *, void *);

void*
swap_in_only(struct spte *p, void *new_kpage)
{
    frame_acquire();
    void *kpage = swap_in(p, new_kpage);
    frame_release();
    return kpage;
}

void
slot_set(size_t slot_idx, bool bool_)
{
    bitmap_set(swap_table, slot_idx, bool_);
}

void
swap_dump()
{
    bitmap_dump(swap_table);
}

bool
swap_test(size_t slot_idx)
{
    return bitmap_test(swap_table, slot_idx);
}