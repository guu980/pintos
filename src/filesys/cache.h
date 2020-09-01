#include <list.h>
#include "devices/block.h"
#include "threads/synch.h"

struct cache_e
  {
    char buf[BLOCK_SECTOR_SIZE];
    block_sector_t sector_idx;
    
    int slot_idx;
    bool dirty;
    int access_count;
    struct list_elem elem;
  };

struct read_ahead_e
{
  block_sector_t sector_idx;
  struct semaphore sector_sema;
  struct list_elem elem;
};

void cache_init(void);
struct cache_e *lookup_cache(block_sector_t);
void cache_evict(void);
struct cache_e *cache_create(block_sector_t);
struct cache_e *cache_load(block_sector_t);
struct cache_e *cache_load_ahead(block_sector_t);
void cache_write_from_buf(block_sector_t, void*);
void cache_read_from_buf(block_sector_t , void*);

void cache_flush(void);
void cache_flush_thread_func(void* aux);

void ra_list_flush(void);
void read_aheader_func(void *aux);

int cache_find_empty_slot(void);
void cache_slot_set(int, bool);

struct read_ahead_e * lookup_ra_list(block_sector_t);
struct read_ahead_e * rae_create(block_sector_t);

void cache_acquire(void);
void cache_release();

void read_ahead_acquire();
void read_ahead_release();