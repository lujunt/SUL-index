# SUL-index

> **Secure Updated Learned Index** — 面向多维数据的混合学习索引
> 上层用多层 GPL 模型预测定位，下层用 ART 自适应基数树处理预测冲突点，通过 Z 曲线把多维坐标映射到一维键值。

包含两套实现：

| 版本 | 数据形态 | 依赖 | 文档 |
|------|---------|------|------|
| **明文** SUL-plain-index | 明文索引参数 + 明文坐标 | 仅 CMake + GCC | `sul_项目文档.md` §1–§10 |
| **密文** SUL-cipher-index | Paillier 同态加密索引 + DSP/DAP 安全协议 + 实验流水线 | ophelib + NTL + GMP | `sul_项目文档.md` §11–§18 |

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
    ├── indexes/                    # 序列化的密文索引（.scidx 二进制）
    ├── record/                     # 实验记录（CSV，按参数命名自动归档）
    ├── main.cpp                    # 明文 demo（含 BruteForceScanner 真值对比）
    ├── main_cipher.cpp             # 密文 demo（K/err 可调 + 插入文件 + build/rangequery 记录）
    ├── main_compare.cpp            # 明文 vs 密文 正确性对比（含插入计时）
    ├── main_query_gen.cpp          # 查询窗口生成器
    ├── main_serde_demo.cpp         # 序列化往返 demo（save → load → query 对比）
    ├── main_split.cpp              # 数据集划分（90/10 默认）→ train.csv + insert.csv
    ├── main_workload.cpp           # 场景实验 demo（按读/写比执行 2000 操作）
    ├── main_update.cpp             # 更新对比 demo（按 ul% 批量插入）
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

> 所有命令都在 `sul-index/` 目录下执行。先按 **步骤 0** 完成构建一次即可，后续实验任选其一启动。

### 步骤 0：环境与构建（仅做一次）

```bash
cd sul-index

# 仅明文版可跳过 ophelib 安装，加 -DBUILD_CIPHER=OFF
./setup_cipher.sh                  # 自动安装 NTL / GMP / 编译 ophelib（需 sudo）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

### 步骤 1：前置准备（查询窗口 + 数据集划分）

> **范围查询实验只从 `query/` 目录读取已有的 CSV**。若 `query/` 中已有所需文件可直接跳过 1.1；以下命令仅在首次使用或需要新比例时执行一次。

```bash
# 1.1 [一次性] 生成 5 个查询窗口文件到 query/
./build/sul_query_gen <dataset_csv> [n_queries=100] [output_dir=query]
# 例：
./build/sul_query_gen datasets/uniform_20000_1_2_.csv 100 query
```

参数：
- `dataset_csv` **必填**，原始数据集 CSV 路径
- `n_queries` 可选，每个比例文件内生成的范围查询条数（默认 100）
- `output_dir` 可选，查询窗口输出目录（默认 `query`）

产出固定 5 个文件：`query/<stem>_{0.25,0.5,1,2,4}.csv`，对应 0.25% / 0.5% / 1% / 2% / 4% 选择率。这些文件**长期复用**，后续步骤 2 的范围查询直接读取这里的文件，不会重新生成。

```bash
# 1.2 把数据集按 train_ratio 切成 train + insert（命名自动派生 dim/N 标记）
./build/sul_split <full_csv> [out_dir=datasets] [train_ratio=0.9] [seed=42] [strata=64]
# 例（默认分层抽样，推荐用于不均匀数据集）：
./build/sul_split datasets/uniform_20000_1_2_.csv datasets 0.9 42 64
# 例（退化为简单随机抽样，仅用于已知均匀分布）：
./build/sul_split datasets/uniform_20000_1_2_.csv datasets 0.9 42 1
```

参数：
- `full_csv` **必填**，要划分的源 CSV 路径
- `out_dir` 可选，输出目录（默认 `datasets`）
- `train_ratio` 可选，训练集比例，须 ∈ (0, 1)（默认 0.9）
- `seed` 可选，随机种子，相同 seed 保证划分可复现（默认 42）
- `strata` 可选，分层桶数；`≥ 2` 启用基于 Z-order 桶的**分层抽样**（默认 64），`≤ 1` 退化为简单随机抽样

抽样策略：
- **分层抽样（默认）**：先用 `ZOrderEncoder` 把每点编码成 Z-key，按字典序排序后切成 `strata` 个等量段；每段独立 shuffle，用最大余数法分配段配额，保证 train 总数严格等于 `round(N × ratio)` 且各段 train:insert 比例近似一致。对不均匀分布数据集，**train 与 insert 的局部密度保持等比例**。
- **简单随机抽样**（`strata ≤ 1`）：mt19937 shuffle 全集后取前 `floor(N × ratio)` 个为 train。仅当数据集已知均匀时使用。

产出 2 个文件：
- `<out_dir>/<stem>_dim{d}_N{n_train}_train.csv`
- `<out_dir>/<stem>_dim{d}_N{n_insert}_insert.csv`

### 步骤 2：一键明密文对比 + 范围查询实验（`sul_compare_demo`）

明文每次现场 build；密文优先反序列化已有索引，无则先 build → save → reload 再查询。**两阶段：构建 + 范围查询**，同时承担范围查询实验的 record 产出。

```bash
./build/sul_compare_demo <dataset_csv> <query_csv> [paillier_key=1024] [err=-1]
# 例：
./build/sul_compare_demo \
    datasets/uniform_20000_1_2_.csv \
    query/uniform_20000_0.25.csv \
    1024 -1
