/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
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
 * Process ID management.
 */

#include <types.h>
#include <kern/errno.h>
#include <kern/wait.h>
#include <limits.h>
#include <lib.h>
#include <array.h>
#include <clock.h>
#include <thread.h>
#include <current.h>
#include <synch.h>
#include <pid.h>
#include <signal.h>

/*
 * Structure for holding PID and return data for a thread.
 *
 * If pi_ppid is INVALID_PID, the parent has gone away and will not be
 * waiting. If pi_ppid is INVALID_PID and pi_exited is true, the
 * structure can be freed.
 */
struct pidinfo {
	pid_t pi_pid;			// process id of this thread
	pid_t pi_ppid;			// process id of parent thread
	volatile bool pi_exited;	// true if thread has exited
	int pi_exitstatus;		// status (only valid if exited)
	struct cv *pi_cv;		// use to wait for thread exit
	int join_count;			// number of threads to be joined
	bool is_detached;		// true if thread has been detached
	bool is_joined;			// true if thread has been joined
	int32_t pi_sig;		// signals sent to the thread
	struct cv *pi_cv2;		// use to stop and continue thread
};


/*
 * Global pid and exit data.
 *
 * The process table is an el-cheapo hash table. It's indexed by
 * (pid % PROCS_MAX), and only allows one process per slot. If a
 * new pid allocation would cause a hash collision, we just don't
 * use that pid.
 */
static struct lock *pidlock;		// lock for global exit data
static struct pidinfo *pidinfo[PROCS_MAX]; // actual pid info
static pid_t nextpid;			// next candidate pid
static int nprocs;			// number of allocated pids



/*
 * Create a pidinfo structure for the specified pid.
 */
static
struct pidinfo *
pidinfo_create(pid_t pid, pid_t ppid)
{
	struct pidinfo *pi;

	KASSERT(pid != INVALID_PID);

	pi = kmalloc(sizeof(struct pidinfo));
	if (pi==NULL) {
		return NULL;
	}

	pi->pi_cv = cv_create("pidinfo cv");
	if (pi->pi_cv == NULL) {
		kfree(pi);
		return NULL;
	}
	
	pi->pi_cv2 = cv_create("pidinfo cv2");
	if(pi->pi_cv2 == NULL){
		kfree(pi);
		return NULL;
	}

	pi->pi_pid = pid;
	pi->pi_ppid = ppid;
	pi->pi_exited = false;
	pi->pi_exitstatus = 0xbaad;  /* Recognizably invalid value */
	pi->join_count = 0;
	pi->is_detached = false;
	pi->is_joined = false;
	pi->pi_sig = 0;

	return pi;
}

/*
 * Clean up a pidinfo structure.
 */
static
void
pidinfo_destroy(struct pidinfo *pi)
{
	KASSERT(pi->pi_exited == true);
	KASSERT(pi->pi_ppid == INVALID_PID);
	cv_destroy(pi->pi_cv);
	kfree(pi);
}

////////////////////////////////////////////////////////////

/*
 * pid_bootstrap: initialize.
 */
void
pid_bootstrap(void)
{
	int i;

	pidlock = lock_create("pidlock");
	if (pidlock == NULL) {
		panic("Out of memory creating pid lock\n");
	}

	/* not really necessary - should start zeroed */
	for (i=0; i<PROCS_MAX; i++) {
		pidinfo[i] = NULL;
	}

	pidinfo[BOOTUP_PID] = pidinfo_create(BOOTUP_PID, INVALID_PID);
	if (pidinfo[BOOTUP_PID]==NULL) {
		panic("Out of memory creating bootup pid data\n");
	}

	nextpid = PID_MIN;
	nprocs = 1;
}

/*
 * pi_get: look up a pidinfo in the process table.
 */
static
struct pidinfo *
pi_get(pid_t pid)
{
	struct pidinfo *pi;

	KASSERT(pid>=0);
	KASSERT(pid != INVALID_PID);
	KASSERT(lock_do_i_hold(pidlock));

	pi = pidinfo[pid % PROCS_MAX];
	if (pi==NULL) {
		return NULL;
	}
	if (pi->pi_pid != pid) {
		return NULL;
	}
	return pi;
}

/*
 * pi_put: insert a new pidinfo in the process table. The right slot
 * must be empty.
 */
static
void
pi_put(pid_t pid, struct pidinfo *pi)
{
	KASSERT(lock_do_i_hold(pidlock));

	KASSERT(pid != INVALID_PID);

	KASSERT(pidinfo[pid % PROCS_MAX] == NULL);
	pidinfo[pid % PROCS_MAX] = pi;
	nprocs++;
}

/*
 * pi_drop: remove a pidinfo structure from the process table and free
 * it. It should reflect a process that has already exited and been
 * waited for.
 */
