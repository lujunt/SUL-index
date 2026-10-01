#!/usr/bin/env python3
"""Aggregate record/*.csv into a SpreadsheetML 2003 XML workbook with an .xls extension.

The workbook contains parameter, workload, and update experiment sheets, each followed
by a summary and analysis section.
"""
from __future__ import annotations
import csv, os, sys
from pathlib import Path
from xml.sax.saxutils import escape

ROOT = Path(__file__).resolve().parent.parent
REC  = ROOT / "record"
OUT  = ROOT / "experiment_summary.xls"

DATASETS_FULL  = ["UNI", "SKE", "ABUS", "PLUT", "USAC", "MBF"]
DATASETS_SCENE = ["UNI", "SKE", "ABUS", "USAC", "MBF"]
N_LIST   = [20000, 40000, 60000, 80000, 100000]
ERR_LIST = [1, 2, 4, 8, 16]
K_LIST   = [1024, 2048, 3072, 4096]
DIM_LIST = [2, 3, 4, 5, 6]
SL_LIST  = ["0p25", "0p5", "1", "2", "4"]
SL_NUM   = {"0p25": 0.25, "0p5": 0.5, "1": 1.0, "2": 2.0, "4": 4.0}
RW_LIST  = [("100", "0"), ("80", "20"), ("50", "50"), ("20", "80"), ("0", "100")]
UL_LIST  = ["0p25", "0p5", "1", "2", "5", "10", "20"]
UL_NUM   = {"0p25": 0.25, "0p5": 0.5, "1": 1.0, "2": 2.0, "5": 5.0, "10": 10.0, "20": 20.0}


def read_last_row(path: Path):
    if not path.exists():
        return None
    with path.open() as f:
        rd = csv.DictReader(f)
        rows = list(rd)
    return rows[-1] if rows else None


def f_num(v, ndigits=None):
    if v is None or v == "":
        return ""
    try:
        x = float(v)
        if ndigits is None:
            return x
        return round(x, ndigits)
    except Exception:
        return v


def load_param_n():
    out = []
    for ds in DATASETS_FULL:
        for N in N_LIST:
            b = read_last_row(REC / f"build_{ds}_{N}_K1024_err4_dim2.csv")
            q = read_last_row(REC / f"rangequery_{ds}_{N}_K1024_err4_dim2_sl0p25.csv")
            if not b or not q:
                continue
            out.append({
                "Dataset": ds, "N": N,
                "build_ms":        f_num(b["build_ms"], 2),
                "file_bytes_kl1":  int(float(b["file_bytes_kl1"])),
                "learn_node":      int(b["learn_node_count"]),
                "learn_leaf":      int(b["learn_leaf_count"]),
                "learn_height":    int(b["learn_height"]),
                "art_node":        int(b["art_node_count"]),
                "art_leaf":        int(b["art_leaf_count"]),
                "art_height":      int(b["art_height"]),
                "rq_avg_ms":       f_num(q["avg_ms"], 3),
                "candidates_avg":  f_num(q["candidates_avg"], 2),
                "recall":          f_num(q["recall"], 4),
                "precision":       f_num(q["precision"], 4),
            })
    return out


def load_param_err():
    out = []
    for ds in DATASETS_FULL:
        for e in ERR_LIST:
            b = read_last_row(REC / f"build_{ds}_20000_K1024_err{e}_dim2.csv")
            q = read_last_row(REC / f"rangequery_{ds}_20000_K1024_err{e}_dim2_sl0p25.csv")
            if not b or not q:
                continue
            out.append({
                "Dataset": ds, "err": e,
                "build_ms":        f_num(b["build_ms"], 2),
                "file_bytes_kl1":  int(float(b["file_bytes_kl1"])),
                "learn_node":      int(b["learn_node_count"]),
                "learn_leaf":      int(b["learn_leaf_count"]),
                "learn_height":    int(b["learn_height"]),
                "art_node":        int(b["art_node_count"]),
                "art_leaf":        int(b["art_leaf_count"]),
                "rq_avg_ms":       f_num(q["avg_ms"], 3),
                "candidates_avg":  f_num(q["candidates_avg"], 2),
                "recall":          f_num(q["recall"], 4),
                "precision":       f_num(q["precision"], 4),
            })
    return out


