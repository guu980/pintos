#include "filesys/cache.h"
#include <string.h>
#include <stdio.h>
#include "filesys/filesys.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "devices/timer.h"

struct list cache_list;

struct lock cache_lock;

bool cache_checker[64];

struct list read_ahead_list;

struct lock read_ahead_lock;

/* Initialize cache_list, cache_lock (not yet!)
  This code will be implanted in init.c  
  */
void
cache_init(void)
{
  memset(cache_checker, false, sizeof(cache_checker));
  list_init(&cache_list);
  list_init(&read_ahead_list);
  lock_init(&cache_lock);
  lock_init(&read_ahead_lock);
  thread_create("cache_flush_thread", PRI_MIN, cache_flush_thread_func, NULL);
  thread_create("read_aheader", PRI_MIN, read_aheader_func, NULL);
}

/* Look up cache list and return the pointer to
  cache_e that corresponds to given sector number
  */
struct cache_e *
lookup_cache (block_sector_t sector_idx)
{
  struct list_elem *temp = NULL;
  struct cache_e *result = NULL;

  /* Find the cache_e corresponding to the given sector_idx,
  iterating the cache_list */
  for(temp = list_begin(&cache_list);
  temp != list_end(&cache_list) ; temp = list_next(temp))
  {
    result = list_entry(temp, struct cache_e, elem);
    if(sector_idx == result->sector_idx)
    {
      return result;
    }
  }
  return NULL;
}

void
cache_evict(void)
{  
  ASSERT(cache_find_empty_slot() == -1);

  struct list_elem *temp;
  struct cache_e *min = list_entry(list_begin(&cache_list), struct cache_e, elem);
  
  for(temp = list_begin(&cache_list);
  temp != list_end(&cache_list); temp = list_next(temp))
  {
    struct cache_e *compare = list_entry(temp, struct cache_e, elem);
    if(compare->access_count < min->access_count)
    {
      min = compare;
    }
  }

  cache_slot_set(min->slot_idx, false);
  list_remove(&min->elem);
  
  if(min->dirty)
  {
    block_write(fs_device, min->sector_idx, min->buf);
  }

  free(min);
}

struct cache_e *
cache_create(block_sector_t sector)
{
  int empty_slot_idx = cache_find_empty_slot();
  if(empty_slot_idx == -1)
    PANIC("There is no empty slot in buffer cache\n");

  struct cache_e *cache_entry = (struct cache_e *)malloc(sizeof(struct cache_e));
  memset(cache_entry->buf, 0, sizeof(cache_entry->buf));
  cache_entry->dirty = false;
  cache_entry->sector_idx = sector;
  cache_entry->slot_idx = empty_slot_idx;
  cache_entry->access_count = 0;

  list_push_back(&cache_list, &cache_entry->elem);
  cache_slot_set(empty_slot_idx, true);

  return cache_entry;
}


struct cache_e *
cache_load(block_sector_t sector_idx)
{
  //Flag for it is waited for aheader
  bool waited_for_aheader = false;

  /* Look whether there is read_ahead_e for this sector. If it is,
    wait until the read aheader finishes reading. After reading,
    there would be new cache_e for that sector, so no further fetching
    would occur.
    */
  read_ahead_acquire();
  struct read_ahead_e *read_ahead_entry = lookup_ra_list(sector_idx);
  if(read_ahead_entry != NULL)
  {
    sema_down(&read_ahead_entry->sector_sema);
    waited_for_aheader = true;
  }
  read_ahead_release();

  cache_acquire();
  struct cache_e *cache_entry = lookup_cache(sector_idx);

  if(cache_entry == NULL)
  {
    if(cache_find_empty_slot() == -1)
    {
      cache_evict();
    }
    cache_entry = cache_create(sector_idx);
    block_read(fs_device, sector_idx, cache_entry->buf);
  }
  cache_entry->access_count++;
  cache_release();

  /* Read Ahead!
    Make rae only when there is same rae in list.
  */
  read_ahead_acquire();
  if(lookup_ra_list(sector_idx+1) == NULL)
  {
    struct read_ahead_e * new_rae = rae_create(sector_idx+1); //here
    sema_down(&new_rae->sector_sema);
  }
  read_ahead_release();

  return cache_entry;
}


/* cache load function for read aheader
  Just read from the block to the sector. This will not be
  interrupted because the sema for this rae is 0, so any other
  request to read this sector will be blocked.
 */
struct cache_e *
cache_load_ahead(block_sector_t sector_idx)
{
  cache_acquire();
  struct cache_e *cache_entry = lookup_cache(sector_idx);

  if(cache_entry == NULL)
  {
    if(cache_find_empty_slot() == -1)
    {
      cache_evict();
    }
    cache_entry = cache_create(sector_idx);
    block_read(fs_device, sector_idx, cache_entry->buf);
  }
  //Access count will be increased by the requesting thread
  cache_release();

  return cache_entry;
}

