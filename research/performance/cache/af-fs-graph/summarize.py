import os
import sys
import glob
import json
import re
import subprocess

def color_text(text, color_code):
    return f"\033[{color_code}m{text}\033[0m"

def parse_sharing_edges(dot_file):
    """Return true-sharing, cross-instance and legacy-unclassified self loops."""
    true_sharing_fields = []
    cross_instance_fields = []
    unclassified_fields = []
    if not os.path.exists(dot_file):
        return true_sharing_fields, cross_instance_fields, unclassified_fields
    with open(dot_file, 'r', encoding='utf-8') as f:
        content = f.read()
    pattern = re.compile(r'"([^"]+)"\s*--\s*"([^"]+)"\s*\[(.*?)\]', re.DOTALL)
    for match in pattern.finditer(content):
        if match.group(1) != match.group(2):
            continue
        field_name = match.group(1).split("::")[-1]
        attrs = match.group(3)
        true_match = re.search(r'true_alpha=(\d+)', attrs)
        false_match = re.search(r'false_alpha=(\d+)', attrs)
        alpha_match = re.search(r'(?<!_)alpha=(\d+)', attrs)
        if true_match or false_match:
            true_alpha = int(true_match.group(1)) if true_match else 0
            false_alpha = int(false_match.group(1)) if false_match else 0
            if true_alpha:
                true_sharing_fields.append((field_name, true_alpha))
            if false_alpha:
                cross_instance_fields.append((field_name, false_alpha))
        elif alpha_match and int(alpha_match.group(1)) > 0:
            unclassified_fields.append((field_name, int(alpha_match.group(1))))
    return true_sharing_fields, cross_instance_fields, unclassified_fields

def parse_array_geometry(dot_file):
    """Return element size, array base and same/cross line pair counts."""
    if not os.path.exists(dot_file):
        return None
    with open(dot_file, 'r', encoding='utf-8') as f:
        content = f.read()
    base_match = re.search(r'array_base=(\d+)', content)
    elem_match = re.search(r'array_elem_size=(\d+)', content)
    if not base_match or not elem_match:
        return None
    same_line = sum(int(v) for v in re.findall(r'count1=(\d+)', content))
    cross_line = sum(int(v) for v in re.findall(r'count2=(\d+)', content))
    pair_counts = {}
    for u, v, attrs in re.findall(r'"([^"]+)"\s*--\s*"([^"]+)"\s*\[(.*?)\]', content, re.DOTALL):
        match = re.search(r'count1=(\d+)', attrs)
        if not match:
            continue
        count = int(match.group(1))
        if count == 0:
            continue
        field_a = u.split("::")[-1]
        field_b = v.split("::")[-1]
        if field_a == field_b:
            continue
        key = tuple(sorted((field_a, field_b)))
        pair_counts[key] = pair_counts.get(key, 0) + count
    return {
        "array_base": int(base_match.group(1)),
        "element_size": int(elem_match.group(1)),
        "same_line": same_line,
        "cross_line": cross_line,
        "line_pairs": sorted(pair_counts.items(), key=lambda item: -item[1]),
    }


