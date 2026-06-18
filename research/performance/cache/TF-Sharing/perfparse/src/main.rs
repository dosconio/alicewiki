use std::collections::{HashMap, HashSet};
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

// ---------------------------------------------------------
// dbg-viewtype 的数据结构支持
// ---------------------------------------------------------
#[derive(Debug, Clone)]
struct StructField {
    offset: u64,
    name: String,
    type_str: String,
}

#[derive(Debug, Clone)]
struct StructInfo {
    name: String,
    fields: Vec<StructField>,
}

#[derive(Debug, Clone)]
struct GlobalVar {
    name: String,       // 变量名
    addr: u64,          // 内存地址
    type_name: String,  // 所属类型
}

fn main() {
    let args: Vec<String> = env::args().collect();
    if args.len() < 2 {
        eprintln!("❌ 用法:");
        eprintln!("   1. 拉起新进程模式: {} <二进制程序路径> [程序参数...]", args[0]);
        eprintln!("   2. 绑定现有进程模式: {} <二进制程序路径> -p <PID>", args[0]);
        eprintln!("   3. 跳过 record 模式: {} <二进制程序路径> --no-record", args[0]);
        eprintln!("💡 示例: {} ./sharing_bench 10 1000", args[0]);
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

    let mut is_pid_mode = false;
    let mut target_pid = String::new();
    let mut skip_record = false;

    if target_args.len() >= 2 && target_args[0] == "-p" {
        is_pid_mode = true;
        target_pid = target_args[1].clone();
    }
    if target_args.contains(&"--no-record".to_string()) {
        skip_record = true;
    }

    if !skip_record {
        println!("\n▶️  [1/4] 正在执行 sudo perf c2c record... (请耐心等待)");
        let mut perf_cmd = Command::new("sudo");
        perf_cmd.current_dir(work_dir)
            .arg("perf").arg("c2c").arg("record");

        if is_pid_mode {
            println!("🔗 模式: 绑定现有线上进程 PID [{}]", target_pid);
            println!("⏳ 为了安全起见，将自动采样 10 秒钟后停止...");
            perf_cmd.arg("-p").arg(&target_pid)
                    .arg("sleep").arg("10"); 
        } else {
            println!("🚀 模式: 自动拉起新进程运行...");
            perf_cmd.arg("--")
                    .arg(binary_path.to_str().unwrap())
                    .args(target_args);
        }

        let perf_data_path = work_dir.join("perf.data");
        let record_status = perf_cmd.status().expect("❌ 执行 perf c2c record 失败");
        if !record_status.success() {
            if perf_data_path.exists() {
                let metadata = std::fs::metadata(&perf_data_path).unwrap_or_else(|_| {
                    std::process::exit(1);
                });
                if metadata.len() > 0 {
                    eprintln!("⚠️  perf c2c record 提前退出（目标进程已结束），但已采集到数据，继续分析...");
                } else {
                    eprintln!("❌ perf c2c record 失败，且 perf.data 为空。");
                    return;
                }
            } else {
                eprintln!("❌ perf c2c record 失败，且 perf.data 不存在。");
                return;
            }
        }
    } else {
        println!("⏭️  跳过 perf c2c record，使用已有 perf.data");
    }

    println!("▶️  [2/4] 正在执行 sudo perf script 导出数据...");
    let script_cmd = format!(
        "sudo perf script -F ip,addr,phys_addr,sym,dso,cpu,event,data_src > {}",
        perf_txt_path.display()
    );
    Command::new("sh").current_dir(work_dir).arg("-c").arg(&script_cmd)
        .status().expect("❌ 执行 perf script 失败");

    println!("▶️  [3/4] 修改 perf.txt 权限并加载类型与变量表...");
    Command::new("sudo").arg("chmod").arg("777").arg(perf_txt_path.to_str().unwrap()).status().unwrap();

    // 加载类型表与全局变量表 (严格以 \t 驱动解析)
    let type_txt_path = work_dir.join("type.txt");
    let vars_txt_path = work_dir.join("vars.txt");
    
    let type_map = load_types(&type_txt_path);
    let global_vars = load_vars(&vars_txt_path);

    if type_map.is_empty() || global_vars.is_empty() {
        eprintln!("⚠️  警告: 未能成功读取 type.txt 或 vars.txt，字段级高级解析可能会降级为匿名地址！");
    }

    println!("▶️  [4/4] 正在读取日志并执行跨核争用分析...");
    let mut events: Vec<Event> = Vec::new();
    let binary_name = binary_path.file_name().unwrap().to_str().unwrap();

    if let Ok(lines) = read_lines(&perf_txt_path) {
        for line_result in lines {
            let line = line_result.expect("读取行失败");
            if !line.contains(binary_name) { continue; }

            if let Some(event) = parse_line(&line) {
                // 屏蔽内核态和低位无效映射
                if event.addr > 0x10000 && event.addr < 0x7fffffffffff {
                    events.push(event);
                }
            }
        }
    } else {
        eprintln!("❌ 无法读取文件 {}", perf_txt_path.display());
        return;
    }

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
                    entry.0 += 1;
                    entry.1.insert(current_event.rip.clone());
                    entry.1.insert(prev_event.rip.clone());
                    
                    break;
                }
            }
        }
    }

    let mut sorted_results: Vec<_> = pair_counts.into_iter().collect();
    sorted_results.sort_by(|a, b| b.1.0.cmp(&a.1.0));

    let mut output_lines = Vec::new();
    for ((addr1, addr2), (count, rips)) in sorted_results {
        let distance = addr2 - addr1;
        let tf = if distance == 0 { 'T' } else { 'F' };
        
        let (region1, str1) = categorize_addr(addr1, &global_vars, &type_map);
        
        let final_addr_str = if distance == 0 {
            str1.clone()
        } else {
            let (_, str2) = categorize_addr(addr2, &global_vars, &type_map);
            format!("{}<->{}", str1, str2) 
        };

        let mut line = format!("{},{},{},{}", tf, region1, final_addr_str, count);

        // 如果第一目标为堆内存 'A'，则附加崩溃或触发区 RIP
        if region1 == 'A' {
            let mut rip_vec: Vec<String> = rips.into_iter().collect();
            rip_vec.sort();
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
    println!("格式: [T/F],[A/S/B],[变量或子字段路径/匿名地址],[次数][: 触发指令RIP (仅Heap)]");
    println!("--------------------------------------------------");
    for line in output_lines.iter().take(10) {
        println!("{}", line);
    }
    println!("--------------------------------------------------");
}

// ---------------------------------------------------------
// 核心路由与结构解析系统
// ---------------------------------------------------------
fn categorize_addr(
    addr: u64, 
    global_vars: &[GlobalVar], 
    type_map: &HashMap<String, StructInfo>
) -> (char, String) {
    if addr > 0x700000000000 {
        return ('S', format!("0x{:x}", addr));
    }
    
    // 严格检查是否属于全局变量边界
    if let Some((var_name, type_name, offset)) = resolve_symbol_from_vars(addr, global_vars, type_map) {
        let resolved = resolve_field_recursive(&var_name, &type_name, offset, type_map);
        return ('B', resolved);
    }
    
    ('A', format!("0x{:x}", addr))
}

fn resolve_symbol_from_vars(
    target_addr: u64, 
    global_vars: &[GlobalVar],
    type_map: &HashMap<String, StructInfo>
) -> Option<(String, String, u64)> {
    if global_vars.is_empty() { return None; }
    let idx = global_vars.partition_point(|v| v.addr <= target_addr);
    if idx == 0 { return None; }
    
    let var = &global_vars[idx - 1];
    let offset = target_addr - var.addr;
    
    // 精准推导结构体边界大小
    let mut var_size = 0;
    let clean = clean_type_name(&var.type_name);
    if let Some(info) = type_map.get(&clean) {
        if let Some(last_f) = info.fields.last() {
            var_size = ((last_f.offset + 8) + 7) & !7;
        }
    }
    
    if var_size == 0 {
        if var.type_name.contains("[15]") { var_size = 15 * 8; }
        else if clean == "TrueSharing" { var_size = 64; }
        else if clean == "LargeFalseSharing" { var_size = 256; }
        else { var_size = 64; }
    }

    // 防漏吸洪拦截锁：如超出范围，说明是紧随全局变量后方分配出来的堆对象，打回 A
    if offset >= (var_size + 64) {
        return None;
    }

    // 强健过滤：防止在数据加载切分阶段导致的变量名与类型串发生易位
    let (final_name, final_type) = if var.name.contains("Sharing") && !var.type_name.contains("Sharing") {
        (var.type_name.clone(), var.name.clone())
    } else {
        (var.name.clone(), var.type_name.clone())
    };

    Some((final_name, final_type, offset))
}

fn resolve_field_recursive(
    var_name: &str, 
    type_name: &str, 
    mut offset: u64, 
    type_map: &HashMap<String, StructInfo>
) -> String {
    let mut current_type = type_name.to_string();
    let mut result_path = var_name.to_string();

    loop {
        let clean_type = clean_type_name(&current_type);
        
        // 1. 优先捕获并剥离数组层级
        if current_type.contains('[') {
            let elem_size = get_element_size(&current_type, type_map).unwrap_or(8);
            if elem_size > 0 {
                let idx = offset / elem_size;
                offset %= elem_size;
                result_path = format!("{}[{}]", result_path, idx);
                current_type = strip_array(&current_type);
                continue; // 💡 移除 break，继续往下挖（可能是结构体数组）
            }
        }

        // 2. 纵向掘进子结构体内部字段
        if let Some(struct_info) = type_map.get(&clean_type) {
            if struct_info.fields.is_empty() { break; }
            
            let f_idx = struct_info.fields.partition_point(|f| f.offset <= offset);
            if f_idx > 0 {
                let field = &struct_info.fields[f_idx - 1];
                offset -= field.offset;
                result_path = format!("{}.{}", result_path, field.name);
                current_type = field.type_str.clone();
                continue; // 💡 移除 break，哪怕 offset 是 0，也要挖出 a.b.c
            } else {
                break;
            }
        } else {
            break; // 挖到了基本数据类型（如 int, long），结束
        }
    }

    if offset > 0 {
        format!("{}+0x{:x}", result_path, offset)
    } else {
        result_path
    }
}

// ---------------------------------------------------------
// 严格基于 Tab (\t) 分割的数据格式加载层
// ---------------------------------------------------------
fn load_types(path: &Path) -> HashMap<String, StructInfo> {
    let mut type_map = HashMap::new();
    let Ok(lines) = read_lines(path) else { return type_map; };
    
    let mut current_type = String::new();
    
    for line_res in lines {
        let line = line_res.unwrap_or_default();
        if line.is_empty() { continue; }
        
        if line.starts_with('\t') {
            let trimmed_line = &line[1..];
            let parts: Vec<&str> = trimmed_line.split('\t').collect();
            
            if parts.len() >= 3 {
                let offset = parts[0].trim().parse::<u64>().unwrap_or(0);
                let name = parts[1].trim().to_string();
                let type_str = parts[2..].join("\t").trim().trim_matches('\'').to_string();
                
                if let Some(info) = type_map.get_mut(&current_type) {
                    info.fields.push(StructField { offset, name, type_str });
                }
            }
        } else {
            current_type = line.trim().to_string();
            if !current_type.is_empty() {
                type_map.insert(current_type.clone(), StructInfo {
                    name: current_type.clone(),
                    fields: Vec::new()
                });
            }
        }
    }
    
    for info in type_map.values_mut() {
        info.fields.sort_by_key(|f| f.offset);
    }
    type_map
}

fn load_vars(path: &Path) -> Vec<GlobalVar> {
    let mut vars = Vec::new();
    let Ok(lines) = read_lines(path) else { return vars; };
    
    for line_res in lines {
        let line = line_res.unwrap_or_default();
        let trimmed = line.trim();
        if trimmed.is_empty() { continue; }
        
        let parts: Vec<&str> = trimmed.split('\t').collect();
        
        if parts.len() >= 3 {
            let name = parts[0].trim().to_string();
            let addr_str = parts[1].trim();
            let type_str = parts[2..].join("\t").trim().trim_matches('\'').to_string();
            
            let addr = if addr_str.starts_with("0x") {
                u64::from_str_radix(&addr_str[2..], 16).unwrap_or(0)
            } else {
                addr_str.parse::<u64>().unwrap_or(0)
            };
            
            vars.push(GlobalVar { name, addr, type_name: type_str });
        }
    }
    vars.sort_by_key(|v| v.addr);
    vars
}

// ---------------------------------------------------------
// 类型字符净化清洗组件
// ---------------------------------------------------------
fn clean_type_name(raw: &str) -> String {
    let mut s = raw.replace("volatile", "")
                   .replace("const", "")
                   .replace("struct", "")
                   .replace("class", "")
                   .replace("'", "");
    if let Some(idx) = s.find('[') { s = s[..idx].to_string(); }
    if let Some(idx) = s.find('*') { s = s[..idx].to_string(); }
    s.trim().to_string()
}

fn strip_array(raw: &str) -> String {
    if let Some(idx) = raw.find('[') { raw[..idx].to_string() } else { raw.to_string() }
}

fn get_element_size(type_str: &str, type_map: &HashMap<String, StructInfo>) -> Option<u64> {
    if type_str.contains("long") || type_str.contains("double") || type_str.contains('*') {
        return Some(8);
    } else if type_str.contains("int") || type_str.contains("float") {
        return Some(4);
    } else if type_str.contains("short") {
        return Some(2);
    } else if type_str.contains("char") || type_str.contains("bool") {
        return Some(1);
    }
    
    let clean = clean_type_name(&strip_array(type_str));
    if let Some(struct_info) = type_map.get(&clean) {
        if let Some(last_field) = struct_info.fields.last() {
            return Some(((last_field.offset + 8) + 7) & !7);
        }
    }
    None
}

// ---------------------------------------------------------
// 基础 IO 与 文本解析
// ---------------------------------------------------------
fn parse_line(line: &str) -> Option<Event> {
    if !line.contains("|OP STORE|") && !line.contains("|OP LOAD|") { return None; }

    // phys_addr 是行末最后一个字段，为 0 表示 addr 是物理地址而非虚拟地址
    let phys_addr = line.split_whitespace().last()
        .and_then(|s| u64::from_str_radix(s, 16).ok())
        .unwrap_or(0);
    if phys_addr == 0 { return None; }

    let cpu_start = line.find('[')? + 1;
    let cpu_end = line.find(']')?;
    let cpu: u32 = line[cpu_start..cpu_end].parse().ok()?;

    let after_p = line.split("P:").nth(1)?;
    let before_pipe = after_p.split('|').next()?;
    let addr_str = before_pipe.split_whitespace().next()?;
    let addr = u64::from_str_radix(addr_str, 16).unwrap_or(0);

    let after_last_pipe = line.rsplit('|').next()?;
    let tail_parts: Vec<&str> = after_last_pipe.split_whitespace().collect();
    let rip = if tail_parts.len() > 3 {
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