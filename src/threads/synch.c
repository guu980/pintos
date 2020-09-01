/* This file is derived from source code for the Nachos
   instructional operating system.  The Nachos copyright notice
   is reproduced in full below. */

/* Copyright (c) 1992-1996 The Regents of the University of California.
   All rights reserved.

   Permission to use, copy, modify, and distribute this software
   and its documentation for any purpose, without fee, and
   without written agreement is hereby granted, provided that the
   above copyright notice and the following two paragraphs appear
   in all copies of this software.

   IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO
   ANY PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR
   CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OF THIS SOFTWARE
   AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA
   HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS"
   BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATION TO
   PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
   MODIFICATIONS.
*/

#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

/* Initializes semaphore SEMA to VALUE.  A semaphore is a
   nonnegative integer along with two atomic operators for
   manipulating it:

   - down or "P": wait for the value to become positive, then
     decrement it.

   - up or "V": increment the value (and wake up one waiting
     thread, if any). */
void
sema_init (struct semaphore *sema, unsigned value) 
{
  ASSERT (sema != NULL);

  sema->value = value;
  list_init (&sema->waiters);
}

/* Down or "P" operation on a semaphore.  Waits for SEMA's value
   to become positive and then atomically decrements it.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but if it sleeps then the next scheduled
   thread will probably turn interrupts back on. */
void
sema_down (struct semaphore *sema) 
{
  enum intr_level old_level;

  ASSERT (sema != NULL);
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  while (sema->value == 0) 
    {
      pri_sort(&sema->waiters, &thread_current ()->elem);
      thread_block ();
    }
  sema->value--;
  intr_set_level (old_level);
}

/* Down or "P" operation on a semaphore, but only if the
   semaphore is not already 0.  Returns true if the semaphore is
   decremented, false otherwise.

   This function may be called from an interrupt handler. */
bool
sema_try_down (struct semaphore *sema) 
{
  enum intr_level old_level;
  bool success;

  ASSERT (sema != NULL);

  old_level = intr_disable ();
  if (sema->value > 0) 
    {
      sema->value--;
      success = true; 
    }
  else
    success = false;
  intr_set_level (old_level);

  return success;
}

/* Up or "V" operation on a semaphore.  Increments SEMA's value
   and wakes up one thread of those waiting for SEMA, if any.

   This function may be called from an interrupt handler. */
void
sema_up (struct semaphore *sema) 
{
  enum intr_level old_level;
  struct thread *next = NULL;

  ASSERT (sema != NULL);

  old_level = intr_disable ();
  if (!list_empty (&sema->waiters))
  {
    struct list_elem *e = list_max(&sema->waiters, &increasing, 0);
    next = list_entry(e, struct thread, elem);
    list_remove(e);
    thread_unblock(next);
  }

  sema->value++;
  check_pri();

  intr_set_level (old_level);
}

bool
increasing (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED)
{
  struct thread *first = list_entry(a, struct thread, elem);
  struct thread *second = list_entry(b, struct thread, elem);
  return first->priority < second->priority;
}

static void sema_test_helper (void *sema_);

/* Self-test for semaphores that makes control "ping-pong"
   between a pair of threads.  Insert calls to printf() to see
   what's going on. */