def summarize_arrays(data_dir):
    print("="*60)
    print("📊 多维数组优化建议 (Array Layout & Transpose)")
    print("="*60)
    txt_files = glob.glob(os.path.join(data_dir, "graph.*.txt"))
    array_files = [
        f for f in txt_files
        if not f.endswith(".layout.txt") and not f.endswith(".placement.txt")
    ]

    if not array_files:
        print("  未检测到数组分析结果。\n")
        return

    for filepath in array_files:
        array_name = os.path.basename(filepath).replace("graph.", "").replace(".txt", "")
        weights = {}
        pairs = {}
        sizes = {}
        kinds = {}
        with open(filepath, 'r', encoding='utf-8') as f:
            for line in f:
                parts = line.strip().split()
                if len(parts) >= 3 and parts[0].startswith("dim"):
                    weights[parts[0]] = float(parts[1])
                    pairs[parts[0]] = int(parts[2])
                    for tok in parts[3:]:
                        if tok.startswith("size="):
                            sizes[parts[0]] = tok[5:]
                        elif tok == "field":
                            kinds[parts[0]] = "field"

        if not weights:
            print(f"🔹 数组实体: {color_text(array_name, '92')}")
            print(f"   ❌ 无法解析 {os.path.basename(filepath)}: 期望 'dim<k> <平均权重> <配对数> [size=<n>] [field]'。")
            print()
            continue

        print(f"🔹 数组实体: {color_text(array_name, '92')}")
        for dim, w in weights.items():
            tag = "字段维" if kinds.get(dim) == "field" else "下标维"
            print(f"   - {dim}[{tag}, 尺寸={sizes.get(dim, '?')}]: 平均权重 = {w}  (配对数 = {pairs.get(dim, 0)})")

        decision_path = filepath.replace(".txt", ".decision.json")
        decision = None
        if os.path.exists(decision_path):
            try:
                with open(decision_path, 'r', encoding='utf-8') as f:
                    decision = json.load(f)
                if decision.get("schema_version") not in (1, 2):
                    raise ValueError("unsupported schema_version")
            except (OSError, ValueError, json.JSONDecodeError) as e:
                print(f"   ❌ 无法解析结构化判决 {os.path.basename(decision_path)}: {e}")
                print()
                continue

        if decision is not None:
            action = decision.get("action")
            selected = decision.get("selected_dimension")
            current = decision.get("current_dimension")
            if action == "keep":
                print(f"   💡 结构化判决: 维持现有布局；{selected} 已是当前最内层。")
            elif action == "axis_permutation":
                if "axis_order" in decision:
                    print("   💡 结构化判决: 执行轴置换，按下方完整轴顺序重排。")
                else:
                    print(f"   💡 结构化判决: 执行轴置换，将 {selected} 移到最内层（当前最内层为 {current}）。")
                print(f"      👉 这表示重排下标顺序，不等同于笼统的“转成 SOA”。")
            elif action == "instance_padding":
                print("   💡 结构化判决: 对数组实例做 Cacheline padding。")
                alignment = decision.get("alignment") or {}
                line = alignment.get("line_size") or 64
                element_stride = alignment.get("element_stride")
                padded_stride = decision.get("element_stride")
                base_mod = alignment.get("base_mod_line")
                steps = []
                if base_mod:
                    steps.append(f"数组起点按 {line} 字节对齐（当前起点 % {line} = {base_mod}，每格都被行边界切开）")
                if element_stride and padded_stride and element_stride != padded_stride:
                    steps.append(f"每格步长从 {element_stride} 补齐到 {padded_stride} 字节"
                                 f"（一格 {element_stride} 字节，一行装不下一格，相邻两格必然共行）")
                if steps:
                    print("      👉 保持字段组织，"
                          + ("同时做两件事（缺一件都不成立）：" if len(steps) == 2 else "只改这一处："))
                    for index, step in enumerate(steps, 1):
                        print(f"         {index}) {step}")
                else:
                    print("      👉 保持字段组织，但让相邻实例从独立缓存行开始。")
            elif action == "partial_soa":
                field = decision.get("selected_field") or "<unknown>"
                print(f"   💡 结构化判决: partial SOA，将热点字段 {field} 拆成独立数组。")
            elif action == "insufficient_evidence":
                print("   ⚠️ 结构化判决: 证据不足，不修改布局。")
            else:
                print(f"   ❌ 未知结构化动作: {action!r}")
            if "axis_order" in decision:
                order = " → ".join(f"dim{d}" for d in decision["axis_order"])
                print(f"      数组轴顺序（最外 → 最内）: {order}")
                print(f"      AoS 实例步长: {decision['element_stride']} 字节")
                if decision.get("field_stride") is not None:
                    print(f"      拆出字段的数组步长: {decision['field_stride']} 字节")
                if action == "instance_padding":
                    alignment = decision.get("alignment") or {}
                    element_stride = alignment.get("element_stride")
                    padded_stride = decision.get("element_stride")
                    elements = 1
                    sized_dims = 0
                    for dim, size in sizes.items():
                        if kinds.get(dim) == "field":
                            continue
                        try:
                            elements *= int(size)
                            sized_dims += 1
                        except ValueError:
                            elements = None
                            break
                    if sized_dims and elements and element_stride and padded_stride and padded_stride != element_stride:
                        before = elements * element_stride
                        after = elements * padded_stride
                        percent = (after - before) * 100.0 / before if before else 0.0
                        print(f"      内存代价: {elements} 格 × {padded_stride - element_stride} 字节补白"
                              f" = {after - before} 字节（数组 {before} → {after} 字节, +{percent:.1f}%）")
            print()
            continue

        usable = {d: w for d, w in weights.items() if pairs.get(d, 0) > 0}
        if not usable:
            print("   ⚠️ 无法判定: 所有维度的配对数都是 0（窗口内没有观测到任何关系）。")
            print()
            continue

        positives = {d: w for d, w in usable.items() if w > 0}
        if not positives:
            neg = ", ".join(f"{d}={w}" for d, w in usable.items() if w < 0)
            print("   ⚠️ 无法判定: 没有任何维度提供正收益证据。")
            if neg:
                print(f"      负值: {neg}")
            else:
                print("      有配对数的维度平均权重均为 0。")
            print("      👉 建议: 保持现状，先排查采样/数据链路，不要据此改动布局。")
            print()
            continue

        max_dim = max(positives, key=positives.get)
        innermost = f"dim{len(weights) - 1}"
        print(f"   💡 诊断: {max_dim} 维度具有最高的平均权重 ({positives[max_dim]})。")
        if max_dim == innermost:
            print(f"      👉 维持现有布局即可: {max_dim} 本来就是最内层（物理上连续的那一维）。")
        else:
            print(f"      👉 强烈建议【转置 (Transpose) / 转为 SOA 架构】，以便让 {max_dim} 维度成为最内层！")
        print()

