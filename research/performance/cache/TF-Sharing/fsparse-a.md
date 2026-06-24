# fsparse_a: Rust 原生 eBPF 堆追踪器

基于 **Aya + gimli** 的 eBPF 内存分配追踪工具，通过 uprobe 拦截 libc 内存函数，结合 DWARF 调试信息将堆地址映射到具体结构体字段。

## 项目概述

fsparse_a 是从 memscope 项目迁移并重构的堆检测功能，使用 Rust 原生实现（脱离 memscope 独立运行），主要特性：

- **eBPF 追踪**：通过 uprobe/uretprobe 拦截 `malloc`/`free`/`calloc`/`realloc`
- **RingBuf 传输**：内核态事件通过 ringbuf 高效传输到用户态
- **DWARF 解析**：使用 gimli 解析调试信息，将地址映射到结构体字段
- **PID 过滤**：仅追踪目标进程，避免系统负载过高
- **CSV 报告**：生成地址-字段映射报告

## 架构设计

```
┌─────────────────────────────────────────────────────────────┐
│                    用户态 (fsparse_a)                        │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐      │
│  │   EbpfLoader │  │   RingBuf    │  │  DwarfInfo   │      │
│  │  (加载字节码) │  │  (消费事件)  │  │ (gimli解析)  │      │
│  └──────────────┘  └──────────────┘  └──────────────┘      │
│         │                 │                 │               │
│         ▼                 ▼                 ▼               │
│  ┌──────────────────────────────────────────────────┐      │
│  │              AllocTable (分配记录管理)            │      │
│  └──────────────────────────────────────────────────┘      │
└─────────────────────────────────────────────────────────────┘
                          ▲
                          │ RingBuf 事件
                          │
┌─────────────────────────────────────────────────────────────┐
│                    内核态 (fsparse_a-ebpf)                   │
│  ┌──────────────────────────────────────────────────┐      │
│  │  uprobe_malloc_entry  → 记录 size + stack        │      │
│  │  uretprobe_malloc    → 发送 addr + size + pcs    │      │
│  │  uprobe_free         → 发送 addr                 │      │
│  │  uprobe_calloc_entry → 记录 nmemb * size         │      │
│  │  uretprobe_calloc    → 发送 addr + size + pcs    │      │
│  │  uprobe_realloc_entry → 记录 old_addr + new_size │      │
│  │  uretprobe_realloc   → 发送 addr + size + pcs    │      │
│  └──────────────────────────────────────────────────┘      │
│                          │                                   │
│                          ▼                                   │
│  ┌──────────────────────────────────────────────────┐      │
│  │  EVENTS (RingBuf) → 用户态                        │      │
│  │  ACTIVE_ALLOCS (HashMap) → addr → AllocInfo      │      │
│  │  PENDING_* (HashMap) → tid → PendingInfo         │      │
│  │  STACK_TRACES (StackTrace) → stack_id → PCs      │      │
│  └──────────────────────────────────────────────────┘      │
└─────────────────────────────────────────────────────────────┘
                          ▲
                          │ uprobe/uretprobe
                          │
┌─────────────────────────────────────────────────────────────┐
│                    目标进程 (libc.so.6)                      │
│  malloc() → uprobe_malloc_entry → uretprobe_malloc          │
│  free()   → uprobe_free                                     │
│  calloc() → uprobe_calloc_entry → uretprobe_calloc          │
│  realloc() → uprobe_realloc_entry → uretprobe_realloc       │
└─────────────────────────────────────────────────────────────┘
```

## 项目结构

```
fsparse_a/
├── Cargo.toml              # Workspace 配置
├── fsparse_a/              # 用户态主程序
│   ├── src/
│   │   ├── main.rs         # 主入口：加载 eBPF、消费事件、生成报告
│   │   ├── dwarf.rs        # DWARF 解析（gimli）
│   │   └── resolver.rs     # AllocTable 管理
│   └── build.rs            # 嵌入 eBPF 字节码
├── fsparse_a-ebpf/         # 内核态 eBPF 程序
│   ├── src/
│   │   └── main.rs         # uprobe/uretprobe 实现
│   └── Cargo.toml
├── fsparse_a-common/       # 共享数据结构
│   ├── src/
│   │   └── lib.rs          # MemEvent, EventType, AllocInfo 等
│   └── Cargo.toml
└── xtask/                  # 构建工具
    └── src/
        └── main.rs         # cargo xtask build-ebpf
```

## 使用方法

### 编译

