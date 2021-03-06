#include <types.h>
#include <lib.h>
#include <synchprobs.h>
#include <synch.h>

/* 
 * Replace this default synchronization mechanism with your own (better) mechanism
 * needed for your solution.   Your mechanism may use any of the available synchronzation
 * primitives, e.g., semaphores, locks, condition variables.   You are also free to 
 * declare other global variables if your solution requires them.
 */

/*
 * replace this with declarations of any synchronization and other variables you need here
 */
static volatile int number_of_cats_eating;
static volatile int number_of_mice_eating;

static struct lock **bowl_locks;
static struct cv *cats_done_eating;
static struct cv *mice_done_eating;


/* 
 * The CatMouse simulation will call this function once before any cat or
 * mouse tries to each.
 *
 * You can use it to initialize synchronization and other variables.
 * 
 * parameters: the number of bowls
 */
void
catmouse_sync_init(int bowls)
{
  number_of_cats_eating = 0;
  number_of_mice_eating = 0;

  bowl_locks = kmalloc(sizeof(struct lock) * bowls);
  for(int i = 0; i < bowls; ++i) {
    bowl_locks[i] = lock_create("bowl " + i);
    if(bowl_locks[i] == NULL) {
      panic("could not create bowl " + i);
    }
  }

  cats_done_eating = cv_create("cats done eating");
  mice_done_eating = cv_create("mice done eatting");

  return;
}

/* 
 * The CatMouse simulation will call this function once after all cat
 * and mouse simulations are finished.
 *
 * You can use it to clean up any synchronization and other variables.
 *
 * parameters: the number of bowls
 */
void
catmouse_sync_cleanup(int bowls)
{
  KASSERT(bowl_locks != NULL);

  for(int i = 0; i < bowls; ++i) {
    KASSERT(bowl_locks[i] != NULL);
    lock_destroy(bowl_locks[i]);
  }
  kfree(bowl_locks);

  KASSERT(cats_done_eating != NULL);
  cv_destroy(cats_done_eating);

  KASSERT(mice_done_eating != NULL);
  cv_destroy(mice_done_eating);
}


/*
 * The CatMouse simulation will call this function each time a cat wants
 * to eat, before it eats.
 * This function should cause the calling thread (a cat simulation thread)
 * to block until it is OK for a cat to eat at the specified bowl.
 *
 * parameter: the number of the bowl at which the cat is trying to eat
 *             legal bowl numbers are 1..NumBowls
 *
 * return value: none
 */

void
cat_before_eating(unsigned int bowl) 
{
  lock_acquire(bowl_locks[bowl-1]);
  while(number_of_mice_eating > 0) {
    cv_wait(mice_done_eating, bowl_locks[bowl-1]);
  }

  number_of_cats_eating++;
}

/*
 * The CatMouse simulation will call this function each time a cat finishes
 * eating.
 *
 * You can use this function to wake up other creatures that may have been
 * waiting to eat until this cat finished.
 *
 * parameter: the number of the bowl at which the cat is finishing eating.
 *             legal bowl numbers are 1..NumBowls
 *
 * return value: none
 */

void
cat_after_eating(unsigned int bowl) 
{
  number_of_cats_eating--;
  if(number_of_cats_eating == 0) {
      cv_broadcast(cats_done_eating, bowl_locks[bowl - 1]);
  }
  lock_release(bowl_locks[bowl-1]);
}

/*
 * The CatMouse simulation will call this function each time a mouse wants
 * to eat, before it eats.
 * This function should cause the calling thread (a mouse simulation thread)
 * to block until it is OK for a mouse to eat at the specified bowl.
 *
 * parameter: the number of the bowl at which the mouse is trying to eat
 *             legal bowl numbers are 1..NumBowls
 *
 * return value: none
 */

void
mouse_before_eating(unsigned int bowl) 
{
  lock_acquire(bowl_locks[bowl-1]);
  while(number_of_cats_eating > 0) {
    cv_wait(cats_done_eating, bowl_locks[bowl-1]);
  }

  number_of_mice_eating++;
}

/*
 * The CatMouse simulation will call this function each time a mouse finishes
 * eating.
 *
 * You can use this function to wake up other creatures that may have been
 * waiting to eat until this mouse finished.
 *
 * parameter: the number of the bowl at which the mouse is finishing eating.
 *             legal bowl numbers are 1..NumBowls
 *
 * return value: none
 */

void
mouse_after_eating(unsigned int bowl) 
{
  number_of_mice_eating--;
  if(number_of_mice_eating == 0) {
      cv_broadcast(mice_done_eating, bowl_locks[bowl - 1]);
  }
  lock_release(bowl_locks[bowl-1]);
}
