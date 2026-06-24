#![no_std]
#![no_main]

use aya_ebpf::{
    helpers::gen::{bpf_get_stack, bpf_get_stackid, bpf_ktime_get_ns},
    macros::{map, uprobe, uretprobe},
    maps::{HashMap, PerCpuArray, RingBuf, StackTrace},
    programs::{ProbeContext, RetProbeContext},
    EbpfContext,
};
use fsparse_a_common::{
    AllocInfo, CallocRetData, EventData, EventType, FreeData, MallocRetData, MemEvent,
    PendingInfo, ReallocRetData, StackPcsValue, MAX_EVENTS, MAX_STACK_DEPTH,
};

#[map]
static EVENTS: RingBuf = RingBuf::with_byte_size(1 << 24, 0);

#[map]
static STACK_TRACES: StackTrace = StackTrace::with_max_entries(MAX_EVENTS, 0);

#[map]
static ACTIVE_ALLOCS: HashMap<u64, AllocInfo> = HashMap::with_max_entries(MAX_EVENTS, 0);

#[map]
static PENDING_MALLOCS: HashMap<u32, PendingInfo> = HashMap::with_max_entries(4096, 0);

#[map]
static PENDING_CALLOCS: HashMap<u32, PendingInfo> = HashMap::with_max_entries(4096, 0);

#[map]
static PENDING_REALLOCS: HashMap<u32, PendingInfo> = HashMap::with_max_entries(4096, 0);

#[map]
static PENDING_STACKS_MALLOC: HashMap<u32, StackPcsValue> = HashMap::with_max_entries(4096, 0);

#[map]
static PENDING_STACKS_CALLOC: HashMap<u32, StackPcsValue> = HashMap::with_max_entries(4096, 0);

#[map]
static PENDING_STACKS_REALLOC: HashMap<u32, StackPcsValue> = HashMap::with_max_entries(4096, 0);

/// Per-CPU scratch space for stack capture (避免在 BPF 栈上分配 256 字节数组)
#[map]
static SCRATCH_STACK: PerCpuArray<StackPcsValue> = PerCpuArray::with_max_entries(1, 0);

/// 目标 PID（0 表示追踪所有进程）
#[map]
static TARGET_PID: aya_ebpf::maps::Array<u32> = aya_ebpf::maps::Array::with_max_entries(1, 0);

/// 诊断计数器
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Stats {
    pub malloc_entry: u64,
    pub malloc_ret: u64,
    pub free_entry: u64,
    pub calloc_entry: u64,
    pub calloc_ret: u64,
    pub realloc_entry: u64,
    pub realloc_ret: u64,
    pub pid_filtered: u64,
    pub arg_none: u64,
    pub scratch_none: u64,
    pub reserve_none: u64,
    pub pending_miss: u64,
    pub submitted: u64,
}

#[map]
static STATS: PerCpuArray<Stats> = PerCpuArray::with_max_entries(1, 0);

#[inline(always)]
fn stats_inc(f: impl Fn(&mut Stats)) {
    if let Some(ptr) = STATS.get_ptr_mut(0) {
        unsafe { f(&mut *ptr); }
    }
}

#[inline(always)]
fn pid_tgid() -> (u32, u32) {
    let v = aya_ebpf::helpers::bpf_get_current_pid_tgid();
    ((v >> 32) as u32, v as u32)
}

/// 检查当前进程是否为目标进程（TARGET_PID 为 0 时不过滤）
#[inline(always)]
fn is_target_pid() -> bool {
    if let Some(&target) = TARGET_PID.get(0) {
        if target != 0 {
            let (pid, _tid) = pid_tgid();
            return pid == target;
        }
    }
    // TARGET_PID 未设置或为 0：不过滤（仅用于调试，生产环境必须设置）
    false
}

const BPF_F_USER_STACK: u64 = 1 << 8;
const BPF_F_FAST_STACK_CMP: u64 = 1 << 9;
const BPF_F_REUSE_STACKID: u64 = 1 << 10;

#[inline(always)]
fn get_stack_id<C: EbpfContext>(ctx: &C) -> i64 {
    let flags = BPF_F_USER_STACK | BPF_F_FAST_STACK_CMP;
    unsafe { bpf_get_stackid(ctx.as_ptr(), &STACK_TRACES as *const _ as *mut _, flags) }
}

#[inline(always)]
fn now_ns() -> u64 {
    unsafe { bpf_ktime_get_ns() }
}

