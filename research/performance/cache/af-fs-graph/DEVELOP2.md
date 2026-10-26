# DEVELOP2.md — 结构体间字段重排：算法设计

本文件描述“结构体间字段重排”（合并 / 迁移 / 分裂）的算法本体。

举例：
```
   1) 合并: PosSample::w | PosSample::x | PosSample::y | PosSample::z | VelSample::dw | VelSample::dx | VelSample::dy | VelSample::dz → 容器 PosSample|VelSample
含义：新容器里装这 8 个字段——原 PosSample 的四个分量 x/y/z/w，加原 VelSample 的四个分量 dx/dy/dz/dw。字段名前面的 PosSample:: / VelSample:: 是“原名归属”，即这个字段原来出自哪个结构体。

   1) 迁移: LiveState::acc → 容器 PosSample|VelSample
含义：把 LiveState 结构体里的 acc 这一个字段搬出去，放进上一条刚合并出来的那个容器（PosSample|VelSample）。

   1) 分裂: StageBuf::s0 | StageBuf::spare | StageBuf::s1 | StageBuf::s2 | StageBuf::s3 → 容器 StageBuf
含义：StageBuf 现在是一个结构体、一个实例，五个字段 s0 s1 s2 s3 spare 全在 64 字节里、同一行。这条建议把它拆成 4 个容器（报告下面列的“组 1..4”才是拆分结果，箭头左边那个字段列表只是“被拆的字段全集”，不是“合成一个容器”）。
```

## 0. 实施状态

| 阶段 | 状态 | 位置 |
|---|---|---|
| P1 跨对象图 + 线程位图 | 已实现，未编译 | `generator/src/main.rs`（`--cross-object`） |
| P2 规划器 | 已实现，未编译 | `planner/`（新 crate） |
| P3 报告一节 | 已实现 | `summarize.py`（`summarize_cross_struct`） |
| 流水线接入 | 已改 | `benchmark/Makefile`（`--cross-object`、跳过 cross dot、调用 planner） |
| P4 验收 | 待跑 | `benchmark/test_fields3.cpp` |
| P5 实测对照 | 未开始 | — |

算法逻辑已用等价 Python 版本在合成图上验证：对 `test_fields3` 的三类结构，输出为“`StageBuf` 按线程拆成 4 个容器、`PosSample` 与 `VelSample` 合并、`LiveState::acc` 迁移进合并后的容器、`ColdBox` 与 `g_bias` 被约束拒绝”，与 §10 的验收预期一致。

## 1. 范围

现有工具只在**单个结构体内部**决定字段顺序与缓存行归属。本文档处理**跨结构体**的字段归属问题，三种操作在同一形式化下统一求解：

| 操作 | 语义 |
|---|---|
| 合并 merge | 把两个结构体的部分字段放进同一个容器 |
| 迁移 migrate | 把某结构体的一个字段移入已存在的容器 |
| 分裂 split | 把某结构体的字段拆进两个及以上容器 |

三者都是“字段到容器的划分”的不同标签，共用同一目标函数与同一可行域，因此实现为一个求解器。

## 2. 记号

- 字段集合 $F$。每个字段 $f\in F$ 带：
  - $\mathrm{owner}(f)$：所属结构体；
  - $s_f$：字段字节大小；
  - $n_f$：该字段所在变量的实例数（容器实例数）；
  - $T_f$：观测到访问该字段实例的线程集合。
- 跨对象边集合 $E=\{(f,g)\}$，每条边携带：
  - $\alpha_e=\text{count1}+\text{count2}$：跨 worker 写对数；
  - $\beta_e=\text{count3}+\text{count4}$：同线程/同核亲和对数；
  - 其中 $\text{count1}$ 专指**同一缓存行**的跨 worker 写对数，是冲突的直接证据。
- 布局 $\mathcal L$：把 $F$ 划分为若干容器 $C_1,\dots,C_m$；每个容器有成员集合、实例数、步长与行数。
- 地址函数 $a_{\mathcal L}(f,i)$：第 $i$ 个实例中字段 $f$ 在新布局下的地址。
- 缓存行宽 $L$（默认 64 字节）。

## 3. 目标函数

对布局 $\mathcal L$，逐边求和：