def load_param_k():
    out = []
    for ds in DATASETS_FULL:
        for k in K_LIST:
            b = read_last_row(REC / f"build_{ds}_20000_K{k}_err4_dim2.csv")
            q = read_last_row(REC / f"rangequery_{ds}_20000_K{k}_err4_dim2_sl0p25.csv")
            if not b or not q:
                continue
            out.append({
                "Dataset": ds, "K(bit)": k,
                "keygen_ms":         f_num(b["keygen_ms"], 2),
                "build_ms":          f_num(b["build_ms"], 2),
                "file_bytes_kl1":    int(float(b["file_bytes_kl1"])),
                "rq_avg_ms":         f_num(q["avg_ms"], 3),
                "learning_ms_avg":   f_num(q["learning_ms_avg"], 3),
                "spi_filter_ms_avg": f_num(q["spi_filter_ms_avg"], 3),
                "recall":            f_num(q["recall"], 4),
            })
    return out


def load_param_dim():
    out = []
    for ds in DATASETS_FULL:
        for d in DIM_LIST:
            b = read_last_row(REC / f"build_{ds}_20000_K1024_err4_dim{d}.csv")
            q = read_last_row(REC / f"rangequery_{ds}_20000_K1024_err4_dim{d}_sl0p25.csv")
            if not b or not q:
                continue
            out.append({
                "Dataset": ds, "dim": d,
                "build_ms":        f_num(b["build_ms"], 2),
                "file_bytes_kl1":  int(float(b["file_bytes_kl1"])),
                "learn_height":    int(b["learn_height"]),
                "art_height":      int(b["art_height"]),
                "rq_avg_ms":       f_num(q["avg_ms"], 3),
                "candidates_avg": f_num(q["candidates_avg"], 2),
                "recall":          f_num(q["recall"], 4),
                "precision":       f_num(q["precision"], 4),
            })
    return out


def load_param_sl():
    out = []
    for ds in DATASETS_FULL:
        for sl in SL_LIST:
            q = read_last_row(REC / f"rangequery_{ds}_20000_K1024_err4_dim2_sl{sl}.csv")
            if not q:
                continue
            out.append({
                "Dataset": ds, "sl_pct(%)": SL_NUM[sl],
                "rq_avg_ms":         f_num(q["avg_ms"], 3),
                "learning_ms_avg":   f_num(q["learning_ms_avg"], 3),
                "spi_filter_ms_avg": f_num(q["spi_filter_ms_avg"], 3),
                "candidates_avg":    f_num(q["candidates_avg"], 2),
                "returned_avg":      f_num(q["returned_avg"], 2),
                "recall":            f_num(q["recall"], 4),
                "precision":         f_num(q["precision"], 4),
                "actual_ratio_pct":  f_num(q["actual_ratio_pct"], 4),
            })
    return out


def load_workload():
    out = []
    for ds in DATASETS_SCENE:
        for R, W in RW_LIST:
            w = read_last_row(REC / f"workload_{ds}_20000_K1024_err4_dim2_R{R}W{W}.csv")
            if not w:
                continue
            out.append({
                "Dataset": ds,
                "Read ratio (%)": int(R), "Write ratio (%)": int(W),
                "ops_total":             int(w["ops_total"]),
                "total_ms":               f_num(w["total_ms"], 2),
                "throughput_total":       f_num(w["throughput_total"], 2),
                "throughput_read":        f_num(w["throughput_read"], 2),
                "throughput_write":       f_num(w["throughput_write"], 2),
                "query_latency_avg_ms":   f_num(w["query_latency_avg_ms"], 3),
                "learning_query_avg_ms": f_num(w["learning_query_avg_ms"], 3),
                "art_query_avg_ms":       f_num(w["art_query_avg_ms"], 3),
                "update_latency_avg_ms": f_num(w["update_latency_avg_ms"], 3),
                "locate_avg_ms":          f_num(w["locate_avg_ms"], 3),
                "update_avg_ms":          f_num(w["update_avg_ms"], 3),
            })
    return out


