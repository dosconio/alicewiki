//! 地址→字段解析器
//!
//! 维护 alloc_table，并将堆地址映射到结构体字段路径。
//! 对应 memscope 的 address_resolver + cacheline_fs_analyzer 中的字段解析逻辑。

use std::collections::HashMap;

use crate::dwarf::DwarfInfo;
use crate::AllocRecord;

/// 字段信息
#[derive(Clone, Debug, Default)]
pub struct FieldInfo {
    pub name: String,
    pub byte_offset: u64,
    pub byte_size: u64,
    pub type_name: String,
}

/// 分配表：维护所有活跃和已释放的内存分配
pub struct AllocTable {
    records: Vec<AllocRecord>,
    by_addr: HashMap<u64, usize>,
    freed_count: usize,
}

impl AllocTable {
    pub fn new() -> Self {
        Self {
            records: Vec::new(),
            by_addr: HashMap::new(),
            freed_count: 0,
        }
    }

    pub fn insert(&mut self, rec: AllocRecord) {
        let idx = self.records.len();
        self.by_addr.insert(rec.addr, idx);
        self.records.push(rec);
    }

    pub fn mark_freed(&mut self, addr: u64, free_ts: u64) {
        if let Some(&idx) = self.by_addr.get(&addr) {
            if self.records[idx].live {
                self.records[idx].live = false;
                self.records[idx].free_timestamp = free_ts;
                self.freed_count += 1;
            }
        }
    }

    pub fn find(&self, addr: u64) -> Option<&AllocRecord> {
        self.by_addr.get(&addr).map(|&i| &self.records[i])
    }

    pub fn iter_live(&self) -> impl Iterator<Item = &AllocRecord> {
        self.records.iter().filter(|r| r.live)
    }

    pub fn iter_all(&self) -> impl Iterator<Item = &AllocRecord> {
        self.records.iter()
    }

    pub fn total_count(&self) -> usize {
        self.records.len()
    }

    pub fn live_count(&self) -> usize {
        self.records.iter().filter(|r| r.live).count()
    }

    pub fn freed_count(&self) -> usize {
        self.freed_count
    }
}

/// 为给定地址找到所属的分配记录
pub fn find_alloc_for_addr(addr: u64, table: &AllocTable) -> Option<&AllocRecord> {
    // 直接查找（精确匹配）
    if let Some(rec) = table.find(addr) {
        return Some(rec);
    }
    // 线性扫描：地址可能落在分配范围内（非基址）
    for rec in table.iter_live() {
        if rec.addr <= addr && addr < rec.addr + rec.size {
            return Some(rec);
        }
    }
    None
}

/// 解析地址对应的字段路径
///
/// 返回 (type_name, field_path, field_type, infer_method, confidence)
pub fn resolve_field_for_addr(
    addr: u64,
    table: &AllocTable,
    dwarf: &DwarfInfo,
) -> (String, String, String, String, f32) {
    let rec = match find_alloc_for_addr(addr, table) {
        Some(r) => r,
        None => return (
            String::new(),
            format!("+0x{:x}", addr),
            String::new(),
            "no_alloc".into(),
            0.0,
        ),
    };

    let offset = addr - rec.addr;

    // 用 malloc 调用点 PC 查源码行，提取 (type*) 强制转换的精准类型。
    // 不依赖 size 猜测——size 启发式会对 int/double/long double 等同字节数类型误判。
    if let Some(type_name) = infer_type_from_callsite(rec, dwarf) {
        if let Some(ty) = dwarf.find_type_by_name(&type_name) {
            let struct_size = ty.byte_size;
            if struct_size > 0 && rec.size > struct_size {
                // 数组分配：计算元素索引和元素内偏移
                let elem_index = offset / struct_size;
                let elem_offset = offset % struct_size;

                if let Some(field) = dwarf.resolve_field_at_offset(&type_name, elem_offset) {
                    return (
                        type_name.clone(),
                        format!("{}[{}].{}", type_name, elem_index, field.name),
                        field.type_name.clone(),
                        "callsite_cast".into(),
                        0.95,
                    );
                }
                return (
                    type_name.clone(),
                    format!("{}[{}]", type_name, elem_index),
                    String::new(),
                    "callsite_cast".into(),
                    0.9,
                );
            } else if struct_size > 0 {
                // 单元素分配
                if let Some(field) = dwarf.resolve_field_at_offset(&type_name, offset) {
                    return (
                        type_name.clone(),
                        format!("{}.{}", type_name, field.name),
                        field.type_name.clone(),
                        "callsite_cast".into(),
                        0.95,
                    );
                }
                return (
                    type_name.clone(),
                    format!("{}+0x{:x}", type_name, offset),
                    String::new(),
                    "callsite_cast".into(),
                    0.9,
                );
            }
        }
    }

    // callsite 无法精准识别类型，仅返回偏移
    (
        "unknown".into(),
        format!("+0x{:x}", offset),
        String::new(),
        "no_type".into(),
        0.0,
    )
}

