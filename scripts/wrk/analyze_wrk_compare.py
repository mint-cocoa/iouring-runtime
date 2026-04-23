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


def scenario_sort_key(name: str) -> tuple[int, str]:
    order = {
        "steady_root": 0,
        "mixed_routes": 1,
        "saturation_root": 2,
    }
    return (order.get(name, math.inf), name)


def generate_report(results_dir: Path) -> None:
    rows = []
    for server_dir in sorted(path for path in results_dir.iterdir() if path.is_dir()):
        server_meta = server_dir / "server.meta.json"
        if not server_meta.exists():
            continue
        server = load_json(server_meta)
        server["server_description"] = server.pop("description", "")
        for meta_path in sorted(server_dir.glob("*.meta.json")):
            if meta_path.name == "server.meta.json":
                continue
            meta = load_json(meta_path)
            meta["workload_description"] = meta.pop("description", "")
            parsed = parse_wrk_output(server_dir / meta["raw_output"])
            rows.append({**server, **meta, **parsed})

    rows.sort(key=lambda item: (scenario_sort_key(item["name"]), item["server"]))

    csv_path = results_dir / "summary.csv"
    with csv_path.open("w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(
            [
                "scenario",
                "server",
                "kind",
                "image",
                "threads",
                "connections",
                "duration",
                "path",
                "requests_per_sec",
                "avg_latency_ms",
                "p50_ms",
                "p90_ms",
                "p99_ms",
                "requests_total",
                "socket_errors",
                "timeout_errors",
                "transfer_per_sec",
                "server_description",
                "workload_description",
            ]
        )
        for item in rows:
            writer.writerow(
                [
                    item["name"],
                    item["server"],
                    item["kind"],
                    item.get("image", ""),
                    item["threads"],
                    item["connections"],
                    item["duration"],
                    item["path"],
                    item["requests_per_sec"],
                    item["avg_latency_ms"],
                    item["p50_ms"],
                    item["p90_ms"],
                    item["p99_ms"],
                    item["requests_total"],
                    item["socket_errors"],
                    item["timeout_errors"],
                    item["transfer_per_sec"],
                    item.get("server_description", ""),
                    item.get("workload_description", ""),
                ]
            )

    scenario_names = sorted({item["name"] for item in rows}, key=scenario_sort_key)
    report_lines = [
        "# wrk Web Server Comparison Report",
        "",
        "## Summary",
        "",
        f"- Servers tested: {len({item['server'] for item in rows})}",
        f"- Scenarios per server: {len(scenario_names)}",
        f"- Total benchmark rows: {len(rows)}",
        "",
    ]

    for scenario_name in scenario_names:
        scenario_rows = [item for item in rows if item["name"] == scenario_name]
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
                f"- Best throughput: `{best_rps['server']}` at `{fmt_num(best_rps['requests_per_sec'])}` req/s",
                f"- Best p99 latency: `{best_p99['server']}` at `{fmt_ms(best_p99['p99_ms'])}`",
                (
                    f"- Fastest zero-error server: `{clean_best['server']}` at "
                    f"`{fmt_num(clean_best['requests_per_sec'])}` req/s"
                    if clean_best
                    else "- Fastest zero-error server: none"
                ),
                "",
                "| Server | Kind | Req/s | Avg | p99 | Errors |",
                "| --- | --- | ---: | ---: | ---: | ---: |",
            ]
        )
        for item in sorted(
            scenario_rows,
            key=lambda entry: entry["requests_per_sec"] or -math.inf,
            reverse=True,
        ):
            report_lines.append(
                "| {server} | {kind} | {rps} | {avg} | {p99} | {errors} |".format(
                    server=item["server"],
                    kind=item["kind"],
                    rps=fmt_num(item["requests_per_sec"]),
                    avg=fmt_ms(item["avg_latency_ms"]),
                    p99=fmt_ms(item["p99_ms"]),
                    errors=item["socket_errors"],
                )
            )
        report_lines.append("")

    report_lines.extend(
        [
            "## Notes",
            "",
            "- `mixed_routes` alternates between `/` and `/health` using the shared Lua script.",
            "- Container-based servers are run from official Docker images with minimal route-only configs.",
            "- Compare zero-error rows first, then use peak throughput as a secondary signal.",
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
