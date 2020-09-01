#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "lib/user/syscall.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "userprog/pagedir.h"
#include "userprog/exception.h"
#include "vm/page.h"

int readcount=0;


static void syscall_handler (struct intr_frame *);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

void valid_vaddr (void *addr)
{
  if(addr<(void *)0x08048000 || addr+3>=PHYS_BASE){
    Exit(-1);
  }
  
  if(pagedir_get_page(thread_current()->pagedir, addr) == NULL
    || pagedir_get_page(thread_current()->pagedir, addr+3) == NULL){
    Exit(-1);
  }
}

static void
syscall_handler (struct intr_frame *f) 
{
  thread_current()->esp = f->esp;

  valid_vaddr(f->esp);
  // esp should be at the top of the stack
  valid_vaddr(f->esp+4);

  switch(*(int *)f->esp){
    case SYS_HALT:{
      shutdown_power_off();
      break;
    }
    case SYS_EXIT:{
      Exit(*(int *)(f->esp+4));
      f->eax = *(int *)(f->esp+4);
      break;
    }
    case SYS_EXEC:{
      valid_vaddr(*(char* *)(f->esp+4));
      f->eax = Exec(*(char* *)(f->esp+4));
      break;
    }
    case SYS_WAIT:{
      valid_vaddr(f->esp+4);
      f->eax = Wait(*(int *)(f->esp+4));
      break;
    }
    case SYS_CREATE:{
      valid_vaddr(*(void* *)(f->esp+4));
      f->eax = Create(*(char* *)(f->esp+4), *(unsigned *)(f->esp+8));
      break;
    }
    case SYS_REMOVE:{
      valid_vaddr(*(void* *)(f->esp+4));
      f->eax = Remove(*(char* *)(f->esp+4));
      break;
    }
    case SYS_OPEN:{
      valid_vaddr(*(void* *)(f->esp+4));
      f->eax = Open(*(char* *)(f->esp+4));
      break;
    }
    case SYS_FILESIZE:{
      f->eax = Filesize(*(int *)(f->esp+4));
      break;
    }
    case SYS_READ:{
      /* Bad address is checked in page fault and will be exit */
      if(!is_user_vaddr(*(char* *)(f->esp+8))) Exit(-1);
      wait_mutex();
      readcount++;
      if(readcount == 1)
        wait_wrt();
      signal_mutex();
      f->eax = Read(*(int *)(f->esp+4), *(char* *)(f->esp+8),
      *(unsigned *)(f->esp+12));
      wait_mutex();
      readcount--;
      if(readcount == 0)
        signal_wrt();
      signal_mutex();
      break;
    }
    case SYS_WRITE:{
      valid_vaddr(*(void* *)(f->esp+8));
      wait_wrt();
      f->eax = Write(*(int *)(f->esp+4), *(char* *)(f->esp+8),
      *(unsigned *)(f->esp+12));
      signal_wrt();
      break;
    }
    case SYS_SEEK:{
      Seek(*(int *)(f->esp+4), *(unsigned *)(f->esp+8));
      break;
    }
    case SYS_TELL:{
      f->eax = Tell(*(int *)(f->esp+4));
      break;
    }
    case SYS_CLOSE:{
      Close(*(int* *)(f->esp+4));
      break;
    }
    case SYS_MMAP:{
      f->eax = Mmap(*(int *)(f->esp+4), *(void* *)(f->esp+8));
      break;
    }
    case SYS_MUNMAP:{
      Munmap(*(int *)(f->esp+4));
      break;
    }
    case SYS_CHDIR:
    {
      valid_vaddr(*(void* *)(f->esp+4));
      f->eax = Chdir(*(char* *)(f->esp+4));
      break;
    }
    case SYS_MKDIR:
    {
      valid_vaddr(*(void* *)(f->esp+4));
      f->eax = Mkdir(*(char* *)(f->esp+4));
      break;
    }
    case SYS_READDIR:
    {
      valid_vaddr(*(void* *)(f->esp+8));
      f->eax = Readdir(*(int *)(f->esp+4), *(char* *)(f->esp+8));
      break;
    }
    case SYS_ISDIR:
    {
      f->eax = Isdir(*(int *)(f->esp+4));
      break;
    }
    case SYS_INUMBER:
    {
      f->eax = Inumber(*(int *)(f->esp+4));
      break;
    }
  }
}

