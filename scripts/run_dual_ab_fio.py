#!/usr/bin/env python3

import argparse
import csv
import json
import os
import statistics
import subprocess
import sys
import time
from pathlib import Path


def sudo_prefix(args):
    if args.no_sudo or os.geteuid() == 0:
        return []
    return ["sudo"]


def run_cmd(cmd, *, check=True):
    print("+ " + " ".join(cmd), flush=True)
    proc = subprocess.run(cmd, text=True)
    if check and proc.returncode != 0:
        raise subprocess.CalledProcessError(proc.returncode, cmd)
    return proc.returncode


def reset_and_prefill(args):
    prefix = sudo_prefix(args)
    for dev, count, label in (
        (args.slc_dev, args.slc_prefill_count, "SLC"),
        (args.tlc_dev, args.tlc_prefill_count, "TLC"),
    ):
        print(f"\nPreparing {label}: reset zone 0 and prefill {count} * {args.prefill_bs}")
        run_cmd(prefix + ["nvme", "zns", "reset-zone", dev, "--start-lba=0"])
        run_cmd(
            prefix
            + [
                "dd",
                "if=/dev/zero",
                f"of={dev}",
                f"bs={args.prefill_bs}",
                f"count={count}",
                "oflag=direct",
                "status=progress",
            ]
        )


def fio_cmd(args, case_name, dev, size, out_path):
    cmd = sudo_prefix(args) + [
        "fio",
        f"--name={case_name}",
        f"--filename={dev}",
        f"--ioengine={args.ioengine}",
        "--direct=1",
        "--rw=read",
        f"--bs={args.bs}",
        f"--iodepth={args.iodepth}",
        "--numjobs=1",
        "--offset=0",
        f"--size={size}",
        "--time_based=1",
        f"--runtime={args.runtime}",
        f"--ramp_time={args.ramp_time}",
        "--percentile_list=50:99",
        "--output-format=json",
        f"--output={out_path}",
    ]
    if args.cpus_allowed:
        cmd.append(f"--cpus_allowed={args.cpus_allowed}")
    return cmd


def extract_result(path, case, iteration, device):
    with open(path) as f:
        data = json.load(f)

    job = data["jobs"][0]
    read = job["read"]
    percentiles = read.get("clat_ns", {}).get("percentile", {})
    bw_bytes = read.get("bw_bytes", 0)

    return {
        "case": case,
        "iteration": iteration,
        "jobname": job.get("jobname", case),
        "device": device,
        "error": job.get("error", 0),
        "read_io_bytes": read.get("io_bytes", 0),
        "read_bw_bytes": bw_bytes,
        "read_bw_mib_s": bw_bytes / (1024 * 1024),
        "read_p50_ns": percentiles.get("50.000000"),
        "read_p99_ns": percentiles.get("99.000000"),
        "job_runtime_ms": job.get("job_runtime", 0),
    }


def summarize(rows):
    summary = {}
    for case in sorted({row["case"] for row in rows}):
        valid = [
            row
            for row in rows
            if row["case"] == case
            and row["error"] == 0
            and row["read_io_bytes"] > 0
            and row["read_p50_ns"] is not None
            and row["read_p99_ns"] is not None
        ]
        summary[case] = {
            "runs": len([row for row in rows if row["case"] == case]),
            "valid_runs": len(valid),
        }
        for key in ("read_bw_mib_s", "read_p50_ns", "read_p99_ns"):
            values = [row[key] for row in valid]
            if not values:
                summary[case][key] = None
                continue
            summary[case][key] = {
                "mean": statistics.mean(values),
                "median": statistics.median(values),
                "min": min(values),
                "max": max(values),
                "stdev": statistics.stdev(values) if len(values) > 1 else 0.0,
            }
    return summary


def write_csv(path, rows):
    fields = [
        "case",
        "iteration",
        "jobname",
        "device",
        "error",
        "read_io_bytes",
        "read_bw_bytes",
        "read_bw_mib_s",
        "read_p50_ns",
        "read_p99_ns",
        "job_runtime_ms",
    ]
    with open(path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def print_summary(summary):
    print("\nSummary")
    print("case valid/runs bw_mib_s_median p50_us_median p99_us_median")
    for case, stats in summary.items():
        bw = stats["read_bw_mib_s"]
        p50 = stats["read_p50_ns"]
        p99 = stats["read_p99_ns"]
        bw_med = bw["median"] if bw else 0
        p50_us = p50["median"] / 1000 if p50 else 0
        p99_us = p99["median"] / 1000 if p99 else 0
        print(
            f"{case} {stats['valid_runs']}/{stats['runs']} "
            f"{bw_med:.2f} {p50_us:.2f} {p99_us:.2f}"
        )


def main():
    parser = argparse.ArgumentParser(
        description="Run repeated ConZone Dual A/B fio read baselines."
    )
    parser.add_argument("--slc-dev", default="/dev/nvme0n1")
    parser.add_argument("--tlc-dev", default="/dev/nvme0n2")
    parser.add_argument("--slc-size", default="176M")
    parser.add_argument("--tlc-size", default="528M")
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--runtime", type=int, default=30)
    parser.add_argument("--ramp-time", type=int, default=5)
    parser.add_argument("--bs", default="4k")
    parser.add_argument("--iodepth", type=int, default=32)
    parser.add_argument("--ioengine", default="io_uring")
    parser.add_argument("--cpus-allowed", default="")
    parser.add_argument("--out-dir", default="log/fio_results/conzone_dual_ab")
    parser.add_argument("--prepare", action="store_true")
    parser.add_argument("--prefill-bs", default="4M")
    parser.add_argument("--slc-prefill-count", type=int, default=44)
    parser.add_argument("--tlc-prefill-count", type=int, default=132)
    parser.add_argument("--no-sudo", action="store_true")
    args = parser.parse_args()

    if args.runs < 1:
        print("--runs must be >= 1", file=sys.stderr)
        return 2

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    if args.prepare:
        reset_and_prefill(args)

    rows = []
    cases = (
        ("A_slc_read", args.slc_dev, args.slc_size),
        ("B_tlc_read", args.tlc_dev, args.tlc_size),
    )

    for iteration in range(1, args.runs + 1):
        print(f"\n=== iteration {iteration}/{args.runs} ===")
        for case, dev, size in cases:
            out_path = out_dir / f"{case}_run{iteration:02d}.json"
            run_cmd(fio_cmd(args, case, dev, size, out_path))
            result = extract_result(out_path, case, iteration, dev)
            rows.append(result)
            print(
                f"{case} run {iteration}: "
                f"io={result['read_io_bytes']} "
                f"bw={result['read_bw_mib_s']:.2f} MiB/s "
                f"p50={result['read_p50_ns']} ns "
                f"p99={result['read_p99_ns']} ns"
            )
            time.sleep(1)

    csv_path = out_dir / "summary.csv"
    json_path = out_dir / "summary.json"
    write_csv(csv_path, rows)
    summary = summarize(rows)
    with open(json_path, "w") as f:
        json.dump({"rows": rows, "summary": summary}, f, indent=2)

    print_summary(summary)
    print(f"\nWrote {csv_path}")
    print(f"Wrote {json_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
