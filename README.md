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
./build/sul_query_gen <dataset_csv> [n_queries=100] [output_dir=query] [--target-hits]
# 例（默认 uniform_volume 模式，适合均匀数据）：
./build/sul_query_gen datasets/uniform_20000_1_2_.csv 100 query
# 例（target_hits 自适应模式，跨数据集横向对比时强烈推荐）：
./build/sul_query_gen datasets/skewed_20000_4_2_.csv 100 query --target-hits
```

参数：
- `dataset_csv` **必填**，原始数据集 CSV 路径
- `n_queries` 可选，每个比例文件内生成的范围查询条数（默认 100）
- `output_dir` 可选，查询窗口输出目录（默认 `query`）
- `--target-hits` 可选 flag，二分自适应模式（见下）

产出固定 5 个文件：`<output_dir>/<stem>_{0.25,0.5,1,2,4}.csv`，对应 0.25% / 0.5% / 1% / 2% / 4% 选择率。这些文件**长期复用**，后续步骤 2 的范围查询直接读取，不会重新生成。

**两种边长选取模式**（决定窗口实际命中点数）：

| 模式 | edge 来源 | 适用 | 实测偏差 |
|---|---|---|---|
| `uniform_volume`（默认） | `edge = ratio^(1/dim)`，按 uniform 假设反推体积比 | 均匀数据集 | uniform 上 ≈ ratio；**skewed 上可达 ratio 的 3-4 倍** |
| `target_hits`（`--target-hits`） | 对每个 center 二分搜索 edge，使实测命中数 ∈ `[N×ratio×0.95, N×ratio×1.05]` | **所有数据集**，特别是 skewed / 跨数据集横向对比 | 严格 ±5%；30 轮收敛失败则 best-effort + stderr 警告 |

`--target-hits` 模式下文件的 `#` 注释 header 额外含 `target / tol / hit_min / hit_mean / hit_max / converged=N/M` 字段，方便核对生成质量。**两种模式输出的 CSV 数据行格式完全一致**，下游 `load_query_file` 无需感知。

> ⚠️ **跨数据集对比时务必使用 `--target-hits`**。否则 `skewed` 和 `uniform` 同一比例（如 1%）的实测命中数可能差几倍，`rangequery_*.csv` 中的 `avg_ms / returned_avg` 列无法直接横向比较。

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
  - 密文：若 `indexes/index_<stem>_K{K}_err{err}_dim{d}.scidx` 存在 → 直接 `load_from_file`；否则 → `bulk_load` + `save_to_file` + 释放 + 重新 `load_from_file`（始终在反序列化后的实例上执行查询）
- **Phase 2 范围查询**：逐条 range_query 在明文/密文各跑一次，比对 orig_id 集合；同时通过 `range_query_with_stats` 拆分密文端学习层 / ART 层耗时
- 退出码：0 = PASS，1 = FAIL

产出：

**① `record/build_<stem>_K{K}_err{err}_dim{d}.csv`** —— 仅 build 路径写入（命中已有 `.scidx` 时跳过）

| 字段 | 含义 |
|---|---|
| `timestamp` | ISO-8601 时间戳（如 `2026-05-23T10:29:40`） |
| `K` / `err` / `dim` / `N` | Paillier 位数 / 学习层误差界 ε / 维度 / 数据集点数 |
| `build_ms` | 密文 `bulk_load` 耗时（含 GPL 学习层 + ART 冲突层完整构建） |
| `keygen_ms` | Paillier 密钥生成耗时 |
| `save_ms` | `save_to_file` 写盘耗时 |
| `load_ms` | `load_from_file` 反序列化耗时 |
| `file_bytes` | **磁盘真实大小**（v2 raw 二进制格式，K=1024/dim=2/N=20000 约 41.7 MiB） |
| `file_bytes_kl1` | **等效存储口径**：假设 EncDataPoint pool 中 ART-key 仅压成 1 个密文/点时的折算大小，公式 `file_bytes − (key_len−1) × N × (4 + ⌈2K/8⌉)`；用于跨 dim 公平对比，**真实磁盘文件不变** |

**② `record/rangequery_<stem>_K{K}_err{err}_dim{d}_sl{窗口比例}.csv`** —— 每次跑都追加一行

