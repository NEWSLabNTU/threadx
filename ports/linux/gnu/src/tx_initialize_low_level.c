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
/**   Initialize                                                          */
/**                                                                       */
/**************************************************************************/
/**************************************************************************/


#define TX_SOURCE_CODE


/* Include necessary system files.  */

#include "tx_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/sysinfo.h>


/* Define various Linux objects used by the ThreadX port.  */

pthread_mutex_t     _tx_linux_mutex;
sem_t               _tx_linux_semaphore;
sem_t               _tx_linux_semaphore_no_idle;
ULONG               _tx_linux_global_int_disabled_flag;
struct timespec     _tx_linux_time_stamp;
__thread int        _tx_linux_threadx_thread = 0;
 
/* Define signals for linux thread. */
#define SUSPEND_SIG SIGUSR1
#define RESUME_SIG  SIGUSR2

static sigset_t     _tx_linux_thread_wait_mask;
static __thread int _tx_linux_thread_suspended; 
static sem_t        _tx_linux_thread_timer_wait;
static sem_t        _tx_linux_thread_other_wait;

/* Define simulated timer interrupt.  This is done inside a thread, which is
   how other interrupts may be defined as well.  See code below for an 
   example.  */

pthread_t           _tx_linux_timer_id;
sem_t               _tx_linux_timer_semaphore;
sem_t               _tx_linux_isr_semaphore;
void               *_tx_linux_timer_interrupt(void *p);

void    _tx_linux_thread_resume_handler(int sig);
void    _tx_linux_thread_suspend_handler(int sig);
void    _tx_linux_thread_suspend(pthread_t thread_id);

#ifdef TX_LINUX_DEBUG_ENABLE

extern ULONG        _tx_thread_system_state;
extern UINT         _tx_thread_preempt_disable;
extern TX_THREAD    *_tx_thread_current_ptr;
extern TX_THREAD    *_tx_thread_execute_ptr;


/* Define debug log in order to debug Linux issues with this port.  */

typedef struct TX_LINUX_DEBUG_ENTRY_STRUCT
{
    char                *tx_linux_debug_entry_action;
    struct timespec     tx_linux_debug_entry_timestamp;
    char                *tx_linux_debug_entry_file;
    unsigned long       tx_linux_debug_entry_line;
    pthread_mutex_t     tx_linux_debug_entry_mutex;
    unsigned long       tx_linux_debug_entry_int_disabled_flag;
    ULONG               tx_linux_debug_entry_system_state;
    UINT                tx_linux_debug_entry_preempt_disable;
    TX_THREAD           *tx_linux_debug_entry_current_thread;
    TX_THREAD           *tx_linux_debug_entry_execute_thread;
} TX_LINUX_DEBUG_ENTRY;


/* Define the maximum size of the Linux debug array.  */

#ifndef TX_LINUX_DEBUG_EVENT_SIZE
#define TX_LINUX_DEBUG_EVENT_SIZE       400
#endif


/* Define the circular array of Linux debug entries.  */

TX_LINUX_DEBUG_ENTRY    _tx_linux_debug_entry_array[TX_LINUX_DEBUG_EVENT_SIZE];


/* Define the Linux debug index.  */

unsigned long           _tx_linux_debug_entry_index =  0;


