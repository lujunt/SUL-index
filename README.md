# SDLM-index (also named SUL-index)

SDLM-index (i.e., SUL-index) is a secure dynamic learned multi-dimensional index, which combines multi-level secure greedy pessimistic linear models for ciphertext-domain index prediction with secure prefix trees that manage conflict points and dynamic updates.

## How to use

### 1. Required libraries

#### Plaintext build

- Linux (tested on Ubuntu 22.04)
- GCC 11 or newer with C++17 support
- CMake 3.16 or newer

```bash
sudo apt update
sudo apt install -y build-essential cmake
```

#### Encrypted build

The encrypted targets additionally require OpenMP, GMP, NTL, and ophelib. The ophelib
headers are vendored under `third_party/ophelib`; the setup script builds its static library.

```bash
cd sul-index
./setup_cipher.sh
```

To build only the plaintext implementation, configure CMake with
`-DBUILD_CIPHER=OFF`.

### 2. Prepare datasets

Store input files under `sul-index/datasets/` or pass another path directly to the
executables. A dataset is a headerless CSV with one row per point:

```text
dim_1,dim_2,...,dim_N,id
```

Coordinates are floating-point values. The loader applies per-dimension min/max
normalization; insertion and evaluation files should reuse the base dataset's
normalization domain.

Query files contain lower bounds followed by upper bounds:

```text
lo_1,lo_2,...,lo_N,hi_1,hi_2,...,hi_N
```

Generate the standard 0.25%, 0.5%, 1%, 2%, and 4% query files with:

```bash
./build/sul_query_gen datasets/uniform_20000_1_2_.csv 100 query --target-hits
```

`--target-hits` is recommended for skewed datasets because it searches for windows
whose observed hit counts match the requested selectivity. Without it, window sizes
are derived from uniform volume.

Split a dataset into training and insertion sets with stratified Z-order sampling:

```bash
./build/sul_split datasets/uniform_20000_1_2_.csv datasets 0.9 42 64
```

The arguments are `full_csv`, `output_dir`, `train_ratio`, `seed`, and `strata`.

### 3. Build

Run all commands from the `sul-index/` directory:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Plaintext-only build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_CIPHER=OFF
cmake --build build -j
```

### 4. Run

#### Plaintext demo

```bash
./build/sul_demo datasets/uniform_20000_1_2_.csv
```

#### Encrypted demo

```bash
./build/sul_cipher_demo \
  datasets/uniform_20000_1_2_.csv \
  query/uniform_20000_dim2_0.25.csv \
  1024 4
```

Optional trailing arguments select an insertion CSV and the number of sampled point
queries.

#### Compare plaintext and encrypted results

```bash
./build/sul_compare_demo \
  datasets/uniform_20000_1_2_.csv \
  query/uniform_20000_dim2_0.25.csv \
  1024 4
```

The comparison exits with status 0 only when all plaintext and encrypted range-query
results match. It writes build and query metrics under `record/` and caches encrypted
indexes under `indexes/`.

#### Serialization round trip

```bash
./build/sul_serde_demo \
  datasets/uniform_20000_1_2_.csv \
  query/uniform_20000_dim2_0.25.csv \
  1024 indexes
```

#### Mixed read/write workload

```bash
./build/sul_workload \
  datasets/uniform_20000_dim2_N18000_train.csv \
  datasets/uniform_20000_dim2_N2000_insert.csv \
  1024 4 50 2000
```

The final arguments are Paillier key size, error bound, read percentage, and operation
count.

#### Adaptive retraining

```bash
./build/sul_update BASE.csv INSERT.csv EVAL_100.csv \
  1024 4 20 \
  --monitor-file MONITOR.csv \
  --beta 1.0 \
  --run-dir record/retrain/example
```

Run `./build/sul_update --help` for the full checkpoint, monitoring, snapshot, and
retraining options. Batch experiment drivers are available under `scripts/`.

### 5. Test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Generated datasets, query windows, indexes, records, logs, build artifacts, and Python
caches are intentionally excluded from version control.

## Project layout

```text
sul-index/
├── include/sul/                 Public plaintext and encrypted index headers
├── src/                         GPL, ART, Z-order, utility, and cipher implementations
├── agreements/agreements/       OSM, SIC, and SPI protocol implementations
├── scripts/                     Dataset preparation and experiment automation
├── tests/                       C++ and Python tests
├── third_party/ophelib/         Vendored ophelib headers
├── CMakeLists.txt
└── setup_cipher.sh
```
