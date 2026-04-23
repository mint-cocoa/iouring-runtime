#!/usr/bin/env python3
from __future__ import annotations

import csv
import json
import math
import sys
from pathlib import Path

from analyze_wrk_results import fmt_ms, fmt_num, parse_wrk_output


def load_json(path: Path) -> dict:
    return json.loads(path.read_text())


def load_cpu_summary(path: Path | None) -> dict:
    if path is None or not path.exists():
        return {}
    return load_json(path)


def scenario_sort_key(name: str) -> tuple[int, str]:
    order = {
        "steady_root": 0,
        "saturation_root": 1,
    }
    return (order.get(name, math.inf), name)


def generate_report(results_dir: Path) -> None:
    rows = []
    for config_dir in sorted(path for path in results_dir.iterdir() if path.is_dir()):
        config_meta = config_dir / "config.meta.json"
        if not config_meta.exists():
            continue
        config = load_json(config_meta)
        config["config_description"] = config.pop("description", "")
        for meta_path in sorted(config_dir.glob("*.meta.json")):
            if meta_path.name == "config.meta.json":
                continue
            meta = load_json(meta_path)
            meta["workload_description"] = meta.pop("description", "")
            raw_path = config_dir / meta["raw_output"]
            parsed = parse_wrk_output(raw_path)
            cpu_summary = load_cpu_summary(
                config_dir / meta["cpu_summary"] if "cpu_summary" in meta else None
            )
            rows.append({**config, **meta, **parsed, **cpu_summary})

    rows.sort(key=lambda item: (scenario_sort_key(item["name"]), item["config"]))

    csv_path = results_dir / "summary.csv"
    with csv_path.open("w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(
            [
                "scenario",
                "config",
                "workers",
                "ring_queue_depth",
                "ring_buf_count",
                "ring_submit_batch_size",
                "ring_cqe_batch_budget",
                "ring_io_timeout_ms",
                "threads",
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
                "process_cpu_avg_pct",
                "process_cpu_max_pct",
                "process_cpu_avg_cores",
                "process_cpu_max_cores",
                "system_cpu_avg_pct",
                "system_cpu_max_pct",
                "config_description",
                "workload_description",
            ]
        )
        for item in rows:
            writer.writerow(
                [
                    item["name"],
                    item["config"],
                    item["workers"],
                    item["ring_queue_depth"],
                    item["ring_buf_count"],
                    item["ring_submit_batch_size"],
                    item["ring_cqe_batch_budget"],
                    item["ring_io_timeout_ms"],
                    item["threads"],
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
                    item.get("process_cpu_avg_pct"),
                    item.get("process_cpu_max_pct"),
                    item.get("process_cpu_avg_cores"),
                    item.get("process_cpu_max_cores"),
                    item.get("system_cpu_avg_pct"),
                    item.get("system_cpu_max_pct"),
                    item.get("config_description", ""),
                    item.get("workload_description", ""),
                ]
            )

    scenario_names = sorted({item["name"] for item in rows}, key=scenario_sort_key)
    report_lines = [
        "# wrk Config Matrix Report",
        "",
        "## Summary",
        "",
        f"- Configurations tested: {len({item['config'] for item in rows})}",
        f"- Workloads per configuration: {len(scenario_names)}",
        f"- Total benchmark rows: {len(rows)}",
        "",
    ]

    for scenario_name in scenario_names:
        scenario_rows = [
            item for item in rows if item["name"] == scenario_name
        ]
        completed = [
            item for item in scenario_rows if (item["requests_total"] or 0) > 0
        ] or scenario_rows
        best_rps = max(completed, key=lambda item: item["requests_per_sec"] or -math.inf)
        best_p99 = min(
            [item for item in completed if item["p99_ms"] is not None],
            key=lambda item: item["p99_ms"],
        )
        clean_rows = [item for item in completed if item["socket_errors"] == 0]
        clean_best = (
            max(clean_rows, key=lambda item: item["requests_per_sec"] or -math.inf)
            if clean_rows else None
        )

        report_lines.extend(
            [
                f"### {scenario_name}",
                "",
                f"- Best throughput: `{best_rps['config']}` at `{fmt_num(best_rps['requests_per_sec'])}` req/s",
                f"- Best p99 latency: `{best_p99['config']}` at `{fmt_ms(best_p99['p99_ms'])}`",
                (
                    f"- Fastest zero-error config: `{clean_best['config']}` at "
                    f"`{fmt_num(clean_best['requests_per_sec'])}` req/s"
                    if clean_best
                    else "- Fastest zero-error config: none"
                ),
                "",
                "| Config | Workers | QueueDepth | BufCount | SubmitBatch | CQEBudget | IoTimeoutMs | Req/s | Avg | p99 | Errors | ProcCPU Avg | ProcCPU Max | SysCPU Avg |",
                "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
            ]
        )
        for item in sorted(
            scenario_rows,
            key=lambda entry: entry["requests_per_sec"] or -math.inf,
            reverse=True,
        ):
            report_lines.append(
                "| {config} | {workers} | {qd} | {buf_count} | {submit} | {cqe} | {io_timeout} | {rps} | {avg} | {p99} | {errors} | {proc_avg} | {proc_max} | {sys_avg} |".format(
                    config=item["config"],
                    workers=item["workers"],
                    qd=item["ring_queue_depth"],
                    buf_count=item["ring_buf_count"],
                    submit=item["ring_submit_batch_size"],
                    cqe=item["ring_cqe_batch_budget"],
                    io_timeout=item["ring_io_timeout_ms"],
                    rps=fmt_num(item["requests_per_sec"]),
                    avg=fmt_ms(item["avg_latency_ms"]),
                    p99=fmt_ms(item["p99_ms"]),
                    errors=item["socket_errors"],
                    proc_avg=fmt_num(item.get("process_cpu_avg_pct")),
                    proc_max=fmt_num(item.get("process_cpu_max_pct")),
                    sys_avg=fmt_num(item.get("system_cpu_avg_pct")),
                )
            )
        report_lines.append("")

    report_lines.extend(
        [
            "## Notes",
            "",
            "- The matrix focuses on settings that affect this tiny in-memory GET workload: workers, queue depth, buffer count, batching, and `io_timeout`.",
            "- Timeouts are reported exactly as `wrk` printed them; non-zero errors are worth retesting because they can be sensitive to local machine scheduling.",
            "- Per-config raw logs and server logs are stored in each configuration subdirectory.",
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
