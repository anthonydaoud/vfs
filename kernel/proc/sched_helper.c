#include "globals.h"
#include "errno.h"

#include "main/interrupt.h"

#include "proc/sched.h"
#include "proc/kthread.h"

#include "util/init.h"
#include "util/debug.h"

void ktqueue_enqueue(ktqueue_t *q, kthread_t *thr);
kthread_t * ktqueue_dequeue(ktqueue_t *q);

/*
 * Updates the thread's state and enqueues it on the given
 * queue. Returns when the thread has been woken up with wakeup_on or
 * broadcast_on.
 *
 * Use the private queue manipulation functions above.
 */
void
sched_sleep_on(ktqueue_t *q)
{
        /* PROCS {{{ */
        curthr->kt_state = KT_SLEEP;
        ktqueue_enqueue(q, curthr);
        sched_switch();
        /* PROCS }}} */
}

kthread_t *
sched_wakeup_on(ktqueue_t *q)
{
        /* PROCS {{{ */
        kthread_t *ret;

        if (sched_queue_empty(q))
                return NULL;

        ret = ktqueue_dequeue(q);
        KASSERT((ret->kt_state == KT_SLEEP)
                || (ret->kt_state == KT_SLEEP_CANCELLABLE));
        sched_make_runnable(ret);
        return ret;
        /* PROCS }}} */
        return NULL;
}

void
sched_broadcast_on(ktqueue_t *q)
{
        /* PROCS {{{ */
        while (sched_wakeup_on(q));
        /* PROCS }}} */
}

