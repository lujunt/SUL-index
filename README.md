# SUL-plain-index

> **Secure Updated Learned Index — 明文实现版本**
> 一个面向多维数据的混合学习索引：上层用多层 GPL 模型预测定位，下层用 ART 自适应基数树处理预测冲突点，通过 Z 曲线把多维坐标映射到一维键值。

参考论文：ALT-Index (Yang et al., 2024) · PGM-Index (Ferragina & Vinciguerra, VLDB 2020) · ART (Leis et al., ICDE 2013)。

完整设计请见 [`sulplian_项目文档.md`](./sulplian_项目文档.md)（v1.3）。

---

## 本次实现范围

| 模块 | 状态 |
|---|---|
| Z 曲线编码（**2-6 维**，16 位/维量化，`key_bytes` 长度 = `2 × dim_count`） | ✅ |
| 学习层多层 GPL 构建（含贪婪线性分段 + 最小二乘拟合 + 多层递归） | ✅ |
| ART 层构建（**Node4 / Node16 / Node48 / Node256** 完整扩容链） | ✅ |
| 学习层 + ART 层点查询 | ✅ |
| 范围查询（边界叶定位 + 候选合并 + 维度过滤） | ✅ |
| **单点插入**（学习层空槽优先，否则下放 ART；自动触发节点扩容） | ✅ |
| `BruteForceScanner` 正确性基准 | ✅ |
| 序列化 / 多 ε 参数扫描 / 实验记录器 | ⏳ 后续工作 |

---

## 环境要求

- 操作系统：WSL2 + Ubuntu 22.04（或任意 Linux）
- 编译器：GCC 11+（支持 C++17）
- 构建系统：CMake ≥ 3.16

安装：
```bash
sudo apt update
sudo apt install -y build-essential cmake
```

---

## 目录结构

```
SUL-index/
├── CLAUDE.md                       # 项目协作约束
├── README.md                       # 本文件
├── sulplian_项目文档.md            # 完整设计文档（v1.3）
├── uniform_20000_1_2_.csv          # 测试数据集（20000 点 · 2D · 均匀分布）
└── sul-index/                      # 工程根目录
    ├── CMakeLists.txt
    ├── main.cpp                    # demo 入口（含 BruteForceScanner 验证）
    ├── include/sul/
    │   ├── types.h                 # DataPoint / GPLInner|LeafNode / ARTNode4|16|48|256 / Leaf
    │   ├── z_order.h               # Z 曲线编码器
    │   ├── gpl_builder.h           # 多层 GPL 构建器
    │   ├── art_tree.h              # ART 树
    │   └── sul_index.h             # 索引主类 SULPlainIndex
    └── src/
        ├── z_order.cpp
        ├── gpl_builder.cpp
        ├── art_tree.cpp
        └── sul_index.cpp
```

---

## 数据集格式

`uniform_20000_1_2_.csv` 每行：

```
x_double, y_double, id_int
```

- `x, y ∈ [0, 1)`：浮点坐标，会被 `scale_unit_double_to_int32` 缩放到 `[0, 2^16)`（即 `[0, 65536)`）整数后再做 Z 曲线编码。
- `id`：原始整数 ID（保存在 `DataPoint.orig_id`，便于追踪）。
- 共 20000 行，无表头。

---

## 启动实验

### 1) 配置 + 编译

```bash
cd sul-index
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

### 2) 运行 demo（默认数据集）

```bash
./build/sul_demo
```

### 3) 指定其他 CSV

```bash
./build/sul_demo /path/to/your_dataset.csv
```

要求 CSV 行格式与上面一致（`x, y, id`，`x, y ∈ [0, 1)`）。

### 4) 指定 error_bound（误差上界 ε）

```bash
./build/sul_demo <csv_path> <error_bound>
```

示例：

```bash
# 使用默认 CSV，ε=8（最紧凑）
./build/sul_demo /path/to/SUL-index/uniform_20000_1_2_.csv 8

# 使用默认 CSV，ε=64（叶子槽更宽裕，ART 冲突点更少）
./build/sul_demo /path/to/SUL-index/uniform_20000_1_2_.csv 64
```

| `error_bound` | 效果 |
|---|---|
| 小（如 8）| 叶子槽紧凑，冲突点多，ART 负担重 |
| 大（如 64）| 叶子槽宽松，冲突点少，内存占用升高 |
| 不传（默认）| 自动计算：`max(8, n / 1000)` |

### 5) 预期输出（uniform_20000_1_2_）

```
===== SUL-plain-index Demo =====
[load] csv: /path/to/SUL-index/uniform_20000_1_2_.csv
[load] read 20000 rows
[cfg]  error_bound=20 dim=2
[build] total_points=20000 leaves=101 inner_layers=2
[build] learning_filled=15675 art_points=4325 art_inner_nodes=8652
[build] elapsed 3.16 ms

