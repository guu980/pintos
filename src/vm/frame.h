/* include */
#include "lib/user/syscall.h"
#include "threads/thread.h"
#include <hash.h>

struct fte
{
    struct hash_elem hash_elem;
    struct list_elem list_elem;
    void* kpage;
    void* upage;

    /* Additional info */
    struct thread* owner;

    /* Flags */
    bool pinned;
};

/* Function prototypes */
void frame_init(void);
struct fte* lookup_frame(const void*);
void frame_update(void*, void*);
void frame_remove(void*);
struct fte* choose_victim(void);
void clock_ticking(void);
void fte_update(struct fte*, void*, void*);
void frame_table_print(void);

void frame_acquire(void);
void frame_release(void);

void pin_fte(void *);
void unpin_fte(void *);