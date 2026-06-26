//! DWARF 调试信息解析（基于 gimli + object）
//!
//! 提取结构体布局、全局变量符号、类型信息，
//! 供地址→字段解析使用。

use std::collections::BTreeMap;
use std::collections::HashMap;
use std::fs::File;
use std::path::Path;

use gimli::{
    AttributeValue, DW_AT_byte_size, DW_AT_data_member_location, DW_AT_encoding, DW_AT_location,
    DW_AT_name, DW_AT_type, DW_TAG_array_type, DW_TAG_base_type, DW_TAG_member,
    DW_TAG_pointer_type, DW_TAG_structure_type, DW_TAG_typedef, DW_TAG_union_type,
    DW_TAG_variable, EndianSlice, LittleEndian, SectionId,
};
use object::{Object, ObjectSection};

use crate::resolver::FieldInfo;

/// 单个结构体字段
#[derive(Clone, Debug)]
pub struct DwarfField {
    pub name: String,
    pub byte_offset: u64,
    pub byte_size: u64,
    pub type_name: String,
    pub is_bitfield: bool,
    pub bit_offset: u64,
    pub bit_size: u64,
}

/// 类型信息
#[derive(Clone, Debug)]
pub struct DwarfType {
    pub name: String,
    pub byte_size: u64,
    pub fields: Vec<DwarfField>,
    pub kind: TypeKind,
    /// DWARF DW_AT_encoding，仅对 base_type 有意义。DW_ATE_float=4 表示浮点。
    /// 0 表示未设置（非 base_type）。
    pub encoding: u64,
}

#[derive(Clone, Debug, PartialEq)]
pub enum TypeKind {
    Struct,
    Union,
    Typedef,
    Base,
    Pointer,
    Array,
    Other,
}

impl DwarfType {
    pub fn layout_string(&self) -> String {
        let mut s = format!("struct {} (size={}):\n", self.name, self.byte_size);
        for f in &self.fields {
            s.push_str(&format!(
                "  +0x{:04x} {:<24} {} (size={})\n",
                f.byte_offset, f.name, f.type_name, f.byte_size
            ));
        }
        s
    }
}

/// 全局变量符号
#[derive(Clone, Debug)]
pub struct GlobalVar {
    pub name: String,
    pub address: u64,
    pub size: u64,
    pub type_name: String,
}

type DwarfReader = EndianSlice<'static, LittleEndian>;

/// DWARF 信息容器
pub struct DwarfInfo {
    pub types: HashMap<String, DwarfType>,
    pub types_by_offset: HashMap<u64, DwarfType>,
    pub globals: Vec<GlobalVar>,
    pub globals_by_name: HashMap<String, usize>,
    pub globals_by_addr: BTreeMap<u64, usize>,
    offset_to_name: HashMap<u64, String>,
    /// addr2line 上下文，用于 PC→源码行查询（精准类型推断）
    addr2line: Option<addr2line::Context<gimli::EndianSlice<'static, gimli::LittleEndian>>>,
}

