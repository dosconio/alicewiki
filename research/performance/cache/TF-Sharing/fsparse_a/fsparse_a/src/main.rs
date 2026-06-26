//! fsparse_a: Rust 原生 eBPF 堆追踪 + DWARF 字段解析
//!
//! 使用 Aya 加载 eBPF 程序，挂载 uprobe/uretprobe 到 libc 的 malloc/free/calloc/realloc，
//! 通过 ringbuf 消费事件，维护 alloc_table，并使用 gimli 解析 DWARF 调试信息，
//! 将堆地址映射到具体结构体字段路径。

use std::collections::BTreeMap;
use std::fs::File;
use std::io::Read;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

use aya::maps::RingBuf;
use aya::programs::UProbe;
use aya::{Ebpf, EbpfLoader};
use clap::{Parser, Subcommand};
use memmap2::Mmap;

mod dwarf;
mod resolver;

use dwarf::DwarfInfo;
use resolver::AllocTable;

// 由 build.rs 生成的 eBPF 字节码
include!(concat!(env!("OUT_DIR"), "/ebpf_bytes.rs"));

use fsparse_a_common::{EventData, EventType, MemEvent};

#[derive(Parser, Debug)]
#[command(name = "fsparse_a", version, about = "eBPF heap tracker with DWARF field resolution")]
struct Cli {
    /// 目标二进制（带调试信息）
    #[arg(long, short)]
    binary: PathBuf,

    /// 目标 PID（仅追踪此 PID 的内存分配）
    #[arg(long, short)]
    pid: Option<u32>,

    /// 输出 CSV 路径（地址-字段映射）
    #[arg(long, short, default_value = "fsparse_allocs.csv")]
    output: PathBuf,

    /// 追踪时长（秒），0 表示手动 Ctrl+C 退出
    #[arg(long, short, default_value = "0")]
    duration: u64,

    #[command(subcommand)]
    command: Option<Command>,
}

#[derive(Subcommand, Debug)]
enum Command {
    /// 仅加载并打印 DWARF 类型布局
    DumpTypes {
        #[arg(long)]
        type_name: Option<String>,
    },
    /// 追踪目标进程
    Trace,
}

/// 内存分配记录
#[derive(Clone, Debug)]
pub struct AllocRecord {
    pub addr: u64,
    pub size: u64,
    pub stack_id: i64,
    pub timestamp: u64,
    pub pid: u32,
    pub tid: u32,
    pub stack_pcs: Vec<u64>,
    pub live: bool,
    /// 释放时间戳（bpf_ktime_get_ns）；0 表示未释放
    pub free_timestamp: u64,
}

fn main() -> anyhow::Result<()> {
    let cli = Cli::parse();

    // 加载 DWARF 调试信息
    let mut dwarf_info = DwarfInfo::load(&cli.binary)?;
    println!(
        "[+] Loaded DWARF: {} types, {} symbols",
        dwarf_info.types_len(),
        dwarf_info.symbols_len()
    );

    if let Some(Command::DumpTypes { type_name }) = &cli.command {
        if let Some(name) = type_name {
            if let Some(ty) = dwarf_info.find_type_by_name(name) {
                println!("{}", ty.layout_string());
            } else {
                println!("Type '{}' not found", name);
            }
        } else {
            for ty in dwarf_info.iter_types() {
                println!(
                    "  {:<40} size={:>5} fields={} enc={}",
                    ty.name,
                    ty.byte_size,
                    ty.fields.len(),
                    ty.encoding
                );
            }
        }
        return Ok(());
    }

    // 默认进入 Trace 模式
    run_trace(&cli, &mut dwarf_info)
}