```bash
cd fsparse_a
cargo xtask build-ebpf      # 编译 eBPF 程序
cargo build --release       # 编译用户态程序
```

正确构建方式 ：

- 构建用户态程序： cargo build -p fsparse_a --release
- 构建全部（eBPF + 用户态）： cargo xtask build 或 cargo xtask build-ebpf 然后再 cargo build -p fsparse_a --release


编译成功。

原因 ：在 workspace 根目录运行 cargo build --release 会编译所有成员（包括 fsparse_a-ebpf ），但 eBPF 程序需要用 bpfel-unknown-none 目标编译，不能用主机目标链接 glibc。

```
cd /home/neu/exp/alicewiki/research/performance/cache/TF-Sharing/fsparse_a && cargo build -p fsparse_a --release 2>&1 | tail -20
```

### 运行

```bash
# 追踪目标进程（指定 PID）
sudo ./target/release/fsparse_a \
    --binary /path/to/target_binary \
    --pid <TARGET_PID> \
    --duration 5 \
    --output report.csv \
    trace

# 仅打印 DWARF 类型布局
sudo ./target/release/fsparse_a \
    --binary /path/to/target_binary \
    dump-types
```

### 参数说明

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `--binary` | 目标二进制（需带调试信息） | 必填 |
| `--pid` | 目标 PID（仅追踪此进程） | 可选 |
| `--output` | CSV 输出路径 | `fsparse_allocs.csv` |
| `--duration` | 追踪时长（秒），0 表示手动 Ctrl+C | `0` |

### 测试示例

```bash
# 编译测试程序（带调试信息）
gcc -g -o /tmp/malloc_test /tmp/malloc_test.c

# 运行测试程序
/tmp/malloc_test &
TARGET_PID=$!

# 启动追踪器
sudo ./target/release/fsparse_a \
    --binary /tmp/malloc_test \
    --pid $TARGET_PID \
    --duration 5 \
    --output /tmp/test.csv \
    trace
```

## 数据结构

### 共享结构 (fsparse_a-common)

```rust
// 事件类型
pub enum EventType {
    MallocEntry = 1,    // malloc 入口（记录 size）
    MallocReturn = 2,   // malloc 返回（记录 addr）
    Free = 3,           // free 入口
    CallocReturn = 7,   // calloc 返回
    ReallocEntry = 8,   // realloc 入口
    ReallocReturn = 9,  // realloc 返回
}

// RingBuf 事件
pub struct MemEvent {
    event_type: u32,    // EventType
    pid: u32,           // 进程 ID
    tid: u32,           // 线程 ID
    timestamp: u64,     // 时间戳 (ns)
    stack_id: i64,      // 调用栈 ID
    stack_depth: u32,   // 调用栈深度
    data: EventData,    // 事件数据（union）
}

// 分配信息（存储在 ACTIVE_ALLOCS）
pub struct AllocInfo {
    size: u64,
    stack_id: i64,
    timestamp: u64,
    pid: u32,
    tid: u32,
}
```

### eBPF Maps

| Map | 类型 | 说明 |
|-----|------|------|
| `EVENTS` | RingBuf | 事件传输（16MB） |
| `ACTIVE_ALLOCS` | HashMap | addr → AllocInfo |
| `PENDING_MALLOCS` | HashMap | tid → PendingInfo |
| `PENDING_CALLOCS` | HashMap | tid → PendingInfo |
| `PENDING_REALLOCS` | HashMap | tid → PendingInfo |
| `STACK_TRACES` | StackTrace | stack_id → PCs |
| `SCRATCH_STACK` | PerCpuArray | 栈捕获临时空间 |
| `TARGET_PID` | Array | 目标 PID 过滤 |
| `STATS` | PerCpuArray | 诊断计数器 |

## CSV 输出格式

```csv
address,size,region,type,field,offset,size_bytes,field_type,infer_method,confidence
0x7f1234567890,256,HEAP,MyStruct,field.subfield,16,4,int32,dwarf,confidence=100
```

| 字段 | 说明 |
|------|------|
| `address` | 分配地址 |
| `size` | 分配大小 |
| `region` | 区域类型（HEAP/STACK/MMAP） |
| `type` | 结构体类型名 |
| `field` | 字段路径（如 `lreg_args[0].tid`） |
| `offset` | 字段偏移 |
| `size_bytes` | 字段大小 |
| `field_type` | 字段类型 |
| `infer_method` | 推断方法（dwarf/no_type） |
| `confidence` | 置信度 |