impl DwarfInfo {
    pub fn load(path: &Path) -> anyhow::Result<Self> {
        let file = File::open(path)?;
        let mmap = unsafe { memmap2::Mmap::map(&file)? };
        let object = object::File::parse(&*mmap)?;

        // 收集 section 数据，泄漏为 'static
        let mut section_data: HashMap<&'static str, &'static [u8]> = HashMap::new();
        for section in object.sections() {
            if let Ok(name) = section.name() {
                if name.starts_with(".debug_") {
                    let data = section.data().unwrap_or(&[]).to_vec();
                    let leaked: &'static [u8] = Box::leak(data.into_boxed_slice());
                    let leaked_name: &'static str =
                        Box::leak(name.to_string().into_boxed_str());
                    section_data.insert(leaked_name, leaked);
                }
            }
        }

        let endian = LittleEndian;
        let dwarf = gimli::Dwarf::load(|id: SectionId| -> Result<DwarfReader, gimli::Error> {
            let name = id.name();
            let data = section_data.get(name).copied().unwrap_or(&[]);
            Ok(EndianSlice::new(data, endian))
        })?;

        let mut types = HashMap::new();
        let mut types_by_offset = HashMap::new();
        let mut globals = Vec::new();
        let mut globals_by_name = HashMap::new();
        let mut globals_by_addr = BTreeMap::new();
        let mut offset_to_name: HashMap<u64, String> = HashMap::new();

        // 第一遍：收集所有类型
        let mut iter = dwarf.units();
        while let Some(header) = iter.next()? {
            let unit = dwarf.unit(header)?;
            let mut entries = unit.entries();
            while let Some((_, entry)) = entries.next_dfs()? {
                let tag = entry.tag();
                let offset = entry.offset().0 as u64;
                match tag {
                    DW_TAG_structure_type | DW_TAG_union_type => {
                        if let Some(ty) = parse_type(&dwarf, &unit, entry)? {
                            if !ty.name.is_empty() {
                                offset_to_name.insert(offset, ty.name.clone());
                                types.insert(ty.name.clone(), ty.clone());
                            }
                            types_by_offset.insert(offset, ty);
                        }
                    }
                    DW_TAG_typedef | DW_TAG_base_type | DW_TAG_pointer_type
                    | DW_TAG_array_type => {
                        if let Some(ty) = parse_simple_type(&dwarf, entry)? {
                            if !ty.name.is_empty() {
                                offset_to_name.insert(offset, ty.name.clone());
                                if !types.contains_key(&ty.name) {
                                    types.insert(ty.name.clone(), ty.clone());
                                }
                            }
                            types_by_offset.insert(offset, ty);
                        }
                    }
                    _ => {}
                }
            }
        }

        // 第二遍：解析全局变量
        let mut iter = dwarf.units();
        while let Some(header) = iter.next()? {
            let unit = dwarf.unit(header)?;
            let mut entries = unit.entries();
            while let Some((_, entry)) = entries.next_dfs()? {
                if entry.tag() == DW_TAG_variable {
                    if let Some(g) = parse_global(&dwarf, &unit, entry)? {
                        globals_by_name.insert(g.name.clone(), globals.len());
                        if g.address != 0 {
                            globals_by_addr.insert(g.address, globals.len());
                        }
                        globals.push(g);
                    }
                }
            }
        }

        // 构建 addr2line 上下文，用于 PC→源码行精准查询
        // section data 已泄漏为 'static，故 Context 也是 'static
        let addr2line_ctx = addr2line::Context::from_dwarf(dwarf).ok();

        Ok(Self {
            types,
            types_by_offset,
            globals,
            globals_by_name,
            globals_by_addr,
            offset_to_name,
            addr2line: addr2line_ctx,
        })
    }

    pub fn types_len(&self) -> usize {
        self.types.len()
    }

    pub fn symbols_len(&self) -> usize {
        self.globals.len()
    }

    pub fn find_type_by_name(&self, name: &str) -> Option<&DwarfType> {
        let clean = if let Some(idx) = name.find(" (") {
            &name[..idx]
        } else {
            name
        };
        self.types.get(clean)
    }

    pub fn iter_types(&self) -> impl Iterator<Item = &DwarfType> {
        self.types.values()
    }

    pub fn find_global_by_addr(&self, addr: u64) -> Option<&GlobalVar> {
        let idx = self.globals_by_addr.range(..=addr).next_back()?.1;
        Some(&self.globals[*idx])
    }

    pub fn find_global_by_name(&self, name: &str) -> Option<&GlobalVar> {
        self.globals_by_name.get(name).map(|&i| &self.globals[i])
    }