/// 获取 per-CPU scratch 指针，并初始化
#[inline(always)]
fn scratch_stack() -> Option<*mut StackPcsValue> {
    let ptr = SCRATCH_STACK.get_ptr_mut(0)?;
    unsafe {
        (*ptr).depth = 0;
        (*ptr)._pad = 0;
        // 不需要清零 pcs，bpf_get_stack 会覆盖
    }
    Some(ptr)
}

/// 捕获栈到 scratch 空间，返回 (stack_id, depth)
#[inline(always)]
fn capture_stack_to_scratch<C: EbpfContext>(ctx: &C, scratch: *mut StackPcsValue) -> (i32, u32) {
    let flags = BPF_F_USER_STACK | BPF_F_FAST_STACK_CMP | BPF_F_REUSE_STACKID;
    let sid = unsafe { bpf_get_stackid(ctx.as_ptr(), &STACK_TRACES as *const _ as *mut _, flags) };

    let pcs_ptr = unsafe { (*scratch).pcs.as_mut_ptr() };
    let stack_len = unsafe {
        bpf_get_stack(
            ctx.as_ptr(),
            pcs_ptr as *mut _,
            core::mem::size_of::<[u64; MAX_STACK_DEPTH]>() as u32,
            BPF_F_USER_STACK,
        )
    };

    let depth = if stack_len > 0 {
        (stack_len as usize / core::mem::size_of::<u64>()) as u32
    } else {
        0
    };
    unsafe { (*scratch).depth = depth; }

    (sid as i32, depth)
}

/// 提交分配事件（带 pcs 数据），pcs 从 scratch 空间读取
#[inline(always)]
fn submit_alloc_event(
    event_type: EventType,
    pid: u32,
    tid: u32,
    ts: u64,
    stack_id: i64,
    pcs_src: *const u64,
    depth: u32,
    addr: u64,
    size: u64,
    old_addr: u64,
) {
    // 构造事件在栈上（MemEvent 约 296 字节，小于 512 字节限制）
    let mut evt: MemEvent = MemEvent {
        event_type: event_type as u32,
        pid,
        tid,
        timestamp: ts,
        stack_id,
        stack_depth: depth,
        _reserved: 0,
        data: EventData {
            malloc_ret: MallocRetData {
                addr: 0,
                size: 0,
                pcs: [0u64; MAX_STACK_DEPTH],
            },
        },
    };

    unsafe {
        let data_ptr = core::ptr::addr_of_mut!(evt.data);
        match event_type {
            EventType::MallocReturn => {
                let d = data_ptr as *mut MallocRetData;
                (*d).addr = addr;
                (*d).size = size;
                let pcs_dst = core::ptr::addr_of_mut!((*d).pcs) as *mut u64;
                core::ptr::copy_nonoverlapping(pcs_src, pcs_dst, MAX_STACK_DEPTH);
            }
            EventType::CallocReturn => {
                let d = data_ptr as *mut CallocRetData;
                (*d).addr = addr;
                (*d).size = size;
                let pcs_dst = core::ptr::addr_of_mut!((*d).pcs) as *mut u64;
                core::ptr::copy_nonoverlapping(pcs_src, pcs_dst, MAX_STACK_DEPTH);
            }
            EventType::ReallocReturn => {
                let d = data_ptr as *mut ReallocRetData;
                (*d).addr = addr;
                (*d).size = size;
                (*d).old_addr = old_addr;
                let pcs_dst = core::ptr::addr_of_mut!((*d).pcs) as *mut u64;
                core::ptr::copy_nonoverlapping(pcs_src, pcs_dst, MAX_STACK_DEPTH);
            }
            _ => {}
        }
    }

    match EVENTS.output(&evt, 0) {
        Ok(()) => stats_inc(|s| s.submitted += 1),
        Err(_) => stats_inc(|s| s.reserve_none += 1),
    }
}

#[inline(always)]
fn submit_free_event(pid: u32, tid: u32, ts: u64, stack_id: i64, addr: u64) {
    let evt: MemEvent = MemEvent {
        event_type: EventType::Free as u32,
        pid,
        tid,
        timestamp: ts,
        stack_id,
        stack_depth: 0,
        _reserved: 0,
        data: EventData {
            free_evt: FreeData { addr },
        },
    };
    match EVENTS.output(&evt, 0) {
        Ok(()) => stats_inc(|s| s.submitted += 1),
        Err(_) => stats_inc(|s| s.reserve_none += 1),
    }
}

