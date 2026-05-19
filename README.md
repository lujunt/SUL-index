# SUL-index

> **Secure Updated Learned Index** — 面向多维数据的混合学习索引
> 上层用多层 GPL 模型预测定位，下层用 ART 自适应基数树处理预测冲突点，通过 Z 曲线把多维坐标映射到一维键值。

包含两套实现：

| 版本 | 数据形态 | 依赖 | 文档 |
|------|---------|------|------|
| **明文** SUL-plain-index | 明文索引参数 + 明文坐标 | 仅 CMake + GCC | `sul_项目文档.md` §1–§11 |
| **密文** SUL-cipher-index | Paillier 同态加密索引 + DSP/DAP 安全协议 | ophelib + NTL + GMP | `sul_项目文档.md` §12–§17 |

参考论文：ALT-Index (Yang et al., 2024) · PGM-Index (Ferragina & Vinciguerra, VLDB 2020) · ART (Leis et al., ICDE 2013) · Paillier 安全协议（OSM / SIC / SPI）见 `sul-index/agreements/`

---

## 环境要求

- Ubuntu 22.04（或同等 Linux）
- GCC 11+（C++17）
- CMake ≥ 3.16

明文版只需：

```bash
sudo apt update && sudo apt install -y build-essential cmake
```

密文版额外需要 NTL / GMP / ophelib，由 `setup_cipher.sh` 一键准备。

---

## 目录结构

```
SUL-index/
├── README.md                       # 本文件
├── sul_项目文档.md                 # 完整设计文档 v1.4（明文 + 密文）
├── SUL-index_plan.md               # 密文版本设计草案
└── sul-index/                      # 工程根目录
    ├── CMakeLists.txt
    ├── setup_cipher.sh             # 密文版依赖一键准备脚本
    ├── datasets/                   # 原始数据集（CSV）
    │   └── uniform_20000_1_2_.csv
    ├── query/                      # 查询窗口（自动生成）
    │   └── uniform_20000_{0.25,0.5,1,2,4}.csv
    ├── main.cpp                    # 明文 demo（含 BruteForceScanner 真值对比）
    ├── main_cipher.cpp             # 密文 demo（CSV 数据 + CSV 查询驱动）
    ├── main_compare.cpp            # 明文 vs 密文 正确性对比
    ├── main_query_gen.cpp          # 查询窗口生成器
    ├── include/sul/
    │   ├── types.h  z_order.h  gpl_builder.h  art_tree.h  sul_index.h
    │   ├── cipher/                 # 密文索引 (cipher_types, crypto_context, sul_cipher_index)
    │   └── util/                   # csv_loader, query_loader
    ├── src/
    │   ├── *.cpp                   # 明文实现
    │   ├── cipher/                 # 密文实现
    │   └── util/                   # CSV / query 加载
    ├── agreements/agreements/      # 安全协议 OSM / SIC / SPI
    └── third_party/ophelib/        # vendored Paillier 库头文件
```

---

## 数据集与查询文件格式

**数据集 CSV**（放在 `sul-index/datasets/`）：
```
dim_1, dim_2, ..., dim_N, id
```
- `dim_i ∈ [0, 1)` 浮点；自动按 `floor(v × 65536)` 缩放到 int32（对齐 `BITS_PER_DIM=16`）
- `id` 整数；保存到 `DataPoint.orig_id` 用于追踪
- 维度 N 自动检测（1 ≤ N ≤ 6）；文件名约定 `<分布>_<总数>_<seed>_<维度>_.csv`

**查询窗口 CSV**（自动生成到 `sul-index/query/`）：
```
lo_1, lo_2, ..., lo_N, hi_1, hi_2, ..., hi_N
```
- 2N 个 [0,1) 浮点，定义超立方体范围查询 `[lo, hi]`
- 首行 `# dataset=... ratio_pct=... edge=...` 注释，加载时自动跳过
- 文件名 `<dataset_stem>_<ratio_pct>.csv`，例 `uniform_20000_0.25.csv` 表示 0.25% 选择率

---

## 启动流程

### A. 明文版（仅 CMake + GCC）

```bash
cd sul-index
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_CIPHER=OFF
cmake --build build -j
./build/sul_demo datasets/uniform_20000_1_2_.csv
```

