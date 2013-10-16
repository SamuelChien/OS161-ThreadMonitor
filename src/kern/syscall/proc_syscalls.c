/*
 * Process-related syscalls.
 * New for ASST2.
 */

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <thread.h>
#include <current.h>
#include <pid.h>
#include <machine/trapframe.h>
#include <syscall.h>
#include <kern/wait.h>
#include <copyinout.h>
/*
 * sys_fork
 * 
 * create a new process, which begins executing in md_forkentry().
 */


int
sys_fork(struct trapframe *tf, pid_t *retval)
{
	struct trapframe *ntf; /* new trapframe, copy of tf */
	int result;

	/*
	 * Copy the trapframe to the heap, because we might return to
	 * userlevel and make another syscall (changing the trapframe)
	 * before the child runs. The child will free the copy.
	 */

	ntf = kmalloc(sizeof(struct trapframe));
	if (ntf==NULL) {
		return ENOMEM;
	}
	*ntf = *tf; /* copy the trapframe */

	result = thread_fork(curthread->t_name, enter_forked_process, 
			     ntf, 0, retval);
	if (result) {
		kfree(ntf);
		return result;
	}

	return 0;
}

/*
 * sys_getpid
 *
 * returns the process id of the current process.
 */

int
sys_getpid(pid_t *retval)
{       
	*retval=curthread->t_pid;
	return 0;
}

/*
 * sys_waitpid
 *
 * Wait for the process specified by pid to exit,and return an
 * encoded exit status in the integer pointed to by status.
 * If that process has exited already, waitpid returns immediately.
 * If that process does not exist, waitpid fails.
 */

int
sys_waitpid(pid_t pid, userptr_t status, int options, pid_t *retval)
{	
	int result;
    int status_exit;
    int copyoutresult;

    if (pid_ischild(pid) == -1){
    	*retval = ECHILD;
        return -1;
	}

	result = pid_join(pid, &status_exit, options);
    status_exit=_MKWAIT_EXIT(status_exit);

    if(result>0){
    	copyoutresult=copyout(&status_exit, status, sizeof(status_exit));
    }
        
    if(copyoutresult == EFAULT ){
    	*retval= EFAULT;
        return -1;
    }

    if(result == -EINVAL ){
    	*retval= EINVAL;
        return -1;
    }

    if(result == -ESRCH ){
    	*retval= ESRCH;
        return -1;
    }

    *retval=result;
    return 0;
}

/*
 * sys_kill
 *
 * send signal to a process
 */

int
sys_kill(pid_t pid, int sig){
	return pid_setflag(pid, sig);
}
