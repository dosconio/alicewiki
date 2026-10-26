use regex::Regex;
use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::env;
use std::fs::File;
use std::io::{BufReader, Read, Write};

// A container wider than this many lines is never proposed: beyond it the
// locality argument no longer justifies the footprint.
const MAX_LINES: u64 = 4;
const MAX_ROUNDS: usize = 200;

#[derive(Debug, Clone)]
#[allow(dead_code)]
struct Field {
    label: String,
    owner: String,
    typ: String,
    offset: u64,
    size: u64,
    instances: u64,
    stride: u64,
    ambiguous: bool,
    threads: BTreeSet<u64>,
}

#[derive(Debug, Clone)]
struct Edge {
    u: String,
    v: String,
    alpha: f64,
    beta: f64,
    count1: f64,
}

#[derive(Debug, Clone)]
struct Container {
    key: String,
    members: Vec<String>,
    instances: u64,
    threads: BTreeSet<u64>,
}

struct Action {
    action: String,
    container: String,
    fields: Vec<String>,
    instances: u64,
    bytes: u64,
    lines: u64,
    score_before: f64,
    score_after: f64,
    strict_thread_superset: bool,
    groups: Vec<Vec<String>>,
}

struct MergeMove {
    gain: f64,
    left: usize,
    right: usize,
}

struct MigrateMove {
    gain: f64,
    source: usize,
    target: usize,
    member: String,
    strict: bool,
}

struct SplitMove {
    gain: f64,
    index: usize,
}

struct Ctx<'a> {
    fields: &'a HashMap<String, Field>,
    edges: &'a [Edge],
    line: u64,
    k: f64,
    g: f64,
    gamma: f64,
    lambda: f64,
}

impl Ctx<'_> {
    fn score(&self, parts: &[Container]) -> f64 {
        score(
            parts,
            self.fields,
            self.edges,
            self.line,
            self.k,
            self.g,
            self.gamma,
        )
    }
}

fn align_up(value: u64, align: u64) -> u64 {
    if value == 0 || align <= 1 {
        return value;
    }
    ((value - 1) / align + 1) * align
}

fn capture_attr(re: &Regex, text: &str) -> Option<String> {
    re.captures(text).map(|c| c[1].to_string())
}

fn parse_threads(text: &str) -> BTreeSet<u64> {
    text.split(',')
        .filter_map(|token| token.trim().parse::<u64>().ok())
        .collect()
}

fn thread_text(threads: &BTreeSet<u64>) -> String {
    threads
        .iter()
        .map(|tid| tid.to_string())
        .collect::<Vec<_>>()
        .join(",")
}

// Alignment comes from the member type, never from the member size: a member
// array such as `long pad[7]` is 8-byte aligned, not 64-byte aligned.
fn member_alignment(type_text: &str, size: u64, line: u64) -> u64 {
    let base = type_text.split('[').next().unwrap_or(type_text);
    let align = if base.contains("char") {
        1
    } else if base.contains("short") {
        2
    } else if base.contains("long") || base.contains("double") || base.contains('*') {
        8
    } else if base.contains("float") || base.contains("int") {
        4
    } else {
        8
    };
    align.min(size.max(1)).min(line).max(1)
}

fn offsets_of(
    container: &Container,
    fields: &HashMap<String, Field>,
    line: u64,
) -> BTreeMap<String, u64> {
    let mut members = container.members.clone();
    members.sort_by_key(|label| (fields[label].offset, label.clone()));
    let mut offsets = BTreeMap::new();
    let mut cursor = 0u64;
    for label in members {
        let field = &fields[&label];
        let align = member_alignment(&field.typ, field.size, line);
        cursor = align_up(cursor, align);
        offsets.insert(label, cursor);
        cursor += field.size;
    }
    offsets
}

fn container_end(container: &Container, fields: &HashMap<String, Field>, line: u64) -> u64 {
    let offsets = offsets_of(container, fields, line);
    container
        .members
        .iter()
        .map(|label| offsets[label] + fields[label].size)
        .max()
        .unwrap_or(0)
}

