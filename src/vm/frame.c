/* include */
#include "vm/frame.h"
#include "vm/page.h"
#include "threads/synch.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "userprog/pagedir.h"
#include <hash.h>
#include <list.h>

/* Frame_Table as hash table is declared
   as global variable. This will be used when we
   call the functions in frame.h
   */
static struct hash Frame_Table;
static struct list Frame_Table_list;
static struct list_elem *clock_tick;

/* Lock for synchronization of update and remove */
struct lock frame_lock;

/* functions for hash_init() */
unsigned frame_hash (const struct hash_elem *, void *aux);
bool frame_less (const struct hash_elem *,
                    const struct hash_elem *, void *aux);

/* frame_hash function */
unsigned
frame_hash (const struct hash_elem *f_, void *aux UNUSED)
{
    const struct fte *f = hash_entry(f_, struct fte, hash_elem);
    return hash_bytes(&f->kpage, sizeof(f->kpage));
}

/* frame_less function */
bool
frame_less (const struct hash_elem *a_, const struct hash_elem *b_,
            void *aux UNUSED)
{    
    const struct fte *a = hash_entry(a_, struct fte, hash_elem);
    const struct fte *b = hash_entry(b_, struct fte, hash_elem);
    return a->kpage < b->kpage;
}

/* Frame_Table initialization 

   We initialize the table at the last step of paging_init()
   in init.c, or the first step of thread_init() in thread.c
   */
void
frame_init()
{
    hash_init(&Frame_Table, frame_hash, frame_less, NULL);
    list_init(&Frame_Table_list);
    lock_init(&frame_lock);
    //lock_init_frame(&frame_lock);
    clock_tick = NULL;
}

/* Look up the Frame_Table and return the address of fte,
   which has corresponding kpage value
   */
struct fte *
lookup_frame(const void* kpage)
{
    struct fte f;
    struct hash_elem *e;

    f.kpage = kpage;
    //lock_acquire(&frame_lock);
    e = hash_find(&Frame_Table, &f.hash_elem);
    //lock_release(&frame_lock);
    return e != NULL ? hash_entry(e, struct fte, hash_elem) : NULL;
}


/* Frame table updates
   
   When new couple of kpage-upage is generated,
   make new fte and insert to the Frame_Table

   This new entry is unpinned, since it is loaded.
   
   This function is applied into only install_page()
   in process.c
   */
void
frame_update(void* kpage, void* upage)
{
    if(lookup_frame(kpage)!=NULL) return;
    //only when kpage is unmapped
    
    struct fte *f;
    f = (struct fte*)malloc(sizeof(struct fte));
    fte_update(f, kpage, upage);
    hash_insert(&Frame_Table, &f->hash_elem);
    list_push_back(&Frame_Table_list, &f->list_elem);
}

/* Frame table remove
   
   Remove the frame of kpage address and
   the corresponding spte also.
   */
void
frame_remove(void* kpage)
{
    struct fte *f = lookup_frame(kpage);
    if(f != NULL)
    {
        //Remove correspoinding spte when it is not swapping
        struct thread * owner = f->owner;
        if(owner->USER_THREAD)
        {
            if(!owner->exiting){
                palloc_free_page(kpage);
                page_remove(f->upage, owner);
            }
        }
        
        //lock_acquire(&frame_lock);
        if(hash_delete(&Frame_Table, &f->hash_elem) != NULL)
        {
            if(clock_tick == &f->list_elem)
                clock_tick = NULL;
            list_remove(&f->list_elem);
            free(f);
        }
        //lock_release(&frame_lock);
    }
}
/*
struct fte *
choose_victim()
{
    struct hash_iterator i;

    hash_first (&i, &Frame_Table);
    int cnt = 5;
    struct fte* f = NULL;
    while (hash_next (&i) && cnt>0)
    {
        f = hash_entry (hash_cur (&i), struct fte, hash_elem);        
        cnt--;
    }
    return f;
}
*/

struct fte *
choose_victim()
{
    if(hash_empty(&Frame_Table))
        ASSERT("No entries in Frame Table but trying to evict\n");

    size_t cnt = 0;
    size_t iterate_size = 2*hash_size(&Frame_Table);
    for(int i=0;i<iterate_size;i++)
    {
        clock_ticking();
        struct fte *f = list_entry(clock_tick, struct fte, list_elem);
        uint32_t *pd = f->owner->pagedir;
        if(!f->pinned)
        {
            if(pagedir_is_accessed(pd, f->upage))
                pagedir_set_accessed(pd, f->upage, false);
            else
            {
                return f;
            }
        }
    }
    PANIC("There is no frame evictable");
}

void
clock_ticking()
{
    if(list_empty(&Frame_Table_list))
        ASSERT("No entries in Frame Table list but trying to tick\n");

    if (clock_tick == NULL || clock_tick == list_rbegin(&Frame_Table_list))
    {
        clock_tick = list_begin(&Frame_Table_list);
        return;
    }
        
    if(clock_tick == &Frame_Table_list.head || clock_tick == &Frame_Table_list.tail)
        ASSERT("Clock tick is on list's head or tail\n");

    clock_tick = list_next(clock_tick);
    return;
}

void
fte_update(struct fte* f, void* kpage, void* upage)
{
    f->kpage = kpage;
    f->upage = upage;
    f->pinned = true;

    //Do we need interrupt disable?
    enum intr_level old_level;
    old_level = intr_disable();
    f->owner = thread_current();
    intr_set_level(old_level);
}

void
frame_table_print()
{
    struct hash_iterator i;

    hash_first (&i, &Frame_Table);
    int cnt = 0;
    while (hash_next (&i))
    {
        struct fte *f = hash_entry (hash_cur (&i), struct fte, hash_elem);        
        cnt++;
        printf("%d : kpage %p mapped to upage %p\n",
        cnt, f->kpage, f->upage);
    }
}

void
frame_acquire()
{
    lock_acquire(&frame_lock);
}

void
frame_release()
{
    lock_release(&frame_lock);
}

void
pin_fte(void *kpage)
{
    struct fte *f = lookup_frame(kpage);
    f->pinned = true;
}

void
unpin_fte(void *kpage)
{
    struct fte *f = lookup_frame(kpage);
    f->pinned = false;
}