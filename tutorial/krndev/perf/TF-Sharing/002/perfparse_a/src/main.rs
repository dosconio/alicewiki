// aa a:heap a:false-sharing
use regex::Regex;
use std::collections::HashMap;
use std::env;
use std::fs::File;
use std::io::{self, BufRead, BufReader, BufWriter, Write};
use std::path::Path;
use std::process::Command;

/// 缓存上下文：包含正则表达式、IP解析缓存、源码文件缓存、以及聚合统计
struct AppContext {
    binary: String,
    ip_cache: HashMap<String, String>,
    file_cache: HashMap<String, Option<Vec<String>>>,
    re_keywords: Regex,
    re_spaces: Regex,
    // 新增：专门用于统计 F,A 中各代码行的出现次数
    fa_summary: HashMap<String, usize>, 
}

impl AppContext {
    fn new(binary: String) -> Self {
        Self {
            binary,
            ip_cache: HashMap::new(),
            file_cache: HashMap::new(),
            re_keywords: Regex::new(r"\b(int|size_t|unsigned|char|short|long|float|double|auto)\b\s*").unwrap(),
            re_spaces: Regex::new(r"\s*([<>=+\-*/&|!(){}\[\];:?])\s*").unwrap(),
            fa_summary: HashMap::new(),
        }
    }
}

/// 获取对应文件的指定行
fn get_source_line(path: &str, line_num: usize, cache: &mut HashMap<String, Option<Vec<String>>>) -> Option<String> {
    let lines_opt = cache.entry(path.to_string()).or_insert_with(|| {
        std::fs::read_to_string(path)
            .ok()
            .map(|s| s.lines().map(String::from).collect())
    });

    if let Some(lines) = lines_opt {
        lines.get(line_num.saturating_sub(1)).cloned()
    } else {
        None
    }
}

/// 将代码行进行深度清洗
fn clean_source_code(code: &str, ctx: &AppContext) -> String {
    let mut s = code.trim().to_string();
    s = s.replace(',', " ");
    s = ctx.re_keywords.replace_all(&s, "").to_string();
    s = ctx.re_spaces.replace_all(&s, "$1").to_string();
    s.trim().to_string()
}

/// 调用 addr2line 将 IP 转换为 "文件名!行号 [代码片段]" 格式
fn resolve_ip(ip: &str, ctx: &mut AppContext) -> String {
    if let Some(resolved) = ctx.ip_cache.get(ip) {
        return resolved.clone();
    }

    let output = Command::new("addr2line")
        .arg("-e")
        .arg(&ctx.binary)
        .arg(ip)
        .output();

    let resolved = match output {
        Ok(output) if output.status.success() => {
            let stdout = String::from_utf8_lossy(&output.stdout);
            let stdout = stdout.trim();

            if stdout == "??:0" || stdout == "??:?" {
                format!("{}!?", ip)
            } else if let Some((path_str, line_str)) = stdout.rsplit_once(':') {
                let clean_line_str = line_str.split_whitespace().next().unwrap_or(line_str);
                
                let file_name = Path::new(path_str)
                    .file_name()
                    .map(|os_str| os_str.to_string_lossy().into_owned())
                    .unwrap_or_else(|| path_str.to_string());

                let mut final_str = format!("{}!{}", file_name, clean_line_str);

                if let Ok(line_num) = clean_line_str.parse::<usize>() {
                    if let Some(raw_code) = get_source_line(path_str, line_num, &mut ctx.file_cache) {
                        let cleaned_code = clean_source_code(&raw_code, ctx);
                        if !cleaned_code.is_empty() {
                            final_str.push_str(&format!(" [{}]", cleaned_code));
                        }
                    }
                }
                final_str
            } else {
                stdout.to_string()
            }
        }
        _ => format!("{}!err", ip),
    };

    ctx.ip_cache.insert(ip.to_string(), resolved.clone());
    resolved
}