```

参数：
- `dataset_csv` **必填**，原始数据集 CSV
- `query_csv` **必填**，由步骤 1.1 生成的范围查询窗口 CSV
- `paillier_key` 可选，Paillier 密钥位数，候选 1024 / 2048 / 3072 / 4096（默认 1024）
- `err` 可选，学习层误差界 ε；≤0 时自动取 `max(8, N/1000)`（默认 -1）。**改变 err 会落到独立 scidx 文件**

行为：
- **Phase 1 构建**：
  - 明文 `bulk_load` 现场构建（无序列化）
  - 密文：若 `indexes/index_K{K}_err{err}_dim{d}_N{N}.scidx` 存在 → 直接 `load_from_file`；否则 → `bulk_load` + `save_to_file` + 释放 + 重新 `load_from_file`（始终在反序列化后的实例上执行查询）
- **Phase 2 范围查询**：逐条 range_query 在明文/密文各跑一次，比对 orig_id 集合；同时通过 `range_query_with_stats` 拆分密文端学习层 / ART 层耗时
- 退出码：0 = PASS，1 = FAIL

产出：
- `record/build_K{K}_err{err}_dim{d}_N{N}.csv`（**仅 build 路径写入**；字段：`build_ms / keygen_ms / save_ms / load_ms / file_bytes`）
- `record/rangequery_K{K}_err{err}_dim{d}_N{N}_sl{窗口比例}.csv`（每次都追加；字段：`total_ms / avg_ms / learning_ms_avg / art_ms_avg / returned_avg / recall / precision`）

### 步骤 3：场景实验（`sul_workload`）

固定 2000 操作按读 / 写比交叉执行。读操作 = **点查询**（坐标从 `train_csv` 随机抽样，同分布、保证可命中），写操作 = `insert_csv` 顺序取用。**写操作导致的索引变化不序列化**。

```bash
./build/sul_workload <train_csv> <insert_csv> \
    [K=1024] [err=-1] [read_pct=50] [ops=2000]
# 例：
./build/sul_workload \
    datasets/uniform_20000_dim2_N18000_train.csv \
    datasets/uniform_20000_dim2_N2000_insert.csv \
    1024 -1 50 2000
```

参数：
- `train_csv` **必填**，训练集 CSV（步骤 1.2 产出的 `*_train.csv`）；同时是**读操作点查询的坐标来源**
- `insert_csv` **必填**，写操作来源 CSV（步骤 1.2 产出的 `*_insert.csv`，循环取用）
- `K` 可选，Paillier 密钥位数（默认 1024）
- `err` 可选，学习层误差界；≤0 时取 `max(8, N/1000)`（默认 -1）
- `read_pct` 可选，读操作占比，整数百分比 ∈ [0, 100]（默认 50）；写占比 = 100 - read_pct
- `ops` 可选，总操作数（默认 2000）

> 注意：此处**不需要 `query_csv`**。`query/` 下的范围查询文件只服务于步骤 2（一键明密文比对）。

文档 §17.2 5 种典型场景对应的尾参：

| 场景 | read_pct | 命令尾参 |
|------|----------|---------|
| 全读 | 100 | `1024 -1 100 2000` |
| 多读少写 | 80  | `1024 -1 80  2000` |
| 读写均衡 | 50  | `1024 -1 50  2000` |
| 少读多写 | 20  | `1024 -1 20  2000` |
| 全写   | 0   | `1024 -1 0   2000` |

产出：`record/workload_K{K}_err{err}_dim{d}_N{N}_R{r}W{w}.csv`（吞吐 + 学习 / ART 层耗时 + 定位 / 更新耗时；`learning_query_avg_ms` / `art_query_avg_ms` 现表示**点查询**两段耗时）

### 步骤 4：更新对比（`sul_update`）

按更新率 `ul%` 批量插入，**自动序列化更新后的索引**便于后续查询。

```bash
./build/sul_update <train_csv> <insert_csv> <query_csv> \
    [K=1024] [err=-1] [ul_pct=0.25]