void Munmap(mapid_t mapping)
{
  frame_acquire();

  struct thread *cur = thread_current();
  struct list_elem *temp;
  bool existing = false;
  struct mmap_files *compare;
  for(temp = list_begin(&cur->mmaplist); temp != list_end(&cur->mmaplist); temp = list_next(temp))
  {
    compare = list_entry(temp, struct mmap_files, elem);
    if(mapping == compare->m_fid)
    {
      existing = true;
      break;
    }
  }

  if(!existing)
  {
    PANIC("No mmap file in list\n");
  }
  void *first_upage = compare->first_upage;
  void *final_upage = compare->final_upage;
  ASSERT(pg_ofs(first_upage) == 0 && pg_ofs(final_upage) == 0);

  void *mapped_upages = first_upage;
  for(mapped_upages ; mapped_upages <= final_upage; mapped_upages += PGSIZE)
  {
    struct spte *p = lookup_page_table(mapped_upages, cur);
    if(p == NULL)
    {
      frame_release();
      PANIC("Some of upage's spte doesn't exist in page table\n");
    }

    if(p->status == IN_FRAME)
    {
      pin_frame_by_upage(mapped_upages, cur);
    }
    switch(p->status)
    {
      case SWAPPED_OUT:
        if(pagedir_is_dirty (cur->pagedir, mapped_upages) || p->dirty)
        {
          swap_in(p, cur);
          file_write_back(p, cur);
        }
        frame_remove(p->kpage);
        break;
      case IN_FILESYS:
        p->status = IN_FRAME;
        page_remove(p->upage, cur);
        break;
      case ALL_ZERO:
        PANIC("Can All zero page be included in mmaped address region?\n");
        break;
      case IN_FRAME:
        if(pagedir_is_dirty (cur->pagedir, mapped_upages) || p->dirty)
        {
          file_write_back(p, cur);
        }
        frame_remove(p->kpage);
        pagedir_clear_page(cur->pagedir, mapped_upages);
        break;
    }
  }

  list_remove(temp);
  file_close(compare->file);
  free(compare);

  frame_release();
}

mapid_t
Mmap (int fd, void *addr)
{
  frame_acquire();

  if(pg_ofs(addr) != 0 || addr == 0 || fd == 0 || fd == 1)
  {
    frame_release();
    return MAP_FAILED; // Bad address
  }
  struct thread *cur = thread_current();
  struct list_elem *temp;
  bool existing = false;
  struct o_file *compare;
  for(temp = list_begin(&cur->filelist); temp != list_end(&cur->filelist); temp = list_next(temp))
  {
    compare = list_entry(temp, struct o_file, elem);
    if(compare->fd == fd)
    {
      existing = true;
      break;
    }
  }

  if(!existing)
  {
    frame_release();
    return MAP_FAILED; //There is no opened file in this process
  }

  struct file *file = file_reopen(compare->file);
  size_t whole_size = file_length(file);
  if(whole_size == 0)
  {
    frame_release();
    return MAP_FAILED;
  }
  uint32_t read_bytes = whole_size;
  uint32_t zero_bytes = (PGSIZE - whole_size % PGSIZE) % PGSIZE;
  off_t ofs = 0;
  void *final_upage = NULL;
  int pg_nums=0;
  
  for(pg_nums; pg_nums<whole_size; pg_nums+=PGSIZE)
  {
    struct spte *p = lookup_page_table(addr+pg_nums, cur);
    if(p != NULL)
    {
      frame_release();
      return MAP_FAILED; // This page is already been used
    }
  }

  for(pg_nums=0; pg_nums < whole_size; pg_nums+=PGSIZE)
  {
    struct spte *p = lookup_page_table(addr+pg_nums, cur);
    if(p == NULL)
    {
      uint32_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      uint32_t page_zero_bytes = PGSIZE - page_read_bytes;
      if(!file_map(cur, file, ofs, addr+pg_nums, page_read_bytes, page_zero_bytes, true))
        PANIC("file map in mmap is failed");
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      ofs += PGSIZE;
      final_upage = addr+pg_nums;
    }
  }

  if(read_bytes > 0 || zero_bytes > 0)
  {
    frame_release();
    PANIC("There are Unread bytes\n");
  }
  mapid_t m_fid;
  struct mmap_files *mfile;
  mfile = (struct mmap_files *)malloc(sizeof(struct mmap_files));
  if(list_empty(&cur->mmaplist))
  {
    m_fid=0;
  }
  else
  {
    struct mmap_files *last_mfile = list_entry(list_rbegin(&cur->mmaplist), struct mmap_files, elem);
    m_fid = last_mfile->m_fid+1;
  }
  mfile->m_fid = m_fid;
  mfile->file = file;
  mfile->first_upage = addr;
  mfile->final_upage = final_upage;
  list_push_back(&cur->mmaplist, &mfile->elem);
  
  frame_release();
  return m_fid;
}

