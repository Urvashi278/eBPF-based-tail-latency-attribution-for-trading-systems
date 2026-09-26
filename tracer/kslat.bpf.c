// SPDX-License-Identifier: GPL-2.0
// kslat: capture every kernel event that can steal time from a target process.
// Tracepoints only (no kprobes/fentry) so it runs on locked-down production kernels.
// Entry/exit pairs are folded into one event with a duration inside BPF.
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>
#include "kslat.h"

char LICENSE[] SEC("license") = "GPL";

const volatile __u32 target_tgid = 0;
const volatile __u32 target_tid = 0;   /* optional: only this thread */

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 64 << 20);
} rb SEC(".maps");

/* per-CPU start slots for interrupt-context pairs (these don't nest per kind) */
struct slot { __u64 ts; __u32 arg; __u32 tid; };
enum { SL_HARDIRQ, SL_SOFTIRQ, SL_VECTOR, SL_MAX };
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, SL_MAX);
    __type(key, __u32);
    __type(value, struct slot);
} irq_start SEC(".maps");

/* per-thread start slots for task-context pairs */
struct task_slot { __u64 ts; __u64 arg; __u64 arg2; char comm[16]; };
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 4096);
    __type(key, __u32);
    __type(value, struct task_slot);
} offcpu SEC(".maps"), sys_start SEC(".maps"), reclaim_start SEC(".maps"),
  compact_start SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 4096);
    __type(key, __u32);
    __type(value, __u64);
} wake_ts SEC(".maps");

volatile __u64 dropped = 0;

static __always_inline int is_target(void)
{
    __u64 id = bpf_get_current_pid_tgid();
    if (target_tid) return (__u32)id == target_tid;
    return (id >> 32) == target_tgid;
}

static __always_inline __u32 cur_tid(void)
{
    return (__u32)bpf_get_current_pid_tgid();
}

static __always_inline void emit(__u32 type, __u32 tid, __u64 ts, __u64 dur,
                                 __u32 a0, __u64 a1, __u64 a2, const char *comm)
{
    struct event *e = bpf_ringbuf_reserve(&rb, sizeof(*e), 0);
    if (!e) { __sync_fetch_and_add(&dropped, 1); return; }
    e->ts = ts; e->dur = dur; e->type = type; e->tid = tid;
    e->cpu = bpf_get_smp_processor_id();
    e->arg0 = a0; e->arg1 = a1; e->arg2 = a2;
    if (comm) __builtin_memcpy(e->comm, comm, 16);
    else __builtin_memset(e->comm, 0, 16);
    /* NO_WAKEUP: a wakeup would raise irq_work on the traced CPU, i.e. the tracer
     * itself would inject latency into the thread it measures. Userspace drains
     * the ring on a timer instead. */
    bpf_ringbuf_submit(e, BPF_RB_NO_WAKEUP);
}

/* ---------------- scheduler: off-CPU time ---------------- */

SEC("tracepoint/sched/sched_switch")
int on_switch(struct trace_event_raw_sched_switch *ctx)
{
    __u64 now = bpf_ktime_get_ns();
    __u32 prev = ctx->prev_pid, next = ctx->next_pid;

    if (is_target()) {                      /* target is being switched out */
        struct task_slot s = {};
        s.ts = now;
        s.arg = ctx->prev_state;
        s.arg2 = ctx->next_pid;             /* who ran instead */
        bpf_probe_read_kernel(s.comm, 16, ctx->next_comm);
        bpf_map_update_elem(&offcpu, &prev, &s, BPF_ANY);
    }
    struct task_slot *s = bpf_map_lookup_elem(&offcpu, &next);
    if (s) {                                /* target is coming back */
        __u64 rq = 0, *w = bpf_map_lookup_elem(&wake_ts, &next);
        if (w && *w >= s->ts) rq = now - *w;
        emit(EV_OFFCPU, next, s->ts, now - s->ts, (__u32)s->arg, rq, s->arg2, s->comm);
        bpf_map_delete_elem(&offcpu, &next);
        bpf_map_delete_elem(&wake_ts, &next);
    }
    return 0;
}

SEC("tracepoint/sched/sched_waking")
int on_waking(struct trace_event_raw_sched_wakeup_template *ctx)
{
    __u32 pid = ctx->pid;
    if (bpf_map_lookup_elem(&offcpu, &pid)) {
        __u64 now = bpf_ktime_get_ns();
        bpf_map_update_elem(&wake_ts, &pid, &now, BPF_ANY);
    }
    return 0;
}

SEC("tracepoint/sched/sched_migrate_task")
int on_migrate(struct trace_event_raw_sched_migrate_task *ctx)
{
    __u32 pid = ctx->pid;
    /* only threads we've seen go off-CPU are tracked here */
    if (bpf_map_lookup_elem(&offcpu, &pid) || is_target())
        emit(EV_MIGRATE, pid, bpf_ktime_get_ns(), 0, ctx->orig_cpu, ctx->dest_cpu, 0, 0);
    return 0;
}