/* Now define the debug entry function.  */
void    _tx_linux_debug_entry_insert(char *action, char *file, unsigned long line)
{

pthread_mutex_t        temp_copy;

    /* Save the current critical section value.  */
    temp_copy =  _tx_linux_mutex;

    /* Lock mutex.  */
    tx_linux_mutex_lock(_tx_linux_mutex);

    /* Get the time stamp.  */
    clock_gettime(CLOCK_REALTIME, &_tx_linux_time_stamp);

    /* Setup the debub entry.  */
    _tx_linux_debug_entry_array[_tx_linux_debug_entry_index].tx_linux_debug_entry_action =             action;
    _tx_linux_debug_entry_array[_tx_linux_debug_entry_index].tx_linux_debug_entry_timestamp =          _tx_linux_time_stamp;
    _tx_linux_debug_entry_array[_tx_linux_debug_entry_index].tx_linux_debug_entry_file =               file;
    _tx_linux_debug_entry_array[_tx_linux_debug_entry_index].tx_linux_debug_entry_line =               line;
    _tx_linux_debug_entry_array[_tx_linux_debug_entry_index].tx_linux_debug_entry_mutex =              temp_copy;
    _tx_linux_debug_entry_array[_tx_linux_debug_entry_index].tx_linux_debug_entry_int_disabled_flag =  _tx_linux_global_int_disabled_flag;
    _tx_linux_debug_entry_array[_tx_linux_debug_entry_index].tx_linux_debug_entry_system_state =       _tx_thread_system_state;
    _tx_linux_debug_entry_array[_tx_linux_debug_entry_index].tx_linux_debug_entry_preempt_disable =    _tx_thread_preempt_disable;
    _tx_linux_debug_entry_array[_tx_linux_debug_entry_index].tx_linux_debug_entry_current_thread =     _tx_thread_current_ptr;
    _tx_linux_debug_entry_array[_tx_linux_debug_entry_index].tx_linux_debug_entry_execute_thread =     _tx_thread_execute_ptr;

    /* Now move to the next entry.  */
    _tx_linux_debug_entry_index++;
    
    /* Determine if we need to wrap the list.  */
    if (_tx_linux_debug_entry_index >= TX_LINUX_DEBUG_EVENT_SIZE)
    {
    
        /* Yes, wrap the list!  */
        _tx_linux_debug_entry_index =  0;
    }

    /* Unlock mutex.  */
    tx_linux_mutex_unlock(_tx_linux_mutex);
}

#endif


/* Define the ThreadX timer interrupt handler.  */

void    _tx_timer_interrupt(void);


/* Define other external function references.  */

VOID    _tx_initialize_low_level(VOID);
VOID    _tx_thread_context_save(VOID);
VOID    _tx_thread_context_restore(VOID);


/* Define other external variable references.  */

extern VOID     *_tx_initialize_unused_memory;


/**************************************************************************/ 
/*                                                                        */ 
/*  FUNCTION                                               RELEASE        */ 
/*                                                                        */ 
/*    _tx_initialize_low_level                            Linux/GNU       */ 
/*                                                           6.1          */
/*  AUTHOR                                                                */
/*                                                                        */
/*    William E. Lamie, Microsoft Corporation                             */
/*                                                                        */
/*  DESCRIPTION                                                           */
/*                                                                        */ 
/*    This function is responsible for any low-level processor            */ 
/*    initialization, including setting up interrupt vectors, setting     */ 
/*    up a periodic timer interrupt source, saving the system stack       */ 
/*    pointer for use in ISR processing later, and finding the first      */ 
/*    available RAM memory address for tx_application_define.             */ 
/*                                                                        */ 
/*  INPUT                                                                 */ 
/*                                                                        */ 
/*    None                                                                */ 
/*                                                                        */ 
/*  OUTPUT                                                                */ 
/*                                                                        */ 
/*    None                                                                */ 
/*                                                                        */ 
/*  CALLS                                                                 */ 
/*                                                                        */ 
/*    sched_setaffinity                                                   */ 
/*    getpid                                                              */ 
/*    _tx_linux_thread_init                                               */ 
/*    pthread_setschedparam                                               */ 
/*    pthread_mutexattr_init                                              */ 
/*    pthread_mutex_init                                                  */ 
/*    _tx_linux_thread_suspend                                            */ 
/*    sem_init                                                            */ 
/*    pthread_create                                                      */ 
/*    printf                                                              */ 
/*                                                                        */ 
/*  CALLED BY                                                             */ 
/*                                                                        */ 
/*    _tx_initialize_kernel_enter           ThreadX entry function        */ 
/*                                                                        */ 
/*  RELEASE HISTORY                                                       */ 
/*                                                                        */ 
/*    DATE              NAME                      DESCRIPTION             */
/*                                                                        */
/*  09-30-2020     William E. Lamie         Initial Version 6.1           */
/*                                                                        */
/**************************************************************************/
VOID   _tx_initialize_low_level(VOID)
{
struct sched_param sp;
pthread_mutexattr_t attr;

#ifdef TX_LINUX_MULTI_CORE
cpu_set_t mask;

    sched_getaffinity(getpid(), sizeof(mask), &mask);
    if (CPU_COUNT(&mask) > 1)
    {

        srand((ULONG)pthread_self());

        /* Limit this ThreadX simulation on Linux to a single core.  */
        CPU_ZERO(&mask);
        CPU_SET(rand() % get_nprocs(), &mask);
        if (sched_setaffinity(getpid(), sizeof(mask), &mask) != 0)
        {
        
            /* Error restricting the process to one core.  */
            printf("ThreadX Linux error restricting the process to one core!\n");
            while(1)
            {
            }
        }
    }
#endif

    /* Pickup the first available memory address.  */

    /* Save the first available memory address.  */
    _tx_initialize_unused_memory =  malloc(TX_LINUX_MEMORY_SIZE);

    /* Init Linux thread. */
    _tx_linux_thread_init();

    /* Set priority and schedual of main thread. */
    sp.sched_priority = TX_LINUX_PRIORITY_SCHEDULE;
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);

    /* Create the system critical section.  This is used by the 
       scheduler thread (which is the main thread) to block all
       other stuff out.  */
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&_tx_linux_mutex, &attr);
    sem_init(&_tx_linux_semaphore, 0, 0);
