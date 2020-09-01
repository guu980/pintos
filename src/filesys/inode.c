#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "filesys/cache.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

#define DIRECT 96
#define INDIRECT 128

struct lock inode_lock;

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    block_sector_t direct[DIRECT];
    block_sector_t indirect;
    block_sector_t double_indirect;

    bool is_dir;
    int is_opened;
    int is_cwd;

    off_t length;                       /* File size in bytes. */
    unsigned magic;                     /* Magic number. */

    char unused[100];
  };

struct indirect_disk
  {
    block_sector_t data[INDIRECT];
  };

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode 
  {
    struct list_elem elem;              /* Element in inode list. */
    block_sector_t sector;              /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    struct inode_disk data;             /* Inode content. */
  };

  
/* Functions for indexed allocation */
bool inode_alloc(struct inode_disk*, size_t, int);
size_t indirect_alloc(struct indirect_disk*, size_t, int);
bool sector_alloc(struct inode *, size_t);

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector (const struct inode *inode, off_t pos, bool writing) 
{
  ASSERT (inode != NULL);
  if (pos < inode->data.length)
    return index_to_sector(inode, byte_to_index(pos));
  else
  {
    if(writing)
    {
      size_t sectors = bytes_to_sectors(pos)
                      - bytes_to_sectors(inode->data.length);
      if(sector_alloc(inode, sectors))
      {
        return index_to_sector(inode, byte_to_index(pos));
      }
    }
    else
    {
      return -1;
    }
  }
  return -1;
}

block_sector_t
byte_to_index(off_t bytes)
{
  return bytes / BLOCK_SECTOR_SIZE;
}

block_sector_t
index_to_sector(struct inode *inode, block_sector_t idx)
{
  block_sector_t sector;

  if(idx < DIRECT)
  {
    sector = inode->data.direct[idx];
  } 
  else if(idx<DIRECT + INDIRECT)
  {
    struct indirect_disk *indirect_first = (struct indirect_disk *)malloc(sizeof(struct indirect_disk));
    cache_read_from_buf(inode->data.indirect, indirect_first);
    sector = indirect_first->data[idx-DIRECT];
    free(indirect_first);
  }
  else if(idx<DIRECT + INDIRECT + INDIRECT*INDIRECT)
  {
    struct indirect_disk *indirect_first = (struct indirect_disk *)malloc(sizeof(struct indirect_disk));
    cache_read_from_buf(inode->data.double_indirect, indirect_first);
    int indirect_second_idx = (idx - DIRECT - INDIRECT) / INDIRECT;
    int indirect_second_ofs = (idx - DIRECT - INDIRECT) % INDIRECT;
    struct indirect_disk *indirect_second = (struct indirect_disk *)malloc(sizeof(struct indirect_disk));
    cache_read_from_buf(indirect_first->data[indirect_second_idx], indirect_second);
    sector = indirect_second->data[indirect_second_ofs];
    free(indirect_second);
    free(indirect_first);
  }
  else
  {
    PANIC("idx exceed the limit!!");
  }
  
  return sector;
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
  lock_init (&inode_lock);
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length, bool is_dir)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  //printf("%d\n", sizeof(*disk_inode));

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      size_t sectors = bytes_to_sectors (length);
      disk_inode->length = length;
      disk_inode->magic = INODE_MAGIC;
      disk_inode->is_dir = is_dir;
      disk_inode->is_cwd = 0;
      disk_inode->is_opened = 0;

      if(inode_alloc(disk_inode, sectors, 0))
      {
        cache_write_from_buf(sector, disk_inode);
        success = true;
      }
      else PANIC("PANIC WHILE CREATING");
        
      free (disk_inode);
    }
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector) 
        {
          inode_reopen (inode);
          return inode; 
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL){
    printf("Malloc Failed\n");
    return NULL;
  }

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  block_read (fs_device, inode->sector, &inode->data);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) 
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);
 
      /* Deallocate blocks if removed. */
      if (inode->removed) 
        {
          free_map_release (inode->sector, 1);
          block_sector_t max_idx = byte_to_index(inode->data.length);
          for(int i = 0; i < max_idx ; i++)
          {
            free_map_release (index_to_sector(inode, i), 1);
          }
        }

      free (inode); 
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;

  while (size > 0) 
    {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset, false);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      struct cache_e *cache_entry = cache_load(sector_idx);
      memcpy(buffer + bytes_read, cache_entry->buf + sector_ofs, chunk_size);
      
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  bool extended = false;

  if (inode->deny_write_cnt)
    return 0;

  byte_to_sector(inode, offset+size, true);
  if(inode->data.length < offset+size)
  {
    inode->data.length = offset+size;
    extended = true;
  }
  cache_write_from_buf(inode->sector, &inode->data);

  while (size > 0) 
    {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset, false);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      struct cache_e * cache_entry = cache_load(sector_idx);
      memcpy (cache_entry->buf + sector_ofs, buffer + bytes_written, chunk_size);
      cache_entry->dirty = true;

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  return inode->data.length;
}