/* ---------------- interrupts (current == interrupted task) ---------------- */

static __always_inline int slot_begin(__u32 k, __u32 arg)
{
    if (!is_target()) return 0;
    struct slot *s = bpf_map_lookup_elem(&irq_start, &k);
    if (!s) return 0;
    s->ts = bpf_ktime_get_ns(); s->arg = arg; s->tid = cur_tid();
    return 0;
}

static __always_inline int slot_end(__u32 k, __u32 type)
{
    struct slot *s = bpf_map_lookup_elem(&irq_start, &k);
    if (!s || !s->ts) return 0;
    __u64 now = bpf_ktime_get_ns();
    emit(type, s->tid, s->ts, now - s->ts, s->arg, 0, 0, 0);
    s->ts = 0;
    return 0;
}

SEC("tracepoint/irq/irq_handler_entry")
int on_irq_entry(struct trace_event_raw_irq_handler_entry *ctx) { return slot_begin(SL_HARDIRQ, ctx->irq); }
SEC("tracepoint/irq/irq_handler_exit")
int on_irq_exit(void *ctx) { return slot_end(SL_HARDIRQ, EV_HARDIRQ); }

SEC("tracepoint/irq/softirq_entry")
int on_softirq_entry(struct trace_event_raw_softirq *ctx) { return slot_begin(SL_SOFTIRQ, ctx->vec); }
SEC("tracepoint/irq/softirq_exit")
int on_softirq_exit(void *ctx) { return slot_end(SL_SOFTIRQ, EV_SOFTIRQ); }

#define VEC_PAIR(name, kind)                                                   \
    SEC("tracepoint/irq_vectors/" #name "_entry")                              \
    int on_##name##_entry(void *ctx) { return slot_begin(SL_VECTOR, kind); }   \
    SEC("tracepoint/irq_vectors/" #name "_exit")                               \
    int on_##name##_exit(void *ctx) { return slot_end(SL_VECTOR, EV_VECTOR); }

VEC_PAIR(local_timer, VK_LOCAL_TIMER)
VEC_PAIR(call_function, VK_CALL_FUNC)
VEC_PAIR(call_function_single, VK_CALL_FUNC_SINGLE)
VEC_PAIR(reschedule, VK_RESCHEDULE)
VEC_PAIR(irq_work, VK_IRQ_WORK)

/* ---------------- memory ---------------- */

SEC("tracepoint/exceptions/page_fault_user")
int on_fault(struct trace_event_raw_exceptions *ctx)
{
    if (!is_target()) return 0;
    emit(EV_FAULT, cur_tid(), bpf_ktime_get_ns(), 0, 0, ctx->address, ctx->error_code, 0);
    return 0;
}

SEC("tracepoint/tlb/tlb_flush")
int on_tlb(struct trace_event_raw_tlb_flush *ctx)
{
    if (!is_target()) return 0;
    emit(EV_TLB, cur_tid(), bpf_ktime_get_ns(), 0, ctx->reason, ctx->pages, 0, 0);
    return 0;
}

static __always_inline int task_begin(void *map, __u64 arg)
{
    if (!is_target()) return 0;
    __u32 tid = cur_tid();
    struct task_slot s = {};
    s.ts = bpf_ktime_get_ns(); s.arg = arg;
    bpf_map_update_elem(map, &tid, &s, BPF_ANY);
    return 0;
}

static __always_inline int task_end(void *map, __u32 type)
{
    __u32 tid = cur_tid();
    struct task_slot *s = bpf_map_lookup_elem(map, &tid);
    if (!s) return 0;
    __u64 now = bpf_ktime_get_ns();
    emit(type, tid, s->ts, now - s->ts, (__u32)s->arg, 0, 0, 0);
    bpf_map_delete_elem(map, &tid);
    return 0;
}

SEC("tracepoint/vmscan/mm_vmscan_direct_reclaim_begin")
int on_reclaim_begin(void *ctx) { return task_begin(&reclaim_start, 0); }
SEC("tracepoint/vmscan/mm_vmscan_direct_reclaim_end")
int on_reclaim_end(void *ctx) { return task_end(&reclaim_start, EV_RECLAIM); }

SEC("tracepoint/compaction/mm_compaction_begin")
int on_compact_begin(void *ctx) { return task_begin(&compact_start, 0); }
SEC("tracepoint/compaction/mm_compaction_end")
int on_compact_end(void *ctx) { return task_end(&compact_start, EV_COMPACTION); }

/* ---------------- syscalls on the hot path ---------------- */

SEC("tracepoint/raw_syscalls/sys_enter")
int on_sys_enter(struct trace_event_raw_sys_enter *ctx) { return task_begin(&sys_start, ctx->id); }
SEC("tracepoint/raw_syscalls/sys_exit")
int on_sys_exit(void *ctx) { return task_end(&sys_start, EV_SYSCALL); }