----- Point Query -----
[pq] queries=500 hits=500 matches_ground_truth=500
[pq] avg latency = 3.87 us
[pq-miss] random_misses=200 correctly_null=200

----- Range Query -----
[rq] queries=50 avg_returned=190.52 avg_truth=190.52
[rq] recall=1 precision=1
[rq] avg latency = 107.22 us

----- Insertion Test -----
[ins] total=3000 (new=1500 dup=1500)
[ins] learning_layer_inserts=877 art_layer_inserts=2123 failed=0
[ins] avg insert latency = 0.22 us/point
[ins] learning_filled 15675 -> 16552  (+ 877)
[ins] art_points 4325 -> 6073  (+ 1748)
[ins] art_inner_nodes 8652 -> 11904  (+ 3252)
[ins] ART expansions: N4->N16=14, N16->N48=126, N48->N256=0
[ins-pq] queries=3000 hits=3000 hit_rate=1
[ins-pq] avg query latency = 0.22 us/point
```

### 6) 关键指标说明

| 字段 | 含义 |
|---|---|
| `leaves` | 学习层 GPL 叶子节点数 |
| `inner_layers` | 学习层 GPL 内部层数（不含叶子层） |
| `learning_filled` | 直接被学习层精确预测命中的点数 |
| `art_points` | 落入 ART 层的冲突点数 |
| `art_inner_nodes` | 所有 ART 树的内部节点（Node4/16/48/256）总数 |
| `learning_layer_inserts` | 插入测试中命中学习层空槽的次数 |
| `art_layer_inserts` | 插入测试中槽位冲突回退到 ART 层的次数 |
| `ART expansions` | 插入过程中三种 ART 节点扩容的次数：N4→N16、N16→N48、N48→N256 |
| `recall` / `precision` | 与 `BruteForceScanner` 全量扫描对照得到的召回率/精确率 |

### 7) 插入测试场景

`main.cpp` 的 Insertion Test 段一次性覆盖三种关键路径：

| 场景 | 触发方式 | 期望表现 |
|---|---|---|
| **学习层插入** | 1500 个全新随机坐标 | 多数命中 GPL 叶子空槽，`learning_filled` 增加 |
| **ART 层插入** | 1500 个已有点坐标的副本 | 预测槽位已被原数据占用，回退到该叶 ART 树 |
| **ART 节点扩容** | 大量插入造成 ART 内部节点超过容量 | 计数器记录 `N4→N16` / `N16→N48` / `N48→N256` 各发生多少次 |

`insert(coords)` 返回 `InsertResult{ Failed | LearningLayer | ARTLayer }`，便于上层精确统计。新增点用 `std::deque<DataPoint>` 存储，保证 push_back 后不会失效已挂在槽位/ART 中的指针。

---

## 与设计文档的差异（v1.3 修订要点）

| 项 | v1.2 文档 | 本实现 |
|---|---|---|
| `key_bytes` 字节数 | 4（固定） | **动态** `2 × dim_count`（2D→4，4D→8，6D→12，上界 `MAX_KEY_BYTES=16`） |
| 每维量化位宽 | 未定 | **`BITS_PER_DIM = 16`**（2^16 量化级，2D 支持百万级数据） |
| ART 层数 | 4 | **动态** = `2 × dim_count`（2D→4 层，6D→12 层） |
| 叶子 `slot_count` | 固定 64 | **动态** `max(2×seg_len + 2×ε, 8)` |
| ART 节点类型 | 仅 Node4/Node16 | 完整 **Node4 / 16 / 48 / 256** 扩容链 |
| Node48/Node256 设计 | "暂不实现" | **已实现**，与 Node16 同模式 |
| 每叶 ART 树 | 仅冲突叶 | 每叶都挂 `ARTTree`（无冲突为空树） |
| 插入路径 | 未实现 | **已实现** `insert(coords)`，学习层 → ART 自动级联 |

完整修订摘要见 `sulplian_项目文档.md` 顶部 "v1.3 变更摘要"。

---

## 后续计划

- 插入路径的删除/更新对偶操作
- 索引序列化 / 反序列化（参见设计文档 §9）
- 多 ε 参数扫描与实验记录器（§10）
- 多种数据分布（uniform / normal / zipfian）对比
- 并发控制（多线程）