fn run_trace(cli: &Cli, dwarf_info: &mut DwarfInfo) -> anyhow::Result<()> {
    // 加载 eBPF 程序
    let mut bpf = EbpfLoader::default()
        .load(BPF_EBPF_BYTES)
        .map_err(|e| anyhow::anyhow!("failed to load eBPF: {}", e))?;

    // 设置目标 PID（在挂载 uprobe 之前，避免捕获无关进程事件）
    if let Some(pid) = cli.pid {
        if let Some(map) = bpf.map_mut("TARGET_PID") {
            let mut target_pid_map: aya::maps::Array<_, u32> =
                aya::maps::Array::try_from(map)?;
            target_pid_map.set(0, pid, 0)?;
            // 验证设置成功
            let verify = target_pid_map.get(&0, 0)?;
            println!("[+] Target PID set: {} (verified: {})", pid, verify);
        } else {
            println!("[!] TARGET_PID map not found");
        }
    }

    // 查找 libc 路径
    let libc_path = find_libc()?;
    println!("[+] libc: {}", libc_path.display());

    // 挂载 uprobe/uretprobe
    // 传 pid 给 Aya：perf_event_open 会用 pid=目标, cpu=-1，仅捕获目标进程事件
    // 这比全局附加（cpu=0, pid=-1）更精确，避免拦截无关进程
    attach_uprobes(&mut bpf, &libc_path, cli.pid)?;

    // 打开 ringbuf
    let mut ringbuf = RingBuf::try_from(bpf.take_map("EVENTS").expect("EVENTS map missing"))?;

    // 维护 alloc table
    let mut alloc_table = AllocTable::new();

    // 信号处理
    let running = Arc::new(AtomicBool::new(true));
    let r = running.clone();
    ctrlc_handler(move || {
        r.store(false, Ordering::SeqCst);
    });

    let target_pid = cli.pid;
    let deadline = if cli.duration > 0 {
        Some(std::time::Instant::now() + std::time::Duration::from_secs(cli.duration))
    } else {
        None
    };

    println!("[+] Tracing... (Ctrl+C to stop)");

    // 主循环：poll ringbuf
    while running.load(Ordering::SeqCst) {
        if let Some(dl) = deadline {
            if std::time::Instant::now() >= dl {
                break;
            }
        }

        // 消费 ringbuf 事件
        let mut evt_count = 0u64;
        while let Some(item) = ringbuf.next() {
            evt_count += 1;
            if item.len() < std::mem::size_of::<MemEvent>() {
                eprintln!("[user] short item: len={} expected={}", item.len(), std::mem::size_of::<MemEvent>());
                continue;
            }
            let evt: &MemEvent = unsafe { &*(item.as_ptr() as *const MemEvent) };

            // 调试：打印前 5 个事件的 pid
            if evt_count <= 5 {
                eprintln!("[user] evt#{} pid={} tid={} event_type={}", evt_count, evt.pid, evt.tid, evt.event_type);
            }

            // PID 过滤
            if let Some(pid) = target_pid {
                if evt.pid != pid {
                    continue;
                }
            }

            handle_event(evt, &mut alloc_table, dwarf_info);
        }
        if evt_count > 0 {
            eprintln!("[user] consumed {} events this round, alloc_table total={}", evt_count, alloc_table.total_count());
        }
        // 短暂休眠避免 busy loop
        std::thread::sleep(std::time::Duration::from_millis(10));
    }

    // 输出 CSV 报告
    println!("[+] Writing report to {}", cli.output.display());
    write_csv(&cli.output, &alloc_table, dwarf_info)?;

    // 打印诊断统计
    print_stats(&bpf);

    println!(
        "[+] Done. {} allocations tracked, {} live, {} freed",
        alloc_table.total_count(),
        alloc_table.live_count(),
        alloc_table.freed_count()
    );

    Ok(())
}