    /// 根据 PC（malloc 调用点）查询源码文件路径和行号。
    /// 用于精准类型推断：从 malloc 调用点的源码行提取 (type*) 强制转换。
    pub fn pc_to_source_line(&self, pc: u64) -> Option<(String, u64)> {
        let ctx = self.addr2line.as_ref()?;
        let loc = ctx.find_location(pc).ok().flatten()?;
        let file = loc.file?.to_string();
        let line = loc.line? as u64;
        Some((file, line))
    }

    /// 根据类型名和字节偏移解析字段
    pub fn resolve_field_at_offset(&self, type_name: &str, offset: u64) -> Option<FieldInfo> {
        let ty = self.find_type_by_name(type_name)?;
        if ty.fields.is_empty() {
            return None;
        }

        let mut best: Option<&DwarfField> = None;
        for f in &ty.fields {
            if f.byte_offset <= offset {
                if best.is_none() || f.byte_offset >= best.unwrap().byte_offset {
                    best = Some(f);
                }
            } else {
                break;
            }
        }

        let f = best?;
        Some(FieldInfo {
            name: f.name.clone(),
            byte_offset: f.byte_offset,
            byte_size: f.byte_size,
            type_name: f.type_name.clone(),
        })
    }
}

fn parse_type(
    dwarf: &gimli::Dwarf<DwarfReader>,
    unit: &gimli::Unit<DwarfReader>,
    entry: &gimli::DebuggingInformationEntry<DwarfReader>,
) -> anyhow::Result<Option<DwarfType>> {
    let name = get_name_attr(dwarf, entry)?;
    let byte_size = get_byte_size_attr(dwarf, entry)?;

    let kind = match entry.tag() {
        DW_TAG_structure_type => TypeKind::Struct,
        DW_TAG_union_type => TypeKind::Union,
        _ => TypeKind::Other,
    };

    // 遍历子条目
    let mut fields = Vec::new();
    let mut child_iter = unit.entries_at_offset(entry.offset())?;
    // 跳过当前 entry 本身
    let _ = child_iter.next_entry();

    // next_dfs 返回 delta_depth（相对前一节点的深度变化）。
    // 父节点已消耗，第一个子节点 delta=1；兄弟节点 delta=0；离开子树 delta=-1。
    // 累计得到绝对深度，当深度 < 1 表示已离开父节点的直接子节点范围。
    let mut abs_depth = 0isize;
    while let Some((delta, child)) = child_iter.next_dfs()? {
        abs_depth += delta;
        if abs_depth < 1 {
            break;
        }
        if child.tag() == DW_TAG_member {
            if let Some(f) = parse_member(dwarf, unit, child)? {
                fields.push(f);
            }
        }
    }

    fields.sort_by_key(|f| f.byte_offset);

    Ok(Some(DwarfType {
        name,
        byte_size,
        fields,
        kind,
        encoding: 0,
    }))
}

fn parse_simple_type(
    dwarf: &gimli::Dwarf<DwarfReader>,
    entry: &gimli::DebuggingInformationEntry<DwarfReader>,
) -> anyhow::Result<Option<DwarfType>> {
    let name = match get_name_attr(dwarf, entry)? {
        n if !n.is_empty() => n,
        _ => return Ok(None),
    };
    let byte_size = get_byte_size_attr_simple(entry);
    let encoding = entry
        .attr_value(DW_AT_encoding)
        .ok()
        .flatten()
        .and_then(|v| match v {
            // DW_AT_encoding 以 gimli::constants::DwAte(u8) 形式存储
            AttributeValue::Encoding(ate) => Some(ate.0 as u64),
            _ => udata_to_u64(v),
        })
        .unwrap_or(0);

    let kind = match entry.tag() {
        DW_TAG_typedef => TypeKind::Typedef,
        DW_TAG_base_type => TypeKind::Base,
        DW_TAG_pointer_type => TypeKind::Pointer,
        DW_TAG_array_type => TypeKind::Array,
        _ => TypeKind::Other,
    };

    Ok(Some(DwarfType {
        name,
        byte_size,
        fields: Vec::new(),
        kind,
        encoding,
    }))
}

