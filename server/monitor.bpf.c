// SPDX-License-Identifier: GPL-2.0
// monitor.bpf.c — eBPF Layer-4 Monitor for Mini CI Grading Server

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>

#define EVENT_SYSCALL_BLOCKED   1
#define EVENT_PROCESS_ENTER     2
#define EVENT_PROCESS_EXIT      3

struct event {
    __u64  timestamp_ns;
    __u32  pid;
    __u32  tgid;
    __u32  event_type;
    __u32  syscall_nr;
    char   comm[16];
};

// [보안 수정 1] 키를 (pid, cgroup_id 하위 32비트) 복합 구조체로 변경하여
// PID 재사용(PID Recycling) 공격을 방지한다.
// 동일 PID라도 cgroup이 다르면 다른 엔트리로 처리됨.
struct pid_key {
    __u32 pid;
    __u32 cgroup_tag; // cgroup_id의 하위 32비트를 보조 식별자로 사용
};

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1024);
    __type(key,   struct pid_key);
    __type(value, __u32);         // 부모 pid 저장 (트리 추적용)
} tracked_pids SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 512);
    __type(key,   __u64);
    __type(value, __u8);
} blocked_syscalls SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 256 * 1024);
} events SEC(".maps");

static __always_inline void emit_event(__u32 event_type, __u32 pid, __u32 tgid, __u64 syscall_nr)
{
    struct event *e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
    if (!e) return;

    e->timestamp_ns = bpf_ktime_get_ns();
    e->pid          = pid;
    e->tgid         = tgid;
    e->event_type   = event_type;
    e->syscall_nr   = (__u32)syscall_nr;
    bpf_get_current_comm(e->comm, sizeof(e->comm));

    bpf_ringbuf_submit(e, 0);
}

// [보안 수정 1] cgroup_id를 포함한 복합 키로 현재 프로세스가 추적 대상인지 확인
static __always_inline __u32 *lookup_tracked(void)
{
    __u64 cg_id  = bpf_get_current_cgroup_id();
    __u64 pidtgid = bpf_get_current_pid_tgid();

    struct pid_key k = {
        .pid        = (__u32)(pidtgid & 0xFFFFFFFF),
        .cgroup_tag = (__u32)(cg_id   & 0xFFFFFFFF),
    };
    return bpf_map_lookup_elem(&tracked_pids, &k);
}

SEC("raw_tracepoint/sys_enter")
int handle_sys_enter(struct bpf_raw_tracepoint_args *ctx)
{
    __u64 syscall_nr = ctx->args[1];

    // [보안 수정 1] 복합 키로 추적 여부 확인
    if (!lookup_tracked()) return 0;

    __u8 *blocked = bpf_map_lookup_elem(&blocked_syscalls, &syscall_nr);
    if (!blocked) return 0;

    __u64 pidtgid = bpf_get_current_pid_tgid();
    __u32 pid  = (__u32)(pidtgid & 0xFFFFFFFF);
    __u32 tgid = (__u32)(pidtgid >> 32);

    emit_event(EVENT_SYSCALL_BLOCKED, pid, tgid, syscall_nr);
    bpf_send_signal(9);

    return 0;
}

SEC("raw_tracepoint/sched_process_fork")
int handle_fork(struct bpf_raw_tracepoint_args *ctx)
{
    struct task_struct *parent_task = (struct task_struct *)ctx->args[0];
    struct task_struct *child_task  = (struct task_struct *)ctx->args[1];

    // [보안 수정 2] pid(스레드ID)가 아닌 tgid(프로세스ID)를 사용해야 올바른 프로세스를 추적함.
    // 멀티스레드 프로세스에서 pid와 tgid가 달라질 수 있음.
    __u32 parent_tgid = BPF_CORE_READ(parent_task, tgid);
    __u32 child_pid   = BPF_CORE_READ(child_task,  pid);
    __u32 child_tgid  = BPF_CORE_READ(child_task,  tgid);

    // [보안 수정 1] 부모의 cgroup_id를 child에게도 적용 (상속 추적)
    // 주의: fork 시점 훅에서는 현재 태스크가 부모이므로 bpf_get_current_cgroup_id()가 부모 cgroup을 반환함
    __u64 cg_id = bpf_get_current_cgroup_id();

    struct pid_key parent_key = {
        .pid        = parent_tgid,
        .cgroup_tag = (__u32)(cg_id & 0xFFFFFFFF),
    };

    __u32 *tracked = bpf_map_lookup_elem(&tracked_pids, &parent_key);
    if (!tracked) return 0;

    // 자식 프로세스도 동일 cgroup 내에서 추적 등록
    struct pid_key child_key = {
        .pid        = child_pid,
        .cgroup_tag = (__u32)(cg_id & 0xFFFFFFFF),
    };
    bpf_map_update_elem(&tracked_pids, &child_key, &parent_tgid, BPF_ANY);

    emit_event(EVENT_PROCESS_ENTER, child_pid, child_tgid, 0);

    return 0;
}

SEC("raw_tracepoint/sched_process_exit")
int handle_exit(struct bpf_raw_tracepoint_args *ctx)
{
    __u64 cg_id   = bpf_get_current_cgroup_id();
    __u64 pidtgid = bpf_get_current_pid_tgid();
    __u32 pid     = (__u32)(pidtgid & 0xFFFFFFFF);
    __u32 tgid    = (__u32)(pidtgid >> 32);

    struct pid_key k = {
        .pid        = pid,
        .cgroup_tag = (__u32)(cg_id & 0xFFFFFFFF),
    };

    __u32 *tracked = bpf_map_lookup_elem(&tracked_pids, &k);
    if (!tracked) return 0;

    emit_event(EVENT_PROCESS_EXIT, pid, tgid, 0);

    // [보안 수정 3] 종료 시 맵에서 즉시 삭제하여 PID 재사용 윈도우를 최소화
    bpf_map_delete_elem(&tracked_pids, &k);

    return 0;
}

char LICENSE[] SEC("license") = "GPL";
