use std::collections::HashMap;
use std::env;
use std::fs::File;
use std::io::{self, BufRead, Write};
use std::path::Path;
use std::process::Command;

#[derive(Debug, Clone)]
struct Event {
    cpu: u32,
    addr: u64,
}

fn main() {
    let args: Vec<String> = env::args().collect();
    if args.len() < 2 {
        eprintln!("❌ 用法: {} <二进制程序路径> [程序参数...]", args[0]);
        return;
    }

    let binary_path_str = &args[1];
    let target_args = &args[2..];

    let binary_path = Path::new(binary_path_str)
        .canonicalize()
        .expect("❌ 找不到指定的二进制文件，请检查路径！");
    let work_dir = binary_path.parent().unwrap_or(Path::new("."));
    
    // 输入输出文件路径
    let perf_txt_path = work_dir.join("perf.txt");
    let hitm_txt_path = work_dir.join("hitm.txt");

    println!("🚀 目标程序: {}", binary_path.display());
    println!("📂 工作目录: {}", work_dir.display());

    println!("\n▶️  [1/4] 正在执行 sudo perf c2c record... (请耐心等待)");
    let mut perf_cmd = Command::new("sudo");
    perf_cmd.current_dir(work_dir)
        .arg("perf").arg("c2c").arg("record").arg("--")
        .arg(binary_path.to_str().unwrap())
        .args(target_args);

    let record_status = perf_cmd.status().expect("❌ 执行 perf c2c record 失败");
    if !record_status.success() {
        eprintln!("❌ perf record 被中断或执行失败。");
        return;
    }

    println!("▶️  [2/4] 正在执行 sudo perf script 导出数据...");
    let script_cmd = format!(
        "sudo perf script -F ip,addr,sym,dso,cpu,event,data_src > {}",
        perf_txt_path.display()
    );
    Command::new("sh").current_dir(work_dir).arg("-c").arg(&script_cmd)
        .status().expect("❌ 执行 perf script 失败");

    println!("▶️  [3/4] 修改 perf.txt 权限并提取符号表...");
    Command::new("sudo").arg("chmod").arg("777").arg(perf_txt_path.to_str().unwrap()).status().unwrap();

    let (symbols, end_addr) = match get_symbols_and_end(binary_path.to_str().unwrap()) {
        Some(res) => res,
        None => {
            eprintln!("❌ 无法获取符号表，请检查 nm 命令！");
            return;
        }
    };

    println!("▶️  [4/4] 正在读取日志并执行跨核争用分析...");
    let mut events: Vec<Event> = Vec::new();
    let binary_name = binary_path.file_name().unwrap().to_str().unwrap();

    if let Ok(lines) = read_lines(&perf_txt_path) {
        for line_result in lines {
            let line = line_result.expect("读取行失败");
            if !line.contains(binary_name) { continue; }

            if let Some(event) = parse_line(&line) {
                // 【核心噪音过滤】：屏蔽掉大于 0x7fffffffffff 的内核态和特殊映射地址
                if event.addr > 0x10000 && event.addr < 0x7fffffffffff {
                    events.push(event);
                }
            }
        }
    } else {
        eprintln!("❌ 无法读取文件 {}", perf_txt_path.display());
        return;
    }

    let mut pair_counts: HashMap<(u64, u64), usize> = HashMap::new();

    for i in 0..events.len() {
        let current_event = &events[i];
        for j in (0..i).rev() {
            let prev_event = &events[j];
            if prev_event.cpu != current_event.cpu {
                let distance = current_event.addr.abs_diff(prev_event.addr);
                if distance < 64 {
                    let min_addr = std::cmp::min(current_event.addr, prev_event.addr);
                    let max_addr = std::cmp::max(current_event.addr, prev_event.addr);
                    *pair_counts.entry((min_addr, max_addr)).or_insert(0) += 1;
                    break;
                }
            }
        }
    }

    // 排序：次数从高到低
    let mut sorted_results: Vec<_> = pair_counts.into_iter().collect();
    sorted_results.sort_by(|a, b| b.1.cmp(&a.1));

    // 构造结构化输出行
    let mut output_lines = Vec::new();
    for ((addr1, addr2), count) in sorted_results {
        let distance = addr2 - addr1;
        
        // 1. 判定 T/F
        let tf = if distance == 0 { 'T' } else { 'F' };
        
        // 2. 判定位置和格式化地址
        let (region1, str1) = categorize_addr(addr1, end_addr, &symbols);
        
        let final_addr_str = if distance == 0 {
            str1 // 真共享只打印一个地址即可
        } else {
            let (_, str2) = categorize_addr(addr2, end_addr, &symbols);
            format!("{}<->{}", str1, str2) // 伪共享打印争用的两个地址
        };

        // 3. 拼接单行 CSV 格式：[T/F],[A/S/B],[Address],[Count]
        let line = format!("{},{},{},{}", tf, region1, final_addr_str, count);
        output_lines.push(line);
    }

    // 写入 hitm.txt
    let mut out_file = File::create(&hitm_txt_path).expect("❌ 无法创建 hitm.txt");
    for line in &output_lines {
        writeln!(out_file, "{}", line).unwrap();
    }

    println!("\n✅ 分析完成！全量诊断数据已导出至: {}", hitm_txt_path.display());
    println!("--------------------------------------------------");
    println!("📄 hitm.txt 头部预览 (Top 10):");
    println!("格式: [T/F Sharing],[位置 A(Heap)/S(Stack)/B(Static)],[十六进制地址/符号],[次数]");
    println!("--------------------------------------------------");
    
    // 输出前十行到控制台
    for line in output_lines.iter().take(10) {
        println!("{}", line);
    }
    println!("--------------------------------------------------");
}

