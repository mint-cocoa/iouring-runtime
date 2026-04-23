#!/usr/bin/env python3
from __future__ import annotations

import csv
import json
import math
import re
import sys
from pathlib import Path


LATENCY_RE = re.compile(r"^\s+Latency\s+(\S+)\s+(\S+)\s+(\S+)\s+")
REQSEC_RE = re.compile(r"^\s+Req/Sec\s+(\S+)\s+(\S+)\s+(\S+)\s+")
PCT_RE = re.compile(r"^\s+(50|75|90|99)%\s+(\S+)")
TOTAL_RE = re.compile(r"^\s+([0-9]+) requests in ([0-9.]+)([smh]), (.+) read$")
REQUESTS_PER_SEC_RE = re.compile(r"^Requests/sec:\s+([0-9.]+)$")
TRANSFER_PER_SEC_RE = re.compile(r"^Transfer/sec:\s+(.+)$")
SOCKET_ERRORS_RE = re.compile(
    r"^\s*Socket errors: connect (\d+), read (\d+), write (\d+), timeout (\d+)$"
)

UNIT_SCALE = {
    "us": 0.001,
    "ms": 1.0,
    "s": 1000.0,
    "m": 60_000.0,
    "h": 3_600_000.0,
}

SI_SCALE = {
    "k": 1_000.0,
    "M": 1_000_000.0,
    "G": 1_000_000_000.0,
}


def parse_time_ms(value: str) -> float:
    match = re.fullmatch(r"([0-9.]+)(us|ms|s|m|h)", value)
    if not match:
        raise ValueError(f"unsupported time value: {value}")
    return float(match.group(1)) * UNIT_SCALE[match.group(2)]


def parse_duration_seconds(value: str, unit: str) -> float:
    return parse_time_ms(f"{value}{unit}") / 1000.0


def parse_si(value: str) -> float:
    match = re.fullmatch(r"([0-9.]+)([kMG])?", value)
    if not match:
        raise ValueError(f"unsupported SI value: {value}")
    number = float(match.group(1))
    suffix = match.group(2)
    if suffix:
        number *= SI_SCALE[suffix]
    return number


def load_json(path: Path) -> dict:
    return json.loads(path.read_text())


def parse_wrk_output(path: Path) -> dict:
    text = path.read_text()
    result = {
        "avg_latency_ms": None,
        "stdev_latency_ms": None,
        "max_latency_ms": None,
        "avg_req_per_sec_per_thread": None,
        "requests_per_sec": None,
        "transfer_per_sec": None,
        "requests_total": None,
        "duration_sec": None,
        "bytes_read_human": None,
        "p50_ms": None,
        "p75_ms": None,
        "p90_ms": None,
        "p99_ms": None,
        "socket_errors": 0,
        "timeout_errors": 0,
    }

    for line in text.splitlines():
        if (match := LATENCY_RE.match(line)):
            result["avg_latency_ms"] = parse_time_ms(match.group(1))
            result["stdev_latency_ms"] = parse_time_ms(match.group(2))
            result["max_latency_ms"] = parse_time_ms(match.group(3))
            continue

        if (match := REQSEC_RE.match(line)):
            result["avg_req_per_sec_per_thread"] = parse_si(match.group(1))
            continue

        if (match := PCT_RE.match(line)):
            result[f"p{match.group(1)}_ms"] = parse_time_ms(match.group(2))
            continue

        if (match := TOTAL_RE.match(line)):
            result["requests_total"] = int(match.group(1))
            result["duration_sec"] = parse_duration_seconds(
                match.group(2), match.group(3)
            )
            result["bytes_read_human"] = match.group(4)
            continue

        if (match := REQUESTS_PER_SEC_RE.match(line)):
            result["requests_per_sec"] = float(match.group(1))
            continue

        if (match := TRANSFER_PER_SEC_RE.match(line)):
            result["transfer_per_sec"] = match.group(1).strip()
            continue

        if (match := SOCKET_ERRORS_RE.match(line)):
            connect, read, write, timeout = map(int, match.groups())
            result["socket_errors"] = connect + read + write + timeout
            result["timeout_errors"] = timeout
            continue

    return result


