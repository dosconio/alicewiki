use regex::Regex;
use std::collections::HashMap;
use std::env;
use std::fs::File;
use std::io::{BufReader, Read, Write};
use std::path::Path;

#[path = "../../type_layout.rs"]
mod type_layout;

fn guess_base_size(type_str: &str) -> u64 {
    type_layout::type_layout(type_str)
        .unwrap_or_else(|| {
            eprintln!("Error: unknown type size: {type_str}");
            std::process::exit(1)
        })
        .0
}

#[derive(Debug, Clone)]
struct Node {
    id: usize,
    size: u64,
    alignment: u64,
    offset: u64,
}

fn align_up(value: u64, align: u64) -> u64 {
    if align <= 1 {
        return value;
    }
    ((value + align - 1) / align) * align
}

fn extract_field_name(name: &str) -> String {
    // If it's Point::x, return x
    if let Some(idx) = name.rfind("::") {
        return name[idx + 2..].to_string();
    }
    // If it's team[0].health, return health
    if let Some(idx) = name.rfind('.') {
        return name[idx + 1..].to_string();
    }
    // If it's just global_counter, return global_counter
    name.to_string()
}

fn get_base_name(dot_file: &str) -> String {
    let path = Path::new(dot_file);
    let file_name = path.file_name().unwrap_or_default().to_string_lossy();
    file_name
        .replace("graph.", "")
        .replace(".fields.dot", "")
        .replace(".dot", "")
}