$$
\Phi(\mathcal L)=\sum_{(f,g)\in E}\sum_{i}\;
w\big(a_{\mathcal L}(f,i),\,a_{\mathcal L}(g,i),\alpha_e,\beta_e\big)
\;-\;\lambda\sum_{f\in F}\mathrm{ind}(\mathcal L,f)
$$

单条边的权重与 `summarier::score_candidate` 同口径，按两端在新布局下是否同行取值：

$$
w=\begin{cases}
k\beta_e-g\alpha_e, & \text{两端同行}\\[2pt]
k(\alpha_e+\beta_e)/\gamma, & \text{两端异行}
\end{cases}
$$

$\mathrm{ind}(\mathcal L,f)$ 是布局引入的额外寻址层数（见 §5 约束 2），$\lambda$ 为其单位代价。

**增益**：方案 $\mathcal L$ 相对现状 $\mathcal L_0$ 的收益为 $\Delta\Phi=\Phi(\mathcal L)-\Phi(\mathcal L_0)$；只接受 $\Delta\Phi>0$ 的方案。

## 4. 布局参数化

一个容器由三元组确定：

$$
C=\big(\text{members},\ \text{instances},\ \text{stride}\big),\qquad
\text{stride}=\left\lceil \frac{\sum_{f\in C}s_f}{L}\right\rceil\cdot L
$$

容器内成员偏移由字节累加与对齐规则给出，因此地址函数完全由划分决定。搜索空间是“字段到容器的划分”这一离散空间，不含连续变量。

## 5. 可行域

以下四类约束在**每次状态转移时**检查，不满足则该转移不可行，而不是事后过滤。

1. **基数相容**：$n_f=n_C$。一侧为单例（$n=1$）时视为广播，默认拒绝。
2. **分片同构**：字段线程集合与目标容器的线程集合必须可映射。
   - $T_f=T_C$：同构，$\mathrm{ind}=0$；
   - $T_f\subsetneq T_C$ 且可通过一次实例索引重排对齐：允许，$\mathrm{ind}=1$，计入 $\lambda$ 惩罚；
   - 其余情况：不可行。
3. **容量**：$\sum_{f\in C}s_f\le \text{stride}_C$，且 $\text{stride}_C \bmod L=0$。
4. **不可迁移集** $\mathcal U$：原子字段、成员数组、地址逃逸字段。$f\in\mathcal U$ 时禁止涉及 $f$ 的成员变更。`volatile` 仅告警（见 §10 D1）。

## 6. 邻域算子

三种操作统一为对划分的三种邻域动作：

- **合并** $M(C_i,C_j)$：要求 $n_{C_i}=n_{C_j}$ 且 $T_{C_i}=T_{C_j}$；结果容器为成员并集。
- **迁移** $G(f,C)$：要求 $n_f=n_C$ 且 $T_f\subseteq T_C$。
- **分裂** $S(C,\mathcal P)$：按线程集合把 $C$ 的成员分为若干子容器，每个子容器内线程集合互不相同。

## 7. 求解算法

采用“最优改进”式局部搜索：每一轮同时评估三类邻域动作，取增益最大者执行，直到没有正增益动作或达到轮数上限。

```
输入: F, E, 初始划分 L0(现状), 参数 k,g,gamma,L,lambda, 上限 R
输出: 划分 L*, plan.json

1  初始划分 L <- L0: 每个 owner 的字段构成一个容器（复现当前 AoS 布局）
2  Phi <- score(L); 记录基线
3  repeat 至多 R 轮:
4      m1 <- 最优迁移 best_migrate(L)     # 单字段移入其他容器
5      m2 <- 最优合并 best_merge(L)       # 两个容器合并
6      m3 <- 最优分裂 best_split(L)       # 一个容器按线程集合拆开
7      若三者的最大增益 <= 0: break
8      执行增益最大的动作, 记录动作与 phi 变化, 更新 L
9  输出 L*, 每个容器的 (members, instances, bytes, lines), 动作历史, 拒绝原因
```

**评分** $score(L)$ 对每条边按 §3 计算；两端同容器时用容器内偏移判断是否同行，异容器一律视为异行（容器按行对齐）。

**迁移的寻址惩罚**：当字段线程集合是目标容器线程集合的真子集时，扣 $\lambda$。

**增量求值**：一次动作只影响相关容器与相邻边，重算 $O(\deg)$；实现上当前直接全量重算 $score$，规模为字段数十、边数百，代价可忽略。