fn container_lines(container: &Container, fields: &HashMap<String, Field>, line: u64) -> u64 {
    align_up(container_end(container, fields, line), line) / line
}

fn footprint(parts: &[Container], fields: &HashMap<String, Field>, line: u64) -> u64 {
    parts
        .iter()
        .map(|container| align_up(container_end(container, fields, line), line))
        .sum()
}

fn score(
    parts: &[Container],
    fields: &HashMap<String, Field>,
    edges: &[Edge],
    line: u64,
    k: f64,
    g: f64,
    gamma: f64,
) -> f64 {
    let mut home_of: HashMap<&str, usize> = HashMap::new();
    let mut offsets: Vec<BTreeMap<String, u64>> = Vec::with_capacity(parts.len());
    for (index, container) in parts.iter().enumerate() {
        offsets.push(offsets_of(container, fields, line));
        for label in &container.members {
            home_of.insert(label.as_str(), index);
        }
    }

    let mut total = 0.0;
    for edge in edges {
        let (Some(&home_u), Some(&home_v)) = (
            home_of.get(edge.u.as_str()),
            home_of.get(edge.v.as_str()),
        ) else {
            continue;
        };
        let same_line = if home_u != home_v {
            false
        } else {
            let start_u = offsets[home_u][&edge.u];
            let end_u = start_u + fields[&edge.u].size;
            let start_v = offsets[home_u][&edge.v];
            let end_v = start_v + fields[&edge.v].size;
            start_u / line <= end_v.saturating_sub(1) / line
                && start_v / line <= end_u.saturating_sub(1) / line
        };
        total += if same_line {
            k * edge.beta - g * edge.alpha
        } else {
            k * (edge.alpha + edge.beta) / gamma
        };
    }
    total
}

fn merged_container(left: &Container, right: &Container) -> Container {
    let mut members = left.members.clone();
    members.extend(right.members.iter().cloned());
    members.sort();
    Container {
        key: format!("{}|{}", left.key, right.key),
        members,
        instances: left.instances,
        threads: left.threads.clone(),
    }
}

fn best_merge(
    ctx: &Ctx,
    parts: &[Container],
    current: f64,
    rejected: &mut Vec<(Vec<String>, String)>,
) -> Option<MergeMove> {
    let mut best: Option<MergeMove> = None;
    for left in 0..parts.len() {
        for right in (left + 1)..parts.len() {
            let a = &parts[left];
            let b = &parts[right];
            if a.instances != b.instances {
                rejected.push((
                    vec![a.key.clone(), b.key.clone()],
                    "cardinality_mismatch".to_string(),
                ));
                continue;
            }
            if a.threads != b.threads {
                rejected.push((
                    vec![a.key.clone(), b.key.clone()],
                    "thread_partition_mismatch".to_string(),
                ));
                continue;
            }
            let merged = merged_container(a, b);
            if container_lines(&merged, ctx.fields, ctx.line) > MAX_LINES {
                rejected.push((
                    vec![a.key.clone(), b.key.clone()],
                    "capacity_exceeded".to_string(),
                ));
                continue;
            }
            if merged.members.iter().any(|label| ctx.fields[label].ambiguous) {
                rejected.push((
                    vec![a.key.clone(), b.key.clone()],
                    "ambiguous_layout".to_string(),
                ));
                continue;
            }
            let mut candidate = parts.to_vec();
            candidate[left] = merged;
            candidate.remove(right);
            let gain = ctx.score(&candidate) - current;
            if gain > 0.0
                && best
                    .as_ref()
                    .map(|move_| gain > move_.gain)
                    .unwrap_or(true)
            {
                best = Some(MergeMove { gain, left, right });
            } else if gain <= 0.0 {
                rejected.push((
                    vec![a.key.clone(), b.key.clone()],
                    "non_positive_gain".to_string(),
                ));
            }
        }
    }
    best
}