fn parse_member(
    dwarf: &gimli::Dwarf<DwarfReader>,
    unit: &gimli::Unit<DwarfReader>,
    entry: &gimli::DebuggingInformationEntry<DwarfReader>,
) -> anyhow::Result<Option<DwarfField>> {
    let name = match get_name_attr(dwarf, entry)? {
        n if !n.is_empty() => n,
        _ => return Ok(None),
    };

    let byte_offset = entry
        .attr_value(DW_AT_data_member_location)?
        .and_then(udata_to_u64)
        .unwrap_or(0);

    // 成员本身没有 byte_size，需通过 DW_AT_type 引用解析目标类型的大小。
    let (type_name, type_byte_size) = resolve_type_ref(dwarf, unit, entry)?;
    // 成员条目通常无 byte_size，优先使用类型引用解析出的大小
    let own_size = get_byte_size_attr(dwarf, entry).unwrap_or(0);
    let byte_size = if own_size > 0 { own_size } else { type_byte_size };

    let bit_size = entry
        .attr_value(gimli::DW_AT_bit_size)?
        .and_then(udata_to_u64)
        .unwrap_or(0);
    let is_bitfield = bit_size > 0;
    let bit_offset = entry
        .attr_value(gimli::DW_AT_bit_offset)?
        .and_then(udata_to_u64)
        .unwrap_or(0);

    Ok(Some(DwarfField {
        name,
        byte_offset,
        byte_size,
        type_name,
        is_bitfield,
        bit_offset,
        bit_size,
    }))
}

fn parse_global(
    dwarf: &gimli::Dwarf<DwarfReader>,
    unit: &gimli::Unit<DwarfReader>,
    entry: &gimli::DebuggingInformationEntry<DwarfReader>,
) -> anyhow::Result<Option<GlobalVar>> {
    let name = match get_name_attr(dwarf, entry)? {
        n if !n.is_empty() => n,
        _ => return Ok(None),
    };

    let address = parse_location(entry).unwrap_or(0);
    let size = get_byte_size_attr(dwarf, entry).unwrap_or(0);
    let type_name = get_type_name(dwarf, unit, entry).unwrap_or_default();

    Ok(Some(GlobalVar {
        name,
        address,
        size,
        type_name,
    }))
}

fn parse_location(entry: &gimli::DebuggingInformationEntry<DwarfReader>) -> Option<u64> {
    let attr = entry.attr(DW_AT_location).ok()??;
    if let AttributeValue::Exprloc(e) = attr.value() {
        let bytes: &[u8] = &*e.0;
        if bytes.len() >= 9 && bytes[0] == 0x03 {
            // DW_OP_addr
            let mut arr = [0u8; 8];
            arr.copy_from_slice(&bytes[1..9]);
            return Some(u64::from_le_bytes(arr));
        }
    }
    None
}

fn get_name_attr(
    dwarf: &gimli::Dwarf<DwarfReader>,
    entry: &gimli::DebuggingInformationEntry<DwarfReader>,
) -> anyhow::Result<String> {
    if let Some(attr) = entry.attr(DW_AT_name)? {
        match attr.value() {
            AttributeValue::String(s) => {
                let bytes: &[u8] = &*s;
                return Ok(String::from_utf8_lossy(bytes).into_owned());
            }
            AttributeValue::DebugStrRef(offset) => {
                if let Ok(s) = dwarf.debug_str.get_str(offset) {
                    let bytes: &[u8] = &*s;
                    return Ok(String::from_utf8_lossy(bytes).into_owned());
                }
            }
            _ => {}
        }
    }
    Ok(String::new())
}

fn get_string_attr_simple(
    entry: &gimli::DebuggingInformationEntry<DwarfReader>,
) -> anyhow::Result<String> {
    if let Some(attr) = entry.attr(DW_AT_name)? {
        if let AttributeValue::String(s) = attr.value() {
            let bytes: &[u8] = &*s;
            return Ok(String::from_utf8_lossy(bytes).into_owned());
        }
    }
    Ok(String::new())
}

