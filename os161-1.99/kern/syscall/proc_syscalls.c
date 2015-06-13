#include <types.h>
#include <kern/errno.h>
#include <kern/unistd.h>
#include <kern/wait.h>
#include <lib.h>
#include <syscall.h>
#include <current.h>
#include <proc.h>
#include <thread.h>
#include <wchan.h>
#include <addrspace.h>
#include <copyinout.h>
#include <mips/trapframe.h>
#include "opt-A2.h"

#if OPT_A2
pid_t sys_fork(struct trapframe* ctf, pid_t* retval) {
    char* proc_name = kmalloc(strlen(curproc->p_name) + strlen("_child"));
    if(proc_name == NULL) return ENOMEM;
    strcpy(proc_name, curproc->p_name);
    strcat(proc_name, "_child");
    struct proc* child = proc_create_runprogram(proc_name);
    if(child == NULL) return ENOMEM;

    struct trapframe* tf = kmalloc(sizeof(*ctf));
    if(tf == NULL) return ENOMEM;
    memcpy(tf, ctf, sizeof(*ctf));
    if(tf == NULL) return ENOMEM;

    struct addrspace* as;
    int result = as_copy(curproc_getas(), &as);
    if(result) return result;
    tf->tf_v0 = (uint32_t) as;

    char* thread_name = kmalloc(strlen(proc_name) + strlen("_thread"));
    if(thread_name == NULL) return ENOMEM;
    strcpy(thread_name, proc_name);
    strcat(thread_name, "_thread");
    thread_fork(thread_name, child, enter_forked_process, tf, 0);

    *retval = child->p_pid;
    return 0;
}
#endif //OPT_A2

  /* this implementation of sys__exit does not do anything with the exit code */
  /* this needs to be fixed to get exit() and waitpid() working properly */

void sys__exit(int exitcode) {
  struct addrspace *as;
  struct proc *p = curproc;

#if OPT_A2
  proc_update_exitcode(_MKWAIT_EXIT(exitcode));
#else
  (void)exitcode;
#endif // OPT_A2

  DEBUG(DB_SYSCALL,"Syscall: _exit(%d)\n",exitcode);

  KASSERT(curproc->p_addrspace != NULL);
  as_deactivate();
  /*
   * clear p_addrspace before calling as_destroy. Otherwise if
   * as_destroy sleeps (which is quite possible) when we
   * come back we'll be calling as_activate on a
   * half-destroyed address space. This tends to be
   * messily fatal.
   */
  as = curproc_setas(NULL);
  as_destroy(as);

  /* detach this thread from its process */
  /* note: curproc cannot be used after this call */
  proc_remthread(curthread);

  /* if this is the last user process in the system, proc_destroy()
     will wake up the kernel menu thread */
  proc_destroy(p);
  
  thread_exit();
  /* thread_exit() does not return, so we should never get here */
  panic("return from thread_exit in sys_exit\n");
}


/* stub handler for getpid() system call                */
int
sys_getpid(pid_t *retval)
{
#if OPT_A2
  *retval = curproc->p_pid;
#else
  *retval = 1;
#endif
  return(0);
}

/* stub handler for waitpid() system call                */

int
sys_waitpid(pid_t pid,
	    userptr_t status,
	    int options,
	    pid_t *retval)
{
#if OPT_A2
  if(options != 0)        return(EINVAL);
  if(!is_valid_proc(pid)) return(ESRCH);
  if(!proc_is_child(pid)) return(ECHILD);

  int exitstatus = proc_wait_for_child_to_die(pid);
  int result = copyout((void *) &exitstatus, status, sizeof(int));
  if (result) return(result);

  *retval = pid;
  return(0);
#else
  int result;
  int exitstatus;

  /* this is just a stub implementation that always reports an
     exit status of 0, regardless of the actual exit status of
     the specified process.   
     In fact, this will return 0 even if the specified process
     is still running, and even if it never existed in the first place.

     Fix this!
  */

  if (options != 0) {
    return(EINVAL);
  }

  result = copyout((void *) &exitstatus, status, sizeof(int));
  if (result) {
    return(result);
  }
  *retval = pid;
  return(0);
#endif
}