fn print_stats(bpf: &aya::Ebpf) {
    // STATS 是 PerCpuArray，用 Array 读取会失败，这里用简单方式：通过 bpftool 或跳过
    // 改为读取 PerCpuArray
    use aya::maps::PerCpuArray;
    #[repr(C)]
    #[derive(Clone, Copy, Default)]
    struct Stats {
        malloc_entry: u64,
        malloc_ret: u64,
        free_entry: u64,
        calloc_entry: u64,
        calloc_ret: u64,
        realloc_entry: u64,
        realloc_ret: u64,
        pid_filtered: u64,
        arg_none: u64,
        scratch_none: u64,
        reserve_none: u64,
        pending_miss: u64,
        submitted: u64,
    }
    unsafe impl aya::Pod for Stats {}

    if let Some(map) = bpf.map("STATS") {
        if let Ok(mut stats_map) = PerCpuArray::<_, Stats>::try_from(map) {
            if let Ok(per_cpu) = stats_map.get(&0, 0) {
                let mut t = Stats::default();
                for s in per_cpu.iter() {
                    t.malloc_entry += s.malloc_entry;
                    t.malloc_ret += s.malloc_ret;
                    t.free_entry += s.free_entry;
                    t.calloc_entry += s.calloc_entry;
                    t.calloc_ret += s.calloc_ret;
                    t.realloc_entry += s.realloc_entry;
                    t.realloc_ret += s.realloc_ret;
                    t.pid_filtered += s.pid_filtered;
                    // arg_none 记录最后一次不匹配的 actual_pid(低32) | target(高32)，取最后一个非零值
                    if s.arg_none != 0 {
                        t.arg_none = s.arg_none;
                    }
                    t.scratch_none += s.scratch_none;
                    t.reserve_none += s.reserve_none;
                    t.pending_miss += s.pending_miss;
                    t.submitted += s.submitted;
                }
                // 解码 arg_none：actual_pid(低32位) | target(高32位)
                let actual_pid = t.arg_none & 0xFFFFFFFF;
                let target_pid = (t.arg_none >> 32) & 0xFFFFFFFF;
                println!("[stats] malloc_e={} malloc_r={} free={} calloc_e={} calloc_r={} realloc_e={} realloc_r={}",
                    t.malloc_entry, t.malloc_ret, t.free_entry,
                    t.calloc_entry, t.calloc_ret, t.realloc_entry, t.realloc_ret);
                println!("[stats] pid_filtered={} last_actual_pid={} last_target_pid={} scratch_none={} reserve_none={} pending_miss={} submitted={}",
                    t.pid_filtered, actual_pid, target_pid, t.scratch_none,
                    t.reserve_none, t.pending_miss, t.submitted);
            }
        }
    }
}

fn handle_event(evt: &MemEvent, alloc_table: &mut AllocTable, dwarf_info: &DwarfInfo) {
    eprintln!("[handle_event] event_type={} pid={} tid={}", evt.event_type, evt.pid, evt.tid);
    match evt.event_type() {
        EventType::MallocReturn | EventType::CallocReturn | EventType::ReallocReturn => {
            let (addr, size, pcs) = match evt.event_type() {
                EventType::MallocReturn => unsafe {
                    let d = evt.data.malloc_ret;
                    (d.addr, d.size, d.pcs.to_vec())
                },
                EventType::CallocReturn => unsafe {
                    let d = evt.data.calloc_ret;
                    (d.addr, d.size, d.pcs.to_vec())
                },
                EventType::ReallocReturn => unsafe {
                    let d = evt.data.realloc_ret;
                    (d.addr, d.size, d.pcs.to_vec())
                },
                _ => unreachable!(),
            };

            let depth = evt.stack_depth as usize;
            let pcs = if pcs.len() >= depth {
                pcs[..depth].to_vec()
            } else {
                pcs
            };

            alloc_table.insert(AllocRecord {
                addr,
                size,
                stack_id: evt.stack_id,
                timestamp: evt.timestamp,
                pid: evt.pid,
                tid: evt.tid,
                stack_pcs: pcs,
                live: true,
                free_timestamp: 0,
            });
            eprintln!("[insert] addr=0x{:x} size={} total={}", addr, size, alloc_table.total_count());

            if cfg!(debug_assertions) {
                eprintln!(
                    "[alloc] pid={} tid={} addr=0x{:x} size={} stack_id={}",
                    evt.pid, evt.tid, addr, size, evt.stack_id
                );
            }
        }
        EventType::Free => unsafe {
            let addr = evt.data.free_evt.addr;
            alloc_table.mark_freed(addr, evt.timestamp);
            if cfg!(debug_assertions) {
                eprintln!("[free]  pid={} tid={} addr=0x{:x} ts={}", evt.pid, evt.tid, addr, evt.timestamp);
            }
        },
        _ => {}
    }
}

