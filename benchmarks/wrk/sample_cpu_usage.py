#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import json
import os
import signal
import time
from pathlib import Path

RUNNING = True


def handle_signal(signum, frame):  # type: ignore[no-untyped-def]
    del signum, frame
    global RUNNING
    RUNNING = False


def read_process_ticks(pid: int) -> int | None:
    try:
        fields = Path(f"/proc/{pid}/stat").read_text().split()
    except FileNotFoundError:
        return None
    return int(fields[13]) + int(fields[14])


def read_system_ticks() -> tuple[int, int]:
    first = Path("/proc/stat").read_text().splitlines()[0].split()
    values = [int(value) for value in first[1:]]
    total = sum(values)
    idle = values[3] + (values[4] if len(values) > 4 else 0)
    return total, idle


def write_summary(path: Path, summary: dict) -> None:
    path.write_text(json.dumps(summary, indent=2) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--interval", type=float, default=0.5)
    parser.add_argument("--csv", required=True)
    parser.add_argument("--summary", required=True)
    args = parser.parse_args()

    signal.signal(signal.SIGTERM, handle_signal)
    signal.signal(signal.SIGINT, handle_signal)

    cpu_count = os.cpu_count() or 1
    clk_tck = os.sysconf(os.sysconf_names["SC_CLK_TCK"])

    csv_path = Path(args.csv)
    summary_path = Path(args.summary)
    csv_path.parent.mkdir(parents=True, exist_ok=True)

    with csv_path.open("w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(
            [
                "timestamp",
                "elapsed_sec",
                "process_cpu_percent",
                "process_cpu_cores",
                "system_cpu_percent",
            ]
        )

        started_at = time.time()
        prev_process = read_process_ticks(args.pid)
        if prev_process is None:
            write_summary(
                summary_path,
                {
                    "pid": args.pid,
                    "cpu_count": cpu_count,
                    "interval_sec": args.interval,
                    "samples": 0,
                },
            )
            return 0

        prev_total, prev_idle = read_system_ticks()
        process_cpu = []
        process_cores = []
        system_cpu = []

        while RUNNING:
            time.sleep(args.interval)
            current_process = read_process_ticks(args.pid)
            current_total, current_idle = read_system_ticks()
            if current_process is None:
                break

            total_delta = current_total - prev_total
            idle_delta = current_idle - prev_idle
            process_delta = current_process - prev_process
            if total_delta <= 0:
                prev_process = current_process
                prev_total = current_total
                prev_idle = current_idle
                continue

            process_pct = (process_delta / total_delta) * 100.0 * cpu_count
            busy_pct = (1.0 - (idle_delta / total_delta)) * 100.0
            process_core_count = process_delta / (clk_tck * args.interval)
            elapsed = time.time() - started_at

            process_cpu.append(process_pct)
            process_cores.append(process_core_count)
            system_cpu.append(busy_pct)
            writer.writerow(
                [
                    f"{time.time():.3f}",
                    f"{elapsed:.3f}",
                    f"{process_pct:.3f}",
                    f"{process_core_count:.3f}",
                    f"{busy_pct:.3f}",
                ]
            )
            handle.flush()

            prev_process = current_process
            prev_total = current_total
            prev_idle = current_idle

    summary = {
        "pid": args.pid,
        "cpu_count": cpu_count,
        "interval_sec": args.interval,
        "samples": len(process_cpu),
        "process_cpu_avg_pct": sum(process_cpu) / len(process_cpu)
        if process_cpu
        else 0.0,
        "process_cpu_max_pct": max(process_cpu) if process_cpu else 0.0,
        "process_cpu_avg_cores": sum(process_cores) / len(process_cores)
        if process_cores
        else 0.0,
        "process_cpu_max_cores": max(process_cores) if process_cores else 0.0,
        "system_cpu_avg_pct": sum(system_cpu) / len(system_cpu)
        if system_cpu
        else 0.0,
        "system_cpu_max_pct": max(system_cpu) if system_cpu else 0.0,
    }
    write_summary(summary_path, summary)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
