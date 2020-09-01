#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H
#include "lib/user/syscall.h"
#include "lib/kernel/list.h"
#include "filesys/off_t.h"
#include "devices/block.h"

#define DIRECT 96
#define INDIRECT 128
#define NAME_MAX 14

struct dir 
  {
    struct inode *inode;                /* Backing store. */
    off_t pos;                          /* Current position. */
  };

struct dir_entry 
  {
    block_sector_t inode_sector;        /* Sector number of header. */
    char name[NAME_MAX + 1];            /* Null terminated file name. */
    bool in_use;                        /* In use or free? */
  };

struct file 
  {
    struct inode *inode;        /* File's inode. */
    off_t pos;                  /* Current position. */
    bool deny_write;            /* Has file_deny_write() been called? */
  };

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

struct inode 
  {
    struct list_elem elem;              /* Element in inode list. */
    block_sector_t sector;              /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    struct inode_disk data;             /* Inode content. */
  };

/* An open file. */
struct o_file
{
  int fd;
  struct file *file;
  struct dir *dir;
  struct list_elem elem;
};

void syscall_init (void);
void *valid_vaddress (void *addr);

void Halt (void) NO_RETURN;
void Exit (int) NO_RETURN;
pid_t Exec (const char *file);
int Wait (pid_t);
bool Create (const char *file, unsigned initial_size);
bool Remove (const char *file);
int Open (const char *file);
int Filesize (int fd);
int Read (int fd, char *buffer, unsigned length);
int Write (int fd, const char *buffer, unsigned length);
void Seek (int fd, unsigned position);
unsigned Tell (int fd);
void Close (int fd);

mapid_t Mmap (int, void *);
void Munmap(mapid_t);

struct list_elem* Find_file(int fd);

bool Chdir(const char* dir);
bool Mkdir(const char* dir);
bool Readdir(int fd, char* name);
bool Isdir(int fd);
int Inumber(int fd);

//bool dir_readdir(struct dir *reading_dir, char *name);
#endif /* userprog/syscall.h */