/// 为整个分配记录解析字段（用于 CSV 输出，使用基址）
pub fn resolve_field_for_alloc(
    rec: &AllocRecord,
    dwarf: &DwarfInfo,
) -> (String, String, String, String, f32) {
    // 优先用 malloc 调用点 PC 查源码行，提取 (type*) 强制转换的真实类型。
    // 这是精准类型推断，不依赖 size 猜测。
    if let Some(type_name) = infer_type_from_callsite(rec, dwarf) {
        // 找到 DWARF 中对应的类型，确认存在并取 byte_size
        if let Some(ty) = dwarf.find_type_by_name(&type_name) {
            let struct_size = ty.byte_size;
            if struct_size > 0 && rec.size > struct_size {
                // 数组分配
                if let Some(field) = dwarf.resolve_field_at_offset(&type_name, 0) {
                    return (
                        type_name.clone(),
                        format!("{}[0].{}", type_name, field.name),
                        field.type_name.clone(),
                        "callsite_cast".into(),
                        0.95,
                    );
                }
                return (
                    type_name.clone(),
                    format!("{}[0]", type_name),
                    String::new(),
                    "callsite_cast".into(),
                    0.95,
                );
            } else if struct_size > 0 {
                if let Some(field) = dwarf.resolve_field_at_offset(&type_name, 0) {
                    return (
                        type_name.clone(),
                        format!("{}.{}", type_name, field.name),
                        field.type_name.clone(),
                        "callsite_cast".into(),
                        0.95,
                    );
                }
            }
            // 类型已知但 size 不匹配结构体，仍返回真实类型
            return (
                type_name.clone(),
                format!("{}[0]", type_name),
                String::new(),
                "callsite_cast".into(),
                0.9,
            );
        }
    }

    // callsite 解析了 PC 但未匹配到 (type*) 强制转换：
    //   说明该 malloc 调用点在用户代码中无强制转换（C 隐式赋值），
    //   或属于库内部 malloc（如 pthread_create 内部分配）。
    //   不回退 size 猜测——size 启发式会对 int/double/long double 等同字节数类型误判。
    if rec.stack_pcs.iter().any(|pc| dwarf.pc_to_source_line(*pc).is_some()) {
        return (
            "unknown".into(),
            "+0x0".into(),
            String::new(),
            "callsite_no_cast".into(),
            0.0,
        );
    }

    // 完全无 PC（栈采样未覆盖用户代码）：无法精准识别类型。
    // 不回退 size 猜测——size 启发式会对 int/double/long double 等同字节数类型误判
    // （如 4字节 int 被猜成 float，8字节 double 被猜成 long long）。
    (
        "unknown".into(),
        "+0x0".into(),
        String::new(),
        "no_type".into(),
        0.0,
    )
}

/// 从 malloc 调用点 PC 查源码行，提取 (type*) 强制转换的真实类型。
/// 例：源码 `arg->sum = (int *)malloc(dim * sizeof(int))` → 提取 "int"
///      源码 `double *a = malloc(n*n*sizeof(double))` → 提取 "double"
/// 返回 DWARF 中存在的基本类型名，若无法提取返回 None。
fn infer_type_from_callsite(rec: &AllocRecord, dwarf: &DwarfInfo) -> Option<String> {
    // stack_pcs[0] 是手动读 [rsp] 获取的调用者返回地址（用户代码 call malloc 后的 PC）。
    // 后续 pcs[1..] 是 bpf_get_stack 的回溯结果（可能含 libc 内部地址）。
    // 遍历找第一个能解析出源码行的 PC（即用户代码的 malloc 调用点）。
    let mut found: Option<(u64, String, u64)> = None;
    for &pc in &rec.stack_pcs {
        if let Some((file_path, line)) = dwarf.pc_to_source_line(pc) {
            found = Some((pc, file_path, line));
            break;
        }
    }
    let (_pc, file_path, line) = found?;

    // file_path 可能是相对路径（如 ../kmeans-pthread.c）或绝对路径。
    // fsparse_a 由 Makefile 在 binary 所在目录启动（cd $(A)），故相对路径可直接读。
    let content = std::fs::read_to_string(&file_path).ok()?;
    let src_line = content.lines().nth((line as usize).saturating_sub(1))?;

    // 正则匹配 (type *) 或 (type*) 强制转换，type 可能含 const/unsigned/long 等
    // 常见形式：
    //   (int *)malloc(...)
    //   (double*)malloc(...)
    //   (struct foo *)malloc(...)
    //   (unsigned long *)malloc(...)
    //   (char *)calloc(...)
    let re = regex::Regex::new(
        r"\(\s*(?:(const|volatile)\s+)?((?:struct\s+|union\s+)?(?:unsigned\s+|signed\s+)?(?:long\s+|short\s+)*(?:int|char|double|float|void|size_t|long|short|[A-Za-z_][A-Za-z0-9_]*)\*?)\s*\)"
    ).ok()?;
    let caps = re.captures(src_line)?;
    let type_str = caps.get(2)?.as_str().trim();

    // 去掉可能的尾部空格，标准化
    let type_name = type_str.split_whitespace().collect::<Vec<_>>().join(" ");

    // 确认 DWARF 中存在该类型（基本类型或结构体）
    if dwarf.find_type_by_name(&type_name).is_some() {
        return Some(type_name);
    }

    // 尝试去掉 "struct "/"union " 前缀再查
    let clean = type_name
        .trim_start_matches("struct ")
        .trim_start_matches("union ");
    if dwarf.find_type_by_name(clean).is_some() {
        return Some(clean.to_string());
    }

    // 对于 "int *" 这类，DWARF 基本类型名就是 "int"
    // type_name 可能是 "int *"，取去掉 * 的部分
    let base = type_name.trim_end_matches('*').trim();
    if dwarf.find_type_by_name(base).is_some() {
        return Some(base.to_string());
    }

    None
}