fn main() {
    let args: Vec<String> = env::args().collect();
    if args.len() < 6 {
        eprintln!("Usage: package_fields <dot_file> <alpha> <beta> <gamma> <cacheline>");
        std::process::exit(1);
    }

    let dot_file = &args[1];
    let alpha_cli: f64 = args[2].parse().unwrap_or(1.0);
    let beta_cli: f64 = args[3].parse().unwrap_or(1.0);
    let gamma_cli: f64 = args[4].parse().unwrap_or(1.0);
    let cacheline: u64 = args[5].parse().unwrap_or(64);

    let base_target = get_base_name(dot_file);

    let file = match File::open(dot_file) {
        Ok(f) => f,
        Err(e) => {
            eprintln!("Error opening file '{}': {}", dot_file, e);
            std::process::exit(1);
        }
    };
    let mut reader = BufReader::new(file);
    let mut file_content = String::new();
    let _ = reader.read_to_string(&mut file_content);

    let node_re = Regex::new(r#"^\s*"([^"]+)"\s*\[.*?type="([^"]+)".*?\];"#).unwrap();
    let edge_re =
        Regex::new(r#""([^"]+)"\s*--\s*"([^"]+)"\s*\[[^\]]*?label="\(([\d\.]+),([\d\.]+)\)""#)
            .unwrap();

    let mut abstract_nodes: HashMap<String, Node> = HashMap::new();
    let mut raw_to_abstract: HashMap<String, String> = HashMap::new();
    let mut field_names_vec: Vec<String> = Vec::new();
    let struct_offset_re = Regex::new(r"(?:^|[, ])offset=(\d+)").unwrap();
    let array_base_re = Regex::new(r"array_base=(\d+)").unwrap();
    let array_elem_re = Regex::new(r"array_elem_size=(\d+)").unwrap();
    let mut array_base: Option<u64> = None;
    let mut array_elem_size: Option<u64> = None;

    for line in file_content.lines() {
        if let Some(caps) = node_re.captures(line) {
            let raw_name = caps[1].to_string();
            let typ = caps[2].to_string();

            // Filtering out external nodes correctly
            if dot_file.ends_with(".fields.dot") {
                let struct_prefix = format!("{}::", base_target);
                if !raw_name.starts_with(&struct_prefix) {
                    continue;
                }
            } else {
                // E.g. graph.team.dot
                let array_prefix = format!("{}[", base_target);
                if !raw_name.starts_with(&array_prefix) {
                    continue; // Skip single globals and cross-struct nodes
                }
            }

            let field_name = extract_field_name(&raw_name);
            raw_to_abstract.insert(raw_name.clone(), field_name.clone());

            if !abstract_nodes.contains_key(&field_name) {
                let metadata = Regex::new(r"(?:^|[, ])size=(\d+)").unwrap();
                let size = metadata
                    .captures(line)
                    .and_then(|c| c[1].parse::<u64>().ok())
                    .filter(|&s| s > 0)
                    .unwrap_or_else(|| guess_base_size(&typ));
                let alignment = type_layout::type_layout(&typ)
                    .map(|l| l.1)
                    .unwrap_or_else(|| (1u64 << size.trailing_zeros()).min(cacheline));
                let offset = struct_offset_re
                    .captures(line)
                    .and_then(|c| c[1].parse::<u64>().ok())
                    .unwrap_or(0);
                if array_base.is_none() {
                    array_base = array_base_re
                        .captures(line)
                        .and_then(|c| c[1].parse::<u64>().ok());
                }
                if array_elem_size.is_none() {
                    array_elem_size = array_elem_re
                        .captures(line)
                        .and_then(|c| c[1].parse::<u64>().ok())
                        .filter(|&s| s > 0);
                }
                let id = field_names_vec.len();
                field_names_vec.push(field_name.clone());
                abstract_nodes.insert(
                    field_name.clone(),
                    Node {
                        id,
                        size,
                        alignment,
                        offset,
                    },
                );
            }
        }
    }

    if field_names_vec.is_empty() {
        eprintln!("Error: No struct fields extracted from {}.", dot_file);
        std::process::exit(1);
    }

    let n = field_names_vec.len();

    let edge_head_re = Regex::new(r#"(?m)^\s*"[^"]+"\s*--\s*"[^"]+"\s*\["#).unwrap();
    let k_edges = edge_head_re.find_iter(&file_content).count();
    let m_edges = edge_re.captures_iter(&file_content).count();
    if m_edges < k_edges {
        eprintln!(
            "Error: 边读取失败 -- '{}' 中有 {} 条边, 只解析出 {} 条。DOT 属性格式可能已变更 (label 写法或属性顺序)。",
            dot_file, k_edges, m_edges
        );
        std::process::exit(1);
    }
    if k_edges == 0 && n > 1 {
        eprintln!(
            "Warning: '{}' 中没有边 (节点 {} 个), 图本身可能是空的。",
            dot_file, n
        );
    }
    println!("[edges] {}: parsed {}/{}", dot_file, m_edges, k_edges);

    let mut struct_span = 0u64;
    let mut struct_align = 1u64;
    for field in &field_names_vec {
        let node = &abstract_nodes[field];
        struct_span = struct_span.max(node.offset + node.size);
        struct_align = struct_align.max(node.alignment);
    }
    let element_size = array_elem_size.unwrap_or_else(|| align_up(struct_span, struct_align));
    let base_mod_line = array_base.map(|base| base % cacheline);
    let stride_mod_line = element_size % cacheline;
    let straddles = stride_mod_line != 0 || base_mod_line.is_some_and(|rem| rem != 0);
    // One element inside one cache line leaves nothing to pack: every extra
    // line costs a full line of memory, and the cross-element conflicts are
    // repaired by the element stride and the array base, not by field order.
    let keep_tight = dot_file.ends_with(".fields.dot") && element_size <= cacheline;

    let mut final_groups: Vec<Vec<String>> = Vec::new();
    if keep_tight {
        let mut fields = field_names_vec.clone();
        fields.sort_by_key(|field| (abstract_nodes[field].offset, field.clone()));
        final_groups.push(fields);
    } else {
        let mut w_matrix = vec![vec![0.0; n]; n];

        for caps in edge_re.captures_iter(&file_content) {
            let u_raw = caps[1].to_string();
            let v_raw = caps[2].to_string();

            if let (Some(u_field), Some(v_field)) =
                (raw_to_abstract.get(&u_raw), raw_to_abstract.get(&v_raw))
            {
                let u_id = abstract_nodes[u_field].id;
                let v_id = abstract_nodes[v_field].id;

                let a: f64 = caps[3].parse().unwrap_or(0.0);
                let b: f64 = caps[4].parse().unwrap_or(0.0);

                let w_same = alpha_cli * b - beta_cli * a;
                let w_diff = alpha_cli * (a + b) / gamma_cli;
                let delta_w = w_same - w_diff;

                w_matrix[u_id][v_id] += delta_w;
                if u_id != v_id {
                    w_matrix[v_id][u_id] += delta_w; // Undirected
                }
            }
        }

        let mut bin_of: Vec<usize> = (0..n).collect();
        let mut bin_size: Vec<u64> = field_names_vec
            .iter()
            .map(|f| abstract_nodes[f].size)
            .collect();
        let mut active_bins: Vec<bool> = vec![true; n];

        loop {
            let mut max_w = 0.0;
            let mut best_pair = None;

            for i in 0..n {
                if !active_bins[i] {
                    continue;
                }
                for j in (i + 1)..n {
                    if !active_bins[j] {
                        continue;
                    }

                    if bin_size[i] + bin_size[j] <= cacheline {
                        if w_matrix[i][j] > max_w {
                            max_w = w_matrix[i][j];
                            best_pair = Some((i, j));
                        }
                    }
                }
            }

            match best_pair {
                Some((u, v)) => {
                    active_bins[v] = false;
                    bin_size[u] += bin_size[v];
                    for i in 0..n {
                        if bin_of[i] == v {
                            bin_of[i] = u;
                        }
                    }
                    for i in 0..n {
                        if active_bins[i] && i != u && i != v {
                            let w_sum = w_matrix[u][i] + w_matrix[v][i];
                            w_matrix[u][i] = w_sum;
                            w_matrix[i][u] = w_sum;
                        }
                    }
                }
                None => break,
            }
        }

        let mut bins: HashMap<usize, Vec<String>> = HashMap::new();
        for i in 0..n {
            bins.entry(bin_of[i])
                .or_default()
                .push(field_names_vec[i].clone());
        }

        final_groups = bins.into_values().collect();
        final_groups.sort_by_key(|group| group[0].clone());
    }

    let out_path = dot_file
        .replace(".fields.dot", ".layout.txt")
        .replace(".dot", ".layout.txt");
    let mut out_file = File::create(&out_path).unwrap();

    writeln!(out_file, "Target: {}", base_target).unwrap();
    writeln!(out_file, "Cacheline size: {}", cacheline).unwrap();
    let elements_per_line = if !straddles && element_size <= cacheline {
        (cacheline / element_size.max(1)).to_string()
    } else {
        "-".to_string()
    };
    writeln!(
        out_file,
        "Alignment: element_size={} base={} base_mod_line={} stride_mod_line={} elements_per_line={} straddles={}",
        element_size,
        array_base
            .map(|base| base.to_string())
            .unwrap_or_else(|| "unknown".to_string()),
        base_mod_line
            .map(|rem| rem.to_string())
            .unwrap_or_else(|| "unknown".to_string()),
        stride_mod_line,
        elements_per_line,
        straddles
    )
    .unwrap();
    let mut remedies: Vec<String> = Vec::new();
    if stride_mod_line != 0 {
        let padded = align_up(element_size, cacheline);
        remedies.push(format!(
            "pad element stride {} -> {} bytes ({} padding bytes per element, +{:.1}%)",
            element_size,
            padded,
            padded - element_size,
            (padded - element_size) as f64 * 100.0 / element_size as f64
        ));
    }
    if base_mod_line.is_some_and(|rem| rem != 0) {
        remedies.push(format!("align array base to {} bytes", cacheline));
    }
    writeln!(
        out_file,
        "Remedy: {}",
        if remedies.is_empty() {
            "none; every element owns whole cache lines".to_string()
        } else {
            remedies.join("; ")
        }
    )
    .unwrap();
    writeln!(out_file, "========================================").unwrap();

    let mut cacheline_id = 0u64;
    let mut placed_bytes = 0u64;
    for mut group in final_groups {
        if keep_tight {
            group.sort_by_key(|field| (abstract_nodes[field].offset, field.clone()));
        } else {
            group.sort_by_key(|field| {
                (
                    std::cmp::Reverse(abstract_nodes[field].alignment),
                    field.clone(),
                )
            });
        }
        let line_start = cacheline_id * cacheline;
        let group_bytes: u64 = group.iter().map(|field| abstract_nodes[field].size).sum();
        let line_span = std::cmp::max(1, (group_bytes + cacheline - 1) / cacheline);
        writeln!(
            out_file,
            "[Cacheline {} aligned_offset={} span={}]",
            cacheline_id, line_start, line_span
        )
        .unwrap();
        let mut offset = line_start;
        for field in group {
            writeln!(out_file, "  - {}", field).unwrap();
            // Keeping a compact struct means keeping its ABI offsets, padding
            // included; only a re-packed layout uses the cumulative placement.
            let field_offset = if keep_tight {
                abstract_nodes[&field].offset
            } else {
                offset
            };
            writeln!(
                out_file,
                "    placement: offset={} size={}",
                field_offset,
                abstract_nodes[&field].size
            )
            .unwrap();
            offset += abstract_nodes[&field].size;
        }
        writeln!(
            out_file,
            "    used={} padding={}",
            group_bytes,
            line_span * cacheline - group_bytes
        )
        .unwrap();
        placed_bytes = line_start + line_span * cacheline;
        cacheline_id += line_span;
    }
    writeln!(
        out_file,
        "Layout size with line isolation: {}",
        placed_bytes
    )
    .unwrap();
    println!("Successfully generated {}", out_path);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn qualified_arrays_keep_their_full_size() {
        assert_eq!(guess_base_size("volatile int[10]"), 40);
        assert_eq!(guess_base_size("const unsigned long long[2]"), 16);
    }
}
