/* include */
#include "vm/page.h"
#include "vm/swap.h"
#include "vm/frame.h"
#include "threads/synch.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include <hash.h>

/* Supporting Page_Table as hash table is declared
   in each threads. This will be used when we
   call the functions in page.h

   Go to thread.c/h
   */

/* Lock for synchronization of update and remove */
struct lock page_lock;

/* functions for hash_init() */
unsigned page_hash (const struct hash_elem *, void *aux);
bool page_less (const struct hash_elem *,
                    const struct hash_elem *, void *aux);

/* page_hash function */
unsigned
page_hash (const struct hash_elem *p_, void *aux UNUSED)
{
    const struct spte *p = hash_entry(p_, struct spte, hash_elem);
    return hash_bytes(&p->upage, sizeof(p->upage));
}

/* page_less function */
bool
page_less (const struct hash_elem *a_, const struct hash_elem *b_,
            void *aux UNUSED)
{    
    const struct spte *a = hash_entry(a_, struct spte, hash_elem);
    const struct spte *b = hash_entry(b_, struct spte, hash_elem);
    return a->upage < b->upage;
}

/* In init.c, this function initialize page lock */
void
page_init()
{
    lock_init(&page_lock);
}

void
page_destroy(struct thread* t)
{
    struct hash_iterator i;

    hash_first (&i, &t->sup_page_table);
    hash_next(&i);
    while (hash_cur (&i))
    {
        struct spte *f = hash_entry (hash_cur (&i), struct spte, hash_elem);
        
        //swap table cleaning
        if(f->status == SWAPPED_OUT){
            slot_set(f->slot_idx, false);
        }
        
        //frame table cleaning
        else if(f->status == IN_FRAME){
            frame_remove(f->kpage);
        }

        hash_next(&i);
        free(f);
    }
}

/* Page_Table Creation

   We initialize the table when init_thread() in thread.c

   It fails at syn-write. Sync Or Memory problem?
   Sync : Usage of Lock?
   Memory : Malloc and Free?
   */
void
spt_create(void* spt)
{   
    hash_init(spt, page_hash, page_less, NULL);
}

/* Look up the Page_Table and return the address of spte,
   which has corresponding kpage value
   */
struct spte *
lookup_page_table(const void* upage, struct thread* t)
{
    struct spte p;
    struct hash_elem *e;

    p.upage = upage;
    e = hash_find(&t->sup_page_table, &p.hash_elem);
    return e != NULL ? hash_entry(e, struct spte, hash_elem) : NULL;
}


/* Page table updates
   
   When new couple of upage-kpage is generated,
   make new spte and insert to the Page_Table
   
   This function is applied into only install_page()
   in process.c
   */
void
page_update(void* upage, void* kpage)
{
    if(lookup_page_table(upage, thread_current())!=NULL)
    {
        return;
    }
    //only when upage is unmapped
    
    struct spte *p;
    p = (struct spte*)malloc(sizeof(struct spte));
    spte_update(p, upage, kpage);
    hash_insert(&thread_current()->sup_page_table, &p->hash_elem);
}

/* Page Table remove

   Remove the spte of upage address for current thread
   this function is used in frame_remove in frame.c
   */
void
page_remove(void* upage, struct thread *t)
{
    struct spte *p = lookup_page_table(upage, t);
    if(p != NULL && p->status == IN_FRAME)
    {
        if(hash_delete(&t->sup_page_table, &p->hash_elem) != NULL)
        {
            free(p);
        }
    }
}

void
spte_update(struct spte* p, void* upage, void* kpage)
{
    p->upage = upage;
    p->kpage = kpage;
    p->status = IN_FRAME;
    p->slot_idx = 0;
    p->dirty = false;
}

void
spte_swap_out(void* upage, struct thread *t, size_t slot_idx)
{
    struct spte *target = lookup_page_table(upage, t);
    if(target != NULL && target->status == IN_FRAME)
    {
        target->status = SWAPPED_OUT;
        target->slot_idx = slot_idx;
        target->dirty = pagedir_is_dirty(t->pagedir, target->upage);
    }
}
/* It loads the page in somewhere not in frame,
return the kpage it is allocated.
*/
void *
page_load(void *upage, struct thread* t)
{
    struct spte *p = lookup_page_table(upage, t);
    if(p == NULL){
        return NULL;
    }
    switch (p->status)
    {
        case SWAPPED_OUT:
            return swap_in(p, t);
            break;
        case IN_FILESYS:
            if(load_file(p, t))
                return p->kpage;
            break;
        case ALL_ZERO:
            return swap_out(upage, PAL_USER|PAL_ZERO);
            //result = new_kpage;
            break;
        case IN_FRAME:
            //printf("Page is in frame. Nothing to do\n");
            return p->kpage;
            break;
        default:
            PANIC ("Impossible status of spte\n");
            break;
    }
    return NULL;
}