### B. 密文版（含安装 ophelib）

```bash
cd sul-index

# 1) 安装 NTL / GMP / 自动 clone+build ophelib（需 sudo）
chmod +x setup_cipher.sh
./setup_cipher.sh

# 2) 全量构建（明文 + 密文）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

### C. 实验流水线（数据集 → 查询窗口 → 双索引对比）

```bash
cd sul-index

# 1) 生成 5 个查询窗口文件（0.25% / 0.5% / 1% / 2% / 4% 选择率）
./build/sul_query_gen datasets/uniform_20000_1_2_.csv 100 query
# 产出：query/uniform_20000_{0.25,0.5,1,2,4}.csv，每文件 100 条范围查询

# 2) 跑密文 demo（任意比例）
./build/sul_cipher_demo \
    datasets/uniform_20000_1_2_.csv \
    query/uniform_20000_0.25.csv \
    [paillier_key=1024]

# 3) 跑明文 vs 密文正确性对比（推荐：回归测试入口）
./build/sul_compare_demo \
    datasets/uniform_20000_1_2_.csv \
    query/uniform_20000_0.25.csv \
    [paillier_key=1024] [n_inserts=5]
# 退出码：0 = PASS，1 = FAIL
```

更换数据集（包括更高维度）只需把新 CSV 放进 `datasets/`，重跑 `sul_query_gen` 即可，无需改任何代码。

---

## 可执行文件总览

| Target | 入参 | 用途 |
|--------|------|------|
| `sul_demo` | `<dataset_csv> [error_bound]` | 明文索引 demo + BruteForceScanner 真值验证 |
| `sul_query_gen` | `<dataset_csv> [n_queries] [output_dir]` | 从数据集生成 5 个不同选择率的查询窗口文件 |
| `sul_cipher_demo` | `<dataset_csv> <query_csv> [paillier_key]` | 密文索引 demo + 批量范围查询延迟统计 |
| `sul_compare_demo` | `<dataset_csv> <query_csv> [paillier_key] [n_inserts]` | 明文 vs 密文逐项一致性对比 |

---

## 预期输出示例（uniform_20000_1_2_ + 0.25% 查询）

`sul_compare_demo` 输出片段：

```
=== Phase 1: 构建索引 ===
  plain  bulk_load: 3.2 ms   leaf=101  learning=15675  art=4325
  Paillier keygen:  56 ms
  cipher bulk_load: 5772 ms  leaf=101  learning=15675  art=4325
  结构规模一致? YES

=== Phase 3: 范围查询对比（来自查询文件） ===
  range_query 一致: 100/100
  平均返回点数: plain=50  cipher=50         # 理论 N×0.25%=50 ✓
  平均单查询:   plain=0.025 ms/q  cipher=798 ms/q

=== 总结 ===
  全部对比: 150/150
  结论: PASS 密文索引与明文索引结果一致
```

---

## 关键设计要点

| 维度 | 选择 |
|------|------|
| 量化精度 | `BITS_PER_DIM = 16`（2^16 量化级，2D 支持百万级数据） |
| Key 字节数 | 运行时 `key_len() = 2 × dim_count`（2D→4 字节 / 4 层 ART） |
| ART 节点 | Node4 / Node16 / Node48 / Node256 完整扩容链 |
| 学习层槽位 | 动态 `slot_count = max(2 × seg_len + 2 × ε, 8)` |
| 密文方案 | Paillier 同态加密（ophelib），DSP/DAP 双方单进程模拟 |
| 安全协议 | OSM（密文乘法）/ SIC（密文比较）/ SPI（点在范围内） |
| 查询窗口 | 超立方体，边长 `pow(ratio, 1/dim)`，中心从数据集随机抽样 |

详细设计与协议流程见 [`sul_项目文档.md`](./sul_项目文档.md)。

---

## 已实现 / 后续工作

- ✅ 明文构建 / 点查询 / 范围查询 / 单点插入 / BruteForce 真值
- ✅ 密文 SQQP / SARTQ / SHRQ / 安全插入
- ✅ CSV 数据集 + 查询窗口实验流水线
- ✅ 明文 vs 密文一致性对比 demo
- ⏳ 序列化 / 反序列化 · 多 ε 参数扫描 · 多分布数据对比 · 并发控制