/// malloc entry: 记录 size 和 stack
#[uprobe]
pub fn uprobe_malloc_entry(ctx: ProbeContext) -> u32 {
    let (cur_pid, cur_tid) = pid_tgid();
    let target = TARGET_PID.get(0).copied().unwrap_or(0);
    if target == 0 || cur_pid != target {
        stats_inc(|s| {
            s.pid_filtered += 1;
            // 记录最后一次不匹配的 actual pid 和 target（低32位/高32位）
            s.arg_none = (cur_pid as u64) | ((target as u64) << 32);
        });
        return 0;
    }
    stats_inc(|s| s.malloc_entry += 1);
    let tid = cur_tid;

    let size: u64 = match ctx.arg(0) {
        Some(v) => v,
        None => {
            stats_inc(|s| s.arg_none += 1);
            return 0;
        }
    };

    let scratch = match scratch_stack() {
        Some(p) => p,
        None => {
            stats_inc(|s| s.scratch_none += 1);
            return 0;
        }
    };
    let (sid, _depth) = capture_stack_to_scratch(&ctx, scratch);

    let pending = PendingInfo {
        size,
        stack_id: sid,
        _pad: 0,
    };
    let _ = PENDING_MALLOCS.insert(&tid, &pending, 0);
    // 将 scratch 中的栈复制到 pending map
    let stack_ref = unsafe { &*scratch };
    let _ = PENDING_STACKS_MALLOC.insert(&tid, stack_ref, 0);
    0
}

/// malloc return: 发出 MallocReturn 事件
#[uretprobe]
pub fn uprobe_malloc_return(ctx: RetProbeContext) -> u32 {
    if !is_target_pid() {
        return 0;
    }
    stats_inc(|s| s.malloc_ret += 1);
    let (pid, tid) = pid_tgid();

    let ret: u64 = match ctx.ret() {
        Some(v) => v,
        None => {
            stats_inc(|s| s.arg_none += 1);
            return 0;
        }
    };
    if ret == 0 {
        return 0;
    }

    let pending = unsafe {
        match PENDING_MALLOCS.get(&tid) {
            Some(p) => *p,
            None => {
                stats_inc(|s| s.pending_miss += 1);
                return 0;
            }
        }
    };
    let _ = PENDING_MALLOCS.remove(&tid);

    // 从 pending map 读取栈到 scratch
    let scratch = match scratch_stack() {
        Some(p) => p,
        None => return 0,
    };
    let depth;
    let pcs_ptr;
    unsafe {
        if let Some(src) = PENDING_STACKS_MALLOC.get(&tid) {
            core::ptr::copy_nonoverlapping(
                src as *const StackPcsValue,
                scratch,
                1,
            );
            depth = (*scratch).depth;
            pcs_ptr = (*scratch).pcs.as_ptr();
        } else {
            depth = 0;
            pcs_ptr = (*scratch).pcs.as_ptr();
            (*scratch).depth = 0;
        }
    }
    let _ = PENDING_STACKS_MALLOC.remove(&tid);

    let ts = now_ns();
    let info = AllocInfo {
        size: pending.size,
        stack_id: pending.stack_id as i64,
        timestamp: ts,
        pid,
        tid,
    };
    let _ = ACTIVE_ALLOCS.insert(&ret, &info, 0);

    submit_alloc_event(
        EventType::MallocReturn,
        pid,
        tid,
        ts,
        info.stack_id,
        pcs_ptr,
        depth,
        ret,
        pending.size,
        0,
    );
    0
}

/// free: 标记分配为已完成，发出 Free 事件
#[uprobe]
pub fn uprobe_free(ctx: ProbeContext) -> u32 {
    if !is_target_pid() {
        return 0;
    }
    let (pid, tid) = pid_tgid();

    let ptr: u64 = match ctx.arg(0) {
        Some(v) => v,
        None => return 0,
    };
    if ptr == 0 {
        return 0;
    }

    let _ = ACTIVE_ALLOCS.remove(&ptr);

    submit_free_event(pid, tid, now_ns(), get_stack_id(&ctx), ptr);
    0
}

/// calloc entry: 记录 nmemb * size 和 stack
#[uprobe]
pub fn uprobe_calloc_entry(ctx: ProbeContext) -> u32 {
    if !is_target_pid() {
        return 0;
    }
    let (_pid, tid) = pid_tgid();

    let nmemb: u64 = match ctx.arg(0) {
        Some(v) => v,
        None => return 0,
    };
    let size: u64 = match ctx.arg(1) {
        Some(v) => v,
        None => return 0,
    };

    let scratch = match scratch_stack() {
        Some(p) => p,
        None => return 0,
    };
    let (sid, _depth) = capture_stack_to_scratch(&ctx, scratch);

    let pending = PendingInfo {
        size: nmemb * size,
        stack_id: sid,
        _pad: 0,
    };
    let _ = PENDING_CALLOCS.insert(&tid, &pending, 0);
    let stack_ref = unsafe { &*scratch };
    let _ = PENDING_STACKS_CALLOC.insert(&tid, stack_ref, 0);
    0
}