static
void
pi_drop(pid_t pid)
{
	struct pidinfo *pi;

	KASSERT(lock_do_i_hold(pidlock));

	pi = pidinfo[pid % PROCS_MAX];
	KASSERT(pi != NULL);
	KASSERT(pi->pi_pid == pid);

	pidinfo_destroy(pi);
	pidinfo[pid % PROCS_MAX] = NULL;
	nprocs--;
}

////////////////////////////////////////////////////////////

/*
 * Helper function for pid_alloc.
 */
static
void
inc_nextpid(void)
{
	KASSERT(lock_do_i_hold(pidlock));

	nextpid++;
	if (nextpid > PID_MAX) {
		nextpid = PID_MIN;
	}
}

/*
 * pid_alloc: allocate a process id.
 */
int
pid_alloc(pid_t *retval)
{
	struct pidinfo *pi;
	pid_t pid;
	int count;

	KASSERT(curthread->t_pid != INVALID_PID);

	/* lock the table */
	lock_acquire(pidlock);

	if (nprocs == PROCS_MAX) {
		lock_release(pidlock);
		return EAGAIN;
	}

	/*
	 * The above test guarantees that this loop terminates, unless
	 * our nprocs count is off. Even so, assert we aren't looping
	 * forever.
	 */
	count = 0;
	while (pidinfo[nextpid % PROCS_MAX] != NULL) {

		/* avoid various boundary cases by allowing extra loops */
		KASSERT(count < PROCS_MAX*2+5);
		count++;

		inc_nextpid();
	}

	pid = nextpid;

	pi = pidinfo_create(pid, curthread->t_pid);
	if (pi==NULL) {
		lock_release(pidlock);
		return ENOMEM;
	}

	pi_put(pid, pi);

	inc_nextpid();

	lock_release(pidlock);

	*retval = pid;
	return 0;
}

/*
 * pid_unalloc - unallocate a process id (allocated with pid_alloc) that
 * hasn't run yet.
 */
void
pid_unalloc(pid_t theirpid)
{
	struct pidinfo *them;

	KASSERT(theirpid >= PID_MIN && theirpid <= PID_MAX);

	lock_acquire(pidlock);

	them = pi_get(theirpid);
	KASSERT(them != NULL);
	KASSERT(them->pi_exited == false);
	KASSERT(them->pi_ppid == curthread->t_pid);

	/* keep pidinfo_destroy from complaining */
	them->pi_exitstatus = 0xdead;
	them->pi_exited = true;
	them->pi_ppid = INVALID_PID;
	pi_drop(theirpid);

	lock_release(pidlock);
}

/*
 * pid_detach - disavows interest in the child thread's exit status, so 
 * it can be freed as soon as it exits. May only be called by the
 * parent thread.
 */
int
pid_detach(pid_t childpid)
{
	struct pidinfo *child_pi;	

	lock_acquire(pidlock);
	
	/*Childpid has not yet exited or is not the first thread of the pid. It must be a child thread*/
	if(childpid == INVALID_PID || childpid == BOOTUP_PID){
		lock_release(pidlock);
		return EINVAL;
	}

	child_pi = pi_get(childpid);
	
	/* No thread could be found corresponding to that specified by childpid*/
	if(child_pi == NULL){
		lock_release(pidlock);
		return ESRCH;
	}
	
	/*The thread childpid is already in the detach state, or child has already
	 * been joined, or the caller is not the parent of childpid
	 */
	if(child_pi->is_joined || child_pi->is_detached ||\
			curthread->t_pid != child_pi->pi_ppid){
		lock_release(pidlock);
		return EINVAL;
	}

	child_pi->pi_ppid = INVALID_PID;
	child_pi->is_detached = true;
	
	//if thread has already exit, free the structure
	if(child_pi->pi_exited){
		pi_drop(child_pi->pi_pid);
	}	

	lock_release(pidlock);
	
	return 0;	
}

/*
 * pid_exit 
 *  - sets the exit status of this thread (i.e. curthread). 
 *  - disowns children. 
 *  - if dodetach is true, children are also detached. 
 *  - wakes any thread waiting for the curthread to exit. 
 *  - frees the PID and exit status if the curthread has been detached. 
 *  - must be called only if the thread has had a pid assigned.
 */
void
pid_exit(int status, bool dodetach)
{
	struct pidinfo *my_pi;
	
	lock_acquire(pidlock);
	//must be called only if the thread has a pid
	my_pi = pi_get(curthread->t_pid);
	KASSERT(my_pi != NULL);
	my_pi->pi_exitstatus = status;

	for(int i = 0; i < nprocs; i++){
		//disown childrenpid_ex
		if(pidinfo[i] && my_pi->pi_pid == pidinfo[i]->pi_ppid){
			if(dodetach){
				pid_detach(pidinfo[i]->pi_pid);
			} else {
			pidinfo[i]->pi_ppid = INVALID_PID;
			}
		}
	}

	my_pi->pi_exited = true;
	cv_broadcast(my_pi->pi_cv, pidlock);

        if(my_pi->is_detached){
                my_pi->pi_ppid = INVALID_PID;
                pi_drop(my_pi->pi_pid);
        }


	lock_release(pidlock);
}

