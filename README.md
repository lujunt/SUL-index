# SUL-plain-index

> **Secure Updated Learned Index — 明文实现版本**
> 一个面向多维数据的混合学习索引：上层用多层 GPL 模型预测定位，下层用 ART 自适应基数树处理预测冲突点，通过 Z 曲线把多维坐标映射到一维键值。

参考论文：ALT-Index (Yang et al., 2024) · PGM-Index (Ferragina & Vinciguerra, VLDB 2020) · ART (Leis et al., ICDE 2013)。

完整设计请见 [`sulplian_项目文档.md`](./sulplian_项目文档.md)（v1.3）。

---

## 本次实现范围

| 模块 | 状态 |
|---|---|
| Z 曲线编码（2-4 维，32 位整数坐标，8 字节 key_bytes） | ✅ |
| 学习层多层 GPL 构建（含贪婪线性分段 + 最小二乘拟合 + 多层递归） | ✅ |
| ART 层构建（**Node4 / Node16 / Node48 / Node256** 完整扩容链） | ✅ |
| 学习层 + ART 层点查询 | ✅ |
| 范围查询（边界叶定位 + 候选合并 + 维度过滤） | ✅ |
| `BruteForceScanner` 正确性基准 | ✅ |
| 插入路径 / 序列化 / 实验记录器 | ⏳ 后续工作 |

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

- `x, y ∈ [0, 1)`：浮点坐标，会被 `scale_unit_double_to_int32` 缩放到 `[0, INT32_MAX)` 整数后再做 Z 曲线编码。
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

### 4) 预期输出（uniform_20000_1_2_）

```
===== SUL-plain-index Demo =====
[load]  csv: /path/to/SUL-index/uniform_20000_1_2_.csv
[load]  read 20000 rows
[cfg]   error_bound=20 dim=2
[build] total_points=20000 leaves=101 inner_layers=2
[build] learning_filled=15675 art_points=4325 art_inner_nodes=25299
[build] elapsed 4.12 ms

----- Point Query -----
[pq] queries=500 hits=500 matches_ground_truth=500
[pq] avg latency = 3.57 us
[pq-miss] random_misses=200 correctly_null=200

----- Range Query -----
[rq] queries=50 avg_returned=190.5 avg_truth=190.5
[rq] recall=1 precision=1
[rq] avg latency = 119.46 us
```

### 5) 关键指标说明

| 字段 | 含义 |
|---|---|
| `leaves` | 学习层 GPL 叶子节点数 |
| `inner_layers` | 学习层 GPL 内部层数（不含叶子层） |
| `learning_filled` | 直接被学习层精确预测命中的点数 |
| `art_points` | 落入 ART 层的冲突点数 |
| `art_inner_nodes` | 所有 ART 树的内部节点（Node4/16/48/256）总数 |
| `recall` / `precision` | 与 `BruteForceScanner` 全量扫描对照得到的召回率/精确率 |

---

## 与设计文档的差异（v1.3 修订要点）

| 项 | v1.2 文档 | 本实现 |
|---|---|---|
| `key_bytes` 字节数 | 4 | **8**（z_value 视为 64 位） |
| ART 层数 | 4 | **8** |
| 叶子 `slot_count` | 固定 64 | **动态** `max(2×seg_len + 2×ε, 8)` |
| ART 节点类型 | 仅 Node4/Node16 | 完整 **Node4 / 16 / 48 / 256** 扩容链 |
| Node48/Node256 设计 | "暂不实现" | **已实现**，与 Node16 同模式 |
| 每叶 ART 树 | 仅冲突叶 | 每叶都挂 `ARTTree`（无冲突为空树） |

完整修订摘要见 `sulplian_项目文档.md` 顶部 "v1.3 变更摘要"。

---

## 后续计划

- 插入路径与原地更新
- 索引序列化 / 反序列化（参见设计文档 §9）
- 多 ε 参数扫描与实验记录器（§10）
- 多种数据分布（uniform / normal / zipfian）对比
- 并发控制（多线程）