bool
inode_alloc(struct inode_disk* disk_inode, size_t sectors, int idx_ofs)
{
  static char zeros[BLOCK_SECTOR_SIZE];

  if(sectors == 0) return true;

  /* Allocate direct inode_disk to filesys blocks */
  size_t direct_sectors = idx_ofs + sectors < DIRECT ?
                          idx_ofs + sectors : DIRECT;
  for(int i = idx_ofs; i<direct_sectors ; i++)
  { 
    if(free_map_allocate (1, disk_inode->direct + i))
    {
      cache_write_from_buf(disk_inode->direct[i], zeros);
      sectors--;
    }
    else PANIC("Failed at single indirect_alloc");
  }

  if(sectors == 0) return true;

  /* update idx_ofs, to point the ofs from indirect disk */
  idx_ofs = 0 > idx_ofs-DIRECT ? 0 : idx_ofs-DIRECT;

  /* Allocate new sector for indirect_disk, only when it is newly declared
    We can know it by idx_ofs. If it is 0, it means it started allocation
    from direct sectors. Therefore, there is no sector for indirect_disk
    */
  struct indirect_disk* disk_indirect = calloc(1, sizeof *disk_indirect);
  /* If idx_ofs is not zero,
    just read the existing indirect_disk from sector
    */
  if(idx_ofs != 0)
  {
    cache_read_from_buf(disk_inode->indirect, disk_indirect);
  }

  /* Fill in the elements of disk_indirect */
  sectors = indirect_alloc(disk_indirect, sectors, idx_ofs);
  
  /* Allocate indirect inode_disk to filesys blocks */
  block_sector_t sector;
  if(idx_ofs == 0)
  {
    if(free_map_allocate(1, &sector))
    {
      disk_inode->indirect = sector;
    }
    else  PANIC("Failed at single indirect_alloc");
  }
  /* If the indirect disk already exists, obtain it */
  else
  {
    sector = disk_inode->indirect;
  }
  cache_write_from_buf(sector, disk_indirect);
  free(disk_indirect);

  if(sectors == 0) return true;

  /* Simple assertion, limiting file size
  if(sectors > INDIRECT * INDIRECT)
  {
    PANIC("File size larger than 8MB");
  } */

  /* update idx_ofs, to point the ofs from indirect disk */
  idx_ofs = 0 > idx_ofs-INDIRECT ? 0 : idx_ofs-INDIRECT;

  /* This allocates double indirect sectors
    First, calloc an indirect_disk to store the double_indirect
  */
  struct indirect_disk* double_indirect_disk
  = calloc(1, sizeof *double_indirect_disk);
  if(idx_ofs != 0)
  {
    cache_read_from_buf(disk_inode->double_indirect, double_indirect_disk);
  }

  /* Store the idx_ofs to decide wheter to free_map_alloc */
  int idx_ofs_double = idx_ofs;

  /* Iterate and allocate indirect disks */
  for(int i = idx_ofs / INDIRECT; i<DIV_ROUND_UP(sectors, INDIRECT); i++)
  {
    /* Make idx_ofs as a idx_ofs for a single indirect disk */
    idx_ofs = idx_ofs % INDIRECT;
    struct indirect_disk* disk_indirect = calloc(1, sizeof *disk_indirect);
    
    if(idx_ofs != 0)
    {
      cache_read_from_buf(double_indirect_disk->data[i], disk_indirect);
    }
  
    /* Fill in the elements of disk_indirect */
    sectors = indirect_alloc(disk_indirect, sectors, idx_ofs);
    
    /* Allocate indirect inode_disk to filesys blocks */
    block_sector_t sector;
    if(idx_ofs == 0){
      if(free_map_allocate(1, &sector))
      {
        double_indirect_disk->data[i] = sector;
      }
      else PANIC("PANIC");
    }
    else
    {
      sector = double_indirect_disk->data[i];
    }    
    cache_write_from_buf(sector, disk_indirect);
    free(disk_indirect);
    
    /* Now make the idx_ofs to 0, since the next indirect disks will
      always be allocated from start
      */
    idx_ofs = 0;
  }

  /* Save the double indirect disk to block and store it in
    inode_disk structure
   */
  if(idx_ofs_double == 0)
  {
    if(free_map_allocate(1, &sector))
    {
      disk_inode->double_indirect = sector; 
    }
    else PANIC("PANIC");
  }
  else
  {
    sector = disk_inode->double_indirect;
  }
  cache_write_from_buf(sector, double_indirect_disk);
  free(double_indirect_disk);

  if(sectors == 0) return true;
  
  return false;
}

/* This allocates one indirect_disk to one sector in block
  And returns the remaining sectors after allocating them.
*/
size_t
indirect_alloc(struct indirect_disk* disk_indirect, size_t sectors, int idx_ofs)
{
  static char zeros[BLOCK_SECTOR_SIZE];
  
  /* Allocate each indirect sectors */
  size_t indirect_sectors = idx_ofs + sectors < INDIRECT ?
                            idx_ofs + sectors : INDIRECT;
  for(int i = idx_ofs; i<indirect_sectors; i++)
  {
    if(free_map_allocate(1, disk_indirect->data + i))
    {
      cache_write_from_buf(disk_indirect->data[i], zeros);
      sectors--;
    }
    else
    {
      PANIC("Failed at single indirect_alloc");
    }
  }
  return sectors;
}

/* This function is called in bytes_to_sectors.
  If the file wants to write to the bytes beyond the length,
  this function calculates how many sectors should be allocated,
  and allocate them from idx_ofs.

  idx_ofs will be from current inode length

  After allocation, update inode length
  */
bool
sector_alloc(struct inode* inode, size_t sectors)
{
  size_t length_idx = bytes_to_sectors(inode->data.length);
  if(inode_alloc(&inode->data, sectors, length_idx))
  {
    return true;
  }
  else
  {
    PANIC("Failed at growing");
  }
  return false;
}

void
inode_acquire()
{
  lock_acquire(&inode_lock);
}

void
inode_release()
{
  lock_release(&inode_lock);
}

block_sector_t
inode_sec(struct inode* inode)
{
  return inode->sector;
}

bool
inode_dir(struct inode* inode)
{
  return inode->data.is_dir;
}

int
inode_dir_opened(struct inode* inode)
{
  return inode->data.is_opened;
}

int
inode_dir_cwd(struct inode* inode)
{
  return inode->data.is_cwd;
}

int inode_open_cnt(struct inode* inode)
{
  return inode->open_cnt;
}