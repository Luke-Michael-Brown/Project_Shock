/*
 * Copyright (c) 2013
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Process support.
 *
 * There is (intentionally) not much here; you will need to add stuff
 * and maybe change around what's already present.
 *
 * p_lock is intended to be held when manipulating the pointers in the
 * proc structure, not while doing any significant work with the
 * things they point to. Rearrange this (and/or change it to be a
 * regular lock) as needed.
 *
 * Unless you're implementing multithreaded user processes, the only
 * process that will have more than one thread is the kernel process.
 */

#define PROCINLINE

#include <types.h>
#include <kern/wait.h>
#include <proc.h>
#include <current.h>
#include <addrspace.h>
#include <vnode.h>
#include <vfs.h>
#include <synch.h>
#include <kern/fcntl.h>

/*
 * The process for the kernel; this holds all the kernel-only threads.
 */
struct proc *kproc;

/*
 * Mechanism for making the kernel menu thread sleep while processes are running
 */
#ifdef UW
/* count of the number of processes, excluding kproc */
static unsigned int proc_count;
/* provides mutual exclusion for proc_count */
/* it would be better to use a lock here, but we use a semaphore because locks are not implemented in the base kernel */ 
static struct semaphore *proc_count_mutex;
/* used to signal the kernel menu thread when there are no processes */
struct semaphore *no_proc_sem;   
#endif  // UW
#if OPT_A2
static struct procarray procs;
static struct semaphore *procs_mutex;
#endif



/*
 * Create a proc structure.
 */
static
struct proc *
proc_create(const char *name)
{
	struct proc *proc;

	proc = kmalloc(sizeof(*proc));
	if (proc == NULL) {
		return NULL;
	}
	proc->p_name = kstrdup(name);
	if (proc->p_name == NULL) {
		kfree(proc);
		return NULL;
	}

	threadarray_init(&proc->p_threads);
#if OPT_A2
	char* lock_name = kmalloc(strlen(proc->p_name) + strlen("_lock"));
	if(lock_name == NULL) {
	    kfree(proc->p_name);
	    kfree(proc);
	    return NULL;
	}
	strcpy(lock_name, proc->p_name);
	strcat(lock_name, "_lock");
	proc->p_cvlock = lock_create(lock_name);
	kfree(lock_name);

	spinlock_init(&proc->p_lock);
	proc->p_exitcode = _MKWAIT_STOP(0);
#endif

	/* VM fields */
	proc->p_addrspace = NULL;

	/* VFS fields */
	proc->p_cwd = NULL;

#ifdef UW
	proc->console = NULL;
#endif // UW
#if OPT_A2
	proc->p_pid = 0;
	proc->p_ppid = NULL;

	pidarray_init(&proc->p_cpids);
	intarray_init(&proc->p_cpids_exitcodes);

	char* cv_name = kmalloc(strlen(proc->p_name) + strlen("_wait_channel"));
	if(cv_name == NULL) {
	    lock_destroy(proc->p_cvlock);
	    spinlock_cleanup(&proc->p_lock);
	    kfree(proc->p_name);
	    kfree(proc);
	    return NULL;
	}
	strcpy(cv_name, proc->p_name);
	strcat(cv_name, "wait_channel");
	proc->p_cv = cv_create(cv_name);
#endif // OPT_A2

	return proc;
}

/*
 * Destroy a proc structure.
 */