#ifdef TX_LINUX_NO_IDLE_ENABLE
    sem_init(&_tx_linux_semaphore_no_idle, 0, 0);
#endif /* TX_LINUX_NO_IDLE_ENABLE */

    /* Initialize the global interrupt disabled flag.  */
    _tx_linux_global_int_disabled_flag =  TX_FALSE;
    
    /* Create semaphore for timer thread. */
    sem_init(&_tx_linux_timer_semaphore, 0, 0);
    
    /* Create semaphore for ISR thread. */
    sem_init(&_tx_linux_isr_semaphore, 0, 0);

    /* Setup periodic timer interrupt.  */
    if(pthread_create(&_tx_linux_timer_id, NULL, _tx_linux_timer_interrupt, NULL))
    {

        /* Error creating the timer interrupt.  */
        printf("ThreadX Linux error creating timer interrupt thread!\n");
        while(1)
        {
        }
    }

    /* Otherwise, we have a good thread create.  Now set the priority to
       a level lower than the system thread but higher than the application
       threads.  */
    sp.sched_priority = TX_LINUX_PRIORITY_ISR;
    pthread_setschedparam(_tx_linux_timer_id, SCHED_FIFO, &sp);

    /* Done, return to caller.  */
}


/* This routine is called after initialization is complete in order to start
   all interrupt threads.  Interrupt threads in addition to the timer may 
   be added to this routine as well.  */

void    _tx_initialize_start_interrupts(void)
{

    /* Kick the timer thread off to generate the ThreadX periodic interrupt
       source.  */
    tx_linux_sem_post(&_tx_linux_timer_semaphore);
}


/* Define the ThreadX system timer interrupt.  Other interrupts may be simulated
   in a similar way.  */