fn best_migrate(
    ctx: &Ctx,
    parts: &[Container],
    current: f64,
    rejected: &mut Vec<(Vec<String>, String)>,
) -> Option<MigrateMove> {
    let mut best: Option<MigrateMove> = None;
    for source in 0..parts.len() {
        for member in parts[source].members.clone() {
            let field = &ctx.fields[&member];
            if field.ambiguous {
                continue;
            }
            for target in 0..parts.len() {
                if target == source {
                    continue;
                }
                let destination = &parts[target];
                if destination.instances != field.instances {
                    rejected.push((
                        vec![member.clone(), destination.key.clone()],
                        "cardinality_mismatch".to_string(),
                    ));
                    continue;
                }
                if !field.threads.is_subset(&destination.threads) {
                    rejected.push((
                        vec![member.clone(), destination.key.clone()],
                        "thread_partition_mismatch".to_string(),
                    ));
                    continue;
                }
                let mut candidate = parts.to_vec();
                candidate[source].members.retain(|label| label != &member);
                candidate[target].members.push(member.clone());
                candidate[target].members.sort();
                if container_lines(&candidate[target], ctx.fields, ctx.line) > MAX_LINES {
                    rejected.push((
                        vec![member.clone(), destination.key.clone()],
                        "capacity_exceeded".to_string(),
                    ));
                    continue;
                }
                if candidate[source].members.is_empty() {
                    candidate.remove(source);
                }
                let strict = field.threads != destination.threads;
                let penalty = if strict { ctx.lambda } else { 0.0 };
                let gain = ctx.score(&candidate) - penalty - current;
                if gain > 0.0
                    && best
                        .as_ref()
                        .map(|move_| gain > move_.gain)
                        .unwrap_or(true)
                {
                    best = Some(MigrateMove {
                        gain,
                        source,
                        target,
                        member: member.clone(),
                        strict,
                    });
                } else if gain <= 0.0 {
                    rejected.push((
                        vec![member.clone(), destination.key.clone()],
                        "non_positive_gain".to_string(),
                    ));
                }
            }
        }
    }
    best
}

fn split_groups(ctx: &Ctx, container: &Container) -> Option<Vec<Container>> {
    let mut groups: BTreeMap<String, Vec<String>> = BTreeMap::new();
    for member in &container.members {
        groups
            .entry(thread_text(&ctx.fields[member].threads))
            .or_default()
            .push(member.clone());
    }
    if groups.len() < 2 {
        return None;
    }
    let conflicting = ctx.edges.iter().any(|edge| {
        edge.count1 > 0.0
            && container.members.contains(&edge.u)
            && container.members.contains(&edge.v)
            && ctx.fields[&edge.u].threads != ctx.fields[&edge.v].threads
    });
    if !conflicting {
        return None;
    }
    let mut containers = Vec::new();
    for (ordinal, (thread_key, members)) in groups.into_iter().enumerate() {
        let instances = ctx.fields[&members[0]].instances;
        containers.push(Container {
            key: format!("{}#{}", container.key, ordinal + 1),
            members,
            instances,
            threads: parse_threads(&thread_key),
        });
    }
    Some(containers)
}

fn best_split(ctx: &Ctx, parts: &[Container], current: f64) -> Option<SplitMove> {
    let mut best: Option<SplitMove> = None;
    for index in 0..parts.len() {
        if parts[index].members.len() < 2 {
            continue;
        }
        let Some(groups) = split_groups(ctx, &parts[index]) else {
            continue;
        };
        let mut candidate = parts.to_vec();
        candidate.remove(index);
        candidate.extend(groups);
        let gain = ctx.score(&candidate) - current;
        if gain > 0.0
            && best
                .as_ref()
                .map(|move_| gain > move_.gain)
                .unwrap_or(true)
        {
            best = Some(SplitMove { gain, index });
        }
    }
    best
}

fn json_escape(value: &str) -> String {
    value.replace('\\', "\\\\").replace('"', "\\\"")
}

