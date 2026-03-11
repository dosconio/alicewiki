use std::collections::HashMap;
use std::env;
use std::fs::File;
use std::io::{self, BufRead};
use std::path::Path;
use std::process::Command;

#[derive(Debug, Clone)]
struct Event {
    cpu: u32,
    level: String,
    addr: u64,
    raw_line: String,
}

fn main() {
    // 1. 获取命令行参数
    let args: Vec<String> = env::args().collect();
    if args.len() < 2 {
        eprintln!("❌ 用法: {} <二进制程序路径>", args[0]);
        eprintln!("💡 示例: {} ./sharing_bench", args[0]);
        return;
    }

    // 解析并获取绝对路径，推导工作目录
    let binary_path_str = &args[1];
    let binary_path = Path::new(binary_path_str)
        .canonicalize()
        .expect("❌ 找不到指定的二进制文件，请检查路径！");
    let work_dir = binary_path.parent().unwrap_or(Path::new("."));
    let perf_txt_path = work_dir.join("perf.txt");

    println!("🚀 目标程序: {}", binary_path.display());
    println!("📂 工作目录: {}", work_dir.display());

    // 2. 自动化执行 perf 采集流程
    println!("\n▶️  [1/4] 正在执行 sudo perf c2c record... (请耐心等待程序运行结束)");
    let record_status = Command::new("sudo")
        .current_dir(work_dir)
        .arg("perf")
        .arg("c2c")
        .arg("record")
        .arg("--")
        .arg(binary_path.to_str().unwrap())
        .status()
        .expect("❌ 执行 perf c2c record 失败");

    if !record_status.success() {
        eprintln!("❌ perf record 被中断或执行失败。");
        return;
    }

    println!("▶️  [2/4] 正在执行 sudo perf script 导出数据...");
    // 由于涉及重定向 ">"，最简单的方式是借助 sh -c 运行
    let script_cmd = format!(
        "sudo perf script -F ip,addr,sym,dso,cpu,event,data_src > {}",
        perf_txt_path.display()
    );
    Command::new("sh")
        .current_dir(work_dir)
        .arg("-c")
        .arg(&script_cmd)
        .status()
        .expect("❌ 执行 perf script 失败");

    println!("▶️  [3/4] 修改 perf.txt 权限...");
    Command::new("sudo")
        .arg("chmod")
        .arg("777")
        .arg(perf_txt_path.to_str().unwrap())
        .status()
        .expect("❌ chmod 失败");

    // 3. 动态调用 nm 获取符号表
    println!("▶️  [4/4] 正在调用 nm 分析 ELF 符号表...");
    let (symbols, end_addr) = match get_symbols_and_end(binary_path.to_str().unwrap()) {
        Some(res) => {
            println!("✅ 成功加载 {} 个符号，动态边界 [_end]: 0x{:x}", res.0.len(), res.1);
            res
        }
        None => {
            eprintln!("❌ 无法从获取符号表，请检查 nm 命令是否可用！");
            return;
        }
    };

    // 4. 读取并分析 perf.txt
    let mut events: Vec<Event> = Vec::new();
    println!("\n📥 开始读取并解析日志数据...");

    // 动态提取二进制文件名（例如 "sharing_bench"），用于过滤噪音
    let binary_name = binary_path.file_name().unwrap().to_str().unwrap();

    if let Ok(lines) = read_lines(&perf_txt_path) {
        for line_result in lines {
            let line = line_result.expect("读取行失败");

            if !line.contains(binary_name) {
                continue;
            }

            if let Some(event) = parse_line(&line) {
                if event.addr > 0x10000 && event.addr <= end_addr {
                    events.push(event);
                }
            }
        }
    } else {
        eprintln!("❌ 无法读取文件 {}", perf_txt_path.display());
        return;
    }

    println!(
        "🔍 成功加载 {} 条精确访问记录，开始统计分析...\n",
        events.len()
    );

    let mut pair_counts: HashMap<(u64, u64), usize> = HashMap::new();

    for i in 0..events.len() {
        let current_event = &events[i];

        for j in (0..i).rev() {
            let prev_event = &events[j];

            if prev_event.cpu != current_event.cpu {
                let distance = current_event.addr.abs_diff(prev_event.addr);

                // 【已修复】：只限制距离 < 64，允许距离为 0 (真共享)
                if distance < 64 {
                    let min_addr = std::cmp::min(current_event.addr, prev_event.addr);
                    let max_addr = std::cmp::max(current_event.addr, prev_event.addr);

                    *pair_counts.entry((min_addr, max_addr)).or_insert(0) += 1;
                    break;
                }
            }
        }
    }

    let mut sorted_results: Vec<_> = pair_counts.into_iter().collect();
    sorted_results.sort_by(|a, b| b.1.cmp(&a.1));

    println!("==================================================================");
    println!("🚨 静态数据区 真/伪共享 最终诊断报告 (Max Addr <= 0x{:x})", end_addr);
    println!("==================================================================");

    if sorted_results.is_empty() {
        println!("✅ 完美！未检测到任何跨核 Cacheline 争用现象。");
    } else {
        for ((addr1, addr2), count) in sorted_results {
            let distance = addr2 - addr1;
            let sym1 = resolve_symbol(addr1, &symbols);
            let sym2 = resolve_symbol(addr2, &symbols);

            // 【动态判定】：距离为 0 是真共享，大于 0 是伪共享
            let sharing_type = if distance == 0 {
                "🔴 真共享 (True Sharing)"
            } else {
                "💥 伪共享 (False Sharing)"
            };

            println!(
                "{} [争用变量]: {} 与 {} | 间距: {:02} 字节 | 跨核 Ping-Pong: {} 次",
                sharing_type, sym1, sym2, distance, count
            );
        }
    }
    println!("==================================================================");
}

// 地址映射转换
fn resolve_symbol(target_addr: u64, symbols: &[(u64, String)]) -> String {
    if symbols.is_empty() { return format!("0x{:x}", target_addr); }
    let idx = symbols.partition_point(|&(addr, _)| addr <= target_addr);
    if idx == 0 { return format!("0x{:x}", target_addr); }
    
    let (base_addr, sym_name) = &symbols[idx - 1];
    let offset = target_addr - base_addr;

    if offset == 0 { sym_name.clone() } else { format!("{}+0x{:x}", sym_name, offset) }
}

// 解析全量符号表
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
    let lvl_start = line.find("|LVL ")? + 5;
    let lvl_end = line[lvl_start..].find('|')? + lvl_start;
    let level = line[lvl_start..lvl_end].trim().to_string();
    let after_p = line.split("P:").nth(1)?;
    let before_pipe = after_p.split('|').next()?;
    let addr_str = before_pipe.split_whitespace().next()?;
    let addr = u64::from_str_radix(addr_str, 16).unwrap_or(0);
    Some(Event { cpu, level, addr, raw_line: line.to_string() })
}

fn read_lines<P>(filename: P) -> io::Result<io::Lines<io::BufReader<File>>> where P: AsRef<Path> {
    let file = File::open(filename)?;
    Ok(io::BufReader::new(file).lines())
}