void
proc_destroy(struct proc *proc)
{
	/*
         * note: some parts of the process structure, such as the address space,
         *  are destroyed in sys_exit, before we get here
         *
         * note: depending on where this function is called from, curproc may not
         * be defined because the calling thread may have already detached itself
         * from the process.
	 */

	KASSERT(proc != NULL);
	KASSERT(proc != kproc);

#if OPT_A2
	spinlock_acquire(&proc->p_lock);
	pid_t* parent_pid = proc->p_ppid;
	spinlock_release(&proc->p_lock);

	if(parent_pid != NULL) {
	    P(procs_mutex);
	    struct proc *parent = procarray_get(&procs, *parent_pid);
	    V(procs_mutex);

	    spinlock_acquire(&proc->p_lock);
	    pid_t pid = proc->p_pid;
	    spinlock_release(&proc->p_lock);

	    unsigned int i = 0;
	    spinlock_acquire(&parent->p_lock);
	    for(i = 0; i < pidarray_num(&parent->p_cpids); ++i) {
		if(*pidarray_get(&parent->p_cpids, i) == pid) break;
	    }
	    spinlock_release(&parent->p_lock);
	
	    int *temp = kmalloc(sizeof(int));
	    KASSERT(temp != NULL); // If it's null we are in trouble

	    spinlock_acquire(&parent->p_lock);
	    *temp = proc->p_exitcode;
	    intarray_set(&parent->p_cpids_exitcodes, i, temp);
	    spinlock_release(&parent->p_lock);
	}

	spinlock_acquire(&proc->p_lock);
	int len = pidarray_num(&proc->p_cpids);
	spinlock_release(&proc->p_lock);

	for(int i = 0; i < len; ++i) {
	    spinlock_acquire(&proc->p_lock);
	    bool is_running = intarray_get(&proc->p_cpids_exitcodes, i) == NULL;
	    spinlock_release(&proc->p_lock);
	    if(!is_running) break;

	    spinlock_acquire(&proc->p_lock);
	    pid_t pid = *pidarray_get(&proc->p_cpids, i);
	    spinlock_release(&proc->p_lock);

	    P(procs_mutex);
	    struct proc* child = procarray_get(&procs, pid);
	    V(procs_mutex);

	    spinlock_acquire(&child->p_lock);
	    kfree(child->p_ppid);
	    child->p_ppid = NULL;
	    spinlock_release(&child->p_lock);
	}

	spinlock_acquire(&proc->p_lock);
	bool parent_is_dead = proc->p_ppid == NULL;
	spinlock_release(&proc->p_lock);

	if(parent_is_dead) {
	    spinlock_acquire(&proc->p_lock);
	    pid_t pid = proc->p_pid;
	    spinlock_release(&proc->p_lock);

	    P(procs_mutex);
	    procarray_set(&procs, pid, NULL);
	    V(procs_mutex);
	}

	spinlock_acquire(&proc->p_lock);
	unsigned int num = pidarray_num(&proc->p_cpids);
	spinlock_release(&proc->p_lock);
	for(unsigned int i = 0; i < num; ++i) {
	    spinlock_acquire(&proc->p_lock);
	    bool is_running = intarray_get(&proc->p_cpids_exitcodes, i) == NULL;
	    pid_t* pid = pidarray_get(&proc->p_cpids, i);
	    spinlock_release(&proc->p_lock);
	    if(!is_running) {
		P(procs_mutex);
		procarray_set(&procs, *pid, NULL);
		V(procs_mutex);
	    }
	}

	spinlock_acquire(&proc->p_lock);
	bool parent_is_alive = proc->p_ppid != NULL;
	spinlock_release(&proc->p_lock);

	if(parent_is_alive) {
	    P(procs_mutex);
	    struct proc *parent = procarray_get(&procs, *proc->p_ppid);
	    V(procs_mutex);

	    lock_acquire(parent->p_cvlock);
	    cv_signal(parent->p_cv, parent->p_cvlock);
	    lock_release(parent->p_cvlock);
	}
#endif // OPT_A2

	/*
	 * We don't take p_lock in here because we must have the only
	 * reference to this structure. (Otherwise it would be
	 * incorrect to destroy it.)
	 */

	/* VFS fields */
	if (proc->p_cwd) {
		VOP_DECREF(proc->p_cwd);
		proc->p_cwd = NULL;
	}


#ifndef UW  // in the UW version, space destruction occurs in sys_exit, not here
	if (proc->p_addrspace) {
		/*
		 * In case p is the currently running process (which
		 * it might be in some circumstances, or if this code
		 * gets moved into exit as suggested above), clear
		 * p_addrspace before calling as_destroy. Otherwise if
		 * as_destroy sleeps (which is quite possible) when we
		 * come back we'll be calling as_activate on a
		 * half-destroyed address space. This tends to be
		 * messily fatal.
		 */
		struct addrspace *as;

		as_deactivate();
		as = curproc_setas(NULL);
		as_destroy(as);
	}
#endif // UW

#ifdef UW
	if (proc->console) {
	  vfs_close(proc->console);
	}
#endif // UW

	threadarray_cleanup(&proc->p_threads);
	kfree(proc->p_name);
#if OPT_A2
	lock_destroy(proc->p_cvlock);

	spinlock_cleanup(&proc->p_lock);
	if(proc->p_ppid != NULL) kfree(proc->p_ppid);

	while(pidarray_num(&proc->p_cpids) > 0) {
	    kfree(pidarray_get(&proc->p_cpids, 0));
	    pidarray_remove(&proc->p_cpids, 0);
	}
	pidarray_cleanup(&proc->p_cpids);

	while(intarray_num(&proc->p_cpids_exitcodes) > 0) {
	    kfree(intarray_get(&proc->p_cpids_exitcodes, 0));
	    intarray_remove(&proc->p_cpids_exitcodes, 0);
	}
	intarray_cleanup(&proc->p_cpids_exitcodes);

	cv_destroy(proc->p_cv);
#endif // OPT_A2
	kfree(proc);

#ifdef UW
	/* decrement the process count */
        /* note: kproc is not included in the process count, but proc_destroy
	   is never called on kproc (see KASSERT above), so we're OK to decrement
	   the proc_count unconditionally here */
	P(proc_count_mutex); 
	KASSERT(proc_count > 0);
	proc_count--;
	/* signal the kernel menu thread if the process count has reached zero */
	if (proc_count == 0) {
	  V(no_proc_sem);
	}
	V(proc_count_mutex);
#endif // UW
	

}

