/* include */
#include "lib/user/syscall.h"
#include "threads/thread.h"
#include "filesys/off_t.h"
#include <hash.h>

enum frame_status{
    SWAPPED_OUT = 001,
    IN_FILESYS = 002,
    ALL_ZERO = 003,
    IN_FRAME = 004
};

/* Struct spte(supporting page table entry) */
struct spte
{
    struct hash_elem hash_elem;
    void* upage;
    void* kpage;

    /* Additional info */
    size_t slot_idx;

    /* Mapped Filesys */
    struct file *file;
    off_t ofs;
    uint32_t page_read_bytes;
    uint32_t page_zero_bytes;
    bool writable;
    bool dirty;

    /* Flags */
    enum frame_status status;
};

/* Function prototypes */
void page_init(void);
void page_destroy(struct thread*);
void spt_create(void*);

struct spte* lookup_page_table(const void*, struct thread*);
void page_update(void*, void*);
void page_remove(void*, struct thread *);
void spte_update(struct spte*, void*, void*);
void spte_swap_out(void*, struct thread *, size_t);

void* page_load(void*, struct thread*);
void pin_frame_by_upage(void*, struct thread*);
void unpin_frame_by_upage(void*, struct thread*);
void page_table_print(struct thread *);

bool file_map(struct thread *, struct file *, off_t, uint8_t *, uint32_t, uint32_t, bool);
bool load_file(struct spte*, struct thread*);
bool load_file_only(struct spte*, struct thread*);
bool file_write_back(struct spte *, struct thread *);
