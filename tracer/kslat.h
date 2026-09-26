/* Shared between the BPF program and the userspace loader. */
#ifndef KSLAT_H
#define KSLAT_H

enum ev_type {
    EV_OFFCPU     = 1,  /* dur = time switched out; arg0 = prev_state; arg1 = runqueue delay; arg2/comm = who ran instead */
    EV_HARDIRQ    = 2,  /* arg0 = irq number */
    EV_SOFTIRQ    = 3,  /* arg0 = softirq vector (NET_RX, TIMER, RCU, ...) */
    EV_VECTOR     = 4,  /* x86 system vector: arg0 = vector kind (enum vec_kind) */
    EV_FAULT      = 5,  /* point: arg1 = address, arg2 = error_code */
    EV_TLB        = 6,  /* point: arg0 = reason, arg1 = pages */
    EV_SYSCALL    = 7,  /* arg0 = syscall nr */
    EV_RECLAIM    = 8,  /* direct reclaim */
    EV_COMPACTION = 9,  /* direct compaction */
    EV_MIGRATE    = 10, /* point: arg0 = orig cpu, arg1 = dest cpu */
};

enum vec_kind {
    VK_LOCAL_TIMER = 1,
    VK_CALL_FUNC   = 2,
    VK_CALL_FUNC_SINGLE = 3,
    VK_RESCHEDULE  = 4,
    VK_IRQ_WORK    = 5,
    VK_OTHER       = 6,
};

struct event {
    unsigned long long ts;    /* CLOCK_MONOTONIC ns, start of event */
    unsigned long long dur;   /* ns, 0 for point events */
    unsigned int type;
    unsigned int tid;
    unsigned int cpu;
    unsigned int arg0;
    unsigned long long arg1;
    unsigned long long arg2;
    char comm[16];
};                            /* 64 bytes */

#endif