# 例：
./build/sul_update \
    datasets/uniform_20000_dim2_N18000_train.csv \
    datasets/uniform_20000_dim2_N2000_insert.csv \
    query/uniform_20000_0.25.csv \
    1024 -1 0.25
```

参数：
- `train_csv` **必填**，训练集 CSV（步骤 1.2 产出的 `*_train.csv`）
- `insert_csv` **必填**，候选更新数据 CSV（按 `ul_pct` 从头截取所需条数）
- `query_csv` **必填**，更新后用于评估查询延迟与召回率的范围查询 CSV
- `K` 可选，Paillier 密钥位数（默认 1024）
- `err` 可选，学习层误差界；≤0 时取 `max(8, N/1000)`（默认 -1）
- `ul_pct` 可选，更新率百分比，实际更新数 = `ceil(N_train × ul_pct / 100)`；候选 0.25 / 0.5 / 1 / 2 / 4（默认 0.25）

产出：
- `record/update_K{K}_err{err}_dim{d}_N{N}_ul{tag}.csv`（更新前后查询延迟 + 召回率）
- `indexes/index_K{K}_err{err}_dim{d}_N{N}_ul{tag}_update.scidx` ← **更新后的索引快照**

> 此 `_update.scidx` 与步骤 2 中的 baseline `.scidx` 区分；后续可用 `SULCipherIndex::load_from_file` 直接加载继续查询。

### 步骤 5：序列化往返单元测试（`sul_serde_demo`，可选）

```bash
./build/sul_serde_demo <dataset_csv> <query_csv> \
    [paillier_key=1024] [out_dir=indexes]
# 例：
./build/sul_serde_demo \
    datasets/uniform_20000_1_2_.csv \
    query/uniform_20000_0.25.csv \
    1024 indexes