## 8. 参数与复杂度

| 参数 | 默认 | 说明 |
|---|---|---|
| $k$ | 1 | 亲和权重 |
| $g$ | 15 | 冲突权重 |
| $\gamma$ | 10 | 异行折扣 |
| $L$ | 64 | 缓存行宽 |
| $\lambda$ | $0.1\cdot\overline{\Delta_e}$ | 额外寻址单位代价，初值取平均边增益的 10% |
| $K$ | 8 | 每字段每轮尝试的目标容器数 |
| $R$ | 200 | 局部搜索轮数上限 |

复杂度：

- 剪枝与排序 $O(E\log E)$；
- 阶段 A 用最大堆维护候选对，单步 $O(\log E)$，最多 $|F|$ 步，即 $O(|F|\log E)$；
- 阶段 B 每轮 $O(|F|\cdot K\cdot \overline{\deg})$；取 $|F|\le 10^2$、$K=8$、$R=200$ 时为 $2\times 10^5$ 量级增量求值，毫秒级。

## 9. 输入与输出

### 9.1 输入图 `graph.fields.cross.dot`（新增，仅 `--cross-object` 时生成）

容器候选**只包含结构体字段**。标量全局变量由 `package_simple` 负责，不进入跨结构体规划。

```
"PosSample::x" [owner="PosSample", type="volatile long int", offset=0, size=8,
                element_stride=32, instances=32, struct_size=32,
                layout_ambiguous=0, threads="0,1,2,3"];
"StageBuf::s0" [owner="StageBuf", ..., element_stride=0, instances=1,
                struct_size=64, layout_ambiguous=0, threads="0"];
"VelSample::dx" -- "PosSample::x" [label="(a,b)", alpha=a, beta=b,
                count1=..., count2=..., count3=..., count4=...,
                count1_same=..., count2_same=..., count3_same=..., count4_same=...,
                true_alpha=..., false_alpha=..., alpha_kind="2"];
```

`element_stride=0, instances=1` 表示单实例（无数组）；`struct_size` 是结构体自身大小，供几何诊断在没有元素步长时使用。

入边规则：

- 两端都是结构体字段，且不是同一抽象字段（同一字段跨实例的配对由几何分析负责）；
- 跨 owner：全部保留；
- 同 owner：仅当两个端点的访问线程集合不同才保留，这是“同一容器内被不同线程写”的分裂信号。

`countK_same` 只统计两个访问的**下标列表完全相同**的配对（忽略变量名与成员路径，例如 `pos_arr[i][t].x` 与 `live_arr[i][t].acc` 都属于 `[i][t]`）。跨实例、跨下标的配对被排除，因为容器变换改变不了它们。求解器默认以同索引计数打分，属性缺失时回退到总计数。

`alpha_kind` 取值：`1`（仅有同行跨 worker 写）、`2`（仅有异行）、`mixed`、`none`。
`layout_ambiguous=1` 表示同一 owner 下观测到多种 (stride, instances) 组合，此时 `element_stride`、`instances` 置 0，求解器必须拒绝涉及该 owner 的合并与迁移。

### 9.2 输出 `graph.fields.cross.plan.json`

```json
{
  "schema_version": 1,
  "source": "graph.fields.cross.dot",
  "line": 64,
  "lambda": 900.0,
  "current_score": 0.0,
  "final_score": 0.0,
  "gain": 0.0,
  "footprint_before": 4096,
  "footprint_after": 6144,
  "actions": [
    {"action": "migrate", "container": "PosSample", "fields": ["LiveState::acc"], "groups": [],
     "instances": 32, "bytes": 40, "lines": 1,
     "score_before": 0.0, "score_after": 900.0, "strict_thread_superset": false},
    {"action": "split", "container": "StageBuf",
     "fields": ["StageBuf::s0", "StageBuf::s1", "StageBuf::spare"],
     "groups": [["StageBuf::s0", "StageBuf::spare"], ["StageBuf::s1"]],
     "instances": 1, "bytes": 64, "lines": 1,
     "score_before": 900.0, "score_after": 2100.0, "strict_thread_superset": false}
  ],
  "rejected": [
    {"fields": "PosSample | VelSample", "reason": "thread_partition_mismatch", "count": 3}
  ],
  "containers": [
    {"container": "PosSample", "fields": ["PosSample::x", "LiveState::acc"],
     "instances": 32, "bytes": 64, "lines": 1, "threads": "0,1,2,3",
     "placements": [{"field": "PosSample::x", "offset": 0, "size": 8},
                    {"field": "LiveState::acc", "offset": 8, "size": 8}]}
  ]
}
```

