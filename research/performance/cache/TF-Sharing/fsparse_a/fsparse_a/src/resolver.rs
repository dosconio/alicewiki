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

    pub fn mark_freed(&mut self, addr: u64) {
        if let Some(&idx) = self.by_addr.get(&addr) {
            if self.records[idx].live {
                self.records[idx].live = false;
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

    // 尝试根据分配大小推断类型
    if let Some((type_name, ty)) = infer_type_by_size(rec.size, dwarf) {
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
                    "array_size_match".into(),
                    0.85,
                );
            }
            return (
                type_name.clone(),
                format!("{}[{}]+0x{:x}", type_name, elem_index, elem_offset),
                String::new(),
                "array_size_match".into(),
                0.5,
            );
        } else if struct_size > 0 {
            // 单元素分配
            if let Some(field) = dwarf.resolve_field_at_offset(&type_name, offset) {
                return (
                    type_name.clone(),
                    format!("{}.{}", type_name, field.name),
                    field.type_name.clone(),
                    "size_match".into(),
                    0.75,
                );
            }
            return (
                type_name.clone(),
                format!("{}+0x{:x}", type_name, offset),
                String::new(),
                "size_match".into(),
                0.4,
            );
        }
    }

    // 无法推断类型，仅返回偏移
    (
        String::new(),
        format!("+0x{:x}", offset),
        String::new(),
        "offset_only".into(),
        0.1,
    )
}

/// 为整个分配记录解析字段（用于 CSV 输出，使用基址）
pub fn resolve_field_for_alloc(
    rec: &AllocRecord,
    dwarf: &DwarfInfo,
) -> (String, String, String, String, f32) {
    // 尝试根据分配大小推断类型
    if let Some((type_name, ty)) = infer_type_by_size(rec.size, dwarf) {
        let struct_size = ty.byte_size;
        if struct_size > 0 && rec.size > struct_size {
            // 数组分配：基址对应 [0]
            // 注意：不添加 (ambiguous) 后缀，cacheline_fs_analyzer 会根据
            // 地址偏移和结构体布局重新计算具体字段（如 lreg_args[3].SXY）
            if let Some(field) = dwarf.resolve_field_at_offset(&type_name, 0) {
                return (
                    type_name.clone(),
                    format!("{}[0].{}", type_name, field.name),
                    field.type_name.clone(),
                    "array_size_match".into(),
                    0.15,
                );
            }
            return (
                type_name.clone(),
                format!("{}[0]", type_name),
                String::new(),
                "array_size_match".into(),
                0.15,
            );
        } else if struct_size > 0 {
            if let Some(field) = dwarf.resolve_field_at_offset(&type_name, 0) {
                return (
                    type_name.clone(),
                    format!("{}.{}", type_name, field.name),
                    field.type_name.clone(),
                    "size_match".into(),
                    0.3,
                );
            }
        }
    }

    (
        String::new(),
        format!("+0x0"),
        String::new(),
        "no_type".into(),
        0.0,
    )
}

/// 根据分配大小推断类型（与 memscope 的 size_index 类似）
fn infer_type_by_size<'a>(
    size: u64,
    dwarf: &'a DwarfInfo,
) -> Option<(String, &'a crate::dwarf::DwarfType)> {
    // 优先匹配：分配大小是某结构体大小的整数倍
    let mut best: Option<(&String, &crate::dwarf::DwarfType)> = None;
    let mut best_ratio = u64::MAX;

    for (name, ty) in dwarf.types.iter() {
        if ty.byte_size == 0 || ty.fields.is_empty() {
            continue;
        }
        if size % ty.byte_size == 0 {
            let ratio = size / ty.byte_size;
            // 偏好 ratio 较小（即结构体较大）的匹配
            if ratio < best_ratio || best.is_none() {
                best_ratio = ratio;
                best = Some((name, ty));
            }
        }
    }

    best.map(|(n, t)| (n.clone(), t))
}