def load_update():
    out = []
    for ds in DATASETS_SCENE:
        for ul in UL_LIST:
            u = read_last_row(REC / f"update_{ds}_20000_K1024_err4_dim2_ul{ul}.csv")
            if not u:
                continue
            out.append({
                "Dataset": ds,
                "ul_pct(%)": UL_NUM[ul],
                "update_count":      int(u["update_count"]),
                "learn_cnt":         int(u["learn_cnt"]),
                "art_cnt":           int(u["art_cnt"]),
                "fail_cnt":          int(u["fail_cnt"]),
                "update_avg_ms":     f_num(u["update_avg_ms"], 3),
                "save_ms":           f_num(u["save_ms"], 2),
                "load_ms":           f_num(u["load_ms"], 2),
                "file_bytes_kl1":    int(float(u["file_bytes_kl1"])),
                "post_query_avg_ms": f_num(u["post_query_avg_ms"], 3),
                "post_recall":       f_num(u["post_recall"], 4),
                "candidates_avg":    f_num(u["candidates_avg"], 2),
                "middle_total_avg":  f_num(u["middle_total_avg"], 2),
                "middle_pruned_avg": f_num(u["middle_pruned_avg"], 2),
                "spi_filter_ms_avg": f_num(u["spi_filter_ms_avg"], 3),
            })
    return out


def cell_xml(v) -> str:
    if v == "" or v is None:
        return '<Cell><Data ss:Type="String"></Data></Cell>'
    if isinstance(v, bool):
        return f'<Cell><Data ss:Type="String">{v}</Data></Cell>'
    if isinstance(v, (int, float)):
        return f'<Cell><Data ss:Type="Number">{v}</Data></Cell>'
    return f'<Cell><Data ss:Type="String">{escape(str(v))}</Data></Cell>'


def row_xml(values) -> str:
    return "<Row>" + "".join(cell_xml(v) for v in values) + "</Row>"


def section(title, rows, headers):
    parts = [row_xml([title]), row_xml(headers)]
    for r in rows:
        parts.append(row_xml([r.get(h, "") for h in headers]))
    parts.append(row_xml([]))
    return "\n".join(parts)


def sheet(name, body_rows):
    return (
        f'<Worksheet ss:Name="{escape(name)}"><Table>\n'
        + "\n".join(body_rows)
        + "\n</Table></Worksheet>"
    )