| 字段 | 含义 |
|---|---|
| `timestamp / K / err / dim / N` | 同上 |
| `sl_pct` | 范围查询窗口边长百分比（从查询文件名解析，如 `0.25` = 0.25%） |
| `query_count` | 本次跑的查询条数 |
| `total_ms` | 密文端所有范围查询合计耗时 |
| `avg_ms` | 密文端单查询平均耗时（= `total_ms / query_count`） |
| `learning_ms_avg` | 密文端单查询中 GPL 学习层耗时（SQQP 定位 + 槽位扫描） |
| `art_ms_avg` | 密文端单查询中 ART 层耗时（SARTQ + SPI 过滤），`avg_ms ≈ learning_ms_avg + art_ms_avg` |
| `returned_avg` | 密文端单查询平均返回点数 |
| `recall` | 密文∩明文 / 明文返回数（密文召回率，目标 = 1） |
| `precision` | 密文∩明文 / 密文返回数（密文精确率，理论 = 1） |
| `actual_ratio_pct` | **实测选择率**（plain 端平均命中数 / N × 100）。`uniform_volume` 模式：uniform 数据 ≈ `sl_pct`，skewed 可与 `sl_pct` 差几倍（体积比 ≠ 点数比）；`target_hits` 模式：所有数据集都严格 ≈ `sl_pct`（±5% 容差） |
| `returned_min / returned_p50 / returned_p95 / returned_max` | plain 端命中数的分布（最小 / 中位数 / 95 分位 / 最大）。skewed 数据下 `max ≫ p50` 体现热点簇查询代价的尾部 |

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

产出：`record/workload_<stem>_K{K}_err{err}_dim{d}_R{r}W{w}.csv` —— 每次跑追加一行

| 字段 | 含义 |
|---|---|
| `timestamp / K / err / dim / N` | 同步骤 2 |
| `read_pct / write_pct` | 读 / 写操作占比（read_pct + write_pct = 100） |
| `ops_total` | 总操作数（默认 2000） |
| `total_ms` | 整个 workload 跑完的墙钟时间 |
| `throughput_total` | 整体吞吐（ops/s） |
| `throughput_read` / `throughput_write` | 仅按读 / 写操作各自数量与各自累计耗时算的吞吐 |
| `query_latency_avg_ms` | 单**点查询**平均耗时（读操作） |
| `learning_query_avg_ms` | 点查询中学习层耗时（**点查询**口径，非范围查询） |
| `art_query_avg_ms` | 点查询中 ART 层耗时（**点查询**口径） |
| `update_latency_avg_ms` | 单**插入**平均耗时（写操作） |
| `locate_avg_ms` | 插入前定位（SQQP）平均耗时 |
| `update_avg_ms` | 插入操作中实际写入（学习层 OSM 或 ART 树）的平均耗时 |

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

**① `record/update_<stem>_K{K}_err{err}_dim{d}_ul{tag}.csv`** —— 每次跑追加一行

| 字段 | 含义 |
|---|---|
| `timestamp / K / err / dim / N` | 同步骤 2（`N` = train_csv 点数） |
| `ul_pct` | 本次的更新率百分比（命令行 `ul_pct` 入参） |
| `update_count` | 实际插入条数（= `ceil(N × ul_pct / 100)`） |
| `update_total_ms` | 全部插入合计耗时 |
| `update_avg_ms` | 单次插入平均耗时 |
| `post_query_avg_ms` | 更新后在 `query_csv` 上跑范围查询的单查询平均耗时 |
| `post_recall` | 更新后密文范围查询的召回率（目标 = 1） |

**② `indexes/index_<stem>_K{K}_err{err}_dim{d}_ul{tag}_update.scidx`** —— 更新后的索引快照，可被 `SULCipherIndex::load_from_file` 直接加载继续查询，与步骤 2 中的 baseline `.scidx` 命名区分（多 `_ul{tag}_update` 后缀）。

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
# 落盘命名：indexes/index_<stem>_K{K}_err{err}_dim{d}.scidx
```

参数：
- `dataset_csv` **必填**，原始数据集 CSV
- `query_csv` **必填**，范围查询窗口 CSV（用于反序列化后查询比对）
- `paillier_key` 可选，Paillier 密钥位数（默认 1024）
- `out_dir` 可选，`.scidx` 落盘目录（默认 `indexes`）

退出码：0 = PASS（save 后 load 的索引查询结果与原索引一致），1 = FAIL。

`.scidx` 二进制布局（见 `src/cipher/sul_cipher_serde.cpp` 顶注释）：

```
Magic "SCIDX002"(8B) Version(u32 = 2) KeySize(u32) SCALE(i32)
IndexConfig(error_bound/max_layers/dim_count)
Paillier KeyPair(n,g,p,q,a，每个 Integer = u32 len + len 字节 big-endian raw)
Plain skeleton: inner_layers + leaf_nodes 结构字段
Encrypted state: EncDataPoint 池 + Enc GPL inner/leaf + Enc ART 树前序遍历
                 每个 Ciphertext = u32 len + len 字节 big-endian raw 大数
                 (v2 改用 raw 后单密文 K=1024 从 516B → 260B，整库腰斩)