## 技术细节

### 1. PID 过滤机制

eBPF 程序通过 `TARGET_PID` Map 过滤目标进程：

```rust
// eBPF 中
fn is_target_pid() -> bool {
    let target = TARGET_PID.get(0).copied().unwrap_or(0);
    if target == 0 { return true; }  // 0 表示追踪所有
    let (pid, _) = pid_tgid();
    pid == target
}
```

用户态设置：
```rust
target_pid_map.set(0, pid, 0)?;
```

### 2. Per-CPU 临时存储

BPF 栈限制为 512 字节，大数组需移至 PerCpuArray：

```rust
// 避免栈溢出
#[map]
static SCRATCH_STACK: PerCpuArray<StackPcsValue> = PerCpuArray::with_max_entries(1, 0);

fn capture_stack_to_scratch(ctx: &ProbeContext, scratch: &mut StackPcsValue) {
    // 使用 scratch 存储调用栈
}
```

### 3. 调用栈捕获

两种方式：
- **bpf_get_stackid**：存储到 `STACK_TRACES` Map
- **手动遍历**：使用 `bpf_get_stack` 直接获取 PCs

### 4. DWARF 解析流程

```rust
// 1. 加载 DWARF 信息
let dwarf_info = DwarfInfo::from_file(&binary)?;

// 2. 解析结构体布局
dwarf_info.find_type_by_name("MyStruct")?;

// 3. 地址映射
let offset = addr - alloc_base_addr;
let field = dwarf_info.resolve_field(type_name, offset)?;
```

### 5. RingBuf 事件消费

```rust
let mut ringbuf = RingBuf::try_from(bpf.take_map("EVENTS")?)?;

while running.load(Ordering::SeqCst) {
    while let Some(item) = ringbuf.next() {
        let evt: &MemEvent = unsafe { &*(item.as_ptr() as *const MemEvent) };
        handle_event(evt, &mut alloc_table, dwarf_info);
    }
    std::thread::sleep(Duration::from_millis(10));
}
```

## 已解决的问题

### 1. BPF 栈大小超限

**问题**：`pcs` 数组（256字节）+ 其他栈变量超过 BPF 栈限制（512字节）。

**解决**：引入 `PerCpuArray<StackPcsValue>` 作为 per-CPU 临时存储。

### 2. 主机卡死

**问题**：全局 uprobe 导致所有进程的 malloc 事件被拦截，系统负载过高。

**解决**：添加 `TARGET_PID` 过滤，仅处理目标进程事件。

### 3. ctrlc_handler 立即退出

**问题**：`std::thread::spawn(move || { let _ = f(); })` 在新线程中立即调用回调，导致 tracer 在 ~694ms 后退出。

**解决**：修复为正确的信号处理逻辑，将回调函数设置到 `HANDLER` 而非立即执行。

### 4. libc 路径不一致

**问题**：Aya 使用 `/lib/x86_64-linux-gnu/libc.so.6`，目标进程加载 `/usr/lib/x86_64-linux-gnu/libc.so.6`。

**解决**：使用 `std::fs::canonicalize` 规范化路径。

### 5. PID 参数未传递

**问题**：`attach_uprobes` 调用时传入 `None` 而非 `cli.pid`。

**解决**：修改为 `attach_uprobes(&mut bpf, &libc_path, cli.pid)?;`。

## 性能指标

测试环境：
- 目标进程：100 次 malloc/free 循环（每次 50ms）
- 追踪时长：5 秒

结果：
- 捕获率：99/100 malloc 事件
- 事件传输：393 个事件通过 RingBuf
- 分配追踪：197 个分配，196 个已释放
- PID 过滤：无误拦截（`pid_filtered=0`）

## 与 memscope 对比

| 特性 | memscope | fsparse_a |
|------|----------|-----------|
| 语言 | C++ + BCC | Rust + Aya |
| eBPF 加载 | BCC 动态编译 | 预编译字节码 |
| DWARF 解析 | libdw | gimli |
| PID 过滤 | 用户态过滤 | 内核态过滤 |
| 独立运行 | 依赖 memscope-collect | 独立二进制 |
| 性能 | 中等 | 高效（RingBuf） |

## 参考资料

- [Aya Book](https://aya-rs.dev/book/)
- [gimli 文档](https://docs.rs/gimli/)
- [eBPF Maps 详解](https://docs.kernel.org/bpf/maps.html)
- [DWARF 调试信息格式](https://dwarfstd.org/)