def fmt_ms(value: float | None) -> str:
    if value is None:
        return "-"
    if value < 1.0:
        return f"{value:.3f} ms"
    return f"{value:.2f} ms"


def fmt_num(value: float | int | None) -> str:
    if value is None:
        return "-"
    if isinstance(value, int):
        return str(value)
    return f"{value:,.2f}"


def generate_report(output_dir: Path) -> None:
    scenarios = []
    for meta_path in sorted(output_dir.glob("*.meta.json")):
        meta = load_json(meta_path)
        raw_path = output_dir / meta["raw_output"]
        parsed = parse_wrk_output(raw_path)
        scenarios.append({**meta, **parsed})

    scenarios.sort(key=lambda item: item["name"])

    csv_path = output_dir / "summary.csv"
    with csv_path.open("w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(
            [
                "scenario",
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
                "notes",
            ]
        )
        for item in scenarios:
            writer.writerow(
                [
                    item["name"],
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
                    item.get("description", ""),
                ]
            )

    completed = [
        item for item in scenarios if (item["requests_total"] or 0) > 0
    ] or scenarios

    best_rps = max(completed, key=lambda item: item["requests_per_sec"] or -math.inf)
    best_p99 = min(
        [item for item in completed if item["p99_ms"] is not None],
        key=lambda item: item["p99_ms"],
    )
    total_errors = sum(item["socket_errors"] for item in scenarios)
    incomplete = [
        item["name"] for item in scenarios if (item["requests_total"] or 0) == 0
    ]

    report_lines = [
        "# wrk Benchmark Report",
        "",
        "## Summary",
        "",
        f"- Scenarios: {len(scenarios)}",
        f"- Best throughput: `{best_rps['name']}` at `{fmt_num(best_rps['requests_per_sec'])}` req/s",
        f"- Best p99 latency: `{best_p99['name']}` at `{fmt_ms(best_p99['p99_ms'])}`",
        f"- Total socket errors across suite: `{total_errors}`",
        "",
        "## Scenario Results",
        "",
        "| Scenario | Threads | Conns | Duration | Path | Req/s | Avg | p50 | p90 | p99 | Total Requests | Errors | Transfer/s |",
        "| --- | ---: | ---: | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |",
    ]

    for item in scenarios:
        report_lines.append(
            "| {name} | {threads} | {connections} | {duration} | `{path}` | {rps} | {avg} | {p50} | {p90} | {p99} | {total} | {errors} | {transfer} |".format(
                name=item["name"],
                threads=item["threads"],
                connections=item["connections"],
                duration=item["duration"],
                path=item["path"],
                rps=fmt_num(item["requests_per_sec"]),
                avg=fmt_ms(item["avg_latency_ms"]),
                p50=fmt_ms(item["p50_ms"]),
                p90=fmt_ms(item["p90_ms"]),
                p99=fmt_ms(item["p99_ms"]),
                total=fmt_num(item["requests_total"]),
                errors=item["socket_errors"],
                transfer=item["transfer_per_sec"] or "-",
            )
        )

    report_lines.extend(
        [
            "",
            "## Notes",
            "",
            "- `mixed_routes` alternates between `/` and `/health` using a Lua script.",
            "- `head_health` sends `HEAD /health` requests using a Lua script.",
            "- Summary rankings exclude scenarios that completed with `0` counted requests.",
            "- Raw `wrk` output for each scenario is stored next to this report.",
        ]
    )

    if incomplete:
        report_lines.extend(
            [
                f"- Zero-count scenarios observed: `{', '.join(incomplete)}`.",
            ]
        )

    (output_dir / "report.md").write_text("\n".join(report_lines) + "\n")


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(f"usage: {argv[0]} <results-dir>", file=sys.stderr)
        return 1

    output_dir = Path(argv[1])
    generate_report(output_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