```

---

## 记录文件命名总览

`record/{kind}_<stem>_K{K}_err{err}_dim{d}[{extra}].csv`，extra 按实验类型：
- `build` → 无 extra
- `rangequery` → `_sl{窗口比例}`（例 `_sl0p25`）
- `workload` → `_R{r}W{w}`（例 `_R50W50`）
- `update` → `_ul{ul比例}`（例 `_ul0p25`）

同参数多次实验会 append 到同一文件，便于横向对比。详细字段定义见 `sul_项目文档.md` §18。

`indexes/index_<stem>_K{K}_err{err}_dim{d}[{extra}].scidx`：
- `<stem>` = 数据集文件名前两个 `_` 分量（如 `uniform_20000` / `skewed_20000`），由 `util::dataset_stem(path)` 统一派生，与 `query/<stem>_*.csv` 同源。**新增 stem 维度后跨数据集同 K/err/dim 不再共享 scidx 缓存**（避免曾经踩过的 uniform/skewed 串味坑）
- N 已隐含在 `<stem>` 中（按当前命名约定），故 scidx 与 record（build/rangequery/workload/update）后缀均不再重复 `_N{N}`
- `extra` 按来源：步骤 2 / 步骤 5 写入的 baseline → 无 extra；步骤 4 写入的更新快照 → `_ul{tag}_update`

加载后的实例为查询只读模式（`is_loaded()==true`），`insert` 会返回 `Failed`。

更换数据集（包括更高维度）只需把新 CSV 放进 `datasets/`，重跑 `sul_query_gen` 即可，无需改任何代码。

---

## 可执行文件总览

| Target | 入参 | 用途 |
|--------|------|------|
| `sul_demo` | `<dataset_csv> [error_bound]` | 明文索引 demo + BruteForceScanner 真值验证 |
| `sul_query_gen` | `<dataset_csv> [n_queries] [output_dir] [--target-hits]` | 从数据集生成 5 个不同选择率的查询窗口文件；默认按 uniform 体积比，`--target-hits` 启用二分自适应（适合 skewed 与跨数据集对比） |
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
  密文索引: 命中 indexes/index_uniform_20000_K1024_err20_dim2.scidx → load
  load_from_file: 221 ms
  cipher: leaf=101  is_loaded=true
  结构规模一致? YES (loaded 模式仅比较 leaf_count)

=== Phase 2: 范围查询对比（来自查询文件） ===
  range_query 一致: 100/100
  平均返回点数: plain=50  cipher=50
  总耗时: plain=2.5 ms  cipher=80385 ms
  平均单查询: plain=0.025 ms/q  cipher=803 ms/q
  cipher 拆分: learning=14 ms/q  art=789 ms/q
  → record: record/rangequery_uniform_20000_K1024_err20_dim2_sl0p25.csv

=== 总结 ===
  range_query 对比: 100/100
  结论: PASS 密文索引与明文索引结果一致
```

> 第 1 次跑会走 build + save + reload，多出 ~6 秒 keygen+构建，并额外打印 `→ record: record/build_uniform_20000_K1024_err20_dim2.csv`（把 `build_ms / keygen_ms / save_ms / load_ms / file_bytes / file_bytes_kl1` 写入 build CSV，并在 stdout 多打两行 `file_bytes` 与 `file_bytes_kl1` 的 MiB 等效值）；之后再跑直接命中 load 路径，仅追加 rangequery CSV。

`sul_serde_demo` 输出片段（save → load → query 对比）：

```
=== Phase 3: 序列化到 indexes/ ===
  写出 = indexes/index_uniform_20000_K1024_err20_dim2.scidx
  耗时 = 175 ms      文件大小 = 42 MB   # v2 raw 二进制；v1 hex 时同输入约 82 MB

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
| 查询窗口 | 超立方体，中心从数据集随机抽样；边长支持两种模式：`uniform_volume`（默认，`edge=ratio^(1/dim)`）/ `target_hits`（`--target-hits`，二分搜索 edge 使实测命中数命中 N×ratio ±5%） |

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
- ✅ 查询窗口生成支持 `--target-hits` 二分自适应（跨数据集 / skewed 实验严格命中 N×ratio ±5%）
- ✅ `rangequery_*.csv` 加 plain 端实测分布列（`actual_ratio_pct / returned_min / returned_p50 / returned_p95 / returned_max`），暴露热点尾部
- ⏳ 多分布数据对比 · 并发控制 · 重训练实验热写