/* Cache load for the data of Sector size, and write to the
  buffer, it doens't need to memcpy after cache_load.
*/
void
cache_write_from_buf(block_sector_t sector, void* buffer)
{
  struct cache_e* cache_entry = cache_load(sector);
  memcpy(cache_entry->buf, buffer, BLOCK_SECTOR_SIZE);
  cache_entry->dirty = true;
  block_write(fs_device, sector, buffer);
}

void
cache_read_from_buf(block_sector_t sector, void* buffer)
{
  struct cache_e* cache_entry = cache_load(sector);
  memcpy(buffer, cache_entry->buf, BLOCK_SECTOR_SIZE);
}

/* Functions for cache_flush */
void
cache_flush(void)
{
  cache_acquire();
  struct list_elem *temp = NULL;
  for(temp = list_begin(&cache_list);
  temp != list_end(&cache_list); temp = list_next(temp))
  {
    struct cache_e *cache_entry = list_entry(temp, struct cache_e, elem);
    if(cache_entry->dirty)
    {
      block_write(fs_device, cache_entry->sector_idx, cache_entry->buf);
      cache_entry->dirty = false;
    }
  }
  cache_release();
}

void
cache_flush_thread_func(void* aux UNUSED)
{
  while(true)
  {
    timer_msleep(100);
    cache_flush();
  }
}

/* Function for read ahead list flush 
  It flushes the read ahead list in filesys_done.
  */
void
ra_list_flush(void)
{
  read_ahead_acquire();
  while(!list_empty(&read_ahead_list))
  {
    struct list_elem *temp = list_pop_front(&read_ahead_list);
    struct read_ahead_e *read_ahead_entry = list_entry(temp, struct read_ahead_e, elem);
    free(read_ahead_entry);
  }
  read_ahead_release();
}

/* Function for read aheader 
  This is thread that read the sector by cache_load_ahead
  It sweeps the list and load the sectors in read ahead list.
  After read, it just removes the list element. Therefore, the
  remaining read_ahead_e has to be removed by afterward request,
  or when the filesys is done.
  */
void
read_aheader_func(void *aux UNUSED)
{
  while(true)
  {
    timer_msleep(50);
    if(!list_empty(&read_ahead_list))
    {
      struct read_ahead_e *read_ahead_entry = 
      list_entry(list_front(&read_ahead_list), struct read_ahead_e, elem);
      cache_load_ahead(read_ahead_entry->sector_idx);
      
      list_remove(&read_ahead_entry->elem);

      /* There might be many threads that are waiting for this sector
        So, we need to sema_up all of them.
      */
      while(!list_empty(&read_ahead_entry->sector_sema.waiters))
        sema_up(&read_ahead_entry->sector_sema);
      
      /* After unblocking the whole waiting thread, they will read
      from cache, so just free the entry
      */
      free(read_ahead_entry);
    }
  }
}

/* Function for read ahead list */

/* This function looks up read ahead list and get the
  entry corresponding to the sector_idx 
  Maybe it has to be synchronized with other
  list modifying functions, such as rae_create or list_remove.
  */
struct read_ahead_e *
lookup_ra_list(block_sector_t sector_idx)
{
  struct list_elem *temp;
  for(temp = list_begin(&read_ahead_list);
  temp != list_end(&read_ahead_list); temp = list_next(temp))
  {
    struct read_ahead_e *read_ahead_entry = list_entry(temp, struct read_ahead_e, elem);
    if(sector_idx == read_ahead_entry->sector_idx)
    {
      return read_ahead_entry;
    }
  }
  return NULL;
}

/* This function creates new read_ahead_e, when one thread
  wants to prefetch the next sector in cache_load
  */
struct read_ahead_e *
rae_create(block_sector_t sector_idx)
{
  struct read_ahead_e *read_ahead_entry = (struct read_ahead_e *)malloc(sizeof(struct read_ahead_e));
  sema_init(&read_ahead_entry->sector_sema, 1);
  read_ahead_entry->sector_idx = sector_idx;

  list_push_back(&read_ahead_list, &read_ahead_entry->elem);

  return read_ahead_entry;
}

/* Functions for slot */
int
cache_find_empty_slot(void)
{
  for(int i=0; i<64; i++)
  {
    if(cache_checker[i] == false)
      return i;
  }

  return -1;
}

void
cache_slot_set(int slot_idx, bool result)
{
  cache_checker[slot_idx] = result;
}

/* Functions for lock */
void
cache_acquire()
{
  lock_acquire(&cache_lock);
}

void
cache_release()
{
  lock_release(&cache_lock);
}

void
read_ahead_acquire()
{
  lock_acquire(&read_ahead_lock);
}

void
read_ahead_release()
{
  lock_release(&read_ahead_lock);
}