void
sema_self_test (void) 
{
  struct semaphore sema[2];
  int i;

  printf ("Testing semaphores...");
  sema_init (&sema[0], 0);
  sema_init (&sema[1], 0);
  thread_create ("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
  for (i = 0; i < 10; i++) 
    {
      sema_up (&sema[0]);
      sema_down (&sema[1]);
    }
  printf ("done.\n");
}

/* Thread function used by sema_self_test(). */
static void
sema_test_helper (void *sema_) 
{
  struct semaphore *sema = sema_;
  int i;

  for (i = 0; i < 10; i++) 
    {
      sema_down (&sema[0]);
      sema_up (&sema[1]);
    }
}

/* Initializes LOCK.  A lock can be held by at most a single
   thread at any given time.  Our locks are not "recursive", that
   is, it is an error for the thread currently holding a lock to
   try to acquire that lock.

   A lock is a specialization of a semaphore with an initial
   value of 1.  The difference between a lock and such a
   semaphore is twofold.  First, a semaphore can have a value
   greater than 1, but a lock can only be owned by a single
   thread at a time.  Second, a semaphore does not have an owner,
   meaning that one thread can "down" the semaphore and then
   another one "up" it, but with a lock the same thread must both
   acquire and release it.  When these restrictions prove
   onerous, it's a good sign that a semaphore should be used,
   instead of a lock. */
void
lock_init (struct lock *lock)
{
  ASSERT (lock != NULL);

  memset (lock, 0, sizeof *lock);  

  lock->holder = NULL;
  sema_init (&lock->semaphore, 1);
  lock->status = 0;

  //initialize lock->i_pri
  lock->i_pri = NULL;
}

void
lock_init_frame (struct lock *lock)
{
  ASSERT (lock != NULL);

  memset (lock, 0, sizeof *lock);  

  lock->holder = NULL;
  sema_init (&lock->semaphore, 1);
  lock->status = 1;

  //initialize lock->i_pri
  lock->i_pri = NULL;
}


/* Acquires LOCK, sleeping until it becomes available if
   necessary.  The lock must not already be held by the current
   thread.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void
lock_acquire (struct lock *lock)
{
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (!lock_held_by_current_thread (lock));

  //get brick
  thread_current()->brick = lock;

  //Priority donation//
  pri_donate(lock);

  sema_down (&lock->semaphore);

  //Put acquired brick to wall
  thread_current()->brick = NULL;
  list_push_back(&thread_current() -> wall, &lock -> elem);
  lock->holder = thread_current ();

  // set lock->i_pri //
  lock->i_pri = thread_current()->priority;
}

/* Tries to acquires LOCK and returns true if successful or false
   on failure.  The lock must not already be held by the current
   thread.

   This function will not sleep, so it may be called within an
   interrupt handler. */
bool
lock_try_acquire (struct lock *lock)
{
  bool success;

  ASSERT (lock != NULL);
  ASSERT (!lock_held_by_current_thread (lock));

  success = sema_try_down (&lock->semaphore);
  if (success)
    lock->holder = thread_current ();
  return success;
}

/* Releases LOCK, which must be owned by the current thread.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to release a lock within an interrupt
   handler. */
void
lock_release (struct lock *lock) 
{
  ASSERT (lock != NULL);
  ASSERT (lock_held_by_current_thread (lock));

  lock->holder = NULL;

  // 2 lines below in same interrupt handler???
  bool stay = list_empty(&(&lock->semaphore)->waiters);
  sema_up (&lock->semaphore);

  //check please for chain
  if(!list_empty(&(lock->semaphore).waiters))
  {
    thread_current()->bj_helper = true;
  }

  list_remove(&lock -> elem);
  if (!stay)
    pri_recover(lock);
}

void
pri_recover (struct lock *lock)
{
  enum intr_level old_level;
  
  old_level = intr_disable ();
  // if current thread's wall is empty, then changd curent thread's priority to original priority (o_pri of current thread)
  // if not recover to i_pri

  thread_current()->in_recover = true;

  if(list_empty(&thread_current()->wall)) //Wall is empty so bj is false
    thread_set_priority(thread_current() -> o_pri); 
    //priority is getting lower or equal
    //changing priority should be conducted
  else
    // Wall is not empty so bj can be false or true
    thread_set_priority(lock->i_pri);
    //priority is getting lower or equal
    //changing priority should be conducted

  thread_current()->in_recover = false;
  
  intr_set_level (old_level);

  /*if it is not priority recovering than
  if priority is getting higher
  then changing shuold be cnducted
  else if priority is getting lower &&
  if bj is false changing priority should be conducted
  if bj is true changing o_pri to new_priority should be conducted
  */

 /* so all case should be conducted same in thread_set_priority
    except when in_recover is false && priorigy is getting lower && bj is true
    */
}

bool
bj(struct thread *current_thread)
{
  struct list *walls = &current_thread -> wall;  
  struct list_elem *each_e;
  for(each_e = list_begin(walls); each_e != list_end(walls); each_e=list_next(each_e))
  {
    if ( !list_empty( &(list_entry(each_e, struct lock, elem)->semaphore).waiters ) )
      return true;
  }
  return false;
}

/* Returns true if the current thread holds LOCK, false
   otherwise.  (Note that testing whether some other thread holds
   a lock would be racy.) */
bool
lock_held_by_current_thread (const struct lock *lock) 
{
  ASSERT (lock != NULL);

  return lock->holder == thread_current ();
}

void
pri_donate(struct lock *lock)
{
  enum intr_level old_level;
  
  old_level = intr_disable ();
  if (lock->holder != NULL &&
      lock->holder->priority < thread_current()->priority)
  {
    // thread_current()->r_pri = lock->holder->priority;
    lock->holder->priority = thread_current()->priority;
    // lock->holder->bj = true;

    /* If donation is conducted, change this lock's holder's wall's locks i_pri to current thread's priority except current lock */
    struct list_elem *each_e;
    struct list *wall_list = &lock->holder->wall;
    for(each_e = list_begin(wall_list); each_e != list_end(wall_list);
        each_e = list_next(each_e))
    {
      if(each_e != &lock->elem)
        list_entry(each_e, struct lock, elem) -> i_pri =
        thread_current() -> priority;
    }

    if (lock->holder->brick != NULL)
      pri_donate(lock->holder->brick);
  }
  intr_set_level (old_level);
}


/* One semaphore in a list. */
struct semaphore_elem 
  {
    struct list_elem elem;              /* List element. */
    struct semaphore semaphore;         /* This semaphore. */
    int priority;                       /* priority of semaphore waiter */
  };

/* Initializes condition variable COND.  A condition variable
   allows one piece of code to signal a condition and cooperating
   code to receive the signal and act upon it. */
void
cond_init (struct condition *cond)
{
  ASSERT (cond != NULL);

  list_init (&cond->waiters);
}

/* Atomically releases LOCK and waits for COND to be signaled by
   some other piece of code.  After COND is signaled, LOCK is
   reacquired before returning.  LOCK must be held before calling
   this function.

   The monitor implemented by this function is "Mesa" style, not
   "Hoare" style, that is, sending and receiving a signal are not
   an atomic operation.  Thus, typically the caller must recheck
   the condition after the wait completes and, if necessary, wait
   again.

   A given condition variable is associated with only a single
   lock, but one lock may be associated with any number of
   condition variables.  That is, there is a one-to-many mapping
   from locks to condition variables.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void
cond_wait (struct condition *cond, struct lock *lock) 
{
  struct semaphore_elem waiter;

  ASSERT (cond != NULL);
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (lock_held_by_current_thread (lock));
  
  sema_init (&waiter.semaphore, 0);
  waiter.priority = thread_current()->priority;
  list_push_back (&cond->waiters, &waiter.elem);
  lock_release (lock);
  sema_down (&waiter.semaphore);
  lock_acquire (lock);
}

/* If any threads are waiting on COND (protected by LOCK), then
   this function signals one of them to wake up from its wait.
   LOCK must be held before calling this function.
l,
   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_signal (struct condition *cond, struct lock *lock UNUSED) 
{
  ASSERT (cond != NULL);
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (lock_held_by_current_thread (lock));

  struct list_elem *e;

  if (!list_empty (&cond->waiters)) {
    e = list_max(&cond->waiters, &sort_cond, 0);
    list_remove(e);
    sema_up (&list_entry(e, struct semaphore_elem, elem)->semaphore);
  }
}

bool
sort_cond (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED)
{
  struct semaphore_elem *first =
    list_entry(a, struct semaphore_elem, elem);
  struct semaphore_elem *second =
    list_entry(b, struct semaphore_elem, elem);
  return first->priority < second->priority;
}

/* Wakes up all threads, if any, waiting on COND (protected by
   LOCK).  LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_broadcast (struct condition *cond, struct lock *lock) 
{
  ASSERT (cond != NULL);
  ASSERT (lock != NULL);

  while (!list_empty (&cond->waiters))
    cond_signal (cond, lock);
}
