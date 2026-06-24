#![no_std]

pub const MAX_STACK_DEPTH: usize = 32;
pub const MAX_EVENTS: u32 = 16384;

/// 事件类型，对应 memscope 的 event_type 枚举
#[repr(u32)]
#[derive(Clone, Copy, Debug)]
pub enum EventType {
    MallocEntry = 1,
    MallocReturn = 2,
    Free = 3,
    Mmap = 4,
    Munmap = 5,
    StackSample = 6,
    CallocReturn = 7,
    ReallocEntry = 8,
    ReallocReturn = 9,
}

/// BPF ring buffer 中传递的事件
#[repr(C)]
#[derive(Clone, Copy)]
pub struct MemEvent {
    pub event_type: u32,
    pub pid: u32,
    pub tid: u32,
    pub timestamp: u64,
    pub stack_id: i64,
    pub stack_depth: u32,
    pub _reserved: u32,
    pub data: EventData,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub union EventData {
    pub malloc_entry: MallocEntryData,
    pub malloc_ret: MallocRetData,
    pub free_evt: FreeData,
    pub mmap_evt: MmapData,
    pub munmap_evt: MunmapData,
    pub calloc_ret: CallocRetData,
    pub realloc_entry: ReallocEntryData,
    pub realloc_ret: ReallocRetData,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct MallocEntryData {
    pub size: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct MallocRetData {
    pub addr: u64,
    pub size: u64,
    pub pcs: [u64; MAX_STACK_DEPTH],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct FreeData {
    pub addr: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct MmapData {
    pub addr: u64,
    pub size: u64,
    pub prot: i32,
    pub flags: i32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct MunmapData {
    pub addr: u64,
    pub size: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct CallocRetData {
    pub addr: u64,
    pub size: u64,
    pub pcs: [u64; MAX_STACK_DEPTH],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct ReallocEntryData {
    pub old_addr: u64,
    pub new_size: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct ReallocRetData {
    pub addr: u64,
    pub size: u64,
    pub old_addr: u64,
    pub pcs: [u64; MAX_STACK_DEPTH],
}

/// BPF HashMap 中存储的分配信息
#[repr(C)]
#[derive(Clone, Copy)]
pub struct AllocInfo {
    pub size: u64,
    pub stack_id: i64,
    pub timestamp: u64,
    pub pid: u32,
    pub tid: u32,
}

/// 线程局部 pending 信息（malloc entry → return 之间暂存）
#[repr(C)]
#[derive(Clone, Copy)]
pub struct PendingInfo {
    pub size: u64,
    pub stack_id: i32,
    pub _pad: u32,
}

/// 栈 PC 数组
#[repr(C)]
#[derive(Clone, Copy)]
pub struct StackPcsValue {
    pub depth: u32,
    pub _pad: u32,
    pub pcs: [u64; MAX_STACK_DEPTH],
}

#[cfg(feature = "std")]
impl MemEvent {
    pub fn event_type(&self) -> EventType {
        match self.event_type {
            1 => EventType::MallocEntry,
            2 => EventType::MallocReturn,
            3 => EventType::Free,
            4 => EventType::Mmap,
            5 => EventType::Munmap,
            6 => EventType::StackSample,
            7 => EventType::CallocReturn,
            8 => EventType::ReallocEntry,
            9 => EventType::ReallocReturn,
            _ => EventType::StackSample,
        }
    }
}