void
Exit(int status)
{
  struct thread *t = thread_current();
  char *save_ptr;

  /* else exit right now */
  struct thread *par_thread=t->parent;
  struct list_elem *temp;

  /* Good bye my children */  
  while(!list_empty(&thread_current()->children))
  {
    temp = list_pop_front (&thread_current()->children);
    struct child *child = list_entry(temp, struct child, elem);
    //Wait((pid_t)child->tid);
    child->child_p->parent = NULL;
    free(child);
  }

  /* Good bye my parents */
  if(par_thread != NULL)
  {
    for(temp = list_begin(&par_thread -> children) ;
    temp != list_end(&par_thread -> children) ; temp = list_next(temp) )
    {
      struct child *child = list_entry(temp, struct child, elem);
      if(child->tid == t->tid)
      {
        //child->child_p = NULL;
        child->exit_status = status;  
      }
    }
  }

  /* Good bye my files */
  while(!list_empty(&thread_current()->filelist))
  {
    temp = list_front (&thread_current()->filelist);
    struct o_file *opening = list_entry(temp, struct o_file, elem);
    Close(opening->fd);
  }

  /* Good bye memory mapped files */
  while(!list_empty(&thread_current()->mmaplist))
  {
    temp = list_front (&thread_current()->mmaplist);
    struct mmap_files *mm_file = list_entry(temp, struct mmap_files, elem);
    Munmap(mm_file->m_fid);
  } 
  
  /* print the status if exit normally */
  printf("%s: exit(%d)\n", strtok_r(t->name, " ", &save_ptr), status);
  if(par_thread != NULL)
    sema_up(&par_thread->sema_wait);
  thread_exit();
}

pid_t
Exec(const char *file)
{
  return process_execute(file);
}

int
Wait(pid_t pid)
{  
  int result;
  result=process_wait((tid_t)pid);

  return result;
}

bool
Create (const char *file, unsigned initial_size)
{
  return filesys_create (file, initial_size, false);
}


bool
Remove (const char *file)
{
  return filesys_remove (file);
}

int
Open (const char *file)
{
  struct file *opening;
  opening = filesys_open(file);

  if(*thread_current()->name == *file)
    file_deny_write(opening);

  //opening file is failed
  if(opening == NULL){
    return -1;
  }

  //adding file to the current thread's file list with allocating fd
  int new_fd;
  if( list_empty(&thread_current()->filelist) )
    new_fd = 2;
  else
  {
    struct list_elem *fdmax_elem;
    fdmax_elem = list_rbegin(&thread_current()->filelist);
    new_fd = list_entry(fdmax_elem, struct o_file, elem)->fd+1;
  }

  //interrupting off maybe doesn't need
  struct o_file *adding;
  adding = (struct o_file *)malloc(sizeof(struct o_file));
  memset (adding, 0, sizeof *adding); //not sure needed
  adding->fd = new_fd;
  
  struct inode * inode = opening->inode;

  if(inode_dir(inode))
  {
    struct dir *opened_dir = dir_open(inode);
    inode->data.is_opened++;
    cache_write_from_buf(inode->sector, &inode->data);

    adding->dir = opened_dir;
    adding->file = NULL;
  }
  else
  {
    adding->dir = NULL;
    adding->file = opening;
  }
  list_push_back(&thread_current()->filelist, &adding->elem);


  return new_fd;
}

int
Filesize (int fd)
{
  struct list_elem *temp = Find_file(fd);
  
  if (temp==NULL)
    return -1;

  return file_length(list_entry(temp, struct o_file, elem)->file);
}

int
Read(int fd, char *buffer, unsigned size)
{ 
  int bytes_read = 0;
  if(fd == 0){
    for(int i=0 ; i<size ; i++){
      buffer[i] = input_getc();
      bytes_read++;
    }
  }
  else{
    struct list_elem* temp = Find_file(fd);

    if(temp==NULL)
      return -1;

    struct file* reading = list_entry(temp, struct o_file, elem)->file;
    frame_acquire();
    for(void *upage = pg_round_down(buffer); upage<buffer+size; upage += PGSIZE)
    {
      if(page_load(upage, thread_current())){
        pin_frame_by_upage(upage, thread_current());
      }
    }
    frame_release();
    bytes_read = file_read(reading, buffer, (off_t)size);
    frame_acquire();
    for(void *upage = pg_round_down(buffer); upage<buffer+size; upage += PGSIZE)
    {
      if(lookup_page_table(upage, thread_current()))
        unpin_frame_by_upage(upage, thread_current());
    }
    frame_release();
  }
  return bytes_read;
}

