#!/usr/bin/env python3
from __future__ import annotations

import csv
import json
import math
import re
import sys
from pathlib import Path

from analyze_wrk_results import fmt_ms, fmt_num, parse_wrk_output


def load_json(path: Path) -> dict:
    return json.loads(path.read_text())


def wrk_thread_sort_key(name: str) -> tuple[int, str]:
    match = re.fullmatch(r"wrk_t(\d+)", name)
    if not match:
        return (math.inf, name)
    return (int(match.group(1)), name)


def generate_report(results_dir: Path) -> None:
    rows = []
    for config_dir in sorted(path for path in results_dir.iterdir() if path.is_dir()):
        config_meta = config_dir / "config.meta.json"
        if not config_meta.exists():
            continue
        config = load_json(config_meta)
        config["config_description"] = config.pop("description", "")
        for meta_path in sorted(config_dir.glob("wrk_t*.meta.json")):
            meta = load_json(meta_path)
            meta["workload_description"] = meta.pop("description", "")
            parsed = parse_wrk_output(config_dir / meta["raw_output"])
            rows.append({**config, **meta, **parsed})

    rows.sort(key=lambda item: (item["workers"], item["wrk_threads"]))

    csv_path = results_dir / "summary.csv"
    with csv_path.open("w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(
            [
                "config",
                "workers",
                "wrk_threads",
                "connections",
                "duration",
                "requests_per_sec",
                "avg_latency_ms",
                "p50_ms",
                "p90_ms",
                "p99_ms",
                "requests_total",
                "socket_errors",
                "timeout_errors",
                "transfer_per_sec",
            ]
        )
        for item in rows:
            writer.writerow(
                [
                    item["config"],
                    item["workers"],
                    item["wrk_threads"],
                    item["connections"],
                    item["duration"],
                    item["requests_per_sec"],
                    item["avg_latency_ms"],
                    item["p50_ms"],
                    item["p90_ms"],
                    item["p99_ms"],
                    item["requests_total"],
                    item["socket_errors"],
                    item["timeout_errors"],
                    item["transfer_per_sec"],
                ]
            )

    best_rps = max(rows, key=lambda item: item["requests_per_sec"] or -math.inf)
    zero_error_rows = [item for item in rows if item["socket_errors"] == 0]
    best_zero_error = (
        max(zero_error_rows, key=lambda item: item["requests_per_sec"] or -math.inf)
        if zero_error_rows else None
    )

    report_lines = [
        "# wrk Thread Matrix Report",
        "",
        "## Summary",
        "",
        f"- Worker configs tested: {len({item['workers'] for item in rows})}",
        f"- wrk thread levels tested: {len({item['wrk_threads'] for item in rows})}",
        f"- Total benchmark rows: {len(rows)}",
        f"- Best throughput overall: `workers={best_rps['workers']}, wrk_threads={best_rps['wrk_threads']}` at `{fmt_num(best_rps['requests_per_sec'])}` req/s",
        (
            f"- Best zero-error throughput: `workers={best_zero_error['workers']}, wrk_threads={best_zero_error['wrk_threads']}` at `{fmt_num(best_zero_error['requests_per_sec'])}` req/s"
            if best_zero_error
            else "- Best zero-error throughput: none"
        ),
        "",
    ]

    workers_list = sorted({item["workers"] for item in rows})
    for workers in workers_list:
        worker_rows = [item for item in rows if item["workers"] == workers]
        best_for_worker = max(
            worker_rows, key=lambda item: item["requests_per_sec"] or -math.inf
        )
        report_lines.extend(
            [
                f"### workers={workers}",
                "",
                f"- Best wrk thread setting: `wrk_threads={best_for_worker['wrk_threads']}` at `{fmt_num(best_for_worker['requests_per_sec'])}` req/s",
                "",
                "| wrk Threads | Conns | Req/s | Avg | p99 | Errors |",
                "| ---: | ---: | ---: | ---: | ---: | ---: |",
            ]
        )
        for item in sorted(worker_rows, key=lambda row: row["wrk_threads"]):
            report_lines.append(
                "| {wrk_threads} | {connections} | {rps} | {avg} | {p99} | {errors} |".format(
                    wrk_threads=item["wrk_threads"],
                    connections=item["connections"],
                    rps=fmt_num(item["requests_per_sec"]),
                    avg=fmt_ms(item["avg_latency_ms"]),
                    p99=fmt_ms(item["p99_ms"]),
                    errors=item["socket_errors"],
                )
            )
        report_lines.append("")

    wrk_threads_list = sorted({item["wrk_threads"] for item in rows})
    report_lines.extend(
        [
            "## Pivot",
            "",
            "| wrk Threads | " + " | ".join(f"workers={workers}" for workers in workers_list) + " |",
            "| ---: | " + " | ".join("---:" for _ in workers_list) + " |",
        ]
    )
    for wrk_threads in wrk_threads_list:
        cells = []
        for workers in workers_list:
            match = next(
                (
                    item
                    for item in rows
                    if item["workers"] == workers and item["wrk_threads"] == wrk_threads
                ),
                None,
            )
            cells.append(fmt_num(match["requests_per_sec"]) if match else "-")
        report_lines.append(
            "| {wrk_threads} | {cells} |".format(
                wrk_threads=wrk_threads,
                cells=" | ".join(cells),
            )
        )

    report_lines.extend(
        [
            "",
            "## Notes",
            "",
            "- This matrix sweeps server `worker_count` against client-side `wrk -t` to separate server scaling from load-generator saturation.",
            "- Connections scale with `wrk_threads * CONNECTIONS_PER_WRK_THREAD`, floored by `MIN_CONNECTIONS`.",
            "- Compare zero-error rows first; very high-throughput rows with timeouts are useful as limits, not defaults.",
        ]
    )

    (results_dir / "report.md").write_text("\n".join(report_lines) + "\n")


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(f"usage: {argv[0]} <results-dir>", file=sys.stderr)
        return 1

    generate_report(Path(argv[1]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
