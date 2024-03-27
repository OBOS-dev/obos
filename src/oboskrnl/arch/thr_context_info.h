#pragma once

// This must define (at least) one structure: obos::arch::ThreadContextInfo.
// It must contain any information to restore the state of a thread after it gets preempted.
// It must also define a function called obos::arch::SwitchToThrContext() that takes in a pointer to that structure, and switches to that context. 
// This function must not return.
// The function is free to modify this structure.
// Another function it should define is obos::arch::SetupThreadContext().
// Look in the x86_64 implementation for parameters and return value.
// For more information on how to correctly implement header, look at the x86_64 implementation.

#if defined(__x86_64__)
#include <arch/x86_64/thr_context_info.h>
#endif