fn write_csv(path: &PathBuf, alloc_table: &AllocTable, dwarf: &DwarfInfo) -> anyhow::Result<()> {
    use std::io::Write;
    let mut f = std::io::BufWriter::new(File::create(path)?);
    writeln!(
        f,
        "address,size,region,type,field,offset,size_bytes,field_type,infer_method,confidence,alloc_ts,free_ts,live"
    )?;

    // 输出全部分配（含已释放），便于下游按 hitm 时间窗匹配 live 期间的分配
    for rec in alloc_table.iter_all() {
        let offset = 0u64;
        let (type_name, field, field_type, infer_method, confidence) =
            resolver::resolve_field_for_alloc(rec, dwarf);

        writeln!(
            f,
            "0x{:x},{},HEAP,{},{},{},{},{},{},confidence={},{},{},{}",
            rec.addr,
            rec.size,
            type_name,
            field,
            offset,
            0,
            field_type,
            infer_method,
            confidence,
            rec.timestamp,
            rec.free_timestamp,
            if rec.live { 1 } else { 0 },
        )?;
    }

    Ok(())
}

fn find_libc() -> anyhow::Result<PathBuf> {
    // 优先使用 ld.so 缓存
    let output = std::process::Command::new("ldconfig")
        .args(["-p"])
        .output()?;
    for line in String::from_utf8_lossy(&output.stdout).lines() {
        if line.contains("libc.so.6") && line.contains("x86-64") {
            if let Some(idx) = line.rfind("=> ") {
                let path = line[idx + 3..].trim();
                let p = PathBuf::from(path);
                if p.exists() {
                    // 规范化路径，确保与进程实际加载的路径一致
                    let canon = std::fs::canonicalize(&p).unwrap_or(p);
                    return Ok(canon);
                }
            }
        }
    }
    // 回退到常见路径
    for p in ["/lib/x86_64-linux-gnu/libc.so.6", "/lib64/libc.so.6", "/usr/lib/libc.so.6"] {
        let p = PathBuf::from(p);
        if p.exists() {
            return Ok(p);
        }
    }
    anyhow::bail!("libc.so.6 not found")
}

fn attach_uprobes(bpf: &mut Ebpf, libc_path: &PathBuf, pid: Option<u32>) -> anyhow::Result<()> {
    let prog_names = [
        "uprobe_malloc_entry",
        "uprobe_malloc_return",
        "uprobe_free",
        "uprobe_calloc_entry",
        "uprobe_calloc_return",
        "uprobe_realloc_entry",
        "uprobe_realloc_return",
    ];

    for name in prog_names {
        let prog: &mut UProbe = bpf
            .program_mut(name)
            .ok_or_else(|| anyhow::anyhow!("program {} not found", name))?
            .try_into()?;
        prog.load()?;
        let func = name.trim_start_matches("uprobe_").trim_end_matches("_entry").trim_end_matches("_return");
        // 映射到 libc 符号
        let sym = match name {
            "uprobe_malloc_entry" | "uprobe_malloc_return" => "malloc",
            "uprobe_free" => "free",
            "uprobe_calloc_entry" | "uprobe_calloc_return" => "calloc",
            "uprobe_realloc_entry" | "uprobe_realloc_return" => "realloc",
            _ => anyhow::bail!("unknown program: {}", name),
        };
        let ret = name.ends_with("_return");
        prog.attach(
            Some(sym),
            0,
            libc_path,
            pid.map(|p| p as i32),
        )?;
        println!("[+] attached {} ({})", name, if ret { "ret" } else { "entry" });
    }

    Ok(())
}

fn ctrlc_handler<F: Fn() + Send + Sync + 'static>(f: F) {
    use std::sync::Once;
    static INIT: Once = Once::new();
    static mut HANDLER: Option<Box<dyn Fn() + Send + Sync>> = None;

    INIT.call_once(|| {
        // 注册 SIGINT handler
        extern "C" fn sigint_handler(_sig: i32) {
            unsafe {
                if let Some(h) = HANDLER.as_ref() {
                    h();
                }
            }
        }
        unsafe {
            libc::signal(libc::SIGINT, sigint_handler as usize);
        }
    });

    // 设置实际的 handler 函数
    unsafe {
        HANDLER = Some(Box::new(f));
    }
}
