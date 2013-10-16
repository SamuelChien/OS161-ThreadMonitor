We have implemented everything without bugs except for the kill system call.
Specifically, the pid_setflags function in pid.c is not implemented correctly as
currently the switch statement is manually setting the signals to one of 3 flags,
and discarding the value of the actual signal.

We are currently working to correct that, but the fix might not be ready before 
midnight. 

Also, we are using bit masks to compare the value of the pid->pi_sig and one of 3
behaviors, which are TERM_MASK, STOP_MASK, and CONT_MASK for terminating, stopping,
and continuing, respectively. 

However, the current values for the bit masks are correct as they were implemented
when our understanding of bit masks were lacking...

So, the signals are handled properly, but with the wrong signal value gets printed
in the test outputs.

Finally, the killtest fails during the execution of the circular stop and continue 
test, presumably because of our implementation of the wait system call. The process
hangs when trying to retrieve the exit status of one of the children, upon the wait
call is made. 
