#!/usr/bin/env python3
"""
CacheLine False Sharing 分析器

功能：将 hitm_a.txt/hitm.txt 中的 FS 事件按 cache line 分组，
推断每个地址所属的字段/数组，输出格式: 字段1,字段2,FS次数

推断方法（按优先级）：
  1. memscope 地址映射（最精确，需要同一进程）
  2. 源码行号投票（基于 perfparse_a 输出的源码信息）
  3. RIP + addr2line 解析（精确 BSS 指针匹配）
  4. 地址范围分组 + 布局推断（基于 numOptions 的偏移映射）
  5. 邻居推断（同一 cache line 内的已解析地址）

用法：
  python3 cacheline_fs_analyzer.py --work-dir /path/to/work --results-dir /path/to/results
  python3 cacheline_fs_analyzer.py --work-dir /path/to/work --num-options 65536
"""

import os
import sys
import csv
import re
import argparse
import subprocess
from collections import defaultdict
from typing import Dict, List, Tuple, Optional, Set, Union

CACHE_LINE_SIZE = 64

# ---------------------------------------------------------------------------
# DWARF 驱动的结构体布局（运行时从二进制提取，替代硬编码的 STRUCT_LAYOUTS）
# ---------------------------------------------------------------------------

_struct_layouts_cache: Dict[str, Dict[int, str]] = {}
_struct_sizes_cache: Dict[str, int] = {}


def _extract_struct_layouts(binary_path: Optional[str]) -> Tuple[Dict[str, Dict[int, str]], Dict[str, int]]:
    """从二进制文件的 DWARF 调试信息中提取所有结构体布局。
    
    返回 (layouts, sizes) 其中:
      layouts: {struct_name: {byte_offset: field_name, ...}, ...}
      sizes:   {struct_name: byte_size, ...}
    """
    if not binary_path or not os.path.isfile(binary_path):
        return {}, {}
    
    cache_key = os.path.realpath(binary_path)
    if cache_key in _struct_layouts_cache:
        return _struct_layouts_cache[cache_key], _struct_sizes_cache.get(cache_key, {})

    layouts: Dict[str, Dict[int, str]] = {}
    sizes: Dict[str, int] = {}

    try:
        # 优先使用 dwarfdump（输出更易解析，名字已解析）
        result = subprocess.run(
            ['dwarfdump', binary_path],
            capture_output=True, text=True, timeout=60
        )
        if result.returncode == 0 and result.stdout:
            return _parse_dwarfdump_output(result.stdout, cache_key)
    except Exception:
        pass

    # dwarfdump 不可用时回退到 readelf
    try:
        result = subprocess.run(
            ['readelf', '--debug-dump=info', '-w', binary_path],
            capture_output=True, text=True, timeout=60
        )
        if result.returncode == 0 and result.stdout:
            return _parse_readelf_dwarf_output(result.stdout, cache_key)
    except Exception:
        pass

    _struct_layouts_cache[cache_key] = layouts
    _struct_sizes_cache[cache_key] = sizes
    return layouts, sizes


def _parse_dwarfdump_output(output: str, cache_key: str) -> Tuple[Dict[str, Dict[int, str]], Dict[str, int]]:
    """解析 dwarfdump 输出，格式如:
    < 1><0x...>    DW_TAG_structure_type
                      DW_AT_name                  StructName
                      DW_AT_byte_size             N
    < 2><0x...>      DW_TAG_member
                        DW_AT_name                  field_name
                        DW_AT_data_member_location  offset
    """
    layouts: Dict[str, Dict[int, str]] = {}
    sizes: Dict[str, int] = {}

    lines = output.split('\n')
    i = 0
    while i < len(lines):
        line = lines[i]
        # 寻找 DW_TAG_structure_type
        if 'DW_TAG_structure_type' in line:
            struct_name = None
            struct_size = 0
            members: Dict[int, str] = {}

            j = i + 1
            current_member_name = None
            current_member_offset = 0
            in_member = False

            while j < len(lines):
                l = lines[j]
                # 下一个顶级 TAG（非 member）则结束当前结构体
                if re.match(r'\s*<\s*1>\s*<', l) and 'DW_TAG_member' not in l:
                    break
                # 检测 DW_TAG_member 开始/结束
                if re.match(r'\s*<\s*2>\s*<', l) and 'DW_TAG_member' in l:
                    # 保存上一个 member
                    if current_member_name and current_member_name != '_vptr':
                        members[current_member_offset] = current_member_name
                    current_member_name = None
                    current_member_offset = 0
                    in_member = True
                    j += 1
                    continue
                if re.match(r'\s*<\s*[23]>\s*<', l) and 'DW_TAG_' in l and 'DW_TAG_member' not in l:
                    if current_member_name and current_member_name != '_vptr':
                        members[current_member_offset] = current_member_name
                    current_member_name = None
                    in_member = False
                    j += 1
                    continue

                # 解析属性
                m = re.search(r'DW_AT_name\s+(.+)', l)
                if m:
                    name_val = m.group(1).strip()
                    # 去掉可能的前缀 "(indirect string, ...)：" 或类似
                    name_val = re.sub(r'^\([^)]*\)\s*:\s*', '', name_val)
                    # 去掉引用部分 "Refers to: ..."
                    name_val = re.sub(r'\s+Refers to:\s+.+$', '', name_val)
                    name_val = re.sub(r'^typedef\s+', '', name_val)
                    if not in_member and struct_name is None:
                        struct_name = name_val
                    elif in_member:
                        current_member_name = name_val

                m = re.search(r'DW_AT_byte_size\s+(-?\d+)', l)
                if m:
                    struct_size = abs(int(m.group(1)))

                m = re.search(r'DW_AT_data_member_location\s+(-?\d+)', l)
                if m:
                    current_member_offset = max(0, int(m.group(1)))

                j += 1

            # 保存最后一个 member
            if current_member_name and current_member_name != '_vptr':
                members[current_member_offset] = current_member_name

            if struct_name and members:
                # 过滤掉标准库类型
                if not struct_name.startswith('_') or struct_name.startswith('_IO_'):
                    layouts[struct_name] = members
                    sizes[struct_name] = struct_size

            i = j - 1
        i += 1

    _struct_layouts_cache[cache_key] = layouts
    _struct_sizes_cache[cache_key] = sizes
    return layouts, sizes


def _parse_readelf_dwarf_output(output: str, cache_key: str) -> Tuple[Dict[str, Dict[int, str]], Dict[str, int]]:
    """解析 readelf --debug-dump=info 输出（回退方案）"""
    layouts: Dict[str, Dict[int, str]] = {}
    sizes: Dict[str, int] = {}

    lines = output.split('\n')
    i = 0
    while i < len(lines):
        line = lines[i]
        if 'DW_TAG_structure_type' in line:
            struct_name = None
            struct_size = 0
            members: Dict[int, str] = {}
            in_member = False

            j = i + 1
            current_member_name = None
            current_member_offset = 0

            while j < len(lines):
                l = lines[j]
                # 下一个 <1> 级别的 TAG（非 member）结束当前结构体
                if re.search(r'<\s*1\s*><', l) and 'DW_TAG_member' not in l and 'DW_TAG_structure_type' not in l:
                    if current_member_name and current_member_name != '_vptr':
                        members[current_member_offset] = current_member_name
                    break

                if 'DW_TAG_member' in l and re.search(r'<\s*2\s*><', l):
                    if current_member_name and current_member_name != '_vptr':
                        members[current_member_offset] = current_member_name
                    current_member_name = None
                    current_member_offset = 0
                    in_member = True
                    j += 1
                    continue
                if re.search(r'<\s*[23]\s*><', l) and 'DW_TAG_' in l and 'DW_TAG_member' not in l:
                    if current_member_name and current_member_name != '_vptr':
                        members[current_member_offset] = current_member_name
                    current_member_name = None
                    in_member = False
                    j += 1
                    continue

                m = re.search(r'DW_AT_name\s*:\s*\([^)]*\):\s*(.+)', l)
                if not m:
                    m = re.search(r'DW_AT_name\s*:\s*(.+)', l)
                if m:
                    name_val = m.group(1).strip()
                    if not in_member and struct_name is None:
                        struct_name = name_val
                    elif in_member:
                        current_member_name = name_val

                m = re.search(r'DW_AT_byte_size\s*:\s*(\d+)', l)
                if m:
                    struct_size = int(m.group(1))

                m = re.search(r'DW_AT_data_member_location\s*:\s*(\d+)', l)
                if m:
                    current_member_offset = int(m.group(1))

                j += 1

            if current_member_name and current_member_name != '_vptr':
                members[current_member_offset] = current_member_name

            if struct_name and members:
                if not struct_name.startswith('_') or struct_name.startswith('_IO_'):
                    layouts[struct_name] = members
                    sizes[struct_name] = struct_size

            i = j - 1
        i += 1

    _struct_layouts_cache[cache_key] = layouts
    _struct_sizes_cache[cache_key] = sizes
    return layouts, sizes