int
Write(int fd, const char *buffer, unsigned size)
{
  int bytes_write = 0;

  if(fd == 1){
    putbuf(buffer, size);
    return size;
  }
  else{
    struct list_elem* temp = Find_file(fd);

    if(temp==NULL)
      return -1;
    
    struct file* paper = list_entry(temp, struct o_file, elem)->file;
    if(paper->inode->data.is_dir)
      return -1;

    frame_acquire();
    for(void *upage = pg_round_down(buffer); upage<buffer+size; upage += PGSIZE)
    {
      if(page_load(upage, thread_current())){
        pin_frame_by_upage(upage, thread_current());
      }
    }
    frame_release();
    bytes_write = file_write(paper, buffer, (off_t)size);
    frame_acquire();
    for(void *upage = pg_round_down(buffer); upage<buffer+size; upage += PGSIZE)
    {
      if(lookup_page_table(upage, thread_current()))
        unpin_frame_by_upage(upage, thread_current());
    }
    frame_release();
  }
  return bytes_write;
}

void
Seek (int fd, unsigned position)
{
  struct list_elem *temp = Find_file(fd);
  file_seek(list_entry(temp, struct o_file, elem)->file, (off_t)position);
}

unsigned
Tell (int fd)
{
  struct list_elem *temp = Find_file(fd);
  return (unsigned)file_tell(list_entry(temp, struct o_file, elem)->file);
}

void
Close (int fd)
{
  struct list_elem* temp = Find_file(fd);

  if(temp!=NULL){
    if(Isdir(fd))
    {
      struct inode* inode = list_entry(temp, struct o_file, elem)->dir->inode;
      if(inode != NULL)
      {
        inode->data.is_opened--;
      }
      cache_write_from_buf(inode->sector, &inode->data);
      dir_close(list_entry(temp, struct o_file, elem)->dir);
      list_remove(temp);
    }
    else
    {
      file_close(list_entry(temp, struct o_file, elem)->file);
      list_remove(temp);
    }
    free(list_entry(temp, struct o_file, elem));
  }
  else {
    Exit(-1);
  }
}

struct list_elem*
Find_file(int fd)
{ 
  struct list* fl = &thread_current()->filelist;
  struct list_elem *temp;
  struct list_elem *result=NULL;

  for(temp=list_begin(fl); temp != list_end(fl); temp = list_next(temp))
  {
    if(fd == list_entry(temp, struct o_file, elem)->fd){
      result = temp;
      break;
    }
  }
  return result;
}

bool
Chdir(const char* dir)
{ 
  struct dir *destination = reach_path(dir);

  if(destination == NULL){
    return false;
  }

  struct inode* inode = thread_current()->cwd->inode;
  inode->data.is_cwd--;
  cache_write_from_buf(inode->sector, &inode->data);

  dir_close(thread_current()->cwd);
  thread_current()->cwd = destination;
  inode = thread_current()->cwd->inode;
  inode->data.is_cwd++;
  cache_write_from_buf(inode->sector, &inode->data);

  return true;
} 

bool
Mkdir(const char* dir)
{
  return filesys_create (dir, 0, true);
}

bool
Readdir(int fd, char* name)
{
  if(!Isdir(fd)){
    //printf("File in Readdir\n");
    return false;
  }
  
  struct list_elem* temp = Find_file(fd);

  if(temp == NULL){
    //printf("There is no o_file for this directory\n");
    return false;
  }

  struct dir *dir = list_entry(temp, struct o_file, elem)->dir;

  if(dir == NULL){
    //printf("Simple check\n");
    return false;
  }

  return dir_readdir(dir, name);
}

bool
Isdir(int fd)
{
  struct list_elem* temp = Find_file(fd);
  if(list_entry(temp, struct o_file, elem)->dir != NULL)
    return inode_dir(list_entry(temp, struct o_file, elem)->dir->inode);
  return false;
}

int
Inumber(int fd)
{
  struct list_elem* temp = Find_file(fd);
  int inum = list_entry(temp, struct o_file, elem)->dir->inode->sector;
  return inum;
}
/*
bool
dir_readdir(struct dir *reading_dir, char *name)
{
  struct dir_entry *dir_e = (struct dir_entry *)malloc(sizeof(struct dir_entry));
  struct inode* inode = reading_dir->inode;
  off_t pos = reading_dir->pos;

  while(inode_read_at(inode, dir_e, sizeof(struct dir_entry), pos) 
  == sizeof(struct dir_entry))
  {
    reading_dir->pos += sizeof(struct dir_entry);
    if(!dir_e->in_use && dir_e->name != "." && dir_e->name != "..")
    {
      memcpy(name,dir_e->name,sizeof(name));
      free(dir_e);
      return true;
    }
  }

  free(dir_e);
  return false;
}
*/