void    *_tx_linux_timer_interrupt(void *p)
{
struct timespec start;
struct timespec now;
struct timespec next;
unsigned long long period_ns;
unsigned long long boundary_ns;
unsigned long long elapsed_ns;
unsigned long long ticks_due;
unsigned long long ticks_delivered;

    (VOID)p;

    /* The tick is a count of periods elapsed on the host's monotonic clock
       since the timer started, not one tick per wakeup.  Waking at "now plus
       one period" added each wakeup's latency to every period and never
       recovered it, so the tick ran slow on a loaded host.  The deadlines are
       absolute boundaries on CLOCK_MONOTONIC, so latency on one wakeup does
       not move the next, a wall-clock step cannot move either, and every tick
       owed since the last wakeup is delivered on this one.  The thread keeps
       the priority _tx_initialize_low_level gave it: lowering it with nice()
       only made a loaded host delay it more.  */
    period_ns = 1000000000ULL / TX_TIMER_TICKS_PER_SECOND;
    ticks_delivered = 0;

    /* Wait startup semaphore. */
    tx_linux_sem_wait(&_tx_linux_timer_semaphore);

    clock_gettime(CLOCK_MONOTONIC, &start);

    while(1)
    {

        /* Sleep to the next tick boundary.  A signal (the scheduler suspends
           and resumes this thread with one) ends the sleep early; sleep again
           to the same absolute deadline.  */
        boundary_ns = (ticks_delivered + 1) * period_ns;
        next.tv_sec = start.tv_sec + (time_t)(boundary_ns / 1000000000ULL);
        next.tv_nsec = start.tv_nsec + (long)(boundary_ns % 1000000000ULL);
        if (next.tv_nsec >= 1000000000)
        {
            next.tv_nsec -= 1000000000;
            next.tv_sec++;
        }
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR)
        {
        }

        clock_gettime(CLOCK_MONOTONIC, &now);
        elapsed_ns = (unsigned long long)(now.tv_sec - start.tv_sec) * 1000000000ULL
                   + (unsigned long long)now.tv_nsec - (unsigned long long)start.tv_nsec;
        ticks_due = elapsed_ns / period_ns;

        /* Deliver every tick owed, one timer interrupt each, so each tick's
           expirations and time-slice accounting run as they would on time.  */
        while (ticks_delivered < ticks_due)
        {
            ticks_delivered++;

            /* Call ThreadX context save for interrupt preparation.  */
            _tx_thread_context_save();

            /* Call trace ISR enter event insert.  */
            _tx_trace_isr_enter_insert(0);

            /* Call the ThreadX system timer interrupt processing.  */
            _tx_timer_interrupt();

            /* Call trace ISR exit event insert.  */
            _tx_trace_isr_exit_insert(0);

            /* Call ThreadX context restore for interrupt completion.  */
            _tx_thread_context_restore();
        }

#ifdef TX_LINUX_NO_IDLE_ENABLE
        tx_linux_mutex_lock(_tx_linux_mutex);

        /* Make sure semaphore is 0. */
        while(!sem_trywait(&_tx_linux_semaphore_no_idle));

        /* Wakeup the system thread by setting the system semaphore.  */
        tx_linux_sem_post(&_tx_linux_semaphore_no_idle);

        tx_linux_mutex_unlock(_tx_linux_mutex);
#endif /* TX_LINUX_NO_IDLE_ENABLE */
    } 
}

/* Define functions for linux thread. */
void    _tx_linux_thread_resume_handler(int sig)
{
    (VOID)sig;
}

void    _tx_linux_thread_suspend_handler(int sig)
{
    (VOID)sig;

    if(pthread_equal(pthread_self(), _tx_linux_timer_id))
        tx_linux_sem_post_nolock(&_tx_linux_thread_timer_wait);
    else
        tx_linux_sem_post_nolock(&_tx_linux_thread_other_wait);

    if(_tx_linux_thread_suspended) 
        return;

    _tx_linux_thread_suspended = 1;
    sigsuspend(&_tx_linux_thread_wait_mask); 
    _tx_linux_thread_suspended = 0;
}

void    _tx_linux_thread_suspend(pthread_t thread_id)
{

    /* Send signal. */
    tx_linux_mutex_lock(_tx_linux_mutex);
    pthread_kill(thread_id, SUSPEND_SIG);
    tx_linux_mutex_unlock(_tx_linux_mutex);

    /* Wait until signal is received. */
    if(pthread_equal(thread_id, _tx_linux_timer_id))
        tx_linux_sem_wait(&_tx_linux_thread_timer_wait);
    else
        tx_linux_sem_wait(&_tx_linux_thread_other_wait);
}

void    _tx_linux_thread_resume(pthread_t thread_id)
{

    /* Send signal. */
    tx_linux_mutex_lock(_tx_linux_mutex);
    pthread_kill(thread_id, RESUME_SIG);
    tx_linux_mutex_unlock(_tx_linux_mutex);
}

void    _tx_linux_thread_init()
{
struct sigaction sa;

    /* Create semaphore for linux thread. */
    sem_init(&_tx_linux_thread_timer_wait, 0, 0);
    sem_init(&_tx_linux_thread_other_wait, 0, 0);

    sigfillset(&_tx_linux_thread_wait_mask);
    sigdelset(&_tx_linux_thread_wait_mask, RESUME_SIG);

    sigfillset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_handler = _tx_linux_thread_resume_handler;
    sigaction(RESUME_SIG, &sa, NULL);

    sa.sa_handler = _tx_linux_thread_suspend_handler;
    sigaction(SUSPEND_SIG, &sa, NULL);
}