void
pin_frame_by_upage(void* upage, struct thread* t)
{
    struct spte *p = lookup_page_table(upage, t);
    ASSERT(p != NULL && p->status == IN_FRAME);
    pin_fte(p->kpage);
}

void
unpin_frame_by_upage(void* upage, struct thread* t)
{
    struct spte *p = lookup_page_table(upage, t);
    ASSERT(p != NULL && p->status == IN_FRAME);
    unpin_fte(p->kpage); 
}

void
page_table_print(struct thread* t)
{
    struct hash_iterator i;

    hash_first (&i, &t->sup_page_table);
    int cnt = 0;
    while (hash_next (&i))
    {
        struct spte *f = hash_entry (hash_cur (&i), struct spte, hash_elem);
        if(f->status != -1){
            cnt++;
            printf("%d : upage %p mapped to kpage %p : %d at slot %d\n",
            cnt, f->upage, f->kpage, f->status, f->slot_idx);
        }
    }
}

bool
file_map(struct thread *cur, struct file *file, off_t ofs, uint8_t *upage, uint32_t page_read_bytes, uint32_t page_zero_bytes, bool writable)
{
    //frame_acquire();

    if(lookup_page_table(upage, cur)!=NULL)
    {
        return false;
    }

    struct spte *p;
    p = (struct spte *)malloc(sizeof(struct spte));

    p->upage = upage;
    p->kpage = NULL;
    p->file = file;
    p->ofs = ofs;
    p->page_read_bytes = page_read_bytes;
    p->page_zero_bytes = page_zero_bytes;
    p->writable = writable;
    p->status = IN_FILESYS;
    p->dirty = false;

    if(hash_insert(&cur->sup_page_table, &p->hash_elem) != NULL)
        return false;

    //frame_release();
    return true;
}

bool
load_file(struct spte *spte, struct thread *t)
{
    //Allocate new kpage to store data from swap disk, which is swapped out before at faulted upage
    void *new_kpage = swap_out(spte->upage, PAL_USER);

    //Copy data from file to new allocated frame
    if(spte==NULL || spte->status != IN_FILESYS)
    {
        PANIC("Unswapped frame trying to swap in");
    }

    struct file *file = spte->file;
    off_t ofs = spte->ofs;
    uint32_t page_read_bytes = spte->page_read_bytes;
    uint32_t page_zero_bytes = spte->page_zero_bytes;
    bool writable = spte->writable;
    
    if(page_read_bytes>PGSIZE)
        ASSERT("page read bytes is larger than page size\n");

    if(page_zero_bytes != PGSIZE - page_read_bytes)
        ASSERT("page zero bytes is incorret\n");

    file_seek(file, ofs);
    uint32_t temp;
    if (temp=file_read (file, new_kpage, page_read_bytes) != (int)page_read_bytes)
    {
        frame_remove(new_kpage);
        return false;
    }
    memset(new_kpage + page_read_bytes, 0, page_zero_bytes);

    //pagedir set (set to present, and update the kpage information)
    pagedir_set_page (t->pagedir, spte->upage, new_kpage, writable);

    //update the SPT. change status to IN_FRAME and update kpage inforamtion
    spte->status = IN_FRAME;
    spte->kpage = new_kpage;

    //add new kpage's fte in the frame table
    frame_update(new_kpage, spte->upage);
    
    unpin_fte(new_kpage);
    return true;
}

bool
load_file_only(struct spte *spte, struct thread *t)
{
    frame_acquire();
    bool result = load_file(spte,t);
    frame_release();
    return result;
}

bool
file_write_back(struct spte *p, struct thread *t)
{
    struct file *file = p->file;
    off_t ofs = p->ofs;
    uint32_t page_read_bytes = p->page_read_bytes;
    uint32_t page_zero_bytes = p->page_zero_bytes;
    void *kpage = p->kpage;

    ASSERT( page_read_bytes + page_zero_bytes == PGSIZE);

    file_seek(file, ofs);
    if (file_write (file, kpage, page_read_bytes) != (int)page_read_bytes)
    {
        return false;
    }
}