/*
 * Create the process structure for the kernel.
 */
void
proc_bootstrap(void)
{
  kproc = proc_create("[kernel]");
  if (kproc == NULL) {
    panic("proc_create for kproc failed\n");
  }
#ifdef UW
  proc_count = 0;
  proc_count_mutex = sem_create("proc_count_mutex",1);
  if (proc_count_mutex == NULL) {
    panic("could not create proc_count_mutex semaphore\n");
  }
  no_proc_sem = sem_create("no_proc_sem",0);
  if (no_proc_sem == NULL) {
    panic("could not create no_proc_sem semaphore\n");
  }
#endif // UW 
#if OPT_A2
  procs_mutex = sem_create("procs_mutex", 1);
  procarray_init(&procs);
  procarray_add(&procs, kproc, NULL);
  procarray_add(&procs, kproc, NULL);
#endif //OPT_A2
}

/*
 * Create a fresh proc for use by runprogram.
 *
 * It will have no address space and will inherit the current
 * process's (that is, the kernel menu's) current directory.
 */
struct proc *
proc_create_runprogram(const char *name)
{
	struct proc *proc;
	char *console_path;

	proc = proc_create(name);
	if (proc == NULL) {
		return NULL;
	}

#ifdef UW
	/* open the console - this should always succeed */
	console_path = kstrdup("con:");
	if (console_path == NULL) {
	  panic("unable to copy console path name during process creation\n");
	}
	if (vfs_open(console_path,O_WRONLY,0,&(proc->console))) {
	  panic("unable to open the console during process creation\n");
	}
	kfree(console_path);
#endif // UW
	  
	/* VM fields */

	proc->p_addrspace = NULL;

	/* VFS fields */

#ifdef UW
	/* we do not need to acquire the p_lock here, the running thread should
           have the only reference to this process */
        /* also, acquiring the p_lock is problematic because VOP_INCREF may block */
	if (curproc->p_cwd != NULL) {
		VOP_INCREF(curproc->p_cwd);
		proc->p_cwd = curproc->p_cwd;
	}
#else // UW
	spinlock_acquire(&curproc->p_lock);
	if (curproc->p_cwd != NULL) {
		VOP_INCREF(curproc->p_cwd);
		proc->p_cwd = curproc->p_cwd;
	}
	spinlock_release(&curproc->p_lock);
#endif // UW

#if OPT_A2
	P(procs_mutex);
	int num = procarray_num(&procs);
	int i = 1;	
	for(i = 1; i < num; ++i) {
	    if(procarray_get(&procs, i) == NULL) break;
	}
	if(i == num) procarray_add(&procs, proc, NULL);
	else         procarray_set(&procs, i, proc);
	V(procs_mutex);

	pid_t *temp = kmalloc(sizeof(pid_t));
	if(temp == NULL) return NULL;
	spinlock_acquire(&curproc->p_lock);
	*temp = curproc->p_pid;
	spinlock_release(&curproc->p_lock);

	spinlock_acquire(&proc->p_lock);
	proc->p_pid = i;
	proc->p_ppid = temp;
	spinlock_release(&proc->p_lock);

	pid_t* child_pid = kmalloc(sizeof(pid_t));
	if(child_pid == NULL) {
	    kfree(temp);
	    return NULL;
	}
	spinlock_acquire(&proc->p_lock);
	*child_pid = proc->p_pid;
	spinlock_release(&proc->p_lock);

	spinlock_acquire(&curproc->p_lock);
	pidarray_add(&curproc->p_cpids, child_pid, NULL);
	intarray_add(&curproc->p_cpids_exitcodes, NULL, NULL);
	spinlock_release(&curproc->p_lock);
#endif // OPT_A2

#ifdef UW
	/* increment the count of processes */
        /* we are assuming that all procs, including those created by fork(),
           are created using a call to proc_create_runprogram  */
	P(proc_count_mutex); 
	proc_count++;
	V(proc_count_mutex);
#endif // UW

	return proc;
}