def summarize_structs(data_dir):
    print("="*60)
    print("📦 结构体内存布局重排建议 (Struct Cacheline Packing)")
    print("="*60)
    layout_files = glob.glob(os.path.join(data_dir, "graph.*.layout.txt"))

    for filepath in layout_files:
        basename = os.path.basename(filepath)
        if basename == "graph.layout.txt":
            continue

        struct_name = basename.replace("graph.", "").replace(".layout.txt", "")

        # 1. Parse Recommended Layout
        recommended_groups = []
        recommended_headers = []
        current_group = []
        with open(filepath, 'r', encoding='utf-8') as f:
            for line in f:
                line = line.strip()
                if line.startswith("[Cacheline"):
                    recommended_headers.append(line.strip("[]"))
                    if current_group:
                        recommended_groups.append(current_group)
                    current_group = []
                elif line.startswith("- "):
                    current_group.append(line[2:])
        if current_group:
            recommended_groups.append(current_group)
            
        flat_recommended = [f for g in recommended_groups for f in g]

        # 2. 调用 dbg-viewtype 直接从二进制提取原生 DWARF 符号偏移
        original = []
        dwarf_error = None
        exe_files = glob.glob(os.path.join(data_dir, "*.exe")) + glob.glob(os.path.join(data_dir, "*.out")) + glob.glob(os.path.join(data_dir, "t*"))
        binary = exe_files[0] if exe_files else None

        if binary is None:
            dwarf_error = "目录下没有找到可执行文件 (.exe/.out/t*)"
        else:
            t_path = os.path.join(data_dir, "tmp_type.out")
            v_path = os.path.join(data_dir, "tmp_var.out")
            try:
                dbg_viewtype = os.environ.get("DBG_VIEWTYPE", "dbg-viewtype")
                proc = subprocess.run([dbg_viewtype, binary, t_path, v_path], capture_output=True, text=True)
                if proc.returncode != 0:
                    dwarf_error = f"dbg-viewtype 退出码 {proc.returncode}: {(proc.stderr or '').strip()[:200]}"
                elif not os.path.exists(t_path):
                    dwarf_error = f"dbg-viewtype 未生成类型文件 {t_path}"
                else:
                    parsing_target = False
                    nodes = []
                    with open(t_path, 'r', encoding='utf-8') as tf:
                        for line in tf:
                            if line.startswith('\t'):
                                if parsing_target:
                                    parts = line.strip().split('\t')
                                    if len(parts) >= 3:
                                        offset = int(parts[0])
                                        name = parts[1]
                                        nodes.append((name, offset))
                            else:
                                line_trim = line.strip()
                                if line_trim:
                                    sname = line_trim.split('\t')[0]
                                    if sname == struct_name:
                                        parsing_target = True
                                    else:
                                        parsing_target = False
                    nodes.sort(key=lambda x: x[1])
                    original = [n[0] for n in nodes]
                    if not original:
                        dwarf_error = f"DWARF 中未找到结构体 '{struct_name}' 的成员（名字不匹配？）"
            except FileNotFoundError:
                dwarf_error = "找不到 dbg-viewtype 可执行文件（不在 PATH 中，且未设置 DBG_VIEWTYPE）"
            except Exception as e:
                dwarf_error = f"调用 dbg-viewtype 失败: {e}"
            finally:
                if os.path.exists(t_path): os.remove(t_path)
                if os.path.exists(v_path): os.remove(v_path)
                
        # 不再过滤原始字段！完整保留 DWARF 中的所有字段（包含如 mana 这样未被访问的冷字段）
        # 解析自环的实例归属：同地址重叠是真共享，不同地址是跨实例伪共享。
        dot_file = os.path.join(data_dir, f"graph.{struct_name}.fields.dot")
        true_sharing_fields, cross_instance_fields, unclassified_fields = parse_sharing_edges(dot_file)
        geometry = parse_array_geometry(dot_file)
        straddles = False
        if geometry:
            line = 64
            element_size = geometry["element_size"]
            base_mod = geometry["array_base"] % line
            stride_mod = element_size % line
            straddles = base_mod != 0 or stride_mod != 0
            print(f"🔹 结构体: {color_text(struct_name, '92')}")
            print(
                f"   🧭 缓存行几何: element={element_size}B line={line}B "
                f"array_base={geometry['array_base']} base%line={base_mod} element%line={stride_mod}"
            )
            print(
                f"      同行跨 worker 冲突 (count1 合计) = {geometry['same_line']}; "
                f"跨行流量 (count2 合计) = {geometry['cross_line']}"
            )
            if straddles:
                print("      → 元素跨缓存行：每个元素被行边界切开，相邻元素共用同一行")
                line_pairs = geometry.get("line_pairs") or []
                if line_pairs:
                    print("      同行冲突按字段对拆开（count1，前 5；同名字段跨实例的部分见下方）：")
                    for (field_a, field_b), count in line_pairs[:5]:
                        print(f"         - {field_a} ↔ {field_b}: {count:,} 次")
                remedies = []
                if base_mod:
                    remedies.append(f"把数组 base 对齐到 {line} 字节")
                if stride_mod:
                    padded = (element_size + line - 1) // line * line
                    remedies.append(f"把元素 stride 从 {element_size} 补齐到 {padded} 字节")
                print(f"   👉 {color_text('优先处方', '93')}: " + "，".join(remedies) + "。")
                print("      对齐后跨元素同行冲突消失；partial SOA 消不掉这类冲突。\n")
            else:
                print("      → 元素独占整行：跨元素不共用缓存行")
        else:
            print(f"🔹 结构体: {color_text(struct_name, '92')}")

        if true_sharing_fields:
            print(f"   🚨 {color_text('真共享警报', '91')}: 侦测到跨核真共享冲突 (True Sharing)！")
            for ts_field, alpha_val in true_sharing_fields:
                print(f"      - 高频竞争字段: {color_text(ts_field, '91')} (跨核真共享冲突强度 Alpha={alpha_val})")
            print(f"   👉 {color_text('优化建议', '93')}: 该字段为多线程高频并发读写热点，建议采用 Thread-Local 线程私有化或原子规约 (Reduce) 降低同步开销。\n")
        if cross_instance_fields:
            print(f"   ⚠️ {color_text('跨实例伪共享', '93')}: 同一抽象字段的不同实例或成员数组元素发生同行竞争。")
            for field, alpha_val in cross_instance_fields:
                print(f"      - 字段: {color_text(field, '93')} (跨实例冲突强度 Alpha={alpha_val})")
            if straddles:
                print("   👉 优先处方: 先把 stride/base 对齐到缓存行；对齐后仍剩下的同元素同行冲突才考虑 partial SOA。")
            else:
                print("   👉 优先处方: partial SOA（将热点字段拆成独立数组）；备选为实例间 padding / thread-partitioned AoS。")
            print("      原子操作不能消除这种伪共享，不能作为默认处方。\n")
        if unclassified_fields:
            print("   ⚠️ 旧版 DOT 自环缺少 true_alpha/false_alpha，无法区分真共享与跨实例伪共享；不自动开处方。")
        if not original:
            print(f"   ❌ 错误: 无法取得原始布局 —— {dwarf_error}")
            print("      DWARF 是原始字段顺序的唯一真值来源，缺少它无法做布局对比。")
            print("      下面只列出推荐分组，不给出对比结论：")
            for i, group in enumerate(recommended_groups):
                print(f"      [{recommended_headers[i]}]: " + " | ".join(group))
        else:
            if original == flat_recommended:
                print("   ✅ 诊断: 推荐字段顺序与当前相同；仍需按推荐 offset/padding 检查缓存行隔离。")
                print("   [维持现有布局]: " + " -> ".join(original))
                for i, group in enumerate(recommended_groups):
                    print(f"      [{recommended_headers[i]}]: " + " | ".join(group))
            else:
                print("   ⚠️ 诊断: 生成了候选布局；需同时应用 offset/padding，并实测验证收益：")
                print("   [原始代码顺序]: " + " -> ".join(original))
                print("   [推荐物理布局]: ")
                
                for i, group in enumerate(recommended_groups):
                    colored_group = []
                    for field in group:
                        orig_idx = original.index(field) if field in original else -1
                        rec_idx = flat_recommended.index(field)
                        if orig_idx == rec_idx:
                            colored_group.append(field) # 没动位置
                        else:
                            colored_group.append(color_text(f"{field}(*)", "93")) # 移动了位置，标黄+星号
                    print(f"      📦 {recommended_headers[i]}: " + " | ".join(colored_group))
        print()