/*
 * pid_join - returns the exit status of the thread associated with
 * targetpid (in the status argument) as soon as it is available. 
 * If the thread has not yet exited, curthread waits unless the flag 
 * WNOHANG is sent. 
 *
 */
int
pid_join(pid_t targetpid, int *status, int flags)
{
	struct pidinfo *my_pi;

	lock_acquire(pidlock);

        /*Check if the thread has exited or thread is not a child thread*/
        if(targetpid == INVALID_PID || targetpid == BOOTUP_PID){
                lock_release(pidlock);
                return -EINVAL;
        }

	my_pi = pi_get(targetpid);

	/*No thread could be found corresponding to that specified targetpid*/
	if(my_pi == NULL){
		lock_release(pidlock);
		return -ESRCH;
	}

	/*The targetpid argument refers to the calling thread*/
	if(my_pi->pi_pid == curthread->t_pid){
		lock_release(pidlock);
		return -EDEADLK;
	}

	/*The thread corresponding to targetpid has been detached*/
	if(my_pi->is_detached){
		lock_release(pidlock);
		return -EINVAL;
	}
	
	/*Return 0 if WNOHANG flag is passed*/
	if(flags == WNOHANG)
	{
		lock_release(pidlock);
		return 0;
	}
	
	/*Update the variable and suspend the execution of the calling thread*/
	if(!my_pi->pi_exited)
	{
		my_pi->join_count++;
		my_pi->is_joined = true;	
		cv_wait(my_pi->pi_cv, pidlock);
	}
	//target must have exited by now.
	
	my_pi->join_count--;
	if(status != NULL){
		*status = my_pi->pi_exitstatus;
	}
	if(my_pi->join_count == 0){
		my_pi->pi_ppid = INVALID_PID;
		pi_drop(my_pi->pi_pid);
	}

	lock_release(pidlock);

	return targetpid;

}


int
pid_ischild(pid_t targetpid)
{       lock_acquire(pidlock);
	struct pidinfo *my_pi;
	my_pi = pi_get(targetpid);

	if (my_pi->pi_ppid == curthread->t_pid){
         lock_release(pidlock);
		return 0;
	}
        lock_release(pidlock);
	return -1;
}


/* pid_setflag - checks for valid/implemented signals and
 * stores the signal flag sent by kill() for the target process */

int
pid_setflag(pid_t targetpid, int sig)
{
	struct pidinfo *info;

	lock_acquire(pidlock);

	info = pi_get(targetpid);
	//check if pid valid
	if(!info){
		lock_release(pidlock);
		return ESRCH;
	}
	//check if signal valid
	if(sig < 0 || sig > 31){
		lock_release(pidlock);
		return EINVAL;
	}
	//Set the mask for implemented signals, otherwise return EUNIMP
	switch(sig){
	case(1):
	case(2):
	case(9):
	case(15):
			sig = TERM_MASK;
			break;
	case(17):
			sig = STOP_MASK;
			break;
	case(19):
			sig = CONT_MASK;
			break;
	case(28):
	case(29):
			lock_release(pidlock);
			return 0;
	default:
		lock_release(pidlock);
		return EUNIMP;
	}

	//Set the flag for a signal
	info->pi_sig |= 1 << (sig - 1);

	//If SIG_CONT issued, signal the stopped thread to continue
	if(info->pi_sig & CONT_MASK){
		cv_signal(info->pi_cv2, pidlock);
		info->pi_sig &= 0xFFFFFFFF - CONT_MASK;
	}

	lock_release(pidlock);

	return 0;
}

/* pid_executeflags(pid_t targetpid) - for a given targetpid, checks its
 * enabled flags and performs the appropriate actions for those flags
 */

void
pid_executeflags(pid_t targetpid) {
	struct pidinfo *info;

	lock_acquire(pidlock);

	info = pi_get(targetpid);

	if(!info){
		lock_release(pidlock);
//		return ESRCH;
	}

	//If the flag is set to terminate, call thread_exit on the calling thread
	if(info->pi_sig & TERM_MASK){
		lock_release(pidlock);
		thread_exit(info->pi_sig & TERM_MASK);
//		return 0;
	}

	//If the flag is set to stop, call cv_wait to stop the thread.
		//make sure to unset the flag before calling wait.
	if(info->pi_sig & STOP_MASK){
		info->pi_sig &= 0xFFFFFFFF - STOP_MASK;
		cv_wait(info->pi_cv2, pidlock);
		lock_release(pidlock);
//		return 0;
	}

	lock_release(pidlock);
//	return 0;
}