/*
 * Add a thread to a process. Either the thread or the process might
 * or might not be current.
 */
int
proc_addthread(struct proc *proc, struct thread *t)
{
	int result;

	KASSERT(t->t_proc == NULL);

	spinlock_acquire(&proc->p_lock);
	result = threadarray_add(&proc->p_threads, t, NULL);
	spinlock_release(&proc->p_lock);
	if (result) {
		return result;
	}
	t->t_proc = proc;
	return 0;
}

/*
 * Remove a thread from its process. Either the thread or the process
 * might or might not be current.
 */
void
proc_remthread(struct thread *t)
{
	struct proc *proc;
	unsigned i, num;

	proc = t->t_proc;
	KASSERT(proc != NULL);

	spinlock_acquire(&proc->p_lock);
	/* ugh: find the thread in the array */
	num = threadarray_num(&proc->p_threads);
	for (i=0; i<num; i++) {
		if (threadarray_get(&proc->p_threads, i) == t) {
			threadarray_remove(&proc->p_threads, i);
			spinlock_release(&proc->p_lock);
			t->t_proc = NULL;
			return;
		}
	}
	/* Did not find it. */
	spinlock_release(&proc->p_lock);
	panic("Thread (%p) has escaped from its process (%p)\n", t, proc);
}

/*
 * Fetch the address space of the current process. Caution: it isn't
 * refcounted. If you implement multithreaded processes, make sure to
 * set up a refcount scheme or some other method to make this safe.
 */
struct addrspace *
curproc_getas(void)
{
	struct addrspace *as;
#ifdef UW
        /* Until user processes are created, threads used in testing 
         * (i.e., kernel threads) have no process or address space.
         */
	if (curproc == NULL) {
		return NULL;
	}
#endif

	spinlock_acquire(&curproc->p_lock);
	as = curproc->p_addrspace;
	spinlock_release(&curproc->p_lock);
	return as;
}

/*
 * Change the address space of the current process, and return the old
 * one.
 */
struct addrspace *
curproc_setas(struct addrspace *newas)
{
	struct addrspace *oldas;
	struct proc *proc = curproc;

	spinlock_acquire(&proc->p_lock);
	oldas = proc->p_addrspace;
	proc->p_addrspace = newas;
	spinlock_release(&proc->p_lock);
	return oldas;
}

#if OPT_A2
/* 
 * Update exit code.
 */
void 
proc_update_exitcode(int exitcode)
{
    spinlock_acquire(&curproc->p_lock);
    curproc->p_exitcode = exitcode;
    spinlock_release(&curproc->p_lock);
}

/*
 *  See's if given pid is a valid process
 */
bool
is_valid_proc(pid_t pid)
{
    P(procs_mutex);
    bool is_valid = procarray_get(&procs, pid) != NULL;
    V(procs_mutex);

    return is_valid;
}

/*
 *  See's if given pid is a child of the current process
 */
bool
proc_is_child(pid_t pid)
{
    bool result = false;

    spinlock_acquire(&curproc->p_lock);
    for(unsigned int i = 0; i < pidarray_num(&curproc->p_cpids); ++i) {
	if(*pidarray_get(&curproc->p_cpids, i) == pid) {
	    result = true;
	    break;
	}
    }
    spinlock_release(&curproc->p_lock);

    return result;
}

/*
 * Sleep until your child with the given pid dies, and return exitcode
 */
int
proc_wait_for_child_to_die(pid_t pid)
{
    unsigned int i;
    lock_acquire(curproc->p_cvlock);
    for(i = 0; i < pidarray_num(&curproc->p_cpids); ++i) {
	if(*pidarray_get(&curproc->p_cpids, i) == pid) break;
    }

    while(intarray_get(&curproc->p_cpids_exitcodes, i) == NULL) {
	cv_wait(curproc->p_cv, curproc->p_cvlock);
    }

    int exitcode = *intarray_get(&curproc->p_cpids_exitcodes, i);
    lock_release(curproc->p_cvlock);

    return exitcode;
}
#endif //OPT_A2