# 落盘命名：indexes/index_K{K}_err{err}_dim{d}_N{N}.scidx
```

参数：
- `dataset_csv` **必填**，原始数据集 CSV
- `query_csv` **必填**，范围查询窗口 CSV（用于反序列化后查询比对）
- `paillier_key` 可选，Paillier 密钥位数（默认 1024）
- `out_dir` 可选，`.scidx` 落盘目录（默认 `indexes`）

退出码：0 = PASS（save 后 load 的索引查询结果与原索引一致），1 = FAIL。

`.scidx` 二进制布局（见 `src/cipher/sul_cipher_serde.cpp` 顶注释）：

```
Magic "SCIDX001"(8B) Version(u32) KeySize(u32) SCALE(i32)
IndexConfig(error_bound/max_layers/dim_count)
Paillier KeyPair(n,g,p,q,a 以 hex 字符串保存)
Plain skeleton: inner_layers + leaf_nodes 结构字段
Encrypted state: EncDataPoint 池 + Enc GPL inner/leaf + Enc ART 树前序遍历
```

---

## 记录文件命名总览

`record/{kind}_K{K}_err{err}_dim{d}_N{N}[{extra}].csv`，extra 按实验类型：
- `build` → 无 extra
- `rangequery` → `_sl{窗口比例}`（例 `_sl0p25`）
- `workload` → `_R{r}W{w}`（例 `_R50W50`）
- `update` → `_ul{ul比例}`（例 `_ul0p25`）

同参数多次实验会 append 到同一文件，便于横向对比。详细字段定义见 `sul_项目文档.md` §18。

`indexes/index_K{K}_err{err}_dim{d}_N{N}[{extra}].scidx`，extra 按来源：
- 步骤 2 / 步骤 5 写入的 baseline → 无 extra
- 步骤 4 写入的更新快照 → `_ul{tag}_update`

加载后的实例为查询只读模式（`is_loaded()==true`），`insert` 会返回 `Failed`。

更换数据集（包括更高维度）只需把新 CSV 放进 `datasets/`，重跑 `sul_query_gen` 即可，无需改任何代码。

---

## 可执行文件总览

| Target | 入参 | 用途 |
|--------|------|------|
| `sul_demo` | `<dataset_csv> [error_bound]` | 明文索引 demo + BruteForceScanner 真值验证 |
| `sul_query_gen` | `<dataset_csv> [n_queries] [output_dir]` | 从数据集生成 5 个不同选择率的查询窗口文件 |
| `sul_split` | `<full_csv> [out_dir=datasets] [ratio=0.9] [seed=42]` | 数据集划分（自动派生 `<stem>_dim{d}_N{n}_{train,insert}.csv`） |
| `sul_compare_demo` | `<dataset_csv> <query_csv> [paillier_key] [err]` | 一键明密文对比 + 范围查询实验：明文 build + 密文 load-or-build-save-load + 范围查询比对 + record/build & rangequery 写入 |
| `sul_serde_demo` | `<dataset_csv> <query_csv> [paillier_key] [out_dir]` | 密文索引序列化往返 + load 后查询比对 |
| `sul_workload` | `<train> <insert> [K] [err] [read_pct] [ops]` | 场景实验，读=点查询(取自 train)+写=insert，按比例执行 ops 次 → record/workload |
| `sul_update` | `<train> <insert> <query> [K] [err] [ul_pct]` | 更新对比，按 ul% 批量插入 → record/update |

---

## 预期输出示例（uniform_20000_1_2_ + 0.25% 查询）

`sul_compare_demo` 输出片段（第 2 次：命中已有 scidx → load 路径）：

```
=== Phase 1: 构建索引 ===
  plain  bulk_load: 3.4 ms   leaf=101  learning=15675  art=4325
  密文索引: 命中 indexes/index_K1024_err20_dim2_N20000.scidx → load
  load_from_file: 221 ms
  cipher: leaf=101  is_loaded=true
  结构规模一致? YES (loaded 模式仅比较 leaf_count)

=== Phase 2: 范围查询对比（来自查询文件） ===
  range_query 一致: 100/100
  平均返回点数: plain=50  cipher=50
  总耗时: plain=2.5 ms  cipher=80385 ms
  平均单查询: plain=0.025 ms/q  cipher=803 ms/q
  cipher 拆分: learning=14 ms/q  art=789 ms/q
  → record: record/rangequery_K1024_err20_dim2_N20000_sl0p25.csv

=== 总结 ===
  range_query 对比: 100/100
  结论: PASS 密文索引与明文索引结果一致
```

> 第 1 次跑会走 build + save + reload，多出 ~6 秒 keygen+构建，并额外打印 `→ record: record/build_K1024_err20_dim2_N20000.csv`（把 `build_ms / keygen_ms / save_ms / load_ms / file_bytes` 写入 build CSV）；之后再跑直接命中 load 路径，仅追加 rangequery CSV。

`sul_serde_demo` 输出片段（save → load → query 对比）：

```
=== Phase 3: 序列化到 indexes/ ===
  写出 = indexes/index_K1024_err20_dim2_N20000.scidx
  耗时 = 175 ms      文件大小 = 86 MB

=== Phase 4: 反序列化 ===
  耗时 = 175 ms      loaded leaf=101  is_loaded=true
  结构规模一致? YES

=== Phase 5: loaded 索引执行查询并比对 ===
  范围查询一致: 100/100
  平均返回点数: orig=50  loaded=50
  平均单查询: orig=803 ms/q  loaded=803 ms/q

=== 总结 ===
  结论: PASS 序列化后查询与原索引一致
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
- ✅ 插入耗时统计（cipher_demo / compare_demo 均输出 total + avg + min/max）
- ✅ 密文索引序列化 / 反序列化（`.scidx` 二进制 → `indexes/`，load 后查询只读）
- ✅ 密文实验流水线：K/err 参数化、insert 文件驱动、record 自动归档（build/rangequery/workload/update 4 类 CSV，按参数命名）
- ✅ 点查询/范围查询的学习层/ART 层耗时拆分（`QueryStats`）
- ✅ 数据集划分工具 `sul_split`（均匀采样，可复现）
- ⏳ 多分布数据对比 · 并发控制 · 重训练实验热写