/// calloc return: 发出 CallocReturn 事件
#[uretprobe]
pub fn uprobe_calloc_return(ctx: RetProbeContext) -> u32 {
    if !is_target_pid() {
        return 0;
    }
    let (pid, tid) = pid_tgid();

    let ret: u64 = match ctx.ret() {
        Some(v) => v,
        None => return 0,
    };
    if ret == 0 {
        return 0;
    }

    let pending = unsafe {
        match PENDING_CALLOCS.get(&tid) {
            Some(p) => *p,
            None => return 0,
        }
    };
    let _ = PENDING_CALLOCS.remove(&tid);

    let scratch = match scratch_stack() {
        Some(p) => p,
        None => return 0,
    };
    let depth;
    let pcs_ptr;
    unsafe {
        if let Some(src) = PENDING_STACKS_CALLOC.get(&tid) {
            core::ptr::copy_nonoverlapping(
                src as *const StackPcsValue,
                scratch,
                1,
            );
            depth = (*scratch).depth;
            pcs_ptr = (*scratch).pcs.as_ptr();
        } else {
            depth = 0;
            pcs_ptr = (*scratch).pcs.as_ptr();
            (*scratch).depth = 0;
        }
    }
    let _ = PENDING_STACKS_CALLOC.remove(&tid);

    let ts = now_ns();
    let info = AllocInfo {
        size: pending.size,
        stack_id: pending.stack_id as i64,
        timestamp: ts,
        pid,
        tid,
    };
    let _ = ACTIVE_ALLOCS.insert(&ret, &info, 0);

    submit_alloc_event(
        EventType::CallocReturn,
        pid,
        tid,
        ts,
        info.stack_id,
        pcs_ptr,
        depth,
        ret,
        pending.size,
        0,
    );
    0
}

/// realloc entry: 记录 new_size 和 stack
#[uprobe]
pub fn uprobe_realloc_entry(ctx: ProbeContext) -> u32 {
    if !is_target_pid() {
        return 0;
    }
    let (_pid, tid) = pid_tgid();

    let _ptr: u64 = match ctx.arg(0) {
        Some(v) => v,
        None => return 0,
    };
    let size: u64 = match ctx.arg(1) {
        Some(v) => v,
        None => return 0,
    };

    let scratch = match scratch_stack() {
        Some(p) => p,
        None => return 0,
    };
    let (sid, _depth) = capture_stack_to_scratch(&ctx, scratch);

    let pending = PendingInfo {
        size,
        stack_id: sid,
        _pad: 0,
    };
    let _ = PENDING_REALLOCS.insert(&tid, &pending, 0);
    let stack_ref = unsafe { &*scratch };
    let _ = PENDING_STACKS_REALLOC.insert(&tid, stack_ref, 0);
    0
}

/// realloc return: 发出 ReallocReturn 事件
#[uretprobe]
pub fn uprobe_realloc_return(ctx: RetProbeContext) -> u32 {
    if !is_target_pid() {
        return 0;
    }
    let (pid, tid) = pid_tgid();

    let ret: u64 = match ctx.ret() {
        Some(v) => v,
        None => return 0,
    };
    if ret == 0 {
        return 0;
    }

    let pending = unsafe {
        match PENDING_REALLOCS.get(&tid) {
            Some(p) => *p,
            None => return 0,
        }
    };
    let _ = PENDING_REALLOCS.remove(&tid);

    let scratch = match scratch_stack() {
        Some(p) => p,
        None => return 0,
    };
    let depth;
    let pcs_ptr;
    unsafe {
        if let Some(src) = PENDING_STACKS_REALLOC.get(&tid) {
            core::ptr::copy_nonoverlapping(
                src as *const StackPcsValue,
                scratch,
                1,
            );
            depth = (*scratch).depth;
            pcs_ptr = (*scratch).pcs.as_ptr();
        } else {
            depth = 0;
            pcs_ptr = (*scratch).pcs.as_ptr();
            (*scratch).depth = 0;
        }
    }
    let _ = PENDING_STACKS_REALLOC.remove(&tid);

    let ts = now_ns();
    let info = AllocInfo {
        size: pending.size,
        stack_id: pending.stack_id as i64,
        timestamp: ts,
        pid,
        tid,
    };
    let _ = ACTIVE_ALLOCS.insert(&ret, &info, 0);

    submit_alloc_event(
        EventType::ReallocReturn,
        pid,
        tid,
        ts,
        info.stack_id,
        pcs_ptr,
        depth,
        ret,
        pending.size,
        0,
    );
    0
}

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}
