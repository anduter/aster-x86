#ifndef PROC_AUDIT_SMP_H
#define PROC_AUDIT_SMP_H

/* SMP concurrency self-tests (audit-smp session).
 *
 * Registered as kernel-thread programs and spawned from sched_start() before
 * the first process is selected, so their affinity/priority are fixed without
 * racing a running thread.  Each test is an independent check and prints an
 * explicit PASS/FAIL line to the serial console:
 *
 *   audit: smp spawn race PASS (rounds=N)
 *   audit: smp wake-vs-block PASS (latency=N ticks)
 *   audit: mm vmalloc concurrent PASS (iters=N)
 *   audit: smp selftest done
 */
void audit_smp_selftest_start(void);

#endif
