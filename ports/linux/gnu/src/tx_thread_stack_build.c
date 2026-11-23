/***************************************************************************
 * Copyright (c) 2024 Microsoft Corporation 
 * 
 * This program and the accompanying materials are made available under the
 * terms of the MIT License which is available at
 * https://opensource.org/licenses/MIT.
 * 
 * SPDX-License-Identifier: MIT
 **************************************************************************/


/**************************************************************************/
/**************************************************************************/
/**                                                                       */ 
/** ThreadX Component                                                     */ 
/**                                                                       */
/**   Thread                                                              */
/**                                                                       */
/**************************************************************************/
/**************************************************************************/


#define TX_SOURCE_CODE


/* Include necessary system files.  */

#include "tx_api.h"
#include "tx_thread.h"
#include <stdio.h>
#include <unistd.h>
#include <limits.h>
#include <stdint.h>


/* Prototype for new thread entry function.  */

void *_tx_linux_thread_entry(void *ptr);


/**************************************************************************/ 
/*                                                                        */ 
/*  FUNCTION                                               RELEASE        */ 
/*                                                                        */ 
/*    _tx_thread_stack_build                              Linux/GNU       */ 
/*                                                           6.1          */
/*  AUTHOR                                                                */
/*                                                                        */
/*    William E. Lamie, Microsoft Corporation                             */
/*                                                                        */
/*  DESCRIPTION                                                           */
/*                                                                        */ 
/*    This function builds a stack frame on the supplied thread's stack.  */
/*    The stack frame results in a fake interrupt return to the supplied  */
/*    function pointer.                                                   */ 
/*                                                                        */ 
/*  INPUT                                                                 */ 
/*                                                                        */ 
/*    thread_ptr                            Pointer to thread control blk */
/*    function_ptr                          Pointer to return function    */
/*                                                                        */ 
/*  OUTPUT                                                                */ 
/*                                                                        */ 
/*    None                                                                */
/*                                                                        */ 
/*  CALLS                                                                 */ 
/*                                                                        */ 
/*    pthread_create                                                      */ 
/*    pthread_setschedparam                                               */ 
/*    _tx_linux_thread_suspend                                            */ 
/*    sem_init                                                            */ 
/*    printf                                                              */ 
/*    _tx_linux_thread_resume                                             */ 
/*                                                                        */ 
/*  CALLED BY                                                             */ 
/*                                                                        */ 
/*    _tx_thread_create                     Create thread service         */
/*    _tx_thread_reset                      Reset thread service          */ 
/*                                                                        */ 
/*  RELEASE HISTORY                                                       */ 
/*                                                                        */ 
/*    DATE              NAME                      DESCRIPTION             */
/*                                                                        */
/*  09-30-2020     William E. Lamie         Initial Version 6.1           */
/*                                                                        */
/**************************************************************************/
VOID   _tx_thread_stack_build(TX_THREAD *thread_ptr, VOID (*function_ptr)(VOID))
{
struct sched_param sp;

    (VOID)function_ptr;

    /* Create the run semaphore for the thread.  This will allow the scheduler
       control over when the thread actually runs.  */
    if(sem_init(&thread_ptr -> tx_thread_linux_thread_run_semaphore, 0, 0))
    {

        /* Display an error message.  */
        printf("ThreadX Linux error creating thread running semaphore!\n");
        while(1)
        {
        }
    }

    /* Run the Linux thread ON the ThreadX thread's own stack, so a host build
       has the stack its target has: a path that overflows the stated stack on
       a board overflows it here too.  With NULL attributes the thread ran on
       the host's default pthread stack (RLIMIT_STACK, usually 8 MiB) and the
       stack handed to tx_thread_create was never used.

       glibc keeps the thread's descriptor at the top of a supplied stack, and
       this port writes its fake stack pointer at the top of the ThreadX stack
       (below), so the host thread gets the stack minus a small top reserve,
       both ends aligned to 16 bytes.

       A stack the host cannot run on (below PTHREAD_STACK_MIN, which differs
       by architecture) is RAISED, not refused: a stated stack size is a
       floor the port raises to its own minimum, because no portable caller
       can name a size that is legal on every host (16 KiB on x86_64 glibc,
       128 KiB on aarch64).  The thread then runs on a host-allocated stack of
       PTHREAD_STACK_MIN bytes, and a warning names the request.  Refusing
       here hung every image whose kernel timer thread kept ThreadX's small
       default stack.  */
    {
    pthread_attr_t  attr;
    uintptr_t       low  = ((uintptr_t) thread_ptr -> tx_thread_stack_start + 15u) & ~(uintptr_t) 15u;
    uintptr_t       high = ((uintptr_t) thread_ptr -> tx_thread_stack_end - 64u) & ~(uintptr_t) 15u;
    size_t          size = (high > low) ? (size_t) (high - low) : 0u;

        if (pthread_attr_init(&attr))
        {

            /* Display an error message.  */
            printf("ThreadX Linux error initialising the thread's attributes!\n");
            while(1)
            {
            }
        }
        if (size < (size_t) PTHREAD_STACK_MIN)
        {

            /* Raise to the host's floor, on a host-allocated stack.  */
            printf("ThreadX Linux warning: thread '%s' stack of %lu bytes is below PTHREAD_STACK_MIN (%lu); "
                   "it runs on a host stack of %lu bytes instead\n",
                   thread_ptr -> tx_thread_name ? thread_ptr -> tx_thread_name : "?",
                   (unsigned long) thread_ptr -> tx_thread_stack_size, (unsigned long) PTHREAD_STACK_MIN,
                   (unsigned long) PTHREAD_STACK_MIN);
            if (pthread_attr_setstacksize(&attr, (size_t) PTHREAD_STACK_MIN))
            {
                printf("ThreadX Linux error setting the thread's stack size!\n");
                while(1)
                {
                }
            }
        }
        else if (pthread_attr_setstack(&attr, (void *) low, size))
        {

            /* Display an error message.  */
            printf("ThreadX Linux error setting the thread's stack!\n");
            while(1)
            {
            }
        }

    /* Create a Linux thread for the application thread.  */
    if(pthread_create(&thread_ptr -> tx_thread_linux_thread_id, &attr, _tx_linux_thread_entry, thread_ptr))
    {

        /* Display an error message.  */
        printf("ThreadX Linux error creating thread!\n");
        while(1)
        {
        }
    }

        pthread_attr_destroy(&attr);
    }

    /* Otherwise, we have a good thread create.  */
    sp.sched_priority = TX_LINUX_PRIORITY_USER_THREAD;
    pthread_setschedparam(thread_ptr -> tx_thread_linux_thread_id, SCHED_FIFO, &sp);

    /* Setup the thread suspension type to solicited thread suspension.  
       Pseudo interrupt handlers will suspend with this field set to 1.  */
    thread_ptr -> tx_thread_linux_suspension_type =  0;

    /* Clear the disabled count that will keep track of the 
       tx_interrupt_control nesting.  */
    thread_ptr -> tx_thread_linux_int_disabled_flag =  0;

    /* Setup a fake thread stack pointer.   */
    thread_ptr -> tx_thread_stack_ptr =  (VOID *) (((CHAR *) thread_ptr -> tx_thread_stack_end) - 8);

    /* Clear the first word of the stack.  */
    *(((ULONG *) thread_ptr -> tx_thread_stack_ptr) - 1) =  0;
}


void *_tx_linux_thread_entry(void *ptr)
{

TX_THREAD  *thread_ptr;

    /* Pickup the current thread pointer.  */
    thread_ptr =  (TX_THREAD *) ptr;
    _tx_linux_threadx_thread = 1;
    nice(20);

    /* Now suspend the thread initially.  If the thread has already
       been scheduled, this will return immediately.  */
    tx_linux_sem_wait(&thread_ptr -> tx_thread_linux_thread_run_semaphore);
    tx_linux_sem_post_nolock(&_tx_linux_semaphore);

    /* Call ThreadX thread entry point.  */
    _tx_thread_shell_entry();

    return EXIT_SUCCESS;
}