def summarize_globals(data_dir):
    print("="*60)
    print("🌍 离散全局变量打包建议 (Global Variables Alignment)")
    print("="*60)
    global_file = os.path.join(data_dir, "graph.layout.txt")
    if not os.path.exists(global_file):
        print("  未检测到全局变量布局结果。\n")
        return

    with open(global_file, 'r', encoding='utf-8') as f:
        lines = f.readlines()

    valid_lines = [l.strip() for l in lines if l.strip()]

    if not valid_lines:
        print("  无离散全局变量需要打包。\n")
        return

    print("🔹 全局变量亲和性分组:")
    for i, line in enumerate(valid_lines):
        print(f"   📦 Cacheline {i}: {line}")
    print("\n   💡 建议: 将属于同一 Cacheline 的全局变量在 C/C++ 中紧凑定义，或利用 __attribute__((aligned(64))) 将它们对齐在一块。")
    print()

if __name__ == "__main__":
    import platform
    if platform.system() == "Windows":
        os.system("color") 

    target_dir = sys.argv[1] if len(sys.argv) > 1 else "."
    if not os.path.isdir(target_dir):
        print(f"错误: 目录 '{target_dir}' 不存在。")
        sys.exit(1)
        
    print(f"\n🚀 正在分析数据目录: {target_dir}\n")
    summarize_arrays(target_dir)
    summarize_structs(target_dir)
    summarize_globals(target_dir)
