#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "filesys/cache.h"
#include "threads/thread.h"

struct dir 
  {
    struct inode *inode;                /* Backing store. */
    off_t pos;                          /* Current position. */
  };

/* Partition that contains the file system. */
struct block *fs_device;

static void do_format (void);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) 
{
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");

  inode_init ();
  free_map_init ();

  if (format) 
    do_format ();

  free_map_open ();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void) 
{
  cache_flush();
  ra_list_flush();
  free_map_close ();
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create (const char *name, off_t initial_size, bool is_dir) 
{
  //create-long testcase
  if(strlen(name) > 128)
    return false;
  
  block_sector_t inode_sector = 0;
  /* if this is directory creation, parse the name and get
    the starting directory
  */
  char* target = (char *)malloc(sizeof(name));
  file_from_path(name, target);
  struct dir * dir = almost_reach_path(name);

  bool affordable = free_map_allocate(1, &inode_sector);

  if(!affordable) {
    free(target);
    dir_close (dir);
    return false;
  }
  
  bool success = (dir != NULL
                  && affordable
                  && inode_create (inode_sector, initial_size, is_dir)
                  && dir_add (dir, target, inode_sector));
  if (!success && inode_sector != 0) {
    free_map_release (inode_sector, 1);
  }
  else if(success && is_dir)
  {
    /* Directory creation is succeeded, update . and .. */
    struct inode* inode = NULL;
    struct dir* new_dir = NULL;
    if(dir_lookup(dir, target, &inode))
      new_dir = dir_open(inode);
    dir_add(new_dir, "..", inode_sec(dir->inode));
    dir_add(new_dir, ".", inode_sector);
    dir_close(new_dir);
  }
  free(target);
  dir_close (dir);

  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (const char *name)
{
  char* target = (char *)malloc(sizeof(name));
  file_from_path(name, target);
  struct dir* dir = almost_reach_path(name);

  struct inode *inode = NULL;

  if (dir != NULL){
    dir_lookup (dir, target, &inode);
  }
  dir_close (dir);
  free(target);

  return file_open (inode);
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *name) 
{
  if(name_is_root(name)) return false;

  char* target = (char *)malloc(sizeof(name));
  file_from_path(name, target);
  struct dir* dir = almost_reach_path(name);
  char entry[NAME_MAX + 1];

  /* Check whether it is directory or not */
  struct inode *inode = NULL;
  if (dir!= NULL){
    dir_lookup (dir, target, &inode);
  }

  bool removable = true;
  bool check = false;

  if(inode_dir(inode)){
    struct dir* new_dir = dir_open(inode);
    if(inode_dir_opened(inode)
      || inode_dir_cwd(inode)
      || (check = dir_readdir(new_dir, entry))) removable = false;
    dir_close(new_dir);
  }
  
  if(!removable){
    dir_close(dir);
    free(target);
    return false;
  }

  bool success = dir != NULL && dir_remove (dir, target);
  dir_close (dir); 
  free(target);

  return success;
}

/* Formats the file system. */
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, 18)) //Increased the number to incorporate . and ..
    PANIC ("root directory creation failed");
  free_map_close ();
  /* Add . and .. directory to the root directory */
  
  struct dir *dir = dir_open_root();
  if (!dir_add(dir, "..\0", ROOT_DIR_SECTOR))
    PANIC ("root directory .. creation failed");
  if (!dir_add(dir, ".\0", ROOT_DIR_SECTOR))
    PANIC ("root directory . creation failed");
  
  printf ("done.\n");
}

/* Obtain the last file or directory from path */
void
file_from_path(const char* name, char* dest)
{
  char chunk1[128];
  memcpy(chunk1, name, strlen(name));
  chunk1[strlen(name)] = '\0';

  if(name_is_root(name)){
    *dest = '.'; *(dest+1) = '\0';
    return;
  }

  char *token, *save_ptr;
  for(token = strtok_r(chunk1, "/", &save_ptr) ; token!=NULL ;
    token = strtok_r(NULL, "/", &save_ptr))
  {
    memcpy(dest, token, strlen(token));
    *(dest+strlen(token)) = '\0';
  }
}

/* This calculates the number of directoris right before the last one */
int
dirs_in_path(const char* name)
{
  char chunk1[128];
  memcpy(chunk1, name, strlen(name));
  chunk1[strlen(name)] = '\0';

  int cnt=0;
  char *token, *save_ptr;
  for(token = strtok_r(chunk1, "/", &save_ptr) ; token!=NULL ;
    token = strtok_r(NULL, "/", &save_ptr))
  {
    cnt++;
  }
  return cnt;
}

/* this return the struct dir* to the directory
  right before the last one. If directory doesn't exist,
  return NULL
  */
struct dir*
almost_reach_path(const char* name)
{
  if(strlen(name) == 0) return NULL;

  //char* target = (char *)malloc(sizeof(name));
  //file_from_path(name, target);

  int nums = dirs_in_path(name);

  struct dir* dir = NULL;
  char chunk[128];
  memcpy(chunk, name, strlen(name));
  chunk[strlen(name)] = '\0';
  if(name[0] == '/')
  { /* This is absolute path */
    dir = dir_open_root();
  }
  else
  {
    if(thread_current()->cwd == NULL)
    {
      dir = dir_open_root();
    }
    else
    {
      dir = dir_reopen(thread_current()->cwd);
    }
  }

  char *token, *save_ptr;
  int cnt = 0;
  for(token = strtok_r(chunk, "/", &save_ptr) ; token!=NULL ;
      token = strtok_r(NULL, "/", &save_ptr))
  {
    if(cnt+1 >= nums) break;
    struct inode* inode = NULL;
    if(dir_lookup(dir, token, &inode)){
      dir_close(dir);
      dir = dir_open(inode);
    }
    else break;
    cnt++;
  }
  //free(target);

  /* The number of upper directories must be less 1 than the whole path */
  //printf("target file : %s\n", target);
  if(token == NULL) return dir; //This is root directory
  if(cnt+1 != nums) dir = NULL;
  //printf("%p\n", dir);

  return dir;
}

struct dir*
reach_path(const char* name)
{
  if(strlen(name) == 0) return NULL;

  int nums = dirs_in_path(name);

  struct dir* dir = NULL;
  char chunk[128];
  memcpy(chunk, name, strlen(name));
  chunk[strlen(name)] = '\0';
  if(name[0] == '/')
  { /* This is absolute path */
    dir = dir_open_root();
  }
  else
  {
    if(thread_current()->cwd == NULL)
    {
      dir = dir_open_root();
    }
    else
    {
      dir = dir_reopen(thread_current()->cwd);
    }
  }

  char *token, *save_ptr;
  int cnt = 0;
  for(token = strtok_r(chunk, "/", &save_ptr) ; token!=NULL ;
      token = strtok_r(NULL, "/", &save_ptr))
  {
    struct inode* inode = NULL;
    if(dir_lookup(dir, token, &inode)){
      dir_close(dir);
      dir = dir_open(inode);
    }
    else {
      break;
    }
    cnt++;
  }

  /* The number of directories or files must be equal the whole path */
  if(cnt != nums) dir = NULL;
  return dir;
}

bool
name_is_root(const char *name)
{
  return (*name == '/' && *(name+1) == '\0') ;
}