# SUL-plain-index 项目文档

> **项目名称**: SUL-plain-index（Secure Updated Learned Index — 明文实现版本）  
> **数据类型**: 32 位整数多维数据点  
> **技术栈**: WSL2 + Ubuntu 22.04 + C++17 (GCC 11+)  
> **构建系统**: CMake 3.16+  
> **版本**: v1.3  
> **日期**: 2026-05-13  

> **v1.3 变更摘要（与 v1.2 对照本次明文实现）**
> - **数据类型与位宽**：坐标采用 32 位整数，2 维 Z 曲线编码后 z_value 占用 ≤ 64 位 → `key_bytes` 由 4 字节扩展为 **8 字节**，ART 层由 4 层增至 **8 层**。
> - **学习层叶子槽位**：`slot_count` 从固定 64 改为**按段长度动态分配**（`slot_count = max(2 × seg_len + 2 × ε, 8)`），槽位占用状态用按字节数组（功能等价于 bitmap）。
> - **ART 节点类型**：实现完整四层扩容链 `Node4 → Node16 → Node48 → Node256`。Node48/Node256 按"延续 Node16 模式"设计（`keys[N] + bitmap + children[N]`，仅容量扩大）。
> - **不在本次范围**：插入路径、序列化与反序列化、实验记录器（见 §6/§9/§10，保留为后续工作）。

---

## 目录

