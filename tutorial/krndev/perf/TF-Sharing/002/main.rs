use std::collections::{HashMap, HashSet}; // 【新增】：引入 HashSet 用于去重收集 RIP
use std::env;
use std::fs::File;
use std::io::{self, BufRead, Write};
use std::path::Path;
use std::process::Command;

#[derive(Debug, Clone)]
struct Event {
    cpu: u32,
    addr: u64,
    rip: String,
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
    
    let perf_txt_path = work_dir.join("perf.txt");
    let hitm_txt_path = work_dir.join("hitm.txt");

    println!("🚀 目标程序: {}", binary_path.display());
    println!("📂 工作目录: {}", work_dir.display());

    println!("\n▶️  [1/4] 正在执行 sudo perf c2c record... (请耐心等待)");
    let mut perf_cmd = Command::new("sudo");
    perf_cmd.current_dir(work_dir)
        .arg("perf").arg("c2c").arg("record")//.arg("-g")
        .arg("--")
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
                if event.addr > 0x10000 && event.addr < 0x7fffffffffff {
                    events.push(event);
                }
            }
        }
    } else {
        eprintln!("❌ 无法读取文件 {}", perf_txt_path.display());
        return;
    }

    // 【核心升级】：Value 由单纯的 usize 次数，升级为 (次数, RIP集合)
    let mut pair_counts: HashMap<(u64, u64), (usize, HashSet<String>)> = HashMap::new();

    for i in 0..events.len() {
        let current_event = &events[i];
        for j in (0..i).rev() {
            let prev_event = &events[j];
            if prev_event.cpu != current_event.cpu {
                let distance = current_event.addr.abs_diff(prev_event.addr);
                if distance < 64 {
                    let min_addr = std::cmp::min(current_event.addr, prev_event.addr);
                    let max_addr = std::cmp::max(current_event.addr, prev_event.addr);
                    
                    let entry = pair_counts.entry((min_addr, max_addr)).or_insert((0, HashSet::new()));
                    entry.0 += 1; // 增加次数
                    entry.1.insert(current_event.rip.clone()); // 记录受害者的 RIP
                    entry.1.insert(prev_event.rip.clone());    // 记录肇事者的 RIP
                    
                    break;
                }
            }
        }
    }

    // 排序：按发生次数 (b.1.0) 从高到低
    let mut sorted_results: Vec<_> = pair_counts.into_iter().collect();
    sorted_results.sort_by(|a, b| b.1.0.cmp(&a.1.0));

    let mut output_lines = Vec::new();
    for ((addr1, addr2), (count, rips)) in sorted_results {
        let distance = addr2 - addr1;
        let tf = if distance == 0 { 'T' } else { 'F' };
        let (region1, str1) = categorize_addr(addr1, end_addr, &symbols);
        
        let final_addr_str = if distance == 0 {
            str1 
        } else {
            let (_, str2) = categorize_addr(addr2, end_addr, &symbols);
            format!("{}<->{}", str1, str2) 
        };

        // 基础格式：[T/F],[A/S/B],[Address],[Count]
        let mut line = format!("{},{},{},{}", tf, region1, final_addr_str, count);

        // 【新增需求】：如果 region 是 'A' (Heap)，则在末尾附加上 RIP
        if region1 == 'A' {
            // 将 HashSet 转化为 Vec 并排序，保证输出顺序稳定美观
            let mut rip_vec: Vec<String> = rips.into_iter().collect();
            rip_vec.sort();
            
            // 拼接所有的 RIP，并统一加上 "0x" 前缀
            let rip_str = rip_vec.iter().map(|r| format!("0x{}", r)).collect::<Vec<_>>().join(",");
            line = format!("{}: {}", line, rip_str);
        }

        output_lines.push(line);
    }

    let mut out_file = File::create(&hitm_txt_path).expect("❌ 无法创建 hitm.txt");
    for line in &output_lines {
        writeln!(out_file, "{}", line).unwrap();
    }

    println!("\n✅ 分析完成！全量诊断数据已导出至: {}", hitm_txt_path.display());
    println!("--------------------------------------------------");
    println!("📄 hitm.txt 头部预览 (Top 10):");
    println!("格式: [T/F],[A/S/B],[地址/符号],[次数][: 触发指令RIP (仅Heap)]");
    println!("--------------------------------------------------");
    
    for line in output_lines.iter().take(10) {
        println!("{}", line);
    }
    println!("--------------------------------------------------");
}

fn categorize_addr(addr: u64, end_addr: u64, symbols: &[(u64, String)]) -> (char, String) {
    if addr <= end_addr {
        ('B', resolve_symbol(addr, symbols))
    } else if addr > 0x700000000000 {
        ('S', format!("0x{:x}", addr))
    } else {
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

    // 【新增】：提取 RIP
    // 在 perf script 的典型输出中，`|BLK  N/A` 之后的部分就是 RIP 和 符号名
    // 通过 rsplit 截取最后一个管道符之后的内容，第三个分词通常就是精确的 16 进制 RIP
    let after_last_pipe = line.rsplit('|').next()?;
    let tail_parts: Vec<&str> = after_last_pipe.split_whitespace().collect();
    let rip = if tail_parts.len() > 2 {
        tail_parts[2].to_string()
    } else {
        "unknown".to_string()
    };
    
    Some(Event { cpu, addr, rip })
}

fn read_lines<P>(filename: P) -> io::Result<io::Lines<io::BufReader<File>>> where P: AsRef<Path> {
    let file = File::open(filename)?;
    Ok(io::BufReader::new(file).lines())
}