// 【新增】：根据内存布局划分区域，并提取符号
fn categorize_addr(addr: u64, end_addr: u64, symbols: &[(u64, String)]) -> (char, String) {
    if addr <= end_addr {
        // B: Static (BSS/Data)
        ('B', resolve_symbol(addr, symbols))
    } else if addr > 0x700000000000 {
        // S: Stack / MMAP
        ('S', format!("0x{:x}", addr))
    } else {
        // A: Heap (Allocated)
        ('A', format!("0x{:x}", addr))
    }
}

fn resolve_symbol(target_addr: u64, symbols: &[(u64, String)]) -> String {
    if symbols.is_empty() { return format!("0x{:x}", target_addr); }
    let idx = symbols.partition_point(|&(addr, _)| addr <= target_addr);
    if idx == 0 { return format!("0x{:x}", target_addr); }
    
    let (base_addr, sym_name) = &symbols[idx - 1];
    let offset = target_addr - base_addr;

    if offset == 0 { sym_name.clone() } else { format!("{}+0x{:x}", sym_name, offset) }
}

fn get_symbols_and_end(binary_path: &str) -> Option<(Vec<(u64, String)>, u64)> {
    let output = Command::new("nm").arg("-n").arg(binary_path).output().ok()?;
    let stdout = String::from_utf8_lossy(&output.stdout);
    let mut symbols = Vec::new();
    let mut end_addr = None;

    for line in stdout.lines() {
        let parts: Vec<&str> = line.split_whitespace().collect();
        if parts.len() >= 3 {
            if let Ok(addr) = u64::from_str_radix(parts[0], 16) {
                let sym_name = parts[2..].join(" ");
                symbols.push((addr, sym_name.clone()));
                if sym_name == "_end" { end_addr = Some(addr); }
            }
        }
    }
    end_addr.map(|ea| (symbols, ea))
}

fn parse_line(line: &str) -> Option<Event> {
    if !line.contains("|OP STORE|") && !line.contains("|OP LOAD|") { return None; }
    let cpu_start = line.find('[')? + 1;
    let cpu_end = line.find(']')?;
    let cpu: u32 = line[cpu_start..cpu_end].parse().ok()?;
    
    let after_p = line.split("P:").nth(1)?;
    let before_pipe = after_p.split('|').next()?;
    let addr_str = before_pipe.split_whitespace().next()?;
    let addr = u64::from_str_radix(addr_str, 16).unwrap_or(0);
    
    // 移除了用不到的 raw_line 和 level 字段，极大降低内存占用
    Some(Event { cpu, addr })
}

fn read_lines<P>(filename: P) -> io::Result<io::Lines<io::BufReader<File>>> where P: AsRef<Path> {
    let file = File::open(filename)?;
    Ok(io::BufReader::new(file).lines())
}
