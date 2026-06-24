#!/usr/bin/env python3
"""
将 cacheline_fs_report.csv 中的 false sharing 次数叠加到 dfg_heap.dot 的边权重上，
输出 all_heap.dot。若节点或边不存在则自动创建（新边缺省 weight=0）。

用法: python3 overlay_fs_to_dot2.py <results_dir>
"""

import sys
import os
import re
import csv


def parse_dot(filepath):
    """解析 DOT 文件，返回 (nodes, edges, header_lines, footer_lines, skipped_node_ids)"""
    nodes = {}          # label -> node_id
    nodes_rev = {}      # node_id -> (label, type_val, size_val, fillcolor)
    edges = {}          # (src_id, dst_id) -> (line, weight)
    header_lines = []
    footer_lines = []
    skipped_node_ids = set()  # "地址(+偏移)"格式的节点 ID
    max_id = 0

    # 匹配无法识别具体变量的原始格式
    raw_addr_pattern = re.compile(r'^0x[0-9a-fA-F]+\(\+\d+\)$')
    raw_offset_pattern = re.compile(r'^\+0x[0-9a-fA-F]+$')
    skip_pattern = lambda label: raw_addr_pattern.match(label) or raw_offset_pattern.match(label)

    with open(filepath, 'r') as f:
        lines = f.readlines()

    edge_started = False
    for line in lines:
        stripped = line.strip()

        # 节点定义:  0 [label="ClassicFalseSharing.field0", type="?", size="0", fillcolor="lightblue"];
        node_match = re.match(
            r'^(\d+)\s+\[label="([^"]+)",\s*type="([^"]*)",\s*size="([^"]*)",\s*fillcolor="([^"]*)"\]',
            stripped
        )
        if node_match:
            nid = int(node_match.group(1))
            label = node_match.group(2)
            type_val = node_match.group(3)
            size_val = node_match.group(4)
            fillcolor = node_match.group(5)
            if skip_pattern(label):
                skipped_node_ids.add(nid)
                if nid > max_id:
                    max_id = nid
                continue  # 跳过此节点，不加入 nodes/nodes_rev/header_lines
            nodes[label] = nid
            nodes_rev[nid] = (label, type_val, size_val, fillcolor)
            if nid > max_id:
                max_id = nid
            if not edge_started:
                header_lines.append(line)
            continue

        # 边定义:  0 -> 5 [label="18", weight="18", penwidth="6.0"];
        edge_match = re.match(
            r'^(\d+)\s*->\s*(\d+)\s*\[label="(\d+)",\s*weight="(\d+)",\s*penwidth="([\d.]+)"\]',
            stripped
        )
        if edge_match:
            edge_started = True
            src = int(edge_match.group(1))
            dst = int(edge_match.group(2))
            weight = int(edge_match.group(4))
            # 跳过连接到被过滤节点的边
            if src in skipped_node_ids or dst in skipped_node_ids:
                continue
            edges[(src, dst)] = (line, weight)
            continue

        if edge_started:
            footer_lines.append(line)
        else:
            header_lines.append(line)

    return nodes, nodes_rev, edges, header_lines, footer_lines, max_id


def calc_penwidth(weight):
    """根据权重计算 penwidth, 上限 6.0"""
    pw = 1.0 + weight * 0.3
    return min(pw, 6.0)


def format_edge(src_id, dst_id, weight):
    """格式化边行"""
    pw = calc_penwidth(weight)
    return f'  {src_id} -> {dst_id} [label="{weight}", weight="{weight}", penwidth="{pw:.1f}"];\n'


def format_node(node_id, label, type_val="?", size_val="0", fillcolor="lightblue"):
    """格式化节点行"""
    return f'  {node_id} [label="{label}", type="{type_val}", size="{size_val}", fillcolor="{fillcolor}"];\n'


def process(results_dir):
    csv_path = os.path.join(results_dir, 'cacheline_fs_report.csv')
    dot_path = os.path.join(results_dir, 'dfg_heap.dot')
    out_path = os.path.join(results_dir, 'all_heap.dot')

    if not os.path.isfile(csv_path):
        print(f"错误: 找不到 {csv_path}")
        sys.exit(1)
    if not os.path.isfile(dot_path):
        print(f"错误: 找不到 {dot_path}")
        sys.exit(1)

    nodes, nodes_rev, edges, header, footer, max_id = parse_dot(dot_path)
    next_id = max_id + 1

    new_nodes = 0
    new_edges = 0
    updated = 0

    # 读取 CSV
    csv_rows = []
    with open(csv_path, 'r', encoding='utf-8') as f:
        reader = csv.reader(f)
        next(reader, None)  # 跳过标题行
        for row in reader:
            if len(row) < 3:
                continue
            src_label = row[0].strip()
            dst_label = row[1].strip()
            try:
                delta = int(row[2].strip())
            except ValueError:
                continue
            csv_rows.append((src_label, dst_label, delta))

    # 匹配无法识别具体变量的原始格式：
    # 1. "0xHEX(+N)" 如 "0x5639f0937430(+48)"
    # 2. "+0xN" 如 "+0x20"
    raw_addr_pattern = re.compile(r'^0x[0-9a-fA-F]+\(\+\d+\)$')
    raw_offset_pattern = re.compile(r'^\+0x[0-9a-fA-F]+$')
    skip_pattern = lambda label: raw_addr_pattern.match(label) or raw_offset_pattern.match(label)

    # 第一遍：确保所有节点存在（跳过原始地址+偏移格式的 label）
    skipped_labels = set()
    for src_label, dst_label, delta in csv_rows:
        for label in (src_label, dst_label):
            if skip_pattern(label):
                skipped_labels.add(label)
                continue
            if label not in nodes:
                nodes[label] = next_id
                nodes_rev[next_id] = (label, label, "0", "lightblue")
                # print(f"新建节点: {label} (id={next_id})")
                next_id += 1
                new_nodes += 1

    # 第二遍：处理边（跳过含原始地址 label 的边）
    for src_label, dst_label, delta in csv_rows:
        if src_label in skipped_labels or dst_label in skipped_labels:
            continue
        src_id = nodes[src_label]
        dst_id = nodes[dst_label]
        key = (src_id, dst_id)

        if key in edges:
            old_line, old_weight = edges[key]
            new_weight = old_weight - delta
            new_line = format_edge(src_id, dst_id, new_weight)
            edges[key] = (new_line, new_weight)
            updated += 1
        else:
            # 缺省 weight=0，减去 delta
            new_weight = 0 - delta
            new_line = format_edge(src_id, dst_id, new_weight)
            edges[key] = (new_line, new_weight)
            # print(f"新建边: {src_label}(id={src_id}) -> {dst_label}(id={dst_id}), weight=0-{delta}={new_weight}")
            new_edges += 1

    # 写入输出文件
    with open(out_path, 'w') as f:
        # header (包含原有节点定义)
        for line in header:
            f.write(line)
        # 新增节点
        for nid in sorted(nodes_rev.keys()):
            if nid > max_id:
                label, t, s, fc = nodes_rev[nid]
                f.write(format_node(nid, label, t, s, fc))
        # 所有边（原有 + 新增）
        # 按 src_id, dst_id 排序以保证输出稳定
        for (src, dst) in sorted(edges.keys()):
            f.write(edges[(src, dst)][0])
        # footer
        for line in footer:
            f.write(line)

    print(f"完成: 新建节点 {new_nodes}, 新建边 {new_edges}, 更新边 {updated}, 输出到 {out_path}")


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(f"用法: {sys.argv[0]} <results_dir>")
        sys.exit(1)
    process(sys.argv[1])