/// 处理单行文本，并更新统计数据
fn process_line(line: &str, ctx: &mut AppContext) -> String {
    if line.starts_with("T,A") || line.starts_with("F,A") {
        if let Some((prefix, ips_part)) = line.split_once(':') {
            let ips: Vec<&str> = ips_part.split(',').map(|s| s.trim()).collect();
            let mut resolved_ips = Vec::new();

            for ip in ips {
                if ip.is_empty() { continue; }
                let resolved = resolve_ip(ip, ctx);

                if !resolved.ends_with("!?") && !resolved.ends_with("!0") && !resolved.ends_with("!err") {
                    resolved_ips.push(resolved);
                }
            }

            resolved_ips.sort();
            resolved_ips.dedup();

            // 新增：如果当前行是 F,A，将其解析出的所有源码片段计入统计
            if line.starts_with("F,A") {
                for rip in &resolved_ips {
                    *ctx.fa_summary.entry(rip.clone()).or_insert(0) += 1;
                }
            }

            if resolved_ips.is_empty() {
                return prefix.to_string();
            } else {
                return format!("{}: {}", prefix, resolved_ips.join(", "));
            }
        }
    }
    line.to_string()
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args: Vec<String> = env::args().collect();
    if args.len() < 2 {
        eprintln!("用法: {} <带调试信息的二进制文件路径>", args[0]);
        std::process::exit(1);
    }

    let mut ctx = AppContext::new(args[1].clone());

    // 1. 处理 hitm.txt -> hitm_a.txt
    let input_file = File::open("hitm.txt").map_err(|e| format!("无法打开 hitm.txt: {}", e))?;
    let reader = BufReader::new(input_file);

    let output_file_a = File::create("hitm_a.txt").map_err(|e| format!("无法创建 hitm_a.txt: {}", e))?;
    let mut writer_a = BufWriter::new(output_file_a);

    for line in reader.lines() {
        let line = line?;
        let processed_line = process_line(&line, &mut ctx);
        writeln!(writer_a, "{}", processed_line)?;
    }

// 2. 汇总 F,A 统计结果 -> hitm_aa.txt
    let output_file_aa = File::create("hitm_aa.txt").map_err(|e| format!("无法创建 hitm_aa.txt: {}", e))?;
    let mut writer_aa = BufWriter::new(output_file_aa);

    // 预编译正则：解析 "文件名!行号 [代码片段]" 格式，以及匹配控制流关键字
    let re_parse = Regex::new(r"^(.*?)!(\d+)(?:\s+\[(.*)\])?$").unwrap();
    // \b 确保匹配的是完整单词，例如不会误伤名字叫 shift 的变量
    let re_ctrl = Regex::new(r"\b(if|for|while)\b").unwrap();

    // 辅助结构体，用于存放解析后的信息
    struct StatRecord {
        raw: String,
        file: String,
        line: usize,
        code: String,
        count: usize,
        remove: bool,
    }

    // 步骤 A: 将 HashMap 的键拆解，提取出文件和行号
    let mut stats: Vec<StatRecord> = ctx.fa_summary.into_iter().map(|(raw, count)| {
        if let Some(caps) = re_parse.captures(&raw) {
            let file = caps.get(1).map_or("", |m| m.as_str()).to_string();
            let line = caps.get(2).map_or(0, |m| m.as_str().parse().unwrap_or(0));
            let code = caps.get(3).map_or("", |m| m.as_str()).to_string();
            StatRecord { raw, file, line, code, count, remove: false }
        } else {
            // 如果解析失败（例如没有行号信息的异常行），原样保留
            StatRecord { raw: raw.clone(), file: String::new(), line: 0, code: String::new(), count, remove: false }
        }
    }).collect();

    // 步骤 B: 按照 文件名 和 行号 升序排序，这样同一个文件里相邻的代码行在数组中就会挨在一起
    stats.sort_by(|a, b| a.file.cmp(&b.file).then(a.line.cmp(&b.line)));

    // 步骤 C: 遍历数组，应用“相邻行控制流过滤”启发式规则
    // saturating_sub(1) 避免数组为空时发生下溢崩溃
    for i in 0..stats.len().saturating_sub(1) {
        // 条件1: 同一个文件，且行号严格相邻
        if stats[i].file == stats[i+1].file && stats[i].line + 1 == stats[i+1].line {
            // 条件2: 出现次数相差 <= 2 (包含完全相等的情况，容错率更高)
            let diff = (stats[i].count as isize - stats[i+1].count as isize).abs();
            if diff <= 2 {
                // 条件3: 检查代码内容是否包含控制流关键字
                let a_ctrl = re_ctrl.is_match(&stats[i].code);
                let b_ctrl = re_ctrl.is_match(&stats[i+1].code);

                // 如果恰好只有其中一行是 if/for/while，则将其标记为删除
                if a_ctrl && !b_ctrl {
                    stats[i].remove = true;
                } else if !a_ctrl && b_ctrl {
                    stats[i+1].remove = true;
                }
            }
        }
    }

    // 步骤 D: 过滤掉被标记为 remove 的干扰行，然后恢复按“出现次数”降序排序
    let mut final_stats: Vec<_> = stats.into_iter().filter(|s| !s.remove).collect();
    final_stats.sort_by(|a, b| b.count.cmp(&a.count).then(a.raw.cmp(&b.raw)));

    // 写入文件
    for stat in final_stats {
        writeln!(writer_aa, "{}: {}", stat.count, stat.raw)?;
    }

    println!("处理完成！");
    println!("- 详细日志已生成: hitm_a.txt");
    println!("- F,A 热点统计已生成: hitm_aa.txt (应用了控制流降噪过滤)");
    Ok(())
}