`action` 取值：`migrate`、`merge`、`split`；`split` 额外给出 `groups`（每个新容器的成员）。`footprint_before/after` 是整个规划范围的容器总占用（按行取整），用于判断“得分换什么内存代价”。`rejected` 的 `reason` 包括 `cardinality_mismatch`、`thread_partition_mismatch`、`capacity_exceeded`、`ambiguous_layout`、`non_positive_gain`（可行但无收益）。

### 9.3 报告内的优先级仲裁

三类结论来自不同分析路径，必须在报告末尾统一裁决，否则读者无法判断该做哪一个：

| 优先级 | 来源 | 触发条件 |
|---|---|---|
| 1 | 跨结构体动作（合并/迁移/分裂） | `plan.json` 中存在正增益动作 |
| 2 | 缓存行几何（base 对齐 / stride 补齐） | 元素跨行且 `count1+count2 > 0` |
| 3 | 字段顺序（`package_fields`） | 元素内字段存在跨线程同行冲突且不满足优先级 1 |
| 4 | 数组轴置换（`summarier`） | 仅当对象不在优先级 1 的涉及集合内 |

被高优先级动作覆盖的对象，其低优先级建议在“结论汇总”一节标注为备选，而不是删除：几何修复与结构重排可以并存，只是顺序不同。

单实例结构体（无数组）的处置单列：若其字段分属不同线程并存在同行冲突，几何手段（base/stride）无效，唯一处方是按线程拆分字段。

## 10. 改动清单与分期

| 阶段 | 改动 | 影响现有产物 |
|---|---|---|
| P1 | `generator`：`--cross-object` 输出 `graph.fields.cross.dot`；节点补充 `threads` 位图、`struct_size`；边补充 `countK_same` | 默认关闭，单对象产物只多 `struct_size` |
| P2 | 新 crate `planner`：解析、约束、局部搜索、`plan.json` | 新增产物 |
| P3 | `summarize.py`：新增“跨结构体字段重排建议”一节；几何处方仅在存在冲突时给出；单实例结构体也输出几何行 | 报告多一段，几何措辞变化 |
| P4 | 以 `test_fields3.c` 做验收：合并 / 迁移 / 分裂各一例 | — |
| P5 | 手工实现一个方案做实测对照 | 正负收益均记录 |

验收依据为 `test_fields3.c` 中三类结构：`PosSample` 与 `VelSample`（合并）、`LiveState::acc`（迁移）、`StageBuf::s0..s3`（分裂）；`ColdBox`（线程集合仅含 0）作为负例，不应被移动或合并。

## 11. 算法能保证与不能保证

**能保证**

- 输出布局的得分不劣于现状：阶段 A、B 只在 $\Phi$ 改进时接受状态；
- 每条推荐都带可行域检查结果；每条被拒的候选都带拒绝原因。

**不能保证**

- 全局最优：贪心加局部搜索仅为局部最优，无最优性证明；
- 跨算例可比：$k,g,\gamma,\lambda$ 未标定，得分只在同一算例内可比；
- 概率解释：窗口内配对是“每个逻辑前驱至多计一次”的计数，不是采样概率，$\alpha,\beta$ 只可作相对强弱；
- 收益外推：仅有两个测试用例，结论限于“机制可行”，不能主张普适收益。

## 12. 待确认决策

- **D1**：`volatile` 字段按“不可迁移”处理，还是仅告警？现有 fixture 中所有字段都是 `volatile long int`，若按不可迁移处理则算法在所有现有算例上空转，建议仅告警。
- **D2**：`planner` 单独成 crate，还是并入 `summarier`？`summarier` 的候选层以单对象坐标为中心，并入需重写整个候选层并牵动冻结基线，建议单独成 crate。
- **D3**：`type_layout.rs` 目前被四个 crate 各自 `#[path]` 引用；新增 `planner` 会形成第五份拷贝，建议同步抽为 `common`（纯重构，输出不变）。