fn get_byte_size_attr(
    _dwarf: &gimli::Dwarf<DwarfReader>,
    entry: &gimli::DebuggingInformationEntry<DwarfReader>,
) -> anyhow::Result<u64> {
    if let Some(v) = entry.attr_value(DW_AT_byte_size)?.and_then(udata_to_u64) {
        return Ok(v);
    }
    Ok(0)
}

fn get_byte_size_attr_simple(entry: &gimli::DebuggingInformationEntry<DwarfReader>) -> u64 {
    entry
        .attr_value(DW_AT_byte_size)
        .ok()
        .flatten()
        .and_then(udata_to_u64)
        .unwrap_or(0)
}

fn udata_to_u64(v: AttributeValue<DwarfReader>) -> Option<u64> {
    match v {
        AttributeValue::Udata(d) => Some(d),
        AttributeValue::Data1(d) => Some(d as u64),
        AttributeValue::Data2(d) => Some(d as u64),
        AttributeValue::Data4(d) => Some(d as u64),
        AttributeValue::Data8(d) => Some(d),
        AttributeValue::Sdata(d) => Some(d as u64),
        _ => None,
    }
}

fn get_type_name(
    dwarf: &gimli::Dwarf<DwarfReader>,
    unit: &gimli::Unit<DwarfReader>,
    entry: &gimli::DebuggingInformationEntry<DwarfReader>,
) -> anyhow::Result<String> {
    let (name, _) = resolve_type_ref(dwarf, unit, entry)?;
    Ok(name)
}

/// 解析 DW_AT_type 引用，返回 (类型名, 字节大小)。
/// 支持链式 typedef 解引用以获取底层类型的大小。
fn resolve_type_ref(
    dwarf: &gimli::Dwarf<DwarfReader>,
    unit: &gimli::Unit<DwarfReader>,
    entry: &gimli::DebuggingInformationEntry<DwarfReader>,
) -> anyhow::Result<(String, u64)> {
    let Some(attr) = entry.attr(DW_AT_type)? else {
        return Ok((String::new(), 0));
    };
    match attr.value() {
        AttributeValue::UnitRef(offset) => {
            resolve_type_at_offset(dwarf, unit, offset)
        }
        AttributeValue::DebugInfoRef(offset) => {
            Ok((format!("<ref_{}>", offset.0), 0))
        }
        _ => Ok((String::new(), 0)),
    }
}

/// 在给定 offset 处解析类型条目，返回 (类型名, 字节大小)。
/// 对 typedef/pointer 等通过 DW_AT_type 链式解引用以获取最终大小。
fn resolve_type_at_offset(
    dwarf: &gimli::Dwarf<DwarfReader>,
    unit: &gimli::Unit<DwarfReader>,
    offset: gimli::UnitOffset,
) -> anyhow::Result<(String, u64)> {
    let mut entries = unit.entries_at_offset(offset)?;
    // entries_at_offset 定位到 offset 处，next_entry 推进到该条目，current() 获取它
    if entries.next_entry()?.is_some() {
        if let Some(e) = entries.current() {
            if e.offset() == offset {
                let name = get_name_attr(dwarf, e)?;
                let byte_size = get_byte_size_attr(dwarf, e).unwrap_or(0);
                // typedef / pointer 等可能本身无 byte_size，继续解引用 DW_AT_type
                if byte_size == 0 {
                    if let Some(attr) = e.attr(DW_AT_type)? {
                        if let AttributeValue::UnitRef(inner) = attr.value() {
                            let (_, inner_size) = resolve_type_at_offset(dwarf, unit, inner)?;
                            if inner_size > 0 {
                                return Ok((name, inner_size));
                            }
                        }
                    }
                }
                return Ok((name, byte_size));
            }
        }
    }
    Ok((String::new(), 0))
}