1. [概述](#1-概述)
2. [系统架构](#2-系统架构)
3. [数据结构设计](#3-数据结构设计)
4. [代码结构设计](#4-代码结构设计)
5. [查询逻辑设计](#5-查询逻辑设计)
6. [插入逻辑设计](#6-插入逻辑设计)
7. [构建流程设计](#7-构建流程设计)
8. [性能分析与调优](#8-性能分析与调优)
9. [序列化与反序列化](#9-序列化与反序列化)
10. [实验记录与分析](#10-实验记录与分析)
11. [参考文献](#11-参考文献)

---

## 1. 概述

### 1.1 项目背景

SUL-plain-index 是一种面向多维数据的混合学习索引结构，为 SUL 系列索引的明文实现版本（后续规划 SUL-cipher-index 密文版本），融合了学习索引（Learned Index）的读性能优势和自适应基数树（Adaptive Radix Tree, ART）的写入高效特性。其设计受 ALT-Index [1] 启发，采用两层混合架构：学习层负责高精度预测，ART 层处理预测冲突数据，同时引入 Z 曲线（Z-order Curve）实现多维数据到一维键值的降维映射。

### 1.2 开发环境与技术栈

| 组件 | 选型 | 说明 |
|------|------|------|
| 操作系统 | WSL2 (Windows Subsystem for Linux 2) | Windows 下运行 Linux 原生环境 |
| Linux 发行版 | Ubuntu 22.04 LTS | 长期支持版本，生态稳定 |
| 编程语言 | C++17 | 利用结构化绑定、if constexpr、string_view 等现代特性 |
| 编译器 | GCC 11.4+ (Ubuntu 22.04 默认) | C++17 完整支持，优化能力成熟 |
| 构建系统 | CMake 3.16+ | 跨平台、与 CLion/VSCode 良好集成 |
| 调试工具 | GDB 12.1 | 命令行调试 |
| 性能分析 | perf (Linux) | CPU 采样、缓存未命中分析 |
| 内存检测 | Valgrind 3.18+ | 内存泄漏和越界检查 |
| 测试框架 | Google Test 1.12+ | C++ 单元测试标准框架 |
| 基准测试 | Google Benchmark 1.7+ | 微基准性能测量 |

> **环境搭建**: 在 Windows 上安装 WSL2，通过 `wsl --install -d Ubuntu-22.04` 一键部署，所有开发、编译、测试均在 Linux 环境中进行。

### 1.3 核心设计原则

- **分层解耦**: 学习层与 ART 层职责分离，分别优化读/写路径
- **零预测误差**: 学习层仅存储可被线性模型精确预测的数据，冲突数据下沉到 ART 层
- **自适应节点**: ART 层采用 Node4/Node16 等自适应节点结构，按字节粒度索引
- **范围查询优化**: 通过边界定位 + 候选集合并实现高效多维范围查询
- **工业级可用性**: 遵循工业项目标准，模块化设计、完整测试覆盖、清晰接口定义
- **明文实现**: 当前为明文（plaintext）版本 SUL-plain-index，数据以明文形式存储和查询；后续规划密文版本 SUL-cipher-index，引入同态加密或可信执行环境（TEE）实现隐私保护索引
- **当前版本**: 暂不考虑并发控制，单线程实现，后续版本再引入并发优化

### 1.4 参考论文

- **ALT-Index [1]**: A Hybrid Learned Index for Concurrent Memory Database Systems (Yang et al., 2024)
- **PGM-Index [2]**: A Fully-Dynamic Compressed Learned Index with Provable Worst-case Bounds (Ferragina & Vinciguerra, VLDB 2020)
- **ART [3]**: The Adaptive Radix Tree: ARTful Indexing for Main-Memory Databases (Leis et al., ICDE 2013)

---

## 2. 系统架构

### 2.1 整体架构

SUL-plain-index 由两层组成：

```
┌─────────────────────────────────────────────────┐
│              SUL-plain-index                     │
│                                                  │
│  ┌───────────────────────────────────────────┐  │
│  │          Learning Layer (学习层)           │  │
│  │  ┌───────┐    ┌───────┐    ┌───────┐     │  │
│  │  │ Root  │───▶│ Inner │───▶│ Leaf  │     │  │
│  │  │ GPL   │    │ GPL   │    │ GPL   │     │  │
│  │  └───────┘    └───────┘    └───┬───┘     │  │
│  │                                │          │  │
│  │              ┌─────────────────┘          │  │
│  │              ▼                             │  │
│  │  ┌───────────────────────┐                │  │
│  │  │   ART Root (per leaf)  │              │  │
│  └──┼───────────────────────┼──────────────┘  │
│     │       ART Layer (ART层)                  │
│     │  ┌──────┐  ┌──────┐  ┌──────┐          │
│     │  │Node4 │─▶│Node16│─▶│ Leaf │          │
│     │  └──────┘  └──────┘  └──────┘          │
│     └─────────────────────────────────────────│
└─────────────────────────────────────────────────┘
```

### 2.2 层次职责

| 层次 | 职责 | 存储数据 | 查询方式 |
|------|------|---------|---------|
| 学习层 | 高精度预测定位 | 可被线性模型精确拟合的数据 | GPL 模型预测 + 二分搜索 |
| ART 层 | 处理冲突/插入数据 | 预测误差点、运行时插入冲突点 | 字节流匹配 Trie 遍历 |

### 2.3 数据流

```
多维数据点 → Z曲线映射 → 一维键值 → 排序 → GPL模型训练 → 预测验证
                                    ↓
                       预测准确 → 学习层存储
                       预测冲突 → ART层存储 (按字节分割)
                                    ↓
                       范围查询 → 定位边界叶子 → 候选集合并 → 维度过滤
```

---

## 3. 数据结构设计

### 3.1 数据类型定义

#### 3.1.1 原始数据

```cpp
// 原始多维数据点
struct DataPoint {
    int32_t dimensions[MAX_DIMS];  // 多维坐标值 (默认 2-4 维)
    int32_t dim_count;              // 维度数
    int32_t orig_id;                // 原始数据 ID（可选，用于追踪与日志）
    uint64_t z_value;              // Z曲线一维映射值 (缓存)
    uint8_t key_bytes[8];          // 字节流：将 z_value 按 8 位分割 (大端序)
                                   // key_bytes[0] = (z_value >> 56) & 0xFF (最高字节)
                                   // ...
                                   // key_bytes[7] =  z_value        & 0xFF (最低字节)
};
```

> **设计说明**: `key_bytes` 是 z_value 的预计算字节表示。ART 层遍历时直接使用此字段，避免查询时重复进行字节分割。由于 2 维 32 位整数坐标经 Z 曲线交错位编码后 z_value 可达 ~64 位，因此按 8 位分割为 8 字节（即 ART 层 8 层）。构建时与 z_value 一起计算并缓存。

#### 3.1.2 Z 曲线编码

Z 曲线（Morton Code）将多维整数坐标交错位编码为一维值，保持空间局部性：

```cpp
// Z曲线编码: 将多维坐标交错位合并
// 例: 二维 (x, y) = (3, 5) → 二进制(011, 101) → 交错 → 011011 = 27
int64_t z_order_encode(const int32_t* coords, int dim_count) {
    int64_t result = 0;
    for (int bit = 0; bit < 32; bit++) {
        for (int d = 0; d < dim_count; d++) {
            result |= ((int64_t)((coords[d] >> bit) & 1)) << (bit * dim_count + d);
        }
    }
    return result;
}

// 字节流生成: 将 z_value 转换为 8 字节大端序
void z_value_to_bytes(uint64_t z_value, uint8_t* out_bytes) {
    for (int i = 0; i < 8; ++i) {
        int shift = (7 - i) * 8;
        out_bytes[i] = (z_value >> shift) & 0xFF;
    }
}
```

### 3.2 学习层数据结构

学习层采用多层 GPL（Greedy Pessimistic Linear）模型结构，参考 PGM-Index [2] 的多层 PLA 构建方法。

#### 3.2.1 GPL 模型节点

##### 内部节点 (GPLInnerNode)

学习层内部节点组成一棵树：根节点位于最顶层，每层节点预测下一层子节点的位置，直至叶子层。

```cpp
struct GPLInnerNode {
    int64_t key;                // 本节点对应子树的最小键值
    double slope;               // 线性模型斜率
    double intercept;           // 线性模型截距
    int32_t child_start;        // 在下一层数组中的起始子节点下标
    int32_t child_count;        // 子节点数量
    // 预测公式: child_idx = clamp(floor(slope * key + intercept), 0, child_count-1)
    // 该 child_idx 是相对偏移，实际子节点在下一层的 child_start + child_idx 处
};
```

##### 叶子节点 (GPLLeafNode)

```cpp
struct GPLLeafNode {
    uint64_t key;                            // 叶子最小键值
    double slope;                            // 线性模型斜率
    double intercept;                        // 线性模型截距
    std::vector<DataPoint*> data_slots;      // 数据点数组（按 slot_count 动态分配）
    std::vector<uint8_t>    occupied;        // 槽位占用标记（功能等价于 bitmap）
    void*   art_root;                        // 挂载的 ART 根节点指针（空树为 nullptr）
    int32_t slot_count;                      // 动态槽位总数（按段长度计算）
    int32_t filled_count;                    // 已占用槽位数
    int32_t seg_start, seg_end;              // GPL 分段在排序数组中的起止下标
};
```

**设计要点**:
- 每个学习层叶子都挂载一棵 `ARTTree`，无冲突时为空树（`art_root == nullptr`）
- 使用 `occupied` 数组加速空槽检测和占用判断（按字节存放，O(1) 访问；功能等价于 bitmap）
- `slot_count = max(2 × seg_len + 2 × ε, 8)` 按段长度动态分配，避免固定 64 槽位无法容纳大段
- 空白槽位用于支持原地插入，减少数据搬移

#### 3.2.2 GPL 模型层次

学习层采用树形层次结构，每层节点以数组形式存储：

```
Layer 0 (Root):       [GPLInnerNode]                         ← 1 个根节点
                        │  slope*k + intercept → child_idx
                        ▼
Layer 1:              [GPLInnerNode] [GPLInnerNode] ...      ← 根节点的子节点
                        │               │
                        ▼               ▼
Layer 2:              [GPLInnerNode]...[GPLInnerNode]        ← 多层内部节点
                        │
                        ▼
Layer N (Leaf):       [GPLLeafNode] [GPLLeafNode] ...        ← 叶子节点层
                         │              │
                      [ART Root]    [ART Root]                ← 每个叶子挂载 ART
```

**层次关系**:
- 所有节点按层存储在 `vector<GPLInnerNode>` 数组中（叶子层用 `vector<GPLLeafNode>`）
- 父节点通过 `child_start` + `child_idx` 定位下一层的子节点
- `child_idx = clamp(floor(slope * query_key + intercept), 0, child_count - 1)`
- 多层结构的关键：每层节点的模型在构建时拟合的是**该层子节点起始键值与子节点下标**的线性关系

#### 3.2.3 GPL 模型参数

| 参数 | 含义 | 建议值 |
|------|------|--------|
| epsilon | 误差界 (error bound) | N_total / 1000 |
| slope | 线性模型斜率 | 由数据分布决定 |
| intercept | 线性模型截距 | 由数据分布决定 |

### 3.3 ART 层数据结构

ART 层处理学习层无法精确预测的数据点。键值已预计算为 `DataPoint.key_bytes`（4 字节），ART 的每一层对应一个字节。

#### 3.3.1 节点类型

参考 ART [3] 的自适应节点设计，SUL-plain-index 采用两类内部节点：

##### Node4（最多 4 个子节点）

```cpp
struct ARTNode4 {
    uint8_t keys[4];         // 键值数组 (最多 4 个不同字节值)
    uint8_t bitmap;          // 位图: bit[i]=1 表示 keys[i] 存在有效子节点
    void* children[4];       // 子节点指针数组
    // children[i] 指向 ARTNode4、ARTNode16 或 ARTLeafNode
};
```

##### Node16（最多 16 个子节点）

```cpp
struct ARTNode16 {
    uint8_t keys[16];        // 键值数组 (最多 16 个不同字节值)
    uint16_t bitmap;         // 位图 (16 位)
    void* children[16];      // 子节点指针数组
};
```

##### Node48（最多 48 个子节点）

```cpp
struct ARTNode48 {
    uint8_t  keys[48];       // 键值数组 (最多 48 个不同字节值)
    uint64_t bitmap;         // 位图 (48 位实际占用)
    void*    children[48];   // 子节点指针数组
};
```

##### Node256（最多 256 个子节点）

```cpp
struct ARTNode256 {
    uint8_t  keys[256];      // 键值数组 (最多 256 个不同字节值)
    uint64_t bitmap[4];      // 位图 (256 位 = 4 × uint64_t)
    void*    children[256];  // 子节点指针数组
};
```

> Node48/Node256 严格延续 Node16 的"keys + bitmap + children"模式，仅容量翻倍。

##### 叶子节点

```cpp
struct ARTLeafNode {
    DataPoint* data_point;   // 关联的数据点 (内含 key_bytes 字节流)
};
```

> **设计说明**: ART 叶子节点仅保存一个指向关联数据点的指针。由于每个 ART 叶子节点只包含一个键值数据点，不需要 bitmap 来指示存在性；键值字节流已存储在 `DataPoint.key_bytes` 中，无需在叶子节点重复存储。

#### 3.3.2 关键设计差异（与标准 ART 对比）

| 特性 | 标准 ART [3] | SUL-plain-index ART 层 |
|------|-------------|-----------|
| 子节点指针 | 存储指针 | 存储指针（内部节点） |
| 路径压缩 | 支持 | **不支持**，每层对应一个字节 |
| 惰性扩展 | 支持 | 不适用（单字节固定层级） |
| Node48/Node256 | 支持 | **已实现**，延续 Node16 的 `keys + bitmap + children` 模式（仅容量扩大） |
| 叶子节点 | 存储完整键 + 值 | 仅存储 DataPoint 指针（键在 DataPoint 中） |
| 叶子 bitmap | 不适用 | **无**（单数据点，不需指示） |

### 3.4 内存布局

```
┌────────────────────────────────────────────────────┐
│             SUL-plain-index Memory                 │
│                                                     │
│  ┌─── Learning Layer ─────────────────┐            │
│  │ [Root GPLInnerNode: 1个]            │            │
│  │ [InnerNode Array: Layer 1..N-1]     │  顺序存储  │
│  │ [LeafNode Array: Layer N]           │            │
│  │   每 LeafNode 内含:                  │            │
│  │     - data_slots[] (定长数组)        │            │
│  │     - bitmap (uint64)                │            │
│  │     - art_root* (指向ART根节点)      │            │
│  └──────────────────────────────────────┘            │
│                                                     │
│  ┌─── ART Layer (独立分配) ─────────────┐            │
│  │ [ARTNode4/ARTNode16 Pool]             │  Pool 分配│
│  │ [ARTLeafNode Pool]                    │            │
│  └──────────────────────────────────────┘            │
└────────────────────────────────────────────────────┘
```

---

## 4. 代码结构设计

### 4.1 项目目录结构

```
sul-index/
├── CMakeLists.txt                    # 构建配置
├── README.md                         # 项目说明
├── LICENSE                           # 开源协议
│
├── include/sul/                      # 公共头文件
│   ├── sul_index.h                   # 索引入口接口
│   ├── types.h                       # 基础类型定义
│   ├── config.h                      # 编译期配置参数
│   │
│   ├── z_order/                      # Z曲线编码模块
│   │   └── z_order.h
│   │
│   ├── learned/                      # 学习层模块
│   │   ├── gpl_model.h               # GPL 模型定义
│   │   ├── gpl_builder.h             # GPL 构建器
│   │   ├── gpl_inner_node.h          # 内部节点
│   │   └── gpl_leaf_node.h           # 叶子节点
│   │
│   ├── art/                          # ART 层模块
│   │   ├── art_node.h                # ART 节点基类
│   │   ├── art_node4.h               # Node4 实现
│   │   ├── art_node16.h              # Node16 实现
│   │   ├── art_leaf.h                # ART 叶子节点
│   │   └── art_tree.h                # ART 树管理
│   │
│   └── query/                        # 查询模块
│       ├── point_query.h             # 点查询
│       ├── range_query.h             # 范围查询
│       └── candidate_set.h           # 候选集管理
│
├── src/                              # 源文件
│   ├── sul_index.cpp                 # 索引入口实现
│   ├── z_order/
│   │   └── z_order.cpp
│   ├── learned/
│   │   ├── gpl_model.cpp
│   │   ├── gpl_builder.cpp
│   │   ├── gpl_inner_node.cpp
│   │   └── gpl_leaf_node.cpp
│   ├── art/
│   │   ├── art_node4.cpp
│   │   ├── art_node16.cpp
│   │   ├── art_leaf.cpp
│   │   └── art_tree.cpp
│   └── query/
│       ├── point_query.cpp
│       ├── range_query.cpp
│       └── candidate_set.cpp
│
├── tests/                            # 单元测试
│   ├── test_z_order.cpp
│   ├── test_gpl.cpp
│   ├── test_art.cpp
│   ├── test_point_query.cpp
│   └── test_range_query.cpp
│
├── benchmarks/                       # 性能基准测试
│   ├── bench_build.cpp
│   ├── bench_point_query.cpp
│   ├── bench_range_query.cpp
│   └── bench_insert.cpp
│
└── docs/                             # 文档
    └── sulplian_项目文档.md
```

### 4.2 核心类设计

#### 4.2.1 索引入口类 SULPlainIndex

```cpp
class SULPlainIndex {
public:
    // 构造函数
    SULPlainIndex(const IndexConfig& config);
    
    // === 构建 ===
    // 批量加载: 传入多维数据点数组
    void bulk_load(const std::vector<DataPoint>& data);
    
    // === 查询 ===
    // 点查询: 查找精确匹配的数据点
    DataPoint* point_query(const int32_t* coords, int dim_count);
    
    // 范围查询: 查找在矩形区域内的所有数据点
    // lower: 左下角坐标, upper: 右上角坐标
    std::vector<DataPoint*> range_query(
        const int32_t* lower, const int32_t* upper, int dim_count);
    
    // === 插入 ===
    void insert(const DataPoint& point);
    
    // === 统计 ===
    size_t memory_usage() const;
    size_t total_points() const;
    
private:
    // 学习层：多层节点数组
    // layers_[0] = 根层, layers_[1..N-1] = 内部层, layers_[N] = 叶子层
    std::vector<std::vector<GPLInnerNode>> inner_layers_;  // 内部层 0..N-1
    std::vector<GPLLeafNode> leaf_nodes_;                   // 叶子层
    
    // ART层 (每个叶子节点挂载一个)
    std::vector<ARTNode4*> art_roots_;
    
    // Z曲线编码器
    ZOrderEncoder encoder_;
    
    // 配置
    IndexConfig config_;
};
```

#### 4.2.2 GPL 构建器

```cpp
class GPLBuilder {
public:
    // 构建多层GPL模型
    // input: 已排序的一维键值数组及其对应的数据点
    // error_bound: 误差界
    // 返回: 多层内部节点数组 + 叶子节点数组 + 冲突数据集
    struct GPLBuildResult {
        std::vector<std::vector<GPLInnerNode>> inner_layers;  // 多层内部节点
        std::vector<GPLLeafNode> leaf_nodes;                   // 叶子节点
        std::vector<DataPoint*> conflict_set;                  // 冲突数据集
    };
    
    GPLBuildResult build(
        const std::vector<int64_t>& sorted_keys,
        const std::vector<DataPoint*>& data_points,
        int32_t error_bound);
    
private:
    // GPL分割算法: 在给定误差界下将数据分为线性段
    std::vector<Segment> gpl_partition(
        const std::vector<int64_t>& keys, int32_t error_bound);
    
    // 递归构建上层索引
    std::vector<GPLInnerNode> build_parent_layer(
        const std::vector<int64_t>& child_keys,  // 子节点起始键值
        int32_t error_bound,
        int32_t child_layer_size);               // 子节点层大小
};
```

#### 4.2.3 ART 树

```cpp
class ARTTree {
public:
    // 按字节流插入数据点
    void insert(const uint8_t* key_bytes, DataPoint* data);
    
    // 按字节流搜索数据点
    DataPoint* search(const uint8_t* key_bytes);
    
    // 范围查询: 在 ART 中查找 [min_key, max_key] 范围内的数据
    std::vector<DataPoint*> range_search(
        const uint8_t* min_key, const uint8_t* max_key);
    
    // 获取最小/最大键值对应的数据
    DataPoint* min_leaf();
    DataPoint* max_leaf();
    
private:
    void* root_;   // ARTNode4* 或 ARTNode16*
};
```

### 4.3 配置参数

```cpp
struct IndexConfig {
    // 学习层参数
    int32_t error_bound = 1000;         // GPL 模型误差界（默认 N/1000）
    int32_t max_layers  = 16;           // 学习层最大层数
    int32_t dim_count   = 2;            // 数据维度

    // 注：叶子节点 slot_count 由 GPL 段长度动态决定，不再使用固定上限。

    // ART 层参数
    int32_t art_key_bytes = 8;          // 键值字节数 (Z 曲线编码 z_value ≤ 64 位 = 8 字节)
    // 注：Node4 / Node16 / Node48 / Node256 全部默认启用，按需自动扩容，无需开关。
};
```

---

## 5. 查询逻辑设计

### 5.1 点查询 (Point Query)

#### 5.1.1 查询流程

```
输入: 多维坐标 → Z曲线编码 → 一维键值 key
                              ↓
                    ┌── 学习层查询 ──┐
                    │  从根节点开始    │
                    │  idx = slope*key │
                    │       + intercept│
                    │  逐层预测至叶子  │
                    └───────┬─────────┘
                            ↓
                    到达 GPL 叶子节点
                            ↓
              在叶子 data_slots[] 中搜索
                            ↓
                 ┌── 命中？──┐
                 │           │
                是           否
                 │           │
                 ▼           ▼
            返回数据点   进入 ART 层查询
                            ↓
                    使用 data_point.key_bytes
                    从 ART 根节点遍历
                    逐字节比较键值数组
                            ↓
                 ┌── 存在？──┐
                 │           │
                是           否
                 │           │
                 ▼           ▼
            返回数据点   返回 nullptr
```

#### 5.1.2 学习层查询伪代码

```
function learned_layer_search(key):
    if inner_layers_ is empty:
        // 只有叶子层，直接搜索
        return search_leaf_layer(key)
    
    current_layer = 0
    current_idx = 0  // 根节点在 layer 0 的下标 0
    
    // 遍历内部层
    while current_layer < inner_layers_.size() - 1:
        node = inner_layers_[current_layer][current_idx]
        // 预测子节点在下一层的下标 (相对偏移转绝对下标)
        child_rel = floor(node.slope * key + node.intercept)
        child_rel = max(0, min(child_rel, node.child_count - 1))
        current_idx = node.child_start + child_rel
        current_layer += 1
    
    // 到达最后一层内部节点，预测叶子节点下标
    node = inner_layers_[current_layer][current_idx]
    leaf_rel = floor(node.slope * key + node.intercept)
    leaf_rel = max(0, min(leaf_rel, node.child_count - 1))
    leaf_idx = node.child_start + leaf_rel
    
    // 在叶子节点数据槽中查找
    leaf = leaf_nodes_[leaf_idx]
    predicted_pos = floor(leaf.slope * key + leaf.intercept)
    search_start = max(0, predicted_pos - error_bound)
    search_end = min(leaf.slot_count - 1, predicted_pos + error_bound)
    
    for pos in search_start..search_end:
        if leaf.bitmap.has(pos) and leaf.data_slots[pos]->z_value == key:
            return leaf.data_slots[pos]
    
    return leaf  // 未找到，返回叶子供 ART 查询
```

#### 5.1.3 ART 层查询伪代码

```
function art_search(key_bytes, art_root):
    current = art_root

    for byte_idx from 0 to 7:  // 8 个字节
        target_byte = key_bytes[byte_idx]

        switch type_of(current):
            case ARTNode4:    scan 4 keys
            case ARTNode16:   scan 16 keys
            case ARTNode48:   scan 48 keys
            case ARTNode256:  scan 256 keys
            case ARTLeaf:     return current.data_point

        if not found: return null
        current = matching child

    // 到达叶子节点 (ARTLeafNode)
    return current.data_point
```

### 5.2 范围查询 (Range Query)

#### 5.2.1 输入输出

- **输入**: 左下角坐标 `lower[dims]` 和右上角坐标 `upper[dims]`
- **输出**: 矩形区域内所有数据点的集合

#### 5.2.2 查询流程

```
Step 1: 边界编码
  lower_point → Z曲线 → z_min, key_bytes_min
  upper_point → Z曲线 → z_max, key_bytes_max

Step 2: 学习层边界定位
  leaf_left  = learned_layer_search(z_min)   // 左边界叶子
  leaf_right = learned_layer_search(z_max)   // 右边界叶子
  
Step 3: 边界内数据收集
  pos_min = leaf_left.predict(z_min)
  pos_max = leaf_right.predict(z_max)
  
  候选数据集 C_data = ∅
  候选 ART 节点集 C_art = ∅
  
  // 处理左边界叶子
  for pos in [pos_min, leaf_left.slot_count - 1]:
      if leaf_left.bitmap.has(pos):
          C_data.add(leaf_left.data_slots[pos])
  if leaf_left.art_root.max_key >= z_min:
      C_art.add(leaf_left.art_root, min_key=key_bytes_min)
  
  // 处理右边界叶子
  for pos in [0, pos_max]:
      if leaf_right.bitmap.has(pos):
          C_data.add(leaf_right.data_slots[pos])
  if z_max >= leaf_right.art_root.min_key:
      C_art.add(leaf_right.art_root, max_key=key_bytes_max)
  
  // 处理中间叶子: 全部数据加入候选
  for leaf in [leaf_left+1, leaf_right-1]:
      C_data.add_all(leaf.all_data_points())
      C_art.add(leaf.art_root)  // 中间叶子的ART全部加入
  
Step 4: ART 层范围查询
  for each (art_root, min_bytes, max_bytes) in C_art:
      C_data.add_all(art_root.range_search(min_bytes, max_bytes))
  
Step 5: 维度过滤
  result = ∅
  for point in C_data:
      if lower[d] <= point.coords[d] <= upper[d] for all d:
          result.add(point)
  
  return result
```

#### 5.2.3 关键优化

1. **位图加速**: 使用 bitmap 快速判断槽位占用状态，避免遍历空槽
2. **边界裁剪**: 左右边界叶子的 ART 仅在键值在范围内时才加入候选
3. **中间叶全量**: 中间叶子数据全部加入，避免逐个判断
4. **最终维度过滤**: Z 曲线保持空间局部性但不精确，最后一步需在各维度上精确判断

### 5.3 查询复杂度分析

| 查询类型 | 学习层 | ART 层 | 总体 |
|---------|--------|--------|------|
| 点查询（命中） | O(log N_model) + O(epsilon) | - | O(log N_model + epsilon) |
| 点查询（未命中） | O(log N_model) + O(epsilon) | O(4) | O(log N_model + epsilon) |
| 范围查询 | O(log N_model + 中间叶子数) | O(4 × \|C_art\|) | O(log N_model + \|candidates\|) |

### 5.4 精确查找（暴力查找 / Brute-Force Scan）

精确查找模块独立于 SUL-plain-index，采用全量扫描方式遍历所有原始数据点，用于验证索引查询结果的正确性和召回率。该模块作为基准真值（Ground Truth）提供方，不依赖索引结构。

#### 5.4.1 设计目标

- **召回率验证**: 以精确查找结果为真值，评估索引查询方案的召回率
- **正确性校验**: 验证索引返回结果是否与暴力扫描结果完全一致
- **性能基准**: 作为查询延迟的上限参考

#### 5.4.2 接口定义

```cpp
// 精确查找器：维护原始数据副本，提供暴力扫描接口
class BruteForceScanner {
public:
    // 构造：接收数据点数组引用
    explicit BruteForceScanner(const std::vector<DataPoint>& data);

    // 精确点查询：扫描全部数据，查找匹配点
    // 返回所有匹配的 DataPoint（理论上最多 1 个，但保留多匹配兼容性）
    std::vector<const DataPoint*> point_query(const int32_t* query_dims, int32_t dim_count) const;

    // 精确范围查询：扫描全部数据，查找在范围 [low, high] 内的所有点
    // 每维 low[i] <= dims[i] <= high[i]
    std::vector<const DataPoint*> range_query(
        const int32_t* low_dims,
        const int32_t* high_dims,
        int32_t dim_count) const;

    // 获取数据总量
    size_t size() const { return data_ref_.size(); }

private:
    const std::vector<DataPoint>& data_ref_;  // 原始数据引用（只读）
};
```

#### 5.4.3 点查询实现

```cpp
std::vector<const DataPoint*> BruteForceScanner::point_query(
    const int32_t* query_dims, int32_t dim_count) const
{
    std::vector<const DataPoint*> results;
    for (const auto& dp : data_ref_) {
        if (dp.dim_count != dim_count) continue;
        bool match = true;
        for (int32_t d = 0; d < dim_count; ++d) {
            if (dp.dimensions[d] != query_dims[d]) {
                match = false;
                break;
            }
        }
        if (match) {
            results.push_back(&dp);
        }
    }
    return results;
}
```

#### 5.4.4 范围查询实现

```cpp
std::vector<const DataPoint*> BruteForceScanner::range_query(
    const int32_t* low_dims,
    const int32_t* high_dims,
    int32_t dim_count) const
{
    std::vector<const DataPoint*> results;
    for (const auto& dp : data_ref_) {
        if (dp.dim_count != dim_count) continue;
        bool in_range = true;
        for (int32_t d = 0; d < dim_count; ++d) {
            if (dp.dimensions[d] < low_dims[d] || dp.dimensions[d] > high_dims[d]) {
                in_range = false;
                break;
            }
        }
        if (in_range) {
            results.push_back(&dp);
        }
    }
    return results;
}
```

#### 5.4.5 召回率计算

```cpp
// 召回率 = 索引返回的真阳性结果数 / 精确查找返回的结果总数
struct RecallMetrics {
    size_t index_result_count;       // 索引查询返回数
    size_t ground_truth_count;       // 精确查找返回数（真值）
    size_t true_positive_count;      // 索引结果中在真值集合中的数量
    double recall;                   // 召回率 = true_positive / ground_truth
    double precision;                // 精确率 = true_positive / index_result_count
};

RecallMetrics compute_recall(
    const std::vector<const DataPoint*>& index_results,
    const std::vector<const DataPoint*>& ground_truth)
{
    RecallMetrics metrics;
    metrics.index_result_count = index_results.size();
    metrics.ground_truth_count = ground_truth.size();

    // 将 ground truth 指针转为集合以便 O(1) 查找
    std::unordered_set<const DataPoint*> truth_set(
        ground_truth.begin(), ground_truth.end());

    metrics.true_positive_count = 0;
    for (const auto* dp : index_results) {
        if (truth_set.count(dp)) {
            ++metrics.true_positive_count;
        }
    }

    metrics.recall = metrics.ground_truth_count > 0
        ? static_cast<double>(metrics.true_positive_count) / metrics.ground_truth_count
        : 1.0;  // 真值为空时召回率定义为 1.0

    metrics.precision = metrics.index_result_count > 0
        ? static_cast<double>(metrics.true_positive_count) / metrics.index_result_count
        : 1.0;

    return metrics;
}
```

#### 5.4.6 复杂度

| 操作 | 时间复杂度 | 空间复杂度 | 说明 |
|------|-----------|-----------|------|
| 点查询 | O(N × D) | O(1) | N 为数据总量，D 为维度数 |
| 范围查询 | O(N × D) | O(K) | K 为结果集大小 |
| 召回率计算 | O(M + G) | O(G) | M 为索引结果数，G 为真值数 |

> **注意**: 精确查找仅用于离线验证和实验分析，不在生产查询路径中使用。其 O(N) 复杂度与索引的 O(log N) 形成鲜明对比，凸显了学习索引的性能优势。

---

## 6. 插入逻辑设计

### 6.1 插入流程

```
输入: DataPoint
  ↓
Z曲线编码 → z_value + key_bytes
  ↓
学习层查询 → 定位叶子节点 leaf
  ↓
在 leaf 中预测插入位置 pos
  ↓
         ┌── 位置为空？──┐
         │              │
        是              否
         │              │
         ▼              ▼
    直接写入槽位    进入 ART 插入
    更新 bitmap      │
         │           ▼
         │    ART 查询，逐字节匹配
         │           │
         │    ┌── 内部节点有空位？──┐
         │    │                   │
         │   是                   否
         │    │                   │
         │    ▼                   ▼
         │  填入空槽            节点扩容
         │  创建 ARTLeafNode    Node4 → Node16
         │  连接子节点指针       原数据+新数据
         │  更新 bitmap         重新插入
         ▼                      
    完成
```

### 6.2 节点扩容规则

| 当前节点 | 已满时 | 新节点 | 操作 |
|---------|--------|--------|------|
| Node4   | 4/4 槽占用     | Node16  | 分配新 Node16，拷贝原 4 个键值+新键值到新节点 |
| Node16  | 16/16 槽占用   | Node48  | 分配新 Node48，拷贝原 16 个键值+新键值到新节点 |
| Node48  | 48/48 槽占用   | Node256 | 分配新 Node256，拷贝原 48 个键值+新键值到新节点 |
| Node256 | 256/256 槽占用 | —       | 不可能发生（每字节只有 256 种取值，全部填满已是上限） |

### 6.3 插入冲突处理

- **学习层冲突**: 预测位置被占用 → 落入 ART 层
- **ART 层冲突**: 内部节点键值数组有空位则填入，否则触发节点扩容（Node4→16→48→256）
- **全局重训练**: 仅当学习层叶子槽位密度过高且无法通过 ART 容纳时才触发批量重建（Node256 已是字节空间上限，理论上 ART 层不再因满而失败）

---

## 7. 构建流程设计

### 7.1 批量构建流程

```
输入: 多维数据点数组
  ↓
Step 1: Z曲线编码 + 字节流生成
  for each point:
      point.z_value = z_order_encode(point.coords)
      point.key_bytes = z_value_to_bytes(point.z_value)  // 预计算 4 字节流
  ↓
Step 2: 排序
  sort data_points by z_value
  ↓
Step 3: 构建多层 GPL 模型 (学习层)
  build_multilayer_gpl(sorted_keys, data_points, error_bound)
  → 叶子节点 + 多层内部节点 + 冲突数据集
  ↓
Step 4: 数据分配
  for each data point:
      用 GPL 模型预测其所在叶子节点
      if 叶子有空槽位:
          写入对应槽位，更新 bitmap
      else:
          加入 conflict_set
  ↓
Step 5: ART 层构建 (对冲突数据)
  for each point in conflict_set:
      使用 point.key_bytes 插入对应叶子的 ART 树中
  ↓
完成
```

### 7.2 GPL 分割算法

GPL（Greedy Pessimistic Linear）算法在 ALT-Index 中提出，时间复杂度 O(n)：

```
function gpl_partition(sorted_keys, error_bound):
    segments = []
    i = 0
    
    while i < len(sorted_keys):
        // 初始化线性段: [i, i+error_bound] 内取两个端点拟合
        segment_start = i
        if i + error_bound >= len(sorted_keys):
            // 最后一段：直接用剩余所有点拟合
            slope, intercept = fit_linear_all(sorted_keys[i:])
            segments.add(Segment(start=i, end=len(sorted_keys)-1, slope, intercept))
            break
        
        // 用两个端点初始化线性模型
        slope, intercept = fit_linear_2points(
            sorted_keys[i], sorted_keys[i + error_bound])
        
        // 贪婪扩展: 在保持误差界内尽可能向右扩展
        j = i + error_bound + 1
        while j < len(sorted_keys):
            predicted = slope * sorted_keys[j] + intercept
            if |predicted - j| > error_bound:
                break
            j += 1
        
        segments.add(Segment(start=i, end=j-1, slope, intercept))
        i = j
    
    return segments
```

### 7.3 多层 GPL 构建

#### 7.3.1 构建原理

多层 GPL 的核心思想来自 PGM-Index [2] 的多层 PLA 模型：底层模型直接覆盖原始数据，上层模型覆盖下层模型的分段边界，递归构建直到顶层仅剩一个节点。

**每层模型的拟合目标**:
- 叶子层：拟合 `(sorted_keys[idx], idx)` 的线性关系，即键值到数据数组下标的映射
- 内部层：拟合 `(child_start_keys[idx], idx)` 的线性关系，即子节点起始键到子节点下标的映射

**关键设计**:
- 每层内部节点的模型斜率/截距基于该层子节点的**起始键值**训练
- 预测时 `child_idx = floor(slope * query_key + intercept)` 给出子节点在数组中的相对偏移
- 父节点通过 `child_start + child_idx` 将相对偏移转换为下一层的绝对下标

#### 7.3.2 构建算法

```
function build_multilayer_gpl(sorted_keys, data_points, error_bound):
    // ===== Step 1: 构建叶子层 =====
    segments = gpl_partition(sorted_keys, error_bound)
    leaf_nodes = []
    for seg in segments:
        leaf = new GPLLeafNode(
            key = sorted_keys[seg.start],
            slope = seg.slope,
            intercept = seg.intercept
        )
        leaf_nodes.append(leaf)
    
    // ===== Step 2: 将数据点分配到叶子节点 =====
    conflict_set = []
    for point in data_points:
        // 线性搜索定位目标叶子 (构建时可使用，查询时会用上层加速)
        leaf_idx = binary_search_leaf(leaf_nodes, point.z_value)
        leaf = leaf_nodes[leaf_idx]
        
        // 用叶子模型预测槽位
        pos = floor(leaf.slope * point.z_value + leaf.intercept)
        pos = max(0, min(pos, leaf.slot_count - 1))
        
        if not leaf.bitmap.has(pos):
            leaf.data_slots[pos] = point
            leaf.bitmap.set(pos)
            leaf.filled_count++
        else:
            conflict_set.add(point)  // 预测冲突，落入 ART
    
    // ===== Step 3: 递归构建上层内部节点 =====
    inner_layers = []
    
    // 提取叶子层起始键值作为上层输入
    child_keys = [leaf.key for leaf in leaf_nodes]
    
    while len(child_keys) > 1:
        // 对子节点起始键值执行 GPL 分割
        segments = gpl_partition(child_keys, error_bound)
        
        parent_nodes = []
        for seg in segments:
            parent = new GPLInnerNode()
            parent.key = child_keys[seg.start]
            // 模型拟合 (child_key, idx - seg.start) → 相对偏移
            // slope 与 intercept 基于 seg 内的 (key, relative_idx) 重新拟合
            parent.slope = seg.slope
            parent.intercept = seg.intercept - seg.start  // 调整截距为相对偏移
            parent.child_start = seg.start
            parent.child_count = seg.end - seg.start + 1
            parent_nodes.append(parent)
        
        inner_layers.insert(0, parent_nodes)  // 插入到最前 (上层在上)
        
        // 提取本层起始键值作为更上层输入
        child_keys = [node.key for node in parent_nodes]
    
    // inner_layers[0] 即为根层 (包含 1 个节点)
    return (inner_layers, leaf_nodes, conflict_set)
```

#### 7.3.3 构建示例

假设排序键值 `[10, 20, 30, 40, 50, 60, 70]`，error_bound = 2:

```
叶子层构建 (GPL 分割 sorted_keys):
  segment 0: keys[0..2] = [10, 20, 30], model: pos = 0.1*k - 1.0
  segment 1: keys[3..6] = [40, 50, 60, 70], model: pos = 0.1*k - 1.0
  → leaf_nodes = [Leaf(10, ...), Leaf(40, ...)]

上层构建 (GPL 分割 child_keys = [10, 40]):
  segment 0: keys[0..1] = [10, 40], model: child_idx = 0.033*k - 0.33
  → parent = GPLInnerNode(key=10, child_start=0, child_count=2)
  → 仅 1 个父节点，为根节点

最终层次:
  Layer 0 (根): [GPLInnerNode(key=10, children=[leaf_0, leaf_1])]
  Layer 1 (叶): [GPLLeafNode(key=10), GPLLeafNode(key=40)]
```

#### 7.3.4 查询示例（基于上例）

查询 key = 55:
```
根层预测: child_idx = floor(0.033 * 55 - 0.33) = floor(1.485) = 1
         → child_start + 1 = 0 + 1 = 1 → leaf_nodes[1]
叶子预测: pos = floor(0.1 * 55 - 1.0) = floor(4.5) = 4
         → 在 data_slots[4] 附近查找
```

### 7.4 ART 构建逻辑

ART 层处理 GPL 模型预测冲突的数据点。构建在 GPL 模型分配完成后进行。

#### 7.4.1 构建流程

```
输入: 冲突数据集 conflict_set (每个点已附加到特定叶子)
  ↓
for each leaf in leaf_nodes:
    if leaf 没有冲突数据:
        leaf.art_root = null  // 该叶子不需要 ART
        continue
    
    创建 ART 根节点 (ARTNode4)
    leaf.art_root = new ARTNode4()
    
    for each point in leaf 的冲突数据:
        art_insert(leaf.art_root, point.key_bytes, point, depth=0)
```

#### 7.4.2 ART 插入算法

```
function art_insert(node, key_bytes, data_point, depth):
    if depth == 4:  // 到达叶子层 (4 字节处理完毕)
        // 在 node 对应位置创建/更新叶子节点
        // node 是内部节点，需要在对应字节位置挂载 ARTLeafNode
        return  // 不应该走到这里，应该在 depth=3 时处理
    
    byte_val = key_bytes[depth]
    
    if node is ARTNode4:
        // 查找 byte_val 是否已存在
        for i in 0..3:
            if node.bitmap.has(i) and node.keys[i] == byte_val:
                // 已存在，继续向下
                if depth == 3:  // 最后一层：更新叶子节点
                    node.children[i] = new ARTLeafNode(data_point)
                else:
                    art_insert(node.children[i], key_bytes, data_point, depth + 1)
                return
        
        // byte_val 不存在，需要插入
        empty_slot = find_empty_slot(node)  // 结合 bitmap 找空位
        if empty_slot >= 0:
            // 有空位
            node.keys[empty_slot] = byte_val
            node.bitmap.set(empty_slot)
            if depth == 3:
                node.children[empty_slot] = new ARTLeafNode(data_point)
            else:
                // 创建新的 ARTNode4 作为中间节点继续
                new_child = new ARTNode4()
                node.children[empty_slot] = new_child
                art_insert(new_child, key_bytes, data_point, depth + 1)
        else:
            // Node4 已满 → 扩容为 Node16
            new_node = expand_node4_to_node16(node)
            replace_node(node, new_node)
            art_insert(new_node, key_bytes, data_point, depth)
    
    if node is ARTNode16:
        // 查找 byte_val
        for i in 0..15:
            if node.bitmap.has(i) and node.keys[i] == byte_val:
                if depth == 3:
                    node.children[i] = new ARTLeafNode(data_point)
                else:
                    art_insert(node.children[i], key_bytes, data_point, depth + 1)
                return
        
        // 查找空位插入
        empty_slot = find_empty_slot(node)
        if empty_slot >= 0:
            node.keys[empty_slot] = byte_val
            node.bitmap.set(empty_slot)
            if depth == 3:
                node.children[empty_slot] = new ARTLeafNode(data_point)
            else:
                new_child = new ARTNode4()
                node.children[empty_slot] = new_child
                art_insert(new_child, key_bytes, data_point, depth + 1)
        else:
            // Node16 已满 → 扩容到 Node48
            new_node = expand_node16_to_node48(node)
            replace_node(node, new_node)
            art_insert(new_node, key_bytes, data_point, depth)

    if node is ARTNode48:
        // 查找 byte_val
        for i in 0..47:
            if node.bitmap.has(i) and node.keys[i] == byte_val:
                // 已存在，继续向下
                ...
                return
        empty_slot = find_empty_slot(node)
        if empty_slot >= 0:
            // 填入
            ...
        else:
            // Node48 已满 → 扩容到 Node256
            new_node = expand_node48_to_node256(node)
            replace_node(node, new_node)
            art_insert(new_node, key_bytes, data_point, depth)

    if node is ARTNode256:
        // 单字节空间上限：理论上不会再满
        for i in 0..255:
            if node.bitmap.has(i) and node.keys[i] == byte_val:
                // 已存在，继续向下
                ...
                return
        empty_slot = find_empty_slot(node)  // 必然 >= 0
        // 填入
        ...
```

#### 7.4.3 节点扩容：Node4 → Node16

```
function expand_node4_to_node16(node4):
    node16 = new ARTNode16()
    
    // 拷贝所有现有键值和子节点
    for i in 0..3:
        if node4.bitmap.has(i):
            // 找到 node16 中第一个空位
            for j in 0..15:
                if not node16.bitmap.has(j):
                    node16.keys[j] = node4.keys[i]
                    node16.children[j] = node4.children[i]
                    node16.bitmap.set(j)
                    break
    
    delete node4
    return node16

function expand_node16_to_node48(node16):
    node48 = new ARTNode48()
    j = 0
    for i in 0..15:
        if node16.bitmap.has(i):
            node48.keys[j] = node16.keys[i]
            node48.children[j] = node16.children[i]
            node48.bitmap.set(j)
            j += 1
    delete node16
    return node48

function expand_node48_to_node256(node48):
    node256 = new ARTNode256()
    j = 0
    for i in 0..47:
        if node48.bitmap.has(i):
            node256.keys[j] = node48.keys[i]
            node256.children[j] = node48.children[i]
            node256.bitmap.set(j)
            j += 1
    delete node48
    return node256
```

> **替换根节点的副作用**: 三个扩容函数在新分配上层节点后，会判断旧节点是否是 ART 根（`root_ == old_node`），若是则将根更新为新节点；返回新节点指针由调用方写回父节点的 `children[idx]`。

#### 7.4.4 构建示例

假设叶子节点有冲突数据点 z_value = 0x12345678 (key_bytes = [0x12, 0x34, 0x56, 0x78]):

```
初始: art_root = new ARTNode4()  (empty)

插入 [0x12, 0x34, 0x56, 0x78]:
  depth=0: byte=0x12, Node4 为空 → 插入 keys[0]=0x12, 创建子 Node4
  depth=1: byte=0x34, 子 Node4 为空 → 插入 keys[0]=0x34, 创建子 Node4
  depth=2: byte=0x56, 子 Node4 为空 → 插入 keys[0]=0x56, 创建子 Node4
  depth=3: byte=0x78, 子 Node4 为空 → 插入 keys[0]=0x78, 创建 ARTLeafNode
  → 4 层树结构，每个字节一层

插入第二个点 [0x12, 0x34, 0xAB, 0xCD]:
  depth=0: byte=0x12 已存在 → 继续
  depth=1: byte=0x34 已存在 → 继续
  depth=2: byte=0xAB 不存在, Node4 有空位 → 插入 keys[1]=0xAB, 创建子 Node4
  depth=3: byte=0xCD, 新子 Node4 为空 → 插入 keys[0]=0xCD, 创建 ARTLeafNode
  → 两个叶子节点共享前两层路径
```

---

## 8. 性能分析与调优

### 8.1 误差界调优

根据 ALT-Index 的分析，误差界 ε 与性能的关系：

```
T_avg(ε) ≈ c · [log₂(N_total / (h·ε)) + k_cal + β₀·(ε/ε₀)·k_ART]
```

- ε 较小时：学习层主导延迟，模型数量多导致 log 因子大
- ε 过大时：ART 层数据增多，Trie 遍历开销变大
- **建议值**: `ε = N_total / 1000`，并在"稳定区域"内微调

### 8.2 内存优化

| 优化项 | 方法 | 效果 |
|--------|------|------|
| 连续内存存储 | 同类型节点存储在 vector 中 | 提高缓存局部性 |
| 位图加速 | 64 位 bitmap 标记槽位 | O(1) 空槽检测 |
| Z 曲线预计算 | 构建时缓存 z_value + key_bytes | 查询时零计算开销 |
| 叶子节点精简 | ART 叶子仅存 DataPoint 指针 | 减少冗余存储 |

### 8.3 预期性能指标

| 指标 | 目标值 | 参考基准 |
|------|--------|---------|
| 点查询延迟 | < 150ns (单线程) | B-tree: ~200-300ns |
| 范围查询延迟 | 数据集相关 | 取决于候选集大小 |
| 插入延迟 | < 500ns | ART: ~300-400ns |
| 内存占用 | < 25 bytes/key (平均) | ART: ~8.1 bytes/key; B-tree: ~30 bytes/key |
| 构建时间 | O(N log N + N·ε) | 主要开销在排序和 GPL 分割 |

---

## 9. 序列化与反序列化

序列化与反序列化模块实现 SUL-plain-index 的持久化存储与恢复，避免每次实验重复构建索引，同时支持索引在不同进程/机器间复用。

### 9.1 设计目标

- **快速复用**: 一次构建、多次加载，跳过耗时的排序和 GPL 训练阶段
- **实验可复现**: 序列化后的索引文件可跨运行使用，确保实验条件一致
- **紧凑存储**: 仅保存必要元数据，控制文件体积
- **版本兼容**: 通过 Magic Number 和版本号保证文件格式可识别

### 9.2 序列化格式

#### 9.2.1 文件布局

```
┌──────────────────────────────────────┐
│           Header (64 bytes)          │
├──────────────────────────────────────┤
│  Magic Number  (8 bytes): "SULIDX01" │
│  Version       (4 bytes): uint32_t   │
│  Dim Count     (4 bytes): uint32_t   │
│  Data Count    (8 bytes): uint64_t   │
│  GPL Count     (4 bytes): uint32_t   │
│  ART Node Count(4 bytes): uint32_t   │
│  Leaf Count    (4 bytes): uint32_t   │
│  Epsilon       (4 bytes): uint32_t   │
│  Reserved      (24 bytes)            │
├──────────────────────────────────────┤
│       DataPoints Array               │
│  ┌────────────────────────────────┐  │
│  │ For each DataPoint:            │  │
│  │   dim_count  (4 bytes)         │  │
│  │   dimensions (dim_count × 4B)  │  │
│  │   z_value    (8 bytes)         │  │
│  │   key_bytes  (4 bytes)         │  │
│  └────────────────────────────────┘  │
├──────────────────────────────────────┤
│       GPL Model Array                │
│  ┌────────────────────────────────┐  │
│  │ For each GPL segment:          │  │
│  │   slope      (double, 8 bytes) │  │
│  │   intercept  (double, 8 bytes) │  │
│  │   start_idx  (uint64_t)        │  │
│  │   end_idx    (uint64_t)        │  │
│  │   is_leaf    (uint8_t)         │  │
│  │   art_offset (int32_t)         │  │
│  └────────────────────────────────┘  │
├──────────────────────────────────────┤
│       ART Nodes (递归)               │
│  ┌────────────────────────────────┐  │
│  │ For each node:                 │  │
│  │   node_type  (uint8_t)         │  │
│  │   Node4: keys[4], bitmap(64b), │  │
│  │            children_offset[4]   │  │
│  │   Node16: keys[16], bitmap(64b)│  │
│  │            children_offset[16]  │  │
│  │   LeafNode: data_point_idx     │  │
│  └────────────────────────────────┘  │
└──────────────────────────────────────┘
```

### 9.3 接口定义

```cpp
// 序列化器
class SULPlainSerializer {
public:
    // 将索引序列化到文件
    static bool save(const SULPlainIndex& index, const std::string& filepath);

    // 从文件反序列化恢复索引
    static std::unique_ptr<SULPlainIndex> load(const std::string& filepath);

    // 验证文件是否为有效的 SUL-plain-index 索引文件
    static bool validate(const std::string& filepath);

private:
    static constexpr uint64_t MAGIC = 0x53554C4944583031ULL;  // "SULIDX01"
    static constexpr uint32_t FORMAT_VERSION = 1;

    // 递归序列化 ART 节点
    static void write_art_node(std::ofstream& out, const ARTNode* node);

    // 递归反序列化 ART 节点
    static ARTNode* read_art_node(std::ifstream& in);
};
```

### 9.4 序列化实现

```cpp
bool SULPlainSerializer::save(const SULPlainIndex& index, const std::string& filepath) {
    std::ofstream out(filepath, std::ios::binary);
    if (!out) return false;

    // --- Header ---
    out.write(reinterpret_cast<const char*>(&MAGIC), sizeof(MAGIC));
    uint32_t version = FORMAT_VERSION;
    out.write(reinterpret_cast<const char*>(&version), sizeof(version));

    uint32_t dim_count = index.get_dim_count();
    out.write(reinterpret_cast<const char*>(&dim_count), sizeof(dim_count));

    uint64_t data_count = index.get_data_count();
    out.write(reinterpret_cast<const char*>(&data_count), sizeof(data_count));

    uint32_t gpl_count = index.get_gpl_count();
    out.write(reinterpret_cast<const char*>(&gpl_count), sizeof(gpl_count));

    uint32_t art_node_count = index.get_art_node_count();
    out.write(reinterpret_cast<const char*>(&art_node_count), sizeof(art_node_count));

    uint32_t leaf_count = index.get_leaf_count();
    out.write(reinterpret_cast<const char*>(&leaf_count), sizeof(leaf_count));

    uint32_t epsilon = index.get_epsilon();
    out.write(reinterpret_cast<const char*>(&epsilon), sizeof(epsilon));

    // reserved padding
    char reserved[24] = {0};
    out.write(reserved, sizeof(reserved));

    // --- DataPoints ---
    for (const auto& dp : index.get_data_points()) {
        out.write(reinterpret_cast<const char*>(&dp.dim_count), sizeof(dp.dim_count));
        out.write(reinterpret_cast<const char*>(dp.dimensions),
                  dp.dim_count * sizeof(int32_t));
        out.write(reinterpret_cast<const char*>(&dp.z_value), sizeof(dp.z_value));
        out.write(reinterpret_cast<const char*>(dp.key_bytes), sizeof(dp.key_bytes));
    }

    // --- GPL Models ---
    for (const auto& gpl : index.get_gpl_segments()) {
        out.write(reinterpret_cast<const char*>(&gpl.slope), sizeof(gpl.slope));
        out.write(reinterpret_cast<const char*>(&gpl.intercept), sizeof(gpl.intercept));
        out.write(reinterpret_cast<const char*>(&gpl.start_idx), sizeof(gpl.start_idx));
        out.write(reinterpret_cast<const char*>(&gpl.end_idx), sizeof(gpl.end_idx));
        uint8_t is_leaf = gpl.is_leaf ? 1 : 0;
        out.write(reinterpret_cast<const char*>(&is_leaf), sizeof(is_leaf));
        out.write(reinterpret_cast<const char*>(&gpl.art_offset), sizeof(gpl.art_offset));
    }

    // --- ART Nodes (DFS 递归) ---
    for (const auto& gpl : index.get_gpl_segments()) {
        if (gpl.is_leaf && gpl.art_root) {
            write_art_node(out, gpl.art_root);
        }
    }

    out.close();
    return out.good();
}
```

### 9.5 反序列化实现

```cpp
std::unique_ptr<SULPlainIndex> SULPlainSerializer::load(const std::string& filepath) {
    std::ifstream in(filepath, std::ios::binary);
    if (!in) return nullptr;

    // --- Header ---
    uint64_t magic;
    in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    if (magic != MAGIC) return nullptr;  // 文件格式不匹配

    uint32_t version;
    in.read(reinterpret_cast<char*>(&version), sizeof(version));
    if (version != FORMAT_VERSION) return nullptr;  // 版本不兼容

    uint32_t dim_count;
    in.read(reinterpret_cast<char*>(&dim_count), sizeof(dim_count));

    uint64_t data_count;
    in.read(reinterpret_cast<char*>(&data_count), sizeof(data_count));

    uint32_t gpl_count, art_node_count, leaf_count, epsilon;
    in.read(reinterpret_cast<char*>(&gpl_count), sizeof(gpl_count));
    in.read(reinterpret_cast<char*>(&art_node_count), sizeof(art_node_count));
    in.read(reinterpret_cast<char*>(&leaf_count), sizeof(leaf_count));
    in.read(reinterpret_cast<char*>(&epsilon), sizeof(epsilon));

    in.ignore(24);  // skip reserved

    // --- DataPoints ---
    std::vector<DataPoint> data_points(data_count);
    for (uint64_t i = 0; i < data_count; ++i) {
        auto& dp = data_points[i];
        in.read(reinterpret_cast<char*>(&dp.dim_count), sizeof(dp.dim_count));
        in.read(reinterpret_cast<char*>(dp.dimensions),
                dp.dim_count * sizeof(int32_t));
        in.read(reinterpret_cast<char*>(&dp.z_value), sizeof(dp.z_value));
        in.read(reinterpret_cast<char*>(dp.key_bytes), sizeof(dp.key_bytes));
    }

    // --- GPL Models ---
    std::vector<GPLSegment> gpl_segments(gpl_count);
    for (uint32_t i = 0; i < gpl_count; ++i) {
        auto& gpl = gpl_segments[i];
        in.read(reinterpret_cast<char*>(&gpl.slope), sizeof(gpl.slope));
        in.read(reinterpret_cast<char*>(&gpl.intercept), sizeof(gpl.intercept));
        in.read(reinterpret_cast<char*>(&gpl.start_idx), sizeof(gpl.start_idx));
        in.read(reinterpret_cast<char*>(&gpl.end_idx), sizeof(gpl.end_idx));
        uint8_t is_leaf;
        in.read(reinterpret_cast<char*>(&is_leaf), sizeof(is_leaf));
        gpl.is_leaf = (is_leaf != 0);
        in.read(reinterpret_cast<char*>(&gpl.art_offset), sizeof(gpl.art_offset));
    }

    // --- ART Nodes ---
    for (uint32_t i = 0; i < gpl_count; ++i) {
        if (gpl_segments[i].is_leaf) {
            gpl_segments[i].art_root = read_art_node(in);
        }
    }

    // 构造索引对象
    auto index = std::make_unique<SULPlainIndex>();
    index->rebuild_from_serialized(std::move(data_points), std::move(gpl_segments),
                                    dim_count, epsilon);
    return index;
}
```

### 9.6 使用示例

```cpp
// === 构建并保存 ===
SULPlainIndex index;
index.build(data_points, epsilon);
SULPlainSerializer::save(index, "index_v1.sul");

// === 后续实验中直接加载 ===
auto index = SULPlainSerializer::load("index_v1.sul");
if (!index) {
    std::cerr << "Failed to load index file" << std::endl;
    return;
}
// 直接执行查询，无需重新构建
auto results = index->range_query(low, high, dim_count);

// === 验证文件格式 ===
if (SULPlainSerializer::validate("index_v1.sul")) {
    std::cout << "Valid SUL-plain-index file" << std::endl;
}
```

### 9.7 文件大小估算

| 组件 | 单元素大小 | 数量 | 总大小估计 |
|------|-----------|------|-----------|
| Header | 64 B | 1 | 64 B |
| DataPoint | 4 + 4D + 8 + 4 | N | N × (16 + 4D) B |
| GPL Segment | 37 B | M | M × 37 B |
| ART Node4 | 1 + 4 + 8 + 4×8 = 45 B | variable | ~A₁ × 45 B |
| ART Node16 | 1 + 16 + 8 + 16×8 = 153 B | variable | ~A₂ × 153 B |
| ART Leaf | 1 + 8 = 9 B | L | L × 9 B |

> 以 N=1,000,000, D=2, ε=1000, M≈1000, A≈5000, L≈20000 为例，总体文件约 25-30 MB。

---

## 10. 实验记录与分析

实验记录模块用于系统化采集索引在构建、查询各阶段的性能指标和结构统计信息，便于横向对比不同参数配置下的表现。

### 10.1 记录维度

#### 10.1.1 构建阶段指标

| 指标 | 类型 | 说明 |
|------|------|------|
| `build_wall_time_ms` | double | 构建总耗时（挂钟时间） |
| `build_cpu_time_ms` | double | 构建 CPU 时间 |
| `sort_time_ms` | double | 排序阶段耗时 |
| `gpl_train_time_ms` | double | GPL 模型训练耗时 |
| `art_build_time_ms` | double | ART 层构建耗时 |
| `total_data_count` | size_t | 总数据点数 |
| `gpl_segment_count` | size_t | GPL 段数量 |
| `art_node_count` | size_t | ART 节点总数 |
| `art_inner_node_count` | size_t | ART 内部节点数（Node4 + Node16） |
| `art_leaf_count` | size_t | ART 叶子节点数 |
| `learning_layer_bytes` | size_t | 学习层内存占用 (bytes) |
| `art_layer_bytes` | size_t | ART 层内存占用 (bytes) |
| `total_index_bytes` | size_t | 索引总内存占用 (bytes) |
| `bytes_per_key` | double | 平均每键字节数 |
| `epsilon` | uint32_t | 误差界参数 |

#### 10.1.2 查询阶段指标

| 指标 | 类型 | 说明 |
|------|------|------|
| `query_type` | enum | 点查询 / 范围查询 |
| `total_queries` | size_t | 查询总数 |
| `total_query_time_ns` | uint64_t | 总查询耗时 (ns) |
| `avg_query_time_ns` | double | 平均查询耗时 (ns) |
| `p50_latency_ns` | double | 中位数延迟 |
| `p99_latency_ns` | double | P99 尾延迟 |
| `learning_layer_hits` | size_t | 学习层命中次数 |
| `art_layer_hits` | size_t | ART 层命中次数 |
| `art_traversal_depth_avg` | double | ART 遍历平均深度 |
| `recall` | double | 召回率（与暴力查找对比） |
| `precision` | double | 精确率 |
| `false_positive_count` | size_t | 假阳性数量 |
| `false_negative_count` | size_t | 假阴性数量 |
| `result_set_size_avg` | double | 平均结果集大小（范围查询） |

### 10.2 数据结构

```cpp
// 构建阶段实验记录
struct BuildRecord {
    // 时间指标
    double build_wall_time_ms    = 0.0;
    double build_cpu_time_ms     = 0.0;
    double sort_time_ms          = 0.0;
    double gpl_train_time_ms     = 0.0;
    double art_build_time_ms     = 0.0;

    // 规模指标
    size_t total_data_count      = 0;
    size_t gpl_segment_count     = 0;
    size_t art_node_count        = 0;
    size_t art_inner_node_count  = 0;
    size_t art_leaf_count        = 0;

    // 内存指标
    size_t learning_layer_bytes  = 0;
    size_t art_layer_bytes       = 0;
    size_t total_index_bytes     = 0;
    double bytes_per_key         = 0.0;

    // 参数
    uint32_t epsilon             = 0;
    uint32_t dim_count           = 0;
};

// 查询阶段实验记录
struct QueryRecord {
    enum QueryType { POINT, RANGE };
    QueryType query_type;

    // 时间指标
    size_t   total_queries       = 0;
    uint64_t total_query_time_ns = 0;
    double   avg_query_time_ns   = 0.0;
    double   p50_latency_ns      = 0.0;
    double   p99_latency_ns      = 0.0;

    // 命中分布
    size_t   learning_layer_hits = 0;
    size_t   art_layer_hits      = 0;
    double   art_traversal_depth_avg = 0.0;

    // 正确性
    double   recall              = 0.0;
    double   precision           = 0.0;
    size_t   false_positive_count = 0;
    size_t   false_negative_count = 0;

    // 结果集
    double   result_set_size_avg = 0.0;
};

// 完整实验记录
struct ExperimentRecord {
    std::string experiment_name;
    std::string timestamp;
    BuildRecord build;
    QueryRecord  point_query;
    QueryRecord  range_query;
};
```

### 10.3 记录器实现

```cpp
class ExperimentLogger {
public:
    // 开始计时
    void start_timer();
    // 结束计时，返回耗时（毫秒）
    double stop_timer_ms();

    // 记录构建指标
    void record_build(const SULPlainIndex& index);

    // 运行查询基准测试并记录
    void run_point_query_benchmark(
        SULPlainIndex& index,
        const BruteForceScanner& scanner,
        const std::vector<std::vector<int32_t>>& queries);

    void run_range_query_benchmark(
        SULPlainIndex& index,
        const BruteForceScanner& scanner,
        const std::vector<RangeQuery>& queries);

    // 导出为 CSV 文件
    void export_csv(const std::string& filepath) const;

    // 打印摘要到控制台
    void print_summary() const;

private:
    ExperimentRecord record_;
    std::chrono::steady_clock::time_point timer_start_;

    // 从索引提取统计信息
    BuildRecord collect_build_stats(const SULPlainIndex& index);

    // 计算延迟分位数
    static double percentile(std::vector<uint64_t> latencies, double p);
};
```

### 10.4 实验记录 CSV 导出格式

```csv
experiment_name,timestamp,dim_count,epsilon,total_data_count,gpl_count,art_nodes,art_leaves,...
build_wall_ms,sort_ms,gpl_train_ms,art_build_ms,index_bytes,bytes_per_key,...
pq_total,pq_avg_ns,pq_p50_ns,pq_p99_ns,pq_learning_hits,pq_art_hits,pq_recall,pq_precision,...
rq_total,rq_avg_ns,rq_p50_ns,rq_p99_ns,rq_learning_hits,rq_art_hits,rq_recall,rq_precision,...
```

每行代表一次完整实验（一组参数配置），便于导入 Python (pandas) 或 Excel 进行可视化分析。

### 10.5 使用示例

```cpp
// === 实验流程 ===
ExperimentLogger logger("exp_epsilon_1000");
BruteForceScanner scanner(data_points);

// 1. 构建索引并计时
logger.start_timer();
SULPlainIndex index;
index.build(data_points, /*epsilon=*/1000);
double build_time = logger.stop_timer_ms();
logger.record_build(index);

// 2. 生成查询负载
auto point_queries = generate_random_point_queries(10000, dim_count);
auto range_queries = generate_random_range_queries(1000, dim_count, selectivity);

// 3. 运行点查询基准
logger.run_point_query_benchmark(index, scanner, point_queries);

// 4. 运行范围查询基准
logger.run_range_query_benchmark(index, scanner, range_queries);

// 5. 输出结果
logger.print_summary();
logger.export_csv("experiments/exp_epsilon_1000.csv");
```

### 10.6 建议实验矩阵

| 实验维度 | 变量 | 建议取值 |
|---------|------|---------|
| 数据规模 N | total_data_count | 10^4, 10^5, 10^6, 10^7 |
| 维度 D | dim_count | 2, 3, 4 |
| 误差界 ε | epsilon | 16, 64, 256, 1000, N/100, N/1000 |
| 数据分布 | 分布类型 | uniform, normal, zipfian |
| 范围选择性 | selectivity | 0.01%, 0.1%, 1%, 10% |

---

## 11. 参考文献

[1] Y. Yang, F. Wang, M. Lei, P. Zhang, and D. Feng, "ALT-index: A Hybrid Learned Index for Concurrent Memory Database Systems," 2024.

[2] P. Ferragina and G. Vinciguerra, "The PGM-index: a fully-dynamic compressed learned index with provable worst-case bounds," PVLDB, vol. 13, no. 8, pp. 1162-1175, 2020.

[3] V. Leis, A. Kemper, and T. Neumann, "The Adaptive Radix Tree: ARTful Indexing for Main-Memory Databases," in ICDE, 2013, pp. 38-49.

---

## 附录 A: 关键术语表

| 术语 | 英文 | 说明 |
|------|------|------|
| Z 曲线 | Z-order Curve / Morton Code | 多维空间填充曲线，保持局部性 |
| GPL | Greedy Pessimistic Linear | 贪婪悲观线性分割算法 |
| ART | Adaptive Radix Tree | 自适应基数树 |
| PGM | Piecewise Geometric Model | 分段几何模型索引 |
| 误差界 | Error Bound | 模型预测允许的最大位置偏差 |
| 位图 | Bitmap | 用比特位标记数组元素是否存在 |
| 字节流 | Byte Stream / key_bytes | z_value 按 8 位分割的 4 字节大端序表示 |

## 附录 B: 实现注意事项

1. **32 位整数**: 数据类型限定为 32 位有符号整数，Z 曲线编码后为 32×dim 位以内的整数；对浮点数据集需先按维度独立缩放到 `[0, INT32_MAX)` 再编码
2. **字节分割**: ART 层每个字节对应一层，z_value 视为 64 位无符号 → **8 字节 = 8 层**
3. **不实现路径压缩**: SUL-plain-index 中 ART 层每层严格对应一个字节，与标准 ART 不同
4. **叶子节点槽位**: 按 GPL 段长度**动态分配** `slot_count = max(2×seg_len + 2×ε, 8)`；`art_root` 字段始终保留（空树为 nullptr）
5. **Node48/Node256**: **已实现**，扩容链 Node4 → Node16 → Node48 → Node256；Node48/256 沿用 Node16 的 `keys + bitmap + children` 模式，仅容量扩大
6. **key_bytes 预计算**: 构建时与 z_value 同时计算并缓存，ART 遍历直接使用无需重复分割
7. **ART 叶子节点**: 仅包含 DataPoint 指针，键值存储在 DataPoint.key_bytes 中
8. **每叶必挂 ART**: 学习层每个叶子节点都对应一棵 `ARTTree`（无冲突时 `root_ = nullptr` 为空树），方便统一索引和后续插入
9. **环境一致性**: 所有开发、编译、测试在 WSL2 Ubuntu 22.04 环境中进行，确保跨机器可复现

---

*文档生成日期: 2026-05-13 | 基于 sul_plan.md / ALT-Index / ART / PGM-Index 文献*