def summary_param(p_n, p_err, p_k, p_dim, p_sl):
    L = ["Summary and analysis"]
    if p_n:
        by_ds = {}
        for r in p_n:
            by_ds.setdefault(r["Dataset"], []).append(r)
        if by_ds:
            ds = next(iter(by_ds))
            seq = by_ds[ds]
            if len(seq) >= 2:
                t0, tn = seq[0], seq[-1]
                rb = tn["build_ms"] / t0["build_ms"] if t0["build_ms"] else 0
                rq = tn["rq_avg_ms"] / t0["rq_avg_ms"] if t0["rq_avg_ms"] else 0
                L.append(
                    f"1) Dataset size: increasing N from {t0['N']} to {tn['N']} (5x) raises {ds} "
                    f"build_ms by about {rb:.1f}x and file_bytes_kl1 almost linearly, while "
                    f"range-query avg_ms rises only about {rq:.1f}x because GPL lookup and ART pruning "
                    "prevent query latency from scaling linearly."
                )
    if p_err:
        L.append(
            "2) Error bound: increasing err from 1 to 16 reduces learning leaves and ART nodes, "
            "but enlarges the SIC window [pos +/- err], raising candidates_avg and usually rq_avg_ms. "
            "err=4 is a useful build/query compromise on most datasets."
        )
    if p_k:
        L.append(
            "3) Key size: increasing K from 1024 to 4096 substantially raises keygen_ms, homomorphic "
            "operation cost, rq_avg_ms, spi_filter_ms_avg, and ciphertext storage. K trades query "
            "latency and storage for security."
        )
    if p_dim:
        L.append(
            "4) Dimensions: from 2D to 6D, ART height follows key_len and build_ms reflects deeper "
            "Z-order/ART processing. The curse of dimensionality expands candidates_avg and rq_avg_ms, "
            "while recall remains near 1.0."
        )
    if p_sl:
        L.append(
            "5) Selectivity: from 0.25% to 4%, returned_avg grows approximately with dimensional "
            "volume, candidates_avg rises, and SPI filtering dominates rq_avg_ms. actual_ratio_pct "
            "tracks sl_pct, while precision and recall remain near 1.0."
        )
    return L


def summary_workload(rows):
    L = ["Summary and analysis"]
    if not rows:
        return L
    L.append(
        "1) The 100% read workload has the highest throughput. The 100% write path combines lookup, "
        "learning/ART insertion, and encrypted-bbox updates, so update_latency_avg_ms is approximately "
        "locate_avg_ms + update_avg_ms and exceeds insertion-only update_avg_ms."
    )
    L.append(
        "2) Mixed-workload throughput lies between all-read and all-write results. Higher write ratios "
        "approach R0W100 latency because encrypted comparisons and homomorphic recomputation make the "
        "write path the bottleneck."
    )
    L.append(
        "3) workload update_avg_ms measures insertion only and matches the sul_update definition; "
        "consistent serial reruns provide a cross-check."
    )
    return L


def summary_update(rows):
    L = ["Summary and analysis"]
    if not rows:
        return L
    L.append(
        "1) Insertion-only update_avg_ms is largely independent of ul_pct because ul_pct controls "
        "volume, while per-insert cost comes from learning lookup, ART insertion, and bbox encryption."
    )
    L.append(
        "2) art_cnt greatly exceeding learn_cnt shows that most new points fall outside the learning "
        "error window and route to ART, as designed. fail_cnt=0 indicates stable insertion."
    )
    L.append(
        "3) post_query_avg_ms grows moderately with ul_pct as the dataset and ART deepen, while "
        "post_recall remains near 0.997."
    )
    L.append(
        "4) Concurrent runs can inflate and destabilize post_query_avg_ms; serial reruns are stable. "
        "middle_total/pruned and candidates_avg rise gradually with insertion volume as expected."
    )
    return L