fn main() {
    let args: Vec<String> = env::args().collect();
    if args.len() < 6 {
        eprintln!("Usage: planner <cross.dot> <alpha> <beta> <gamma> <cacheline>");
        std::process::exit(1);
    }
    let dot_file = &args[1];
    let k: f64 = args[2].parse::<f64>().unwrap_or(1.0);
    let g: f64 = args[3].parse::<f64>().unwrap_or(1.0);
    let gamma: f64 = args[4].parse::<f64>().unwrap_or(1.0).max(1.0);
    let line: u64 = args[5].parse::<u64>().unwrap_or(64);

    let file = match File::open(dot_file) {
        Ok(handle) => handle,
        Err(error) => {
            eprintln!("Error opening '{}': {}", dot_file, error);
            std::process::exit(1);
        }
    };
    let mut reader = BufReader::new(file);
    let mut content = String::new();
    let _ = reader.read_to_string(&mut content);

    let node_re = Regex::new(r#"^\s*"([^"]+)"\s*\[(.*)\];"#).unwrap();
    let owner_re = Regex::new(r#"owner="([^"]*)""#).unwrap();
    let type_re = Regex::new(r#"type="([^"]*)""#).unwrap();
    let offset_re = Regex::new(r"(?:^|[, ])offset=(\d+)").unwrap();
    let size_re = Regex::new(r"(?:^|[, ])size=(\d+)").unwrap();
    let stride_re = Regex::new(r"element_stride=(\d+)").unwrap();
    let instances_re = Regex::new(r"(?:^|[, ])instances=(\d+)").unwrap();
    let ambiguous_re = Regex::new(r"layout_ambiguous=(\d+)").unwrap();
    let threads_re = Regex::new(r#"threads="([^"]*)""#).unwrap();

    let mut fields: HashMap<String, Field> = HashMap::new();
    for line_text in content.lines() {
        let Some(caps) = node_re.captures(line_text) else {
            continue;
        };
        if line_text.contains("--") {
            continue;
        }
        let label = caps[1].to_string();
        let body: &str = &caps[2];
        let owner = capture_attr(&owner_re, body).unwrap_or_else(|| label.clone());
        let typ = capture_attr(&type_re, body).unwrap_or_default();
        let offset = offset_re
            .captures(body)
            .and_then(|c| c[1].parse::<u64>().ok())
            .unwrap_or(0);
        let size = size_re
            .captures(body)
            .and_then(|c| c[1].parse::<u64>().ok())
            .unwrap_or(0)
            .max(1);
        let stride = stride_re
            .captures(body)
            .and_then(|c| c[1].parse::<u64>().ok())
            .unwrap_or(0);
        let instances = instances_re
            .captures(body)
            .and_then(|c| c[1].parse::<u64>().ok())
            .unwrap_or(1)
            .max(1);
        let ambiguous = ambiguous_re
            .captures(body)
            .and_then(|c| c[1].parse::<u64>().ok())
            .unwrap_or(0)
            != 0;
        let threads = threads_re
            .captures(body)
            .map(|c| parse_threads(&c[1]))
            .unwrap_or_default();
        fields.insert(
            label.clone(),
            Field {
                label,
                owner,
                typ,
                offset,
                size,
                instances,
                stride,
                ambiguous,
                threads,
            },
        );
    }

    if fields.is_empty() {
        eprintln!("Error: no field nodes found in '{}'.", dot_file);
        std::process::exit(1);
    }

    let edge_re = Regex::new(r#""([^"]+)"\s*--\s*"([^"]+)"\s*\[([^\]]*)\]"#).unwrap();
    let alpha_re = Regex::new(r"(?:^|[, ])alpha=(\d+)").unwrap();
    let beta_re = Regex::new(r"(?:^|[, ])beta=(\d+)").unwrap();
    let count1_re = Regex::new(r"(?:^|[, ])count1=(\d+)").unwrap();
    // Same-index counts isolate the pairs one container change can actually
    // affect; the totals are dominated by unrelated cross-instance traffic.
    let same_res = [
        Regex::new(r"(?:^|[, ])count1_same=(\d+)").unwrap(),
        Regex::new(r"(?:^|[, ])count2_same=(\d+)").unwrap(),
        Regex::new(r"(?:^|[, ])count3_same=(\d+)").unwrap(),
        Regex::new(r"(?:^|[, ])count4_same=(\d+)").unwrap(),
    ];
    let mut edges: Vec<Edge> = Vec::new();
    for caps in edge_re.captures_iter(&content) {
        let u = caps[1].to_string();
        let v = caps[2].to_string();
        if !fields.contains_key(&u) || !fields.contains_key(&v) {
            continue;
        }
        let body: &str = &caps[3];
        let read = |re: &Regex| -> f64 {
            re.captures(body)
                .and_then(|c| c[1].parse::<f64>().ok())
                .unwrap_or(0.0)
        };
        let mut same_counts = [0.0f64; 4];
        let mut has_same = true;
        for (index, re) in same_res.iter().enumerate() {
            match re.captures(body).and_then(|c| c[1].parse::<f64>().ok()) {
                Some(value) => same_counts[index] = value,
                None => {
                    has_same = false;
                    break;
                }
            }
        }
        let (alpha, beta, count1) = if has_same {
            (
                same_counts[0] + same_counts[1],
                same_counts[2] + same_counts[3],
                same_counts[0],
            )
        } else {
            (read(&alpha_re), read(&beta_re), read(&count1_re))
        };
        edges.push(Edge {
            u,
            v,
            alpha,
            beta,
            count1,
        });
    }

    if edges.is_empty() {
        eprintln!("Error: no cross-object edge found in '{}'.", dot_file);
        std::process::exit(1);
    }

    let mut deltas: Vec<f64> = edges
        .iter()
        .map(|edge| k * edge.beta - g * edge.alpha - k * (edge.alpha + edge.beta) / gamma)
        .filter(|delta| *delta > 0.0)
        .collect();
    let lambda = if deltas.is_empty() {
        0.0
    } else {
        deltas.sort_by(|a, b| a.partial_cmp(b).unwrap());
        0.1 * deltas.iter().sum::<f64>() / deltas.len() as f64
    };

    let ctx = Ctx {
        fields: &fields,
        edges: &edges,
        line,
        k,
        g,
        gamma,
        lambda,
    };

    // Initial partition reproduces the current AoS layout: one container per
    // owner, holding every field of that owner.
    let mut owners: BTreeMap<String, Container> = BTreeMap::new();
    for field in fields.values() {
        let entry = owners.entry(field.owner.clone()).or_insert_with(|| Container {
            key: field.owner.clone(),
            members: Vec::new(),
            instances: field.instances,
            threads: BTreeSet::new(),
        });
        entry.members.push(field.label.clone());
        entry.threads.extend(field.threads.iter().copied());
        entry.instances = entry.instances.min(field.instances);
    }
    let mut parts: Vec<Container> = owners.into_values().collect();
    for container in &mut parts {
        container.members.sort();
    }

    let baseline = ctx.score(&parts);
    let footprint_before = footprint(&parts, &fields, line);
    let mut current = baseline;
    let mut actions: Vec<Action> = Vec::new();
    let mut rejected: Vec<(Vec<String>, String)> = Vec::new();

    for _round in 0..MAX_ROUNDS {
        let migration = best_migrate(&ctx, &parts, current, &mut rejected);
        let merge = best_merge(&ctx, &parts, current, &mut rejected);
        let split = best_split(&ctx, &parts, current);

        let mut choice: Option<(f64, u8)> = None;
        if let Some(move_) = &migration {
            choice = Some((move_.gain, 0));
        }
        if let Some(move_) = &merge {
            if choice.map(|(gain, _)| move_.gain > gain).unwrap_or(true) {
                choice = Some((move_.gain, 1));
            }
        }
        if let Some(move_) = &split {
            if choice.map(|(gain, _)| move_.gain > gain).unwrap_or(true) {
                choice = Some((move_.gain, 2));
            }
        }
        let Some((_, kind)) = choice else {
            break;
        };

        let before = current;
        if kind == 0 {
            let Some(move_) = migration else {
                break;
            };
            let destination_key = parts[move_.target].key.clone();
            parts[move_.target].members.push(move_.member.clone());
            parts[move_.target].members.sort();
            parts[move_.source].members.retain(|label| label != &move_.member);
            if parts[move_.source].members.is_empty() {
                parts.remove(move_.source);
            }
            let target_index = parts
                .iter()
                .position(|container| container.key == destination_key)
                .unwrap_or(0);
            let penalty = if move_.strict { lambda } else { 0.0 };
            current = ctx.score(&parts) - penalty;
            let placed = &parts[target_index];
            actions.push(Action {
                action: "migrate".to_string(),
                container: placed.key.clone(),
                fields: vec![move_.member.clone()],
                instances: placed.instances,
                bytes: align_up(container_end(placed, &fields, line), line),
                lines: container_lines(placed, &fields, line),
                score_before: before,
                score_after: current,
                strict_thread_superset: move_.strict,
                groups: Vec::new(),
            });
        } else if kind == 1 {
            let Some(move_) = merge else {
                break;
            };
            let merged = merged_container(&parts[move_.left], &parts[move_.right]);
            parts[move_.left] = merged;
            parts.remove(move_.right);
            current = ctx.score(&parts);
            let placed = &parts[move_.left];
            actions.push(Action {
                action: "merge".to_string(),
                container: placed.key.clone(),
                fields: placed.members.clone(),
                instances: placed.instances,
                bytes: align_up(container_end(placed, &fields, line), line),
                lines: container_lines(placed, &fields, line),
                score_before: before,
                score_after: current,
                strict_thread_superset: false,
                groups: Vec::new(),
            });
        } else {
            let Some(move_) = split else {
                break;
            };
            let container = parts[move_.index].clone();
            let Some(groups) = split_groups(&ctx, &container) else {
                break;
            };
            parts.remove(move_.index);
            let mut split_fields: Vec<String> = Vec::new();
            let mut split_groups: Vec<Vec<String>> = Vec::new();
            for group in &groups {
                split_fields.extend(group.members.iter().cloned());
                split_groups.push(group.members.clone());
            }
            parts.extend(groups);
            current = ctx.score(&parts);
            actions.push(Action {
                action: "split".to_string(),
                container: container.key.clone(),
                fields: split_fields,
                instances: container.instances,
                bytes: align_up(container_end(&container, &fields, line), line),
                lines: container_lines(&container, &fields, line),
                score_before: before,
                score_after: current,
                strict_thread_superset: false,
                groups: split_groups,
            });
        }
    }

    let out_path = dot_file.replace(".dot", ".plan.json");
    let mut out = match File::create(&out_path) {
        Ok(handle) => handle,
        Err(error) => {
            eprintln!("Error creating '{}': {}", out_path, error);
            std::process::exit(1);
        }
    };

    let _ = writeln!(out, "{{");
    let _ = writeln!(out, "  \"schema_version\": 1,");
    let _ = writeln!(out, "  \"source\": \"{}\",", json_escape(dot_file));
    let _ = writeln!(out, "  \"line\": {},", line);
    let _ = writeln!(out, "  \"lambda\": {},", lambda);
    let _ = writeln!(out, "  \"current_score\": {},", baseline);
    let _ = writeln!(out, "  \"final_score\": {},", current);
    let _ = writeln!(out, "  \"gain\": {},", current - baseline);
    let footprint_after = footprint(&parts, &fields, line);
    let _ = writeln!(out, "  \"footprint_before\": {},", footprint_before);
    let _ = writeln!(out, "  \"footprint_after\": {},", footprint_after);
    let _ = writeln!(out, "  \"actions\": [");
    for (index, action) in actions.iter().enumerate() {
        let comma = if index + 1 == actions.len() { "" } else { "," };
        let fields_json = action
            .fields
            .iter()
            .map(|label| format!("\"{}\"", json_escape(label)))
            .collect::<Vec<_>>()
            .join(",");
        let groups_json = action
            .groups
            .iter()
            .map(|group| {
                let members = group
                    .iter()
                    .map(|label| format!("\"{}\"", json_escape(label)))
                    .collect::<Vec<_>>()
                    .join(",");
                format!("[{}]", members)
            })
            .collect::<Vec<_>>()
            .join(",");
        let _ = writeln!(
            out,
            "    {{\"action\":\"{}\",\"container\":\"{}\",\"fields\":[{}],\"groups\":[{}],\"instances\":{},\"bytes\":{},\"lines\":{},\"score_before\":{},\"score_after\":{},\"strict_thread_superset\":{}}}{}",
            action.action,
            json_escape(&action.container),
            fields_json,
            groups_json,
            action.instances,
            action.bytes,
            action.lines,
            action.score_before,
            action.score_after,
            action.strict_thread_superset,
            comma
        );
    }
    let _ = writeln!(out, "  ],");
    let rejected_unique: BTreeMap<(String, String), usize> =
        rejected
            .iter()
            .fold(BTreeMap::new(), |mut acc, (fields_pair, reason)| {
                let key = (fields_pair.join(" | "), reason.clone());
                *acc.entry(key).or_insert(0) += 1;
                acc
            });
    let rejected_items: Vec<_> = rejected_unique.into_iter().collect();
    let _ = writeln!(out, "  \"rejected\": [");
    for (index, ((pair, reason), count)) in rejected_items.iter().enumerate() {
        let comma = if index + 1 == rejected_items.len() {
            ""
        } else {
            ","
        };
        let _ = writeln!(
            out,
            "    {{\"fields\":\"{}\",\"reason\":\"{}\",\"count\":{}}}{}",
            json_escape(pair),
            json_escape(reason),
            count,
            comma
        );
    }
    let _ = writeln!(out, "  ],");
    let mut sorted_parts = parts.clone();
    sorted_parts.sort_by(|a, b| a.key.cmp(&b.key));
    let _ = writeln!(out, "  \"containers\": [");
    for (index, container) in sorted_parts.iter().enumerate() {
        let comma = if index + 1 == sorted_parts.len() {
            ""
        } else {
            ","
        };
        let fields_json = container
            .members
            .iter()
            .map(|label| format!("\"{}\"", json_escape(label)))
            .collect::<Vec<_>>()
            .join(",");
        let bytes = align_up(container_end(container, &fields, line), line);
        let offsets = offsets_of(container, &fields, line);
        let placements_json = container
            .members
            .iter()
            .map(|member| {
                format!(
                    "{{\"field\":\"{}\",\"offset\":{},\"size\":{}}}",
                    json_escape(member),
                    offsets[member],
                    fields[member].size
                )
            })
            .collect::<Vec<_>>()
            .join(",");
        let _ = writeln!(
            out,
            "    {{\"container\":\"{}\",\"fields\":[{}],\"instances\":{},\"bytes\":{},\"lines\":{},\"threads\":\"{}\",\"placements\":[{}]}}{}",
            json_escape(&container.key),
            fields_json,
            container.instances,
            bytes,
            container_lines(container, &fields, line),
            thread_text(&container.threads),
            placements_json,
            comma
        );
    }
    let _ = writeln!(out, "  ]");
    let _ = writeln!(out, "}}");

    println!(
        "[plan] {}: {} fields, {} edges, score {:.1} -> {:.1} (gain {:.1}), {} actions",
        out_path,
        fields.len(),
        edges.len(),
        baseline,
        current,
        current - baseline,
        actions.len()
    );
    println!(
        "  footprint {} B -> {} B ({} lines -> {} lines)",
        footprint_before,
        footprint_after,
        footprint_before / line.max(1),
        footprint_after / line.max(1)
    );
    for action in &actions {
        println!(
            "  {} -> {} ({} B, {} lines, {:.1} -> {:.1})",
            action.action,
            action.container,
            action.bytes,
            action.lines,
            action.score_before,
            action.score_after
        );
    }
    if actions.is_empty() {
        println!("  no positive move: keep the current cross-struct layout");
    }
}