def addr_to_cacheline(addr: int) -> int:
    return (addr // CACHE_LINE_SIZE) * CACHE_LINE_SIZE


def parse_hitm_file(hitm_path: str) -> List[Dict]:
    events = []
    if not os.path.exists(hitm_path):
        print(f"错误：找不到文件 {hitm_path}")
        return events

    with open(hitm_path, 'r') as f:
        for line in f:
            line = line.strip()
            if not line:
                continue

            parts = line.split(',', 3)
            if len(parts) < 3:
                continue

            sharing_type = parts[0]
            region = parts[1]
            addr_info = parts[2]
            rest = parts[3] if len(parts) > 3 else ''

            if sharing_type != 'F' or region != 'A':
                continue

            count_match = re.match(r'(\d+)', rest)
            count = int(count_match.group(1)) if count_match else 0

            if '<->' in addr_info:
                addr1_str, addr2_str = addr_info.split('<->', 1)
            elif ' ' in addr_info:
                addr1_str, addr2_str = addr_info.split(' ', 1)
            else:
                continue

            try:
                addr1 = int(addr1_str.strip(), 16)
                addr2 = int(addr2_str.strip(), 16)
            except ValueError:
                continue

            source_info = rest.split(':', 1)[1].strip() if ':' in rest else ''

            events.append({
                'addr1': addr1,
                'addr2': addr2,
                'count': count,
                'source': source_info,
                'raw': line
            })

    return events


def detect_pie_base(hitm_path: str, binary_path: str) -> Optional[int]:
    if not os.path.exists(hitm_path) or not os.path.exists(binary_path):
        return None

    result = subprocess.run(
        ['nm', '-n', binary_path],
        capture_output=True, text=True, timeout=5
    )
    text_symbols = []
    for line in result.stdout.strip().split('\n'):
        parts = line.split()
        if len(parts) >= 3 and parts[1] == 'T':
            try:
                addr = int(parts[0], 16)
                text_symbols.append((addr, parts[2]))
            except ValueError:
                pass

    if not text_symbols:
        return None

    min_sym_addr = min(a for a, _ in text_symbols)
    max_sym_addr = max(a for a, _ in text_symbols)

    rips = set()
    with open(hitm_path, 'r') as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            parts = line.split(',', 3)
            if len(parts) < 4:
                continue
            source_info = parts[3].split(':', 1)[1].strip() if ':' in parts[3] else ''
            for rip_str in source_info.split(','):
                rip_str = rip_str.strip().replace('0x', '').replace('0X', '')
                if rip_str and all(c in '0123456789abcdefABCDEF' for c in rip_str):
                    try:
                        rips.add(int(rip_str, 16))
                    except ValueError:
                        pass

    if not rips:
        return None

    min_rip = min(rips)
    max_rip = max(rips)

    for sym_addr, sym_name in text_symbols:
        pie_base_candidate = min_rip - sym_addr
        if pie_base_candidate <= 0:
            continue
        
        pie_base_aligned = (pie_base_candidate // 0x1000) * 0x1000
        if pie_base_aligned <= 0:
            continue
        
        test_offset = min_rip - pie_base_aligned
        result2 = subprocess.run(
            ['addr2line', '-e', binary_path, '-f', hex(test_offset)],
            capture_output=True, text=True, timeout=2
        )
        if result2.returncode == 0:
            output = result2.stdout.strip()
            lines = output.split('\n')
            if len(lines) >= 1 and '??' not in lines[0] and '?' not in lines[0]:
                return pie_base_aligned

    return None


def infer_fields_from_rip(hitm_path: str, binary_path: str, pie_base: int = None) -> Dict[int, Dict[str, int]]:
    addr_field_votes = defaultdict(lambda: defaultdict(int))

    if not os.path.exists(hitm_path) or not os.path.exists(binary_path):
        return addr_field_votes

    if pie_base is None:
        pie_base = detect_pie_base(hitm_path, binary_path)
        if pie_base:
            print(f"  自动检测 PIE 基地址: 0x{pie_base:x}")
        else:
            print("  无法自动检测 PIE 基地址，尝试直接解析...")
            pie_base = 0

    bss_map = build_rip_to_bss_map(binary_path)
    if bss_map:
        bss_count = len(set(bss_map.values()))
        print(f"  从反汇编中解析到 {len(bss_map)} 条指令, {bss_count} 个 BSS 全局指针")

    rip_cache = {}

    with open(hitm_path, 'r') as f:
        for line in f:
            line = line.strip()
            if not line:
                continue

            parts = line.split(',', 3)
            if len(parts) < 3:
                continue

            sharing_type = parts[0]
            region = parts[1]
            addr_info = parts[2]
            rest = parts[3] if len(parts) > 3 else ''

            if sharing_type not in ('F', 'T') or region != 'A':
                continue

            addrs = re.findall(r'0x[0-9a-fA-F]+', addr_info)

            source_info = rest.split(':', 1)[1].strip() if ':' in rest else ''
            if not source_info:
                continue

            rips = [r.strip() for r in source_info.split(',') if r.strip()]

            for rip_str in rips:
                rip_clean = rip_str.replace('0x', '').replace('0X', '')
                if not rip_clean or not all(c in '0123456789abcdefABCDEF' for c in rip_clean):
                    continue

                if rip_clean in rip_cache:
                    field_name = rip_cache[rip_clean]
                else:
                    result = resolve_rip_to_field(rip_clean, binary_path, pie_base, bss_map)
                    field_name = result[0] if result else None
                    rip_cache[rip_clean] = field_name

                if field_name:
                    for addr_str in addrs:
                        try:
                            addr = int(addr_str, 16)
                            addr_field_votes[addr][field_name] += 1
                        except ValueError:
                            pass

    return addr_field_votes


def build_rip_to_bss_map(binary_path: str) -> Dict[int, str]:
    rip_to_bss = {}
    if not os.path.exists(binary_path):
        return rip_to_bss

    valid_bss_symbols = set()
    nm_result = subprocess.run(
        ['nm', '-S', binary_path],
        capture_output=True, text=True, timeout=10
    )
    if nm_result.returncode == 0:
        for line in nm_result.stdout.strip().split('\n'):
            parts = line.split()
            if len(parts) >= 4 and parts[2] in ('B', 'b', 'D', 'd'):
                try:
                    size = int(parts[1], 16)
                    if size == 8:
                        valid_bss_symbols.add(parts[3])
                except ValueError:
                    pass

    if not valid_bss_symbols:
        return rip_to_bss

    result = subprocess.run(
        ['objdump', '-d', binary_path],
        capture_output=True, text=True, timeout=30
    )
    if result.returncode != 0:
        return rip_to_bss

    for line in result.stdout.split('\n'):
        match = re.match(r'\s+([0-9a-f]+):\s+.*#\s+([0-9a-f]+)\s+<([^>]+)>', line)
        if match:
            offset = int(match.group(1), 16)
            symbol = match.group(3)
            if symbol in valid_bss_symbols:
                rip_to_bss[offset] = symbol

    _extend_bss_map_with_thread_context(rip_to_bss, result.stdout, valid_bss_symbols)

    return rip_to_bss


def _extend_bss_map_with_thread_context(rip_to_bss: Dict[int, str],
                                         disasm_output: str,
                                         valid_bss_symbols: set):
    bs_thread_start = None
    bs_thread_end = None
    for line in disasm_output.split('\n'):
        m = re.match(r'^([0-9a-f]+)\s+<[^>]*bs_thread[^>]*>:', line)
        if m:
            bs_thread_start = int(m.group(1), 16)
        elif bs_thread_start and not bs_thread_end:
            m2 = re.match(r'^([0-9a-f]+)\s+<([^>]+)>:', line)
            if m2:
                bs_thread_end = int(m2.group(1), 16)
                break

    if not bs_thread_start:
        return

    if not bs_thread_end:
        bs_thread_end = bs_thread_start + 0x10000

    current_field = None
    for line in disasm_output.split('\n'):
        m = re.match(r'\s+([0-9a-f]+):\s+(.*)', line)
        if not m:
            continue
        offset = int(m.group(1), 16)
        if offset < bs_thread_start or offset >= bs_thread_end:
            continue

        instr = m.group(2)
        bss_match = re.search(r'#\s+([0-9a-f]+)\s+<([^>]+)>', instr)
        if bss_match:
            symbol = bss_match.group(2)
            if symbol in valid_bss_symbols:
                current_field = symbol

        if current_field and offset not in rip_to_bss:
            rip_to_bss[offset] = current_field


def resolve_rip_to_field(rip_hex: str, binary_path: str, pie_base: int = 0,
                         bss_map: Dict[int, str] = None) -> Optional[Tuple[str, str]]:
    try:
        rip_val = int(rip_hex, 16)
    except ValueError:
        return None

    file_offset = rip_val - pie_base

    if bss_map and file_offset in bss_map:
        bss_name = bss_map[file_offset]
        return (bss_name, f'BSS:{bss_name}')

    result = subprocess.run(
        ['addr2line', '-e', binary_path, '-f', hex(file_offset)],
        capture_output=True, text=True, timeout=5
    )

    if result.returncode != 0:
        return None

    lines = result.stdout.strip().split('\n')
    if len(lines) < 2:
        return None

    func_name = lines[0].strip()
    location = lines[1].strip() if len(lines) > 1 else ''

    if func_name in ('??', '?'):
        return None

    if location and location != '??:0' and location != '??:?':
        field = extract_field_from_location(location, binary_path, func_name)
        if field:
            return (field, location)

    # 从源码行号提取失败，尝试从函数名中提取字段
    field = extract_field_from_func_name(func_name)
    if field:
        return (field, func_name)

    return None


HEAPLESS_FUNCTIONS = {'CNDF', 'BlkSchlsEqEuroNoDiv', 'CumNormalDist', 'NormalDist'}


def is_function_body_heapless(source_lines: list, line_num: int) -> bool:
    func_start = find_function_start(source_lines, line_num)
    if func_start is None:
        return False
    func_end = func_start
    depth = 0
    for i in range(func_start - 1, len(source_lines)):
        for ch in source_lines[i]:
            if ch == '{':
                depth += 1
            elif ch == '}':
                depth -= 1
                if depth == 0:
                    func_end = i + 1
                    break
        if depth == 0 and func_end > func_start:
            break
    for i in range(func_start - 1, func_end):
        line = source_lines[i].strip()
        if re.search(r'[a-zA-Z_][a-zA-Z0-9_]*\s*\[', line):
            return False
    return True


def extract_field_from_location(location: str, binary_path: str, func_name: str = '') -> Optional[str]:
    match = re.match(r'^(.+?):(\d+)', location)
    if not match:
        return None

    file_path = match.group(1)
    line_num = int(match.group(2))

    try:
        with open(file_path, 'r') as f:
            source_lines = f.readlines()
    except FileNotFoundError:
        return None

    if line_num <= 0 or line_num > len(source_lines):
        return None

    code = source_lines[line_num - 1].strip()

    array_name = extract_array_access(code, func_name)
    if array_name:
        return array_name

    for delta in range(-2, 3):
        if delta == 0:
            continue
        nearby_line = line_num + delta
        if 0 < nearby_line <= len(source_lines):
            nearby_code = source_lines[nearby_line - 1].strip()
            array_name = extract_array_access(nearby_code, func_name)
            if array_name:
                return array_name

    demangled = demangle(func_name)
    func_short = demangled.split('(')[0].strip().split()[-1] if demangled else func_name

    if func_short in HEAPLESS_FUNCTIONS:
        return None

    if is_function_body_heapless(source_lines, line_num):
        return None

    caller_array = trace_caller_array(file_path, source_lines, line_num, func_name)
    if caller_array:
        return caller_array

    local_var = extract_local_var(code)
    if local_var:
        mapped = map_local_var_to_array(file_path, source_lines, line_num, func_name, local_var)
        if mapped:
            return mapped

    return None


def extract_array_access(code: str, func_name: str = '') -> Optional[str]:
    keywords = {'if', 'for', 'while', 'switch', 'return', 'else', 'int',
                'float', 'double', 'char', 'void', 'const', 'unsigned',
                'long', 'short', 'auto', 'sizeof', 'NULL', 'break',
                'continue', 'case', 'default', 'struct', 'typedef',
                'new', 'delete', 'this', 'true', 'false'}

    lhs_match = re.search(r'([a-zA-Z_][a-zA-Z0-9_]*)\s*\[[^\]]*\]\s*=', code)
    if lhs_match:
        name = lhs_match.group(1)
        if name not in keywords:
            type_name = extract_type_from_func_name(func_name)
            if type_name:
                return f"{type_name}.{name}"
            return name

    match = re.search(r'([a-zA-Z_][a-zA-Z0-9_]*)\s*\[', code)
    if match:
        name = match.group(1)
        if name not in keywords:
            type_name = extract_type_from_func_name(func_name)
            if type_name:
                return f"{type_name}.{name}"
            return name

    match = re.search(r'->([a-zA-Z_][a-zA-Z0-9_]*)', code)
    if match:
        field_name = match.group(1)
        type_name = extract_type_from_func_name(func_name)
        if type_name:
            return f"{type_name}.{field_name}"
        return field_name

    match = re.search(r'([a-zA-Z_][a-zA-Z0-9_]*)\s*\.\s*([a-zA-Z_][a-zA-Z0-9_]*)', code)
    if match and match.group(1) not in keywords:
        return match.group(2)

    return None


def extract_type_from_func_name(func_name: str) -> Optional[str]:
    if not func_name:
        return None
    
    demangled = demangle(func_name)
    if not demangled:
        demangled = func_name
    
    match = re.search(r'\(([^)]+)\)', demangled)
    if match:
        params = match.group(1)
        ptr_match = re.search(r'(?:\*|&)?\s*([A-Z][a-zA-Z0-9_]+)', params)
        if ptr_match:
            return ptr_match.group(1)
    
    cpp_match = re.match(r'_Z\d+[a-zA-Z0-9_]*P(\d+)([A-Z][a-zA-Z0-9_]+)', func_name)
    if cpp_match:
        return cpp_match.group(2)
    
    return None


def extract_field_from_func_name(func_name: str) -> Optional[str]:
    demangled = demangle(func_name)
    if not demangled:
        demangled = func_name

    type_name = extract_type_from_func_name(func_name)
    if not type_name:
        return None

    # 提取函数短名：从 mangled 名中提取（比 demangled 更可靠）
    m = re.match(r'_Z\d+([a-zA-Z_][a-zA-Z0-9_]*)P\d+', func_name)
    if m:
        func_short = m.group(1)
    elif demangled != func_name:
        # demangle 成功，从 demangled 提取
        func_short = demangled.split('(')[0].strip().split()[-1]
    else:
        return None

    field_match = re.search(r'_field(\d+)$', func_short)
    if field_match:
        return f"{type_name}.field{field_match.group(1)}"

    field_match = re.search(r'_inner(\d+)$', func_short)
    if field_match:
        return f"{type_name}.inner{field_match.group(1)}"

    if func_short.endswith('_outer'):
        return f"{type_name}.outer_field1"

    field_match = re.search(r'_group(\d+)$', func_short)
    if field_match:
        return f"{type_name}.shared_group{field_match.group(1)}"

    field_match = re.search(r'_isolated(\d+)$', func_short)
    if field_match:
        return f"{type_name}.isolated{field_match.group(1)}"

    field_match = re.search(r'_optimized_field(\d+)$', func_short)
    if field_match:
        return f"{type_name}.optimized_field{field_match.group(1)}"

    if func_short.endswith('_char'):
        return f"{type_name}.char_field"
    if func_short.endswith('_int') and '_int_field' not in func_short:
        return f"{type_name}.int_field"
    if func_short.endswith('_long'):
        return f"{type_name}.long_field"

    if func_short.endswith('_array'):
        return f"{type_name}.dynamic_data"

    if re.search(r'_idx\d+$', func_short):
        return f"{type_name}.data"

    return None


def extract_local_var(code: str) -> Optional[str]:
    match = re.search(r'([a-zA-Z_][a-zA-Z0-9_]*)\s*=', code)
    if match:
        name = match.group(1)
        keywords = {'if', 'for', 'while', 'switch', 'return', 'else', 'int',
                    'float', 'double', 'char', 'void', 'const', 'unsigned'}
        if name not in keywords:
            return name
    return None


def trace_caller_array(file_path: str, source_lines: list, line_num: int, func_name: str) -> Optional[str]:
    demangled = demangle(func_name)
    func_short = demangled.split('(')[0].strip()
    func_short = func_short.split()[-1] if func_short else ''

    for i in range(len(source_lines)):
        line = source_lines[i].strip()
        if func_short + '(' in line:
            call_start = line.index(func_short + '(')
            paren_start = call_start + len(func_short)
            depth = 0
            args_str = ''
            for j in range(paren_start, len(line)):
                if line[j] == '(':
                    depth += 1
                elif line[j] == ')':
                    depth -= 1
                    if depth == 0:
                        break
                args_str += line[j]

            args = split_args(args_str[1:])
            for arg in args:
                arg = arg.strip()
                arr_match = re.search(r'([a-zA-Z_][a-zA-Z0-9_]*)\s*\[', arg)
                if arr_match:
                    return arr_match.group(1)

    return None


def get_func_param_index(source_lines: list, func_name: str, var_name: str) -> Optional[int]:
    func_short = func_name.split('(')[0].strip()
    func_short = func_short.split()[-1] if func_short else ''

    for i, line in enumerate(source_lines):
        stripped = line.strip()
        if func_short in stripped and '(' in stripped:
            if not stripped.startswith(func_short) and func_short + '(' not in stripped:
                continue
            paren_start = stripped.index('(')
            depth = 0
            params_str = ''
            for j in range(paren_start, len(stripped)):
                if stripped[j] == '(':
                    depth += 1
                elif stripped[j] == ')':
                    depth -= 1
                    if depth == 0:
                        break
                params_str += stripped[j]

            params = split_args(params_str[1:])
            for pi, param in enumerate(params):
                param = param.strip()
                tokens = param.split()
                for token in tokens:
                    clean = token.lstrip('*&').rstrip('),')
                    if clean == var_name:
                        return pi
            break
    return None


def find_caller_arg_at_index(source_lines: list, func_name: str, param_index: int) -> Optional[str]:
    func_short = func_name.split('(')[0].strip()
    func_short = func_short.split()[-1] if func_short else ''

    for i, line in enumerate(source_lines):
        stripped = line.strip()
        if func_short + '(' in stripped:
            call_start = stripped.index(func_short + '(')
            paren_start = call_start + len(func_short)
            depth = 0
            args_str = ''
            for j in range(paren_start, len(stripped)):
                if stripped[j] == '(':
                    depth += 1
                elif stripped[j] == ')':
                    depth -= 1
                    if depth == 0:
                        break
                args_str += stripped[j]

            args = split_args(args_str[1:])
            if param_index < len(args):
                arg = args[param_index].strip()
                arr_match = re.search(r'([a-zA-Z_][a-zA-Z0-9_]*)\s*\[', arg)
                if arr_match:
                    return arr_match.group(1)
            break
    return None


def map_local_var_to_array(file_path: str, source_lines: list, line_num: int,
                           func_name: str, local_var: str) -> Optional[str]:
    func_start = find_function_start(source_lines, line_num)
    if func_start is None:
        func_start = max(1, line_num - 100)

    for i in range(func_start - 1, min(line_num, len(source_lines))):
        line = source_lines[i].strip()
        assign_match = re.search(rf'^{re.escape(local_var)}\s*=\s*(.+?);', line)
        if assign_match:
            rhs = assign_match.group(1).strip()
            arr_match = re.search(r'([a-zA-Z_][a-zA-Z0-9_]*)\s*\[', rhs)
            if arr_match:
                return arr_match.group(1)
            var_match = re.search(r'([a-zA-Z_][a-zA-Z0-9_]*)', rhs)
            if var_match:
                rhs_var = var_match.group(1)
                keywords = {'if', 'for', 'while', 'switch', 'return', 'else',
                            'int', 'float', 'double', 'char', 'void', 'const',
                            'unsigned', 'exp', 'log', 'sqrt', 'abs', 'fabs'}
                if rhs_var not in keywords and rhs_var != local_var:
                    return map_local_var_to_array(file_path, source_lines, i + 1, func_name, rhs_var)

    demangled = demangle(func_name)
    param_index = get_func_param_index(source_lines, demangled, local_var)
    if param_index is not None:
        caller_array = find_caller_arg_at_index(source_lines, demangled, param_index)
        if caller_array:
            return caller_array

    return None


def find_function_start(source_lines: list, line_num: int) -> Optional[int]:
    brace_depth = 0
    for i in range(line_num - 1, -1, -1):
        line = source_lines[i]
        for ch in reversed(line):
            if ch == '}':
                brace_depth += 1
            elif ch == '{':
                if brace_depth == 0:
                    for j in range(i, -1, -1):
                        if re.match(r'^[a-zA-Z_]', source_lines[j].strip()):
                            return j + 1
                    return i + 1
                brace_depth -= 1
    return None


def demangle(name: str) -> str:
    if name.startswith('_Z'):
        try:
            result = subprocess.run(
                ['c++filt', name],
                capture_output=True, text=True, timeout=2
            )
            if result.returncode == 0:
                return result.stdout.strip()
        except Exception:
            pass
    return name


def get_func_param_name(source_lines: list, func_name: str, var_name: str) -> Optional[str]:
    for i, line in enumerate(source_lines):
        stripped = line.strip()
        if func_name.split('(')[0].strip() in stripped and '(' in stripped:
            paren_start = stripped.index('(')
            depth = 0
            params_str = ''
            for j in range(paren_start, len(stripped)):
                if stripped[j] == '(':
                    depth += 1
                elif stripped[j] == ')':
                    depth -= 1
                    if depth == 0:
                        break
                params_str += stripped[j]

            params = split_args(params_str[1:])
            for pi, param in enumerate(params):
                param = param.strip()
                tokens = param.split()
                for ti, token in enumerate(tokens):
                    if token == var_name or token.rstrip(')') == var_name:
                        if ti > 0:
                            return tokens[ti - 1].lstrip('*&').rstrip(')')
            break
    return None


def find_caller_arg_array(source_lines: list, func_name: str, param_name: str) -> Optional[str]:
    func_short = func_name.split('(')[0].strip()
    func_short = func_short.split()[-1] if func_short else ''

    for i, line in enumerate(source_lines):
        stripped = line.strip()
        if func_short in stripped and func_short + '(' in stripped:
            call_start = stripped.index(func_short + '(')
            paren_start = call_start + len(func_short)
            depth = 0
            args_str = ''
            for j in range(paren_start, len(stripped)):
                if stripped[j] == '(':
                    depth += 1
                elif stripped[j] == ')':
                    depth -= 1
                    if depth == 0:
                        break
                args_str += stripped[j]

            args = split_args(args_str[1:])
            for arg in args:
                arg = arg.strip()
                arr_match = re.search(r'([a-zA-Z_][a-zA-Z0-9_]*)\s*\[', arg)
                if arr_match:
                    return arr_match.group(1)
            break
    return None


def split_args(args_str: str) -> list:
    args = []
    depth = 0
    current = ''
    for ch in args_str:
        if ch == '(' or ch == '<':
            depth += 1
            current += ch
        elif ch == ')' or ch == '>':
            depth -= 1
            current += ch
        elif ch == ',' and depth == 0:
            args.append(current)
            current = ''
        else:
            current += ch
    if current.strip():
        args.append(current)
    return args


def infer_fields_from_source_voting(hitm_path: str) -> Dict[int, Dict[str, int]]:
    addr_field_votes = defaultdict(lambda: defaultdict(int))

    if not os.path.exists(hitm_path):
        return addr_field_votes

    with open(hitm_path, 'r') as f:
        for line in f:
            line = line.strip()
            if not line:
                continue

            parts = line.split(',', 3)
            if len(parts) < 4:
                continue

            sharing_type = parts[0]
            region = parts[1]
            if sharing_type not in ('F', 'T') or region != 'A':
                continue

            addr_info = parts[2]
            rest = parts[3]

            count_match = re.match(r'(\d+)', rest)
            count = int(count_match.group(1)) if count_match else 0

            source_info = rest.split(':', 1)[1].strip() if ':' in rest else ''

            addrs = re.findall(r'0x[0-9a-fA-F]+', addr_info)
            addr_vals = []
            for a in addrs:
                try:
                    addr_vals.append(int(a, 16))
                except ValueError:
                    pass

            source_parts = source_info.split(',')
            for source_part in source_parts:
                source_part = source_part.strip()

                var_match = re.search(r'\[([^\]]+)\]', source_part)
                if not var_match:
                    continue

                code = var_match.group(1)

                field_match = re.search(r'->([a-zA-Z_][a-zA-Z0-9_]*)(?:\[[^\]]*\])?', code)
                if field_match:
                    field_name = field_match.group(1)
                    for addr in addr_vals:
                        addr_field_votes[addr][field_name] += count
                    continue

                arr_match = re.search(r'([a-zA-Z_][a-zA-Z0-9_]*)\[', code)
                if arr_match:
                    var_name = arr_match.group(1)
                    if var_name not in ('if', 'for', 'while', 'switch', 'return'):
                        for addr in addr_vals:
                            addr_field_votes[addr][var_name] += count

    return addr_field_votes


def parse_memscope_data(memscope_path: str) -> List[Dict]:
    allocations = []
    if not os.path.exists(memscope_path):
        return allocations

    with open(memscope_path, 'r') as f:
        lines = f.readlines()
        if not lines:
            return allocations

        header = lines[0].strip().split(',')
        for line in lines[1:]:
            line = line.strip()
            if not line:
                continue

            parts = _parse_csv_line(line)
            if len(parts) < len(header):
                continue

            row = {}
            for i, key in enumerate(header):
                if i < len(parts):
                    row[key] = parts[i]

            try:
                addr_str = row.get('addr', row.get('address', '')).strip()
                if not addr_str:
                    continue
                addr = int(addr_str, 16)
                size = int(row.get('size', '0') or '0')
                allocations.append({
                    'addr': addr,
                    'size': size,
                    'type': row.get('type', '') or '',
                    'field': row.get('field', '') or '',
                    'offset': int(row.get('offset', '0') or '0'),
                })
            except (ValueError, KeyError):
                continue

    return allocations


def _parse_csv_line(line: str) -> List[str]:
    parts = []
    current = ""
    in_brackets = 0

    for char in line:
        if char == '<':
            in_brackets += 1
            current += char
        elif char == '>':
            in_brackets -= 1
            current += char
        elif char == ',' and in_brackets == 0:
            parts.append(current)
            current = ""
        else:
            current += char

    parts.append(current)
    return parts


def find_field_for_addr(addr: int, memscope_allocs: List[Dict],
                        layouts: Dict[str, Dict[int, str]] = None,
                        sizes: Dict[str, int] = None) -> Optional[str]:
    # 第一遍：精确匹配（地址在分配范围内）
    for alloc in memscope_allocs:
        alloc_addr = alloc['addr']
        alloc_size = alloc['size']
        if alloc_addr <= addr < alloc_addr + alloc_size:
            field = alloc.get('field', '')
            type_name = alloc.get('type', '')
            # 去掉 (ambiguous) 等后缀
            clean_type = type_name
            if ' (' in clean_type:
                clean_type = clean_type[:clean_type.index(' (')]

            offset = addr - alloc_addr

            # 如果有 struct layout，根据偏移重新计算字段
            if layouts and sizes and clean_type in layouts:
                struct_size = sizes.get(clean_type, 0)
                if struct_size > 0 and alloc_size > struct_size:
                    # 数组分配：计算元素索引和元素内偏移
                    elem_index = offset // struct_size
                    elem_offset = offset % struct_size
                    layout = layouts[clean_type]
                    # 找到 elem_offset 对应的字段
                    nearest_offset = None
                    for fo in sorted(layout.keys()):
                        if fo <= elem_offset:
                            nearest_offset = fo
                        else:
                            break
                    if nearest_offset is not None:
                        return f"{clean_type}[{elem_index}].{layout[nearest_offset]}"
                    return f"{clean_type}[{elem_index}]+0x{elem_offset:x}"
                elif struct_size > 0:
                    # 单元素分配
                    layout = layouts[clean_type]
                    nearest_offset = None
                    for fo in sorted(layout.keys()):
                        if fo <= offset:
                            nearest_offset = fo
                        else:
                            break
                    if nearest_offset is not None:
                        return f"{clean_type}.{layout[nearest_offset]}"
                    return f"{clean_type}+0x{offset:x}"

            # 没有 layout，回退到原逻辑
            if field:
                if type_name and type_name in field:
                    return field
                elif type_name:
                    return f"{type_name}.{field}"
                else:
                    return field
            elif type_name:
                return type_name
            else:
                return f"+0x{offset:x}"

    # 第二遍：附近分配推断（地址不在任何分配范围内，但靠近某个数组分配）
    # 场景：fsparse_a 漏掉了某些 malloc 调用，但 hitm 地址落在某个
    # 已知数组分配的延伸范围内（例如 malloc(512) 被记录为 malloc(1024)，
    # 或者分配基址有偏差）
    if layouts and sizes:
        best_match = None
        best_dist = float('inf')
        for alloc in memscope_allocs:
            type_name = alloc.get('type', '')
            clean_type = type_name
            if ' (' in clean_type:
                clean_type = clean_type[:clean_type.index(' (')]
            if clean_type not in layouts:
                continue
            struct_size = sizes.get(clean_type, 0)
            if struct_size <= 0:
                continue
            alloc_addr = alloc['addr']
            alloc_size = alloc['size']
            # 计算地址相对于分配基址的偏移
            offset = addr - alloc_addr
            # 允许偏移为负（地址在分配之前）或超过分配大小
            # 但限制在合理范围内（不超过 2 倍分配大小）
            if offset < -struct_size or offset > alloc_size + struct_size * 2:
                continue
            dist = 0
            if offset < 0:
                dist = -offset
            elif offset >= alloc_size:
                dist = offset - alloc_size + 1
            if dist < best_dist:
                best_dist = dist
                # 推断数组索引和元素内偏移
                if offset < 0:
                    elem_index = offset // struct_size
                    elem_offset = offset % struct_size
                    if elem_offset < 0:
                        elem_offset += struct_size
                        elem_index -= 1
                else:
                    elem_index = offset // struct_size
                    elem_offset = offset % struct_size
                layout = layouts[clean_type]
                nearest_offset = None
                for fo in sorted(layout.keys()):
                    if fo <= elem_offset:
                        nearest_offset = fo
                    else:
                        break
                if nearest_offset is not None:
                    best_match = f"{clean_type}[{elem_index}].{layout[nearest_offset]}"
                else:
                    best_match = f"{clean_type}[{elem_index}]+0x{elem_offset:x}"

        # 只有在距离足够近时才返回推断结果（不超过 2 个结构体大小）
        if best_match and best_dist < 256:
            return best_match

    return None


def resolve_addr_field(addr: int,
                       addr_fields_rip: Dict[int, Dict[str, int]],
                       addr_field_votes: Dict[int, Dict[str, int]],
                       memscope_allocs: List[Dict]) -> Optional[str]:
    # 只使用 RIP 直接推断（不使用投票）
    if addr in addr_fields_rip:
        votes = addr_fields_rip[addr]
        if votes:
            # 只选择有绝对多数的字段，或者返回 None（不使用投票）
            sorted_fields = sorted(votes.items(), key=lambda x: x[1], reverse=True)
            if len(sorted_fields) == 1:
                return sorted_fields[0][0]
            # 如果有多个可能的字段，且前两个差距很大，则选择
            if len(sorted_fields) >= 2 and sorted_fields[0][1] > sorted_fields[1][1] * 2:
                return sorted_fields[0][0]
            # 否则返回 None，不使用投票
            return None

    # 只使用 memscope 匹配（不使用投票）
    field_memscope = find_field_for_addr(addr, memscope_allocs)
    if field_memscope:
        return field_memscope

    return None


def infer_from_neighbors(addr: int,
                         addr_field_map: Dict[int, str],
                         events: List[Dict]) -> Optional[str]:
    cl = addr_to_cacheline(addr)

    neighbor_fields = defaultdict(int)
    for other_addr, other_field in addr_field_map.items():
        if other_addr == addr:
            continue
        if addr_to_cacheline(other_addr) == cl:
            neighbor_fields[other_field] += 1

    if neighbor_fields:
        return max(neighbor_fields, key=neighbor_fields.get)

    for event in events:
        if addr_to_cacheline(event['addr1']) == cl or addr_to_cacheline(event['addr2']) == cl:
            other = event['addr2'] if addr_to_cacheline(event['addr1']) == cl else event['addr1']
            if other in addr_field_map:
                neighbor_fields[addr_field_map[other]] += event['count']

    if neighbor_fields:
        return max(neighbor_fields, key=neighbor_fields.get)

    return None


def cluster_addresses_by_range(all_addrs: Set[int],
                              addr_field_map: Dict[int, str],
                              events: List[Dict],
                              addr_fields_rip: Dict[int, Dict[str, int]]) -> Dict[int, str]:
    ADDR_GAP_THRESHOLD = 0x1000

    sorted_addrs = sorted(all_addrs)
    groups = []
    current_group = [sorted_addrs[0]]

    for addr in sorted_addrs[1:]:
        if addr - current_group[-1] < ADDR_GAP_THRESHOLD:
            current_group.append(addr)
        else:
            groups.append(current_group)
            current_group = [addr]
    groups.append(current_group)

    updated_map = dict(addr_field_map)

    for group in groups:
        resolved = {a: f for a, f in updated_map.items() if a in group}
        if not resolved:
            field_votes = defaultdict(int)
            for addr in group:
                if addr in addr_fields_rip:
                    for field, count in addr_fields_rip[addr].items():
                        field_votes[field] += count
            if field_votes:
                best_field = max(field_votes, key=field_votes.get)
                for addr in group:
                    if addr not in updated_map:
                        updated_map[addr] = best_field
            continue

        field_counts = defaultdict(int)
        for field in resolved.values():
            field_counts[field] += 1
        best_field = max(field_counts, key=field_counts.get)

        for addr in group:
            if addr not in updated_map:
                updated_map[addr] = best_field

    return updated_map


def auto_detect_num_options(work_dir: str) -> Optional[int]:
    candidate_files = ['in_64K.txt', 'in_16K.txt', 'in_4K.txt', 'input.txt', 'in.txt']
    for fname in candidate_files:
        candidate = os.path.join(work_dir, fname)
        if os.path.exists(candidate):
            try:
                with open(candidate, 'r') as f:
                    first_line = f.readline().strip()
                    num = int(first_line)
                    if num > 0 and num < 100000000:
                        return num
            except (ValueError, IOError):
                pass
    return None


def detect_buffer_base_from_ranges(all_addrs: Set[int]) -> Optional[int]:
    if not all_addrs:
        return None

    sorted_addrs = sorted(all_addrs)
    groups = []
    current_group = [sorted_addrs[0]]

    for addr in sorted_addrs[1:]:
        if addr - current_group[-1] < 0x1000:
            current_group.append(addr)
        else:
            groups.append(current_group)
            current_group = [addr]
    groups.append(current_group)

    if len(groups) >= 2:
        group_ranges = [(min(g), max(g), len(g)) for g in groups]
        group_ranges.sort(key=lambda x: x[0])
        largest = max(group_ranges, key=lambda x: x[2])
        if largest[2] >= 10:
            return (largest[0] // CACHE_LINE_SIZE) * CACHE_LINE_SIZE

    return None


def assign_fields_by_layout(all_addrs: Set[int],
                            addr_field_map: Dict[int, str],
                            addr_fields_rip: Dict[int, Dict[str, int]],
                            num_options: int = None) -> Dict[int, str]:
    result = dict(addr_field_map)

    sorted_addrs = sorted(all_addrs)
    groups = []
    current_group = [sorted_addrs[0]]

    for addr in sorted_addrs[1:]:
        if addr - current_group[-1] < 0x1000:
            current_group.append(addr)
        else:
            groups.append(current_group)
            current_group = [addr]
    groups.append(current_group)

    group_ranges = [(min(g), max(g), len(g)) for g in groups]

    if not num_options or num_options <= 0:
        return result

    elem_size = 4
    arr_size = num_options * elem_size

    BUFFER_FIELD_NAMES = ['sptprice', 'strike', 'rate', 'volatility', 'otime']
    STANDALONE_ARRAYS = {'prices', 'otype', 'data'}

    group_rip_votes = []
    for group in groups:
        votes = defaultdict(int)
        for addr in group:
            if addr in addr_fields_rip:
                for field, count in addr_fields_rip[addr].items():
                    if field in STANDALONE_ARRAYS or field in BUFFER_FIELD_NAMES:
                        votes[field] += count
        group_rip_votes.append(votes)

    group_dominant = []
    for gi, votes in enumerate(group_rip_votes):
        if votes:
            dom = max(votes, key=votes.get)
            group_dominant.append((gi, dom, votes[dom]))
        else:
            group_dominant.append((gi, None, 0))

    print(f"  地址组数: {len(groups)}, 各组 RIP 投票: ", end='')
    group_desc = []
    for gi, (gmin, gmax, gcount) in enumerate(group_ranges):
        dom, score = group_dominant[gi][1], group_dominant[gi][2]
        group_desc.append(f"G{gi}(0x{gmin:x}-0x{gmax:x},{gcount}addr)={dom}({score})")
    print(', '.join(group_desc))

    buffer_group_idx = None
    standalone_groups = []

    for gi, dom, score in group_dominant:
        if dom in BUFFER_FIELD_NAMES:
            if buffer_group_idx is None:
                buffer_group_idx = gi
            elif group_rip_votes[gi].get(dom, 0) > group_rip_votes[buffer_group_idx].get(
                    group_dominant[buffer_group_idx][1], 0):
                standalone_groups.append((buffer_group_idx, group_dominant[buffer_group_idx][1]))
                buffer_group_idx = gi
            else:
                standalone_groups.append((gi, dom))
        elif dom in STANDALONE_ARRAYS:
            standalone_groups.append((gi, dom))
        else:
            standalone_groups.append((gi, dom))

    if buffer_group_idx is None and len(groups) >= 1:
        group_ranges_sorted = sorted(enumerate(group_ranges), key=lambda x: x[1][2], reverse=True)
        buffer_group_idx = group_ranges_sorted[0][0]
        standalone_groups = [(gi, None) for gi in range(len(groups)) if gi != buffer_group_idx]

    for gi, dom in standalone_groups:
        group = groups[gi]
        gmin, gmax, gcount = group_ranges[gi]

        if dom and dom in STANDALONE_ARRAYS:
            for addr in group:
                if addr not in result:
                    result[addr] = dom
        elif dom:
            for addr in group:
                if addr not in result:
                    result[addr] = dom
        else:
            rips_votes = group_rip_votes[gi]
            if rips_votes:
                for addr in group:
                    if addr not in result:
                        if addr in addr_fields_rip:
                            votes = addr_fields_rip[addr]
                            if votes:
                                result[addr] = max(votes, key=votes.get)
                        else:
                            best_field = max(rips_votes, key=rips_votes.get)
                            result[addr] = best_field
            else:
                existing_fields = set()
                for addr in group:
                    if addr in result:
                        f = result[addr]
                        if not f.startswith('0x') and '+' not in f:
                            existing_fields.add(f)
                if existing_fields:
                    best_field = max(existing_fields, key=lambda f: sum(1 for a in group if result.get(a) == f))
                    for addr in group:
                        if addr not in result:
                            if best_field:
                                result[addr] = best_field
                            else:
                                result[addr] = BUFFER_FIELD_NAMES[0]

    if buffer_group_idx is not None:
        group = groups[buffer_group_idx]
        gmin, gmax, gcount = group_ranges[buffer_group_idx]
        base = (gmin // CACHE_LINE_SIZE) * CACHE_LINE_SIZE

        for addr in group:
            if addr not in result:
                offset = addr - base
                idx = offset // arr_size
                if 0 <= idx < len(BUFFER_FIELD_NAMES):
                    result[addr] = BUFFER_FIELD_NAMES[idx]
                else:
                    result[addr] = BUFFER_FIELD_NAMES[min(idx, len(BUFFER_FIELD_NAMES) - 1)]

    field_counts = defaultdict(int)
    for addr in result:
        f = result[addr]
        if not f.startswith('0x') and '+' not in f:
            field_counts[f] += 1
    print(f"  布局推断字段分布: {dict(field_counts)}")

    return result


def resolve_by_groups(all_addrs: Set[int],
                      addr_fields_rip: Dict[int, Dict[str, int]],
                      addr_field_votes: Dict[int, Dict[str, int]],
                      memscope_allocs: List[Dict]) -> Dict[int, str]:
    ADDR_GAP_THRESHOLD = 0x1000

    sorted_addrs = sorted(all_addrs)
    groups = []
    current_group = [sorted_addrs[0]]

    for addr in sorted_addrs[1:]:
        if addr - current_group[-1] < ADDR_GAP_THRESHOLD:
            current_group.append(addr)
        else:
            groups.append(current_group)
            current_group = [addr]
    groups.append(current_group)

    group_field_votes = []
    for group in groups:
        votes = defaultdict(int)
        for addr in group:
            if addr in addr_fields_rip:
                for field, count in addr_fields_rip[addr].items():
                    votes[field] += count
        group_field_votes.append(votes)

    all_fields = set()
    for votes in group_field_votes:
        all_fields.update(votes.keys())

    field_group_votes = {f: [0] * len(groups) for f in all_fields}
    for gi, votes in enumerate(group_field_votes):
        for field, count in votes.items():
            field_group_votes[field][gi] = count

    addr_field_map = {}
    for gi, group in enumerate(groups):
        exclusive_votes = {}
        for field, group_votes in field_group_votes.items():
            total_for_field = sum(group_votes)
            if total_for_field > 0:
                field_votes_in_this_group = group_votes[gi]
                if field_votes_in_this_group == total_for_field:
                    exclusive_votes[field] = field_votes_in_this_group

        if exclusive_votes:
            best_field = max(exclusive_votes, key=exclusive_votes.get)
        elif group_field_votes[gi]:
            best_field = max(group_field_votes[gi], key=group_field_votes[gi].get)
        else:
            best_field = None

        if best_field:
            for addr in group:
                if addr not in addr_field_map:
                    addr_field_map[addr] = best_field

    for addr in all_addrs:
        if addr not in addr_field_map:
            field = resolve_addr_field(addr, addr_fields_rip, addr_field_votes, memscope_allocs)
            if field:
                addr_field_map[addr] = field

    return addr_field_map


def _detect_array_fields(layout: Dict[int, str], struct_size: int) -> Dict[str, Dict]:
    """从 member 间隔推断哪些字段是数组。
    
    - 若 member 间距 >= 16 且不是单成员结构体的对齐填充，则认为是大数组。
    - 单成员结构体无法从间距推断真实 member 大小，跳过数组检测。
    """
    arrays = {}
    offsets = sorted(layout.keys())
    if len(offsets) <= 1:
        return arrays  # 单成员结构体：间距=struct_size-0 可能是对齐填充，不可靠
    for idx, off in enumerate(offsets):
        name = layout[off]
        if idx + 1 < len(offsets):
            end = offsets[idx + 1]
        else:
            end = struct_size
        member_size = end - off
        if member_size >= 16:
            elem_size = 8 if member_size % 8 == 0 else 4
            arrays[name] = {
                'offset': off,
                'elem_size': elem_size,
                'count': member_size // elem_size,
            }
    return arrays


def extract_struct_type_from_rips(addr_fields_rip: Dict[int, Dict[str, int]]) -> Set[str]:
    struct_types = set()
    for addr, votes in addr_fields_rip.items():
        for field_name in votes:
            dot_pos = field_name.find('.')
            if dot_pos > 0:
                struct_types.add(field_name[:dot_pos])
    return struct_types


def resolve_field_by_struct_offset(addr: int, struct_type: str,
                                    binary_path: Optional[str] = None) -> Optional[str]:
    """基于结构体布局映射地址到字段名（DWARF 驱动，无硬编码）。"""
    layouts, sizes = _extract_struct_layouts(binary_path)
    if struct_type not in layouts:
        return None

    layout = layouts[struct_type]
    struct_size = sizes.get(struct_type, 64)

    cl = addr_to_cacheline(addr)
    offset_in_cl = addr - cl

    # 自动检测数组字段
    arrays = _detect_array_fields(layout, struct_size)
    for arr_name, arr_info in arrays.items():
        arr_offset = arr_info['offset']
        arr_elem_size = arr_info['elem_size']
        arr_count = arr_info['count']
        arr_end = arr_offset + arr_elem_size * arr_count
        if arr_offset <= offset_in_cl < arr_end:
            idx = (offset_in_cl - arr_offset) // arr_elem_size
            return f"{struct_type}.{arr_name}[{idx}]"

    for field_offset in sorted(layout.keys()):
        if offset_in_cl == field_offset:
            return f"{struct_type}.{layout[field_offset]}"

    if offset_in_cl < struct_size:
        nearest_offset = None
        for field_offset in sorted(layout.keys()):
            if field_offset <= offset_in_cl:
                nearest_offset = field_offset
            else:
                break
        if nearest_offset is not None:
            return f"{struct_type}.{layout[nearest_offset]}"

    return None


def find_candidate_struct_for_cacheline(cl: int, addrs_in_cl: Set[int],
                                         struct_types: Set[str],
                                         binary_path: Optional[str] = None) -> Optional[str]:
    """为缓存行选择最匹配的结构体类型（DWARF 驱动）。"""
    layouts, sizes = _extract_struct_layouts(binary_path)
    best_type = None
    best_matches = 0

    for st in struct_types:
        if st not in layouts:
            continue
        layout = layouts[st]
        arrays = _detect_array_fields(layout, sizes.get(st, 64))
        matches = 0
        for addr in addrs_in_cl:
            offset_in_cl = addr - cl
            if offset_in_cl in layout:
                matches += 1
                continue
            for arr_name, arr_info in arrays.items():
                arr_start = arr_info['offset']
                arr_end = arr_start + arr_info['elem_size'] * arr_info['count']
                if arr_start <= offset_in_cl < arr_end:
                    matches += 1
                    break
        if matches > best_matches:
            best_matches = matches
            best_type = st

    return best_type


def resolve_fields_by_struct_layout(all_addrs: Set[int],
                                     addr_fields_rip: Dict[int, Dict[str, int]],
                                     events: List[Dict],
                                     binary_path: Optional[str] = None,
                                     addr_field_votes: Dict[int, Dict[str, int]] = None) -> Dict[int, str]:
    """通过 DWARF 结构体布局推断地址 → 字段映射（通用版本）。"""
    addr_field_map = {}

    cachelines = defaultdict(set)
    for addr in all_addrs:
        cl = addr_to_cacheline(addr)
        cachelines[cl].add(addr)

    struct_types = extract_struct_type_from_rips(addr_fields_rip)

    # 如果 addr_fields_rip 为空，尝试从 addr_field_votes（源码推断）中提取字段名，
    # 然后通过 DWARF 查找包含这些字段的结构体类型
    if not struct_types and binary_path and addr_field_votes:
        layouts, sizes = _extract_struct_layouts(binary_path)
        # 收集所有源码推断的字段名
        all_field_names = set()
        for votes in addr_field_votes.values():
            all_field_names.update(votes.keys())
        # 在 DWARF 结构体中查找包含这些字段名的类型
        for type_name, layout in layouts.items():
            layout_fields = set(layout.values())
            if all_field_names & layout_fields:
                struct_types.add(type_name)

    if not struct_types:
        return addr_field_map

    cl_struct_type = {}
    for cl, addrs_in_cl in cachelines.items():
        st = find_candidate_struct_for_cacheline(cl, addrs_in_cl, struct_types, binary_path)
        if st:
            cl_struct_type[cl] = st

    for addr in all_addrs:
        cl = addr_to_cacheline(addr)
        st = cl_struct_type.get(cl)
        if st:
            field = resolve_field_by_struct_offset(addr, st, binary_path)
            if field:
                addr_field_map[addr] = field

    # 后处理：为结构体大小 == cacheline 大小的类型推断数组索引
    # 当 struct_size == CACHE_LINE_SIZE 时，每个元素占一个完整的 cacheline，
    # resolve_field_by_struct_offset 无法区分数组索引，需要根据地址连续性推断
    if binary_path:
        layouts, sizes = _extract_struct_layouts(binary_path)
        for st in set(cl_struct_type.values()):
            struct_size = sizes.get(st, 0)
            if struct_size != CACHE_LINE_SIZE:
                continue
            # 收集属于此类型的所有地址
            type_addrs = []
            for addr in all_addrs:
                cl = addr_to_cacheline(addr)
                if cl_struct_type.get(cl) == st:
                    type_addrs.append(addr)
            if not type_addrs:
                continue
            # 按 cacheline 分组，确定数组基址
            cl_addrs = sorted(set(addr_to_cacheline(a) for a in type_addrs))
            if len(cl_addrs) <= 1:
                continue
            # 推断数组基址：最小的 cacheline 地址
            base_cl = cl_addrs[0]
            for addr in type_addrs:
                cl = addr_to_cacheline(addr)
                elem_index = (cl - base_cl) // struct_size
                old_field = addr_field_map.get(addr, '')
                if old_field and '[' not in old_field:
                    # 将 "lreg_args.SX" 转换为 "lreg_args[2].SX"
                    dot_pos = old_field.find('.')
                    if dot_pos > 0:
                        type_prefix = old_field[:dot_pos]
                        field_suffix = old_field[dot_pos + 1:]
                        addr_field_map[addr] = f"{type_prefix}[{elem_index}].{field_suffix}"

    return addr_field_map


def analyze(events: List[Dict],
            addr_fields_rip: Dict[int, Dict[str, int]],
            addr_field_votes: Dict[int, Dict[str, int]],
            memscope_allocs: List[Dict],
            work_dir: str = None,
            num_options: int = None,
            binary_path: Optional[str] = None) -> Dict:
    all_addrs = set()
    for event in events:
        all_addrs.add(event['addr1'])
        all_addrs.add(event['addr2'])

    memscope_matched = 0
    for addr in all_addrs:
        if find_field_for_addr(addr, memscope_allocs):
            memscope_matched += 1
    if memscope_allocs:
        if memscope_matched > 0:
            print(f"  memscope 地址匹配: {memscope_matched}/{len(all_addrs)}")
        else:
            print(f"  memscope 地址匹配: 0/{len(all_addrs)} (数据可能来自不同运行，将使用 RIP+反汇编推断)")

    addr_field_map: Dict[int, str] = {}
    rip_resolved = 0
    mem_resolved = 0
    layout_resolved = 0

    # 提取 struct layouts 供 find_field_for_addr 使用
    struct_layouts, struct_sizes = _extract_struct_layouts(binary_path)

    layout_map = resolve_fields_by_struct_layout(all_addrs, addr_fields_rip, events, binary_path, addr_field_votes)
    for addr in all_addrs:
        if addr in layout_map:
            addr_field_map[addr] = layout_map[addr]
            layout_resolved += 1

    resolved = len(addr_field_map)
    print(f"  结构体偏移推断: {layout_resolved}/{len(all_addrs)}")

    for addr in all_addrs:
        if addr not in addr_field_map:
            field = find_field_for_addr(addr, memscope_allocs, struct_layouts, struct_sizes)
            if field:
                addr_field_map[addr] = field
                mem_resolved += 1

    resolved = len(addr_field_map)
    print(f"  memscope 解析: +{mem_resolved} → {resolved}/{len(all_addrs)}")

    for addr in all_addrs:
        if addr not in addr_field_map:
            field = resolve_addr_field(addr, addr_fields_rip, addr_field_votes, memscope_allocs)
            if field:
                addr_field_map[addr] = field
                rip_resolved += 1

    resolved = len(addr_field_map)
    unresolved = len(all_addrs) - resolved
    print(f"  地址解析: {resolved}/{len(all_addrs)} 已解析 (布局:{layout_resolved} memscope:{mem_resolved} RIP:{rip_resolved}), {unresolved} 未解析")

    # 禁用组解析和范围聚类，因为它们使用投票
    # addr_field_map = resolve_by_groups(all_addrs, addr_fields_rip, addr_field_votes, memscope_allocs)
    # addr_field_map = cluster_addresses_by_range(all_addrs, addr_field_map, events, addr_fields_rip)
    resolved_after_cluster = resolved

    if work_dir and not num_options:
        num_options = auto_detect_num_options(work_dir)
        if num_options:
            print(f"  自动检测 numOptions: {num_options}")

    addr_field_map = assign_fields_by_layout(all_addrs, addr_field_map, addr_fields_rip, num_options)
    print(f"  布局推断后: {len(addr_field_map)} 个地址已映射")

    resolved_after = len(addr_field_map)
    print(f"  邻居推断后: {resolved_after}/{len(all_addrs)} 已解析")

    for addr in all_addrs:
        if addr not in addr_field_map:
            cl = addr_to_cacheline(addr)
            offset_in_cl = addr - cl
            addr_field_map[addr] = f"0x{addr:x}(+{offset_in_cl})"

    pair_counts: Dict[Tuple[str, str], int] = defaultdict(int)
    field_total: Dict[str, int] = defaultdict(int)

    for event in events:
        field1 = addr_field_map.get(event['addr1'], f"0x{event['addr1']:x}")
        field2 = addr_field_map.get(event['addr2'], f"0x{event['addr2']:x}")
        count = event['count']

        sorted_pair = tuple(sorted([field1, field2]))
        pair_counts[sorted_pair] += count

        field_total[field1] += count
        field_total[field2] += count

    cacheline_details: Dict[int, Dict] = defaultdict(lambda: {
        'addrs': set(),
        'addr_details': [],
        'events': [],
        'total_fs': 0,
    })

    for addr in all_addrs:
        cl = addr_to_cacheline(addr)
        cacheline_details[cl]['addrs'].add(addr)

    for event in events:
        cl1 = addr_to_cacheline(event['addr1'])
        cl2 = addr_to_cacheline(event['addr2'])
        cacheline_details[cl1]['events'].append(event)
        cacheline_details[cl1]['total_fs'] += event['count']
        if cl2 != cl1:
            cacheline_details[cl2]['events'].append(event)
            cacheline_details[cl2]['total_fs'] += event['count']

    for cl in cacheline_details:
        addr_fs_count: Dict[int, int] = defaultdict(int)
        for event in cacheline_details[cl]['events']:
            if addr_to_cacheline(event['addr1']) == cl:
                addr_fs_count[event['addr1']] += event['count']
            if addr_to_cacheline(event['addr2']) == cl:
                addr_fs_count[event['addr2']] += event['count']

        details = []
        for addr in sorted(cacheline_details[cl]['addrs']):
            details.append({
                'addr': addr,
                'field': addr_field_map.get(addr, f"0x{addr:x}"),
                'fs_count': addr_fs_count.get(addr, 0),
            })
        cacheline_details[cl]['addr_details'] = details

    return {
        'pair_counts': pair_counts,
        'field_total': field_total,
        'addr_field_map': addr_field_map,
        'cacheline_details': dict(cacheline_details),
    }


def generate_reports(analysis: Dict, output_dir: str, events: List[Dict]):
    os.makedirs(output_dir, exist_ok=True)

    pair_counts = analysis['pair_counts']
    field_total = analysis['field_total']
    addr_field_map = analysis['addr_field_map']
    cacheline_details = analysis['cacheline_details']

    csv_path = os.path.join(output_dir, "cacheline_fs_report.csv")
    with open(csv_path, 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(['字段1', '字段2', 'FS次数'])
        sorted_pairs = sorted(pair_counts.items(), key=lambda x: -x[1])
        for (f1, f2), count in sorted_pairs:
            if f1 == f2:
                continue
            writer.writerow([f1, f2, count])

    print(f"CSV 报告已生成：{csv_path}")

    field_csv_path = os.path.join(output_dir, "field_false_sharing_stats.csv")
    with open(field_csv_path, 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(['字段名称', 'False Sharing 次数'])
        sorted_fields = sorted(field_total.items(), key=lambda x: -x[1])
        for field, count in sorted_fields:
            writer.writerow([field, count])

    print(f"字段汇总已生成：{field_csv_path}")

    txt_path = os.path.join(output_dir, "cacheline_fs_report.txt")
    with open(txt_path, 'w') as f:
        f.write("=" * 100 + "\n")
        f.write("CacheLine False Sharing 分析报告\n")
        f.write("=" * 100 + "\n\n")

        f.write(f"Cache Line 大小: {CACHE_LINE_SIZE} 字节\n")
        f.write(f"总 FS 事件数: {len(events)}\n")
        f.write(f"涉及 Cache Line 数: {len(cacheline_details)}\n")
        f.write(f"唯一地址数: {len(addr_field_map)}\n\n")

        f.write("-" * 100 + "\n")
        f.write("字段对 FS 汇总（格式: 字段1,字段2,FS次数）:\n")
        f.write("-" * 100 + "\n")
        sorted_pairs = sorted(pair_counts.items(), key=lambda x: -x[1])
        for (f1, f2), count in sorted_pairs:
            f.write(f"  {f1},{f2},{count}\n")

        f.write("\n" + "-" * 100 + "\n")
        f.write("每字段 FS 汇总:\n")
        f.write("-" * 100 + "\n")
        sorted_fields = sorted(field_total.items(), key=lambda x: -x[1])
        for field, count in sorted_fields:
            f.write(f"  {field}: {count}\n")

        f.write("\n" + "-" * 100 + "\n")
        f.write("地址-字段映射:\n")
        f.write("-" * 100 + "\n")
        for addr in sorted(addr_field_map.keys()):
            cl = addr_to_cacheline(addr)
            offset = addr - cl
            f.write(f"  0x{addr:x} (+{offset}) -> {addr_field_map[addr]}\n")

        f.write("\n" + "-" * 100 + "\n")
        f.write("详细分析（按 Cache Line）:\n")
        f.write("-" * 100 + "\n\n")

        sorted_cls = sorted(cacheline_details.items(), key=lambda x: -x[1]['total_fs'])
        for i, (cl, detail) in enumerate(sorted_cls, 1):
            f.write(f"【CacheLine #{i}】 0x{cl:x}\n")
            f.write(f"  FS次数: {detail['total_fs']}\n")
            f.write(f"  地址-字段映射:\n")
            for d in detail['addr_details']:
                offset = d['addr'] - cl
                f.write(f"    0x{d['addr']:x} (+{offset}) -> {d['field']}  FS={d['fs_count']}\n")

            f.write(f"\n  该 CacheLine 内的 FS 事件:\n")
            cl_events = []
            for event in detail['events']:
                cl1 = addr_to_cacheline(event['addr1'])
                cl2 = addr_to_cacheline(event['addr2'])
                if cl1 == cl or cl2 == cl:
                    cl_events.append(event)
            cl_events.sort(key=lambda e: e['count'], reverse=True)
            for event in cl_events[:10]:
                f1 = addr_field_map.get(event['addr1'], f"0x{event['addr1']:x}")
                f2 = addr_field_map.get(event['addr2'], f"0x{event['addr2']:x}")
                f.write(f"    0x{event['addr1']:x}({f1}) <-> 0x{event['addr2']:x}({f2})  count={event['count']}\n")
            if len(cl_events) > 10:
                f.write(f"    ... 还有 {len(cl_events) - 10} 个事件\n")
            f.write("\n")

    print(f"文本报告已生成：{txt_path}")
    return csv_path, txt_path


def format_addr_details(details: List[Dict]) -> str:
    parts = []
    for d in details:
        offset = d['addr'] - (d['addr'] // CACHE_LINE_SIZE) * CACHE_LINE_SIZE
        parts.append(f"0x{d['addr']:x}(+{offset})->{d['field']}={d['fs_count']}")
    return '; '.join(parts)


def print_summary(analysis: Dict):
    pair_counts = analysis['pair_counts']
    field_total = analysis['field_total']

    print("\n" + "=" * 80)
    print("CacheLine False Sharing 分析结果")
    print("=" * 80)

    print(f"\n{'字段1':<20} {'字段2':<20} {'FS次数':>8}")
    print("-" * 80)
    sorted_pairs = sorted(pair_counts.items(), key=lambda x: -x[1])
    for (f1, f2), count in sorted_pairs:
        print(f"{f1:<20} {f2:<20} {count:>8}")

    print(f"\n{'字段名称':<20} {'FS次数':>8}")
    print("-" * 40)
    sorted_fields = sorted(field_total.items(), key=lambda x: -x[1])
    for field, count in sorted_fields:
        print(f"{field:<20} {count:>8}")


def main():
    parser = argparse.ArgumentParser(
        description='CacheLine False Sharing 分析器 - 按 cache line 聚合 FS 事件',
    )

    parser.add_argument('--hitm', default=None, help='hitm 文件路径')
    parser.add_argument('--memscope', default=None, help='memscope_resolved.csv 文件路径')
    parser.add_argument('--binary', default=None, help='带调试信息的二进制文件路径（用于 addr2line）')
    parser.add_argument('--work-dir', default=None, help='工作目录')
    parser.add_argument('--results-dir', default=None, help='结果目录')
    parser.add_argument('--output-dir', default=None, help='输出目录')
    parser.add_argument('--cacheline-size', type=int, default=64, help='Cache Line 大小')
    parser.add_argument('--num-options', type=int, default=None, help='numOptions 参数（用于偏移布局精化）')

    args = parser.parse_args()

    global CACHE_LINE_SIZE
    CACHE_LINE_SIZE = args.cacheline_size

    hitm_path = args.hitm
    memscope_path = args.memscope
    binary_path = args.binary
    output_dir = args.output_dir

    if not hitm_path and args.work_dir:
        for fname in ['hitm_a.txt', 'hitm_aa.txt', 'hitm.txt']:
            candidate = os.path.join(args.work_dir, fname)
            if os.path.exists(candidate):
                hitm_path = candidate
                break

    if not hitm_path:
        print("错误：找不到 hitm 文件。")
        sys.exit(1)

    if not binary_path and args.work_dir:
        candidates = []
        for fname in os.listdir(args.work_dir):
            if fname.endswith(('.cpp', '.c', '.txt', '.csv', '.data', '.o', '.bpf.o',
                               '.h', '.hpp', '.md', '.sh', '.py', '.vcproj',
                               '.sln', '.Makefile', '.mk')):
                continue
            candidate = os.path.join(args.work_dir, fname)
            if os.path.isfile(candidate) and os.access(candidate, os.X_OK):
                result = subprocess.run(
                    ['file', candidate],
                    capture_output=True, text=True, timeout=2
                )
                if 'ELF' in result.stdout and 'executable' in result.stdout:
                    candidates.append(candidate)
        if candidates:
            elf_candidates = [c for c in candidates if 'dynamically linked' in subprocess.run(
                ['file', c], capture_output=True, text=True, timeout=2
            ).stdout]
            binary_path = elf_candidates[0] if elf_candidates else candidates[0]

    if not memscope_path and args.results_dir:
        candidate = os.path.join(args.results_dir, 'memscope_resolved.csv')
        if os.path.exists(candidate):
            memscope_path = candidate

    if not output_dir:
        if args.results_dir:
            output_dir = args.results_dir
        elif args.work_dir:
            output_dir = os.path.join(args.work_dir, 'results')
        else:
            output_dir = os.path.dirname(hitm_path)

    print(f"输入文件: {hitm_path}")
    print(f"二进制文件: {binary_path or '(未提供)'}")
    print(f"Memscope: {memscope_path or '(未提供)'}")
    print(f"输出目录: {output_dir}")

    events = parse_hitm_file(hitm_path)
    print(f"\n解析到 {len(events)} 个 F,A (堆上 False Sharing) 事件")

    if not events:
        print("没有检测到堆上 False Sharing 事件，退出。")
        sys.exit(0)

    addr_fields_rip = {}
    if binary_path:
        print("使用 RIP + addr2line 推断字段归属...")
        hitm_txt = hitm_path
        if hitm_path.endswith('_a.txt'):
            hitm_txt_candidate = hitm_path.replace('_a.txt', '.txt')
            if os.path.exists(hitm_txt_candidate):
                hitm_txt = hitm_txt_candidate

        pie_base = detect_pie_base(hitm_txt, binary_path)
        if pie_base:
            print(f"  自动检测 PIE 基地址: 0x{pie_base:x}")
        else:
            print("  无法自动检测 PIE 基地址")

        addr_fields_rip = infer_fields_from_rip(hitm_txt, binary_path, pie_base)
        if addr_fields_rip:
            print(f"  通过 addr2line 推断了 {len(addr_fields_rip)} 个地址的字段")
        else:
            print("  addr2line 未推断出任何字段")

    addr_field_votes = infer_fields_from_source_voting(hitm_path)
    if addr_field_votes:
        print(f"基于源码行号投票推断字段归属... 推断了 {len(addr_field_votes)} 个地址")

    memscope_allocs = []
    if memscope_path:
        memscope_allocs = parse_memscope_data(memscope_path)
        print(f"解析到 {len(memscope_allocs)} 个 memscope 分配记录")

    work_dir_for_detect = args.work_dir or os.path.dirname(hitm_path)
    analysis = analyze(events, addr_fields_rip, addr_field_votes, memscope_allocs, work_dir_for_detect, args.num_options, binary_path)
    print(f"\n分析完成")

    generate_reports(analysis, output_dir, events)
    print_summary(analysis)

    print(f"\n分析完成！")


if __name__ == '__main__':
    main()
