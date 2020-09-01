#ifndef FILESYS_INODE_H
#define FILESYS_INODE_H

#include <stdbool.h>
#include "filesys/off_t.h"
#include "devices/block.h"

struct bitmap;

void inode_init (void);
bool inode_create (block_sector_t, off_t, bool);
struct inode *inode_open (block_sector_t);
struct inode *inode_reopen (struct inode *);
block_sector_t inode_get_inumber (const struct inode *);
void inode_close (struct inode *);
void inode_remove (struct inode *);
off_t inode_read_at (struct inode *, void *, off_t size, off_t offset);
off_t inode_write_at (struct inode *, const void *, off_t size, off_t offset);
void inode_deny_write (struct inode *);
void inode_allow_write (struct inode *);
off_t inode_length (const struct inode *);

block_sector_t byte_to_index(off_t);
block_sector_t index_to_sector(struct inode *, block_sector_t);

void inode_acquire();
void inode_release();

block_sector_t inode_sec(struct inode*);
bool inode_dir(struct inode*);
int inode_dir_opened(struct inode* inode);
int inode_dir_cwd(struct inode* inode);
int inode_open_cnt(struct inode* inode);
#endif /* filesys/inode.h */