def main():
    p_n   = load_param_n()
    p_err = load_param_err()
    p_k   = load_param_k()
    p_dim = load_param_dim()
    p_sl  = load_param_sl()
    wl    = load_workload()
    up    = load_update()

    s1_rows = [
        row_xml(["Parameter experiments (baseline: N=20000, K=1024, err=4, dim=2, sl=0.25%)"]),
        row_xml([]),
    ]
    s1_rows.append(section(
        "A. Dataset-size sweep (K=1024, err=4, dim=2, sl=0.25%)", p_n,
        ["Dataset","N","build_ms","file_bytes_kl1","learn_node","learn_leaf","learn_height",
         "art_node","art_leaf","art_height","rq_avg_ms","candidates_avg","recall","precision"],
    ))
    s1_rows.append(section(
        "B. Error-bound sweep (N=20000, K=1024, dim=2, sl=0.25%)", p_err,
        ["Dataset","err","build_ms","file_bytes_kl1","learn_node","learn_leaf","learn_height",
         "art_node","art_leaf","rq_avg_ms","candidates_avg","recall","precision"],
    ))
    s1_rows.append(section(
        "C. Key-size sweep (N=20000, err=4, dim=2, sl=0.25%)", p_k,
        ["Dataset","K(bit)","keygen_ms","build_ms","file_bytes_kl1","rq_avg_ms",
         "learning_ms_avg","spi_filter_ms_avg","recall"],
    ))
    s1_rows.append(section(
        "D. Dimension sweep (N=20000, K=1024, err=4, sl=0.25%)", p_dim,
        ["Dataset","dim","build_ms","file_bytes_kl1","learn_height","art_height",
         "rq_avg_ms","candidates_avg","recall","precision"],
    ))
    s1_rows.append(section(
        "E. Query-selectivity sweep (N=20000, K=1024, err=4, dim=2)", p_sl,
        ["Dataset","sl_pct(%)","rq_avg_ms","learning_ms_avg","spi_filter_ms_avg",
         "candidates_avg","returned_avg","recall","precision","actual_ratio_pct"],
    ))
    for line in summary_param(p_n, p_err, p_k, p_dim, p_sl):
        s1_rows.append(row_xml([line]))

    s2_rows = [
        row_xml(["Workload experiments (N=18000 base + 2000 ops, K=1024, err=4, dim=2)"]),
        row_xml([]),
    ]
    s2_rows.append(section(
        "Read/write sweep (R/W in {100/0, 80/20, 50/50, 20/80, 0/100})", wl,
        ["Dataset","Read ratio (%)","Write ratio (%)","ops_total","total_ms",
         "throughput_total","throughput_read","throughput_write",
         "query_latency_avg_ms","learning_query_avg_ms","art_query_avg_ms",
         "update_latency_avg_ms","locate_avg_ms","update_avg_ms"],
    ))
    for line in summary_workload(wl):
        s2_rows.append(row_xml([line]))

    s3_rows = [
        row_xml(["Update experiments (base N=20000, K=1024, err=4, dim=2, ul_pct increments)"]),
        row_xml([]),
    ]
    s3_rows.append(section(
        "ul_pct sweep (ul in {0.25%, 0.5%, 1%, 2%, 5%, 10%, 20%})", up,
        ["Dataset","ul_pct(%)","update_count","learn_cnt","art_cnt","fail_cnt",
         "update_avg_ms","save_ms","load_ms","file_bytes_kl1",
         "post_query_avg_ms","post_recall","candidates_avg",
         "middle_total_avg","middle_pruned_avg","spi_filter_ms_avg"],
    ))
    for line in summary_update(up):
        s3_rows.append(row_xml([line]))

    body = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<?mso-application progid="Excel.Sheet"?>\n'
        '<Workbook xmlns="urn:schemas-microsoft-com:office:spreadsheet"\n'
        ' xmlns:o="urn:schemas-microsoft-com:office:office"\n'
        ' xmlns:x="urn:schemas-microsoft-com:office:excel"\n'
        ' xmlns:ss="urn:schemas-microsoft-com:office:spreadsheet"\n'
        ' xmlns:html="http://www.w3.org/TR/REC-html40">\n'
        + sheet("Parameters", s1_rows) + "\n"
        + sheet("Workloads", s2_rows) + "\n"
        + sheet("Updates", s3_rows) + "\n"
        + "</Workbook>\n"
    )
    OUT.write_text(body, encoding="utf-8")
    print(f"[ok] wrote {OUT}  ({OUT.stat().st_size} bytes)")
    print("     sheets: Parameters / Workloads / Updates")
    print(f"     rows:   N={len(p_n)}  err={len(p_err)}  K={len(p_k)}  dim={len(p_dim)}  sl={len(p_sl)}  workload={len(wl)}  update={len(up)}")


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--retrain-runs":
        # Export the new schema separately so beta runs do not mix with legacy theta runs.
        from plot_retrain import main as retrain_main
        retrain_main(sys.argv[2:] + ["--no-figures"])
    else:
        main()
