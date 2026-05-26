#!/usr/bin/env python3

import argparse
import csv
import json
import os
import statistics
import subprocess
import time
from pathlib import Path


def sudo_prefix(args):
    if args.no_sudo or os.geteuid() == 0:
        return []
    return ["sudo"]


def run_cmd(cmd, *, check=True):
    print("+ " + " ".join(str(x) for x in cmd), flush=True)
    proc = subprocess.run(cmd, text=True)
    if check and proc.returncode != 0:
        raise subprocess.CalledProcessError(proc.returncode, cmd)
    return proc.returncode


def parse_list(value, cast=str):
    return [cast(x.strip()) for x in value.split(",") if x.strip()]


def reset_zone(args, dev, slba):
    run_cmd(sudo_prefix(args) + ["nvme", "zns", "reset-zone", dev, f"--start-lba={slba}"])


def prefill(args, dev, label):
    prefix = sudo_prefix(args)
    if args.reset:
        reset_zone(args, dev, args.start_lba)

    cmd = prefix + [
        "fio",
        f"--name=prefill_{label}",
        f"--filename={dev}",
        f"--ioengine={args.ioengine}",
        "--direct=1",
        "--rw=write",
        f"--bs={args.prefill_bs}",
        "--iodepth=1",
        "--numjobs=1",
        f"--offset={args.offset}",
        f"--size={args.size}",
        "--output-format=json",
        f"--output={args.out_dir}/prefill_{label}.json",
    ]
    if args.cpus_allowed:
        cmd.append(f"--cpus_allowed={args.cpus_allowed}")
    run_cmd(cmd)


def is_write_workload(rw):
    return rw == "write"


def fio_cmd(args, case, dev, bs, iodepth, rw, out_path):
    cmd = sudo_prefix(args) + [
        "fio",
        f"--name={case}",
        f"--filename={dev}",
        f"--ioengine={args.ioengine}",
        "--direct=1",
        f"--rw={rw}",
        f"--bs={bs}",
        f"--iodepth={iodepth}",
        "--numjobs=1",
        f"--offset={args.offset}",
        f"--size={args.size}",
        "--percentile_list=50:90:99:99.9",
        "--output-format=json",
        f"--output={out_path}",
    ]
    if not is_write_workload(rw):
        cmd.extend(
            [
                "--time_based=1",
                f"--runtime={args.runtime}",
                f"--ramp_time={args.ramp_time}",
            ]
        )
    if args.cpus_allowed:
        cmd.append(f"--cpus_allowed={args.cpus_allowed}")
    return cmd


def extract_result(path, media, case, run, bs, iodepth, rw):
    with open(path) as f:
        data = json.load(f)

    job = data["jobs"][0]
    direction = "write" if is_write_workload(rw) else "read"
    section = job[direction]
    clat = section.get("clat_ns", {})
    pct = clat.get("percentile", {})
    bw_bytes = section.get("bw_bytes", 0)

    return {
        "media": media,
        "case": case,
        "run": run,
        "direction": direction,
        "rw": rw,
        "bs": bs,
        "iodepth": iodepth,
        "error": job.get("error", 0),
        "io_bytes": section.get("io_bytes", 0),
        "bw_bytes": bw_bytes,
        "bw_mib_s": bw_bytes / (1024 * 1024),
        "iops": section.get("iops", 0),
        "lat_mean_ns": clat.get("mean"),
        "lat_p50_ns": pct.get("50.000000"),
        "lat_p90_ns": pct.get("90.000000"),
        "lat_p99_ns": pct.get("99.000000"),
        "lat_p999_ns": pct.get("99.900000"),
        "runtime_ms": job.get("job_runtime", 0),
    }


def write_csv(path, rows):
    fields = [
        "media",
        "case",
        "run",
        "direction",
        "rw",
        "bs",
        "iodepth",
        "error",
        "io_bytes",
        "bw_bytes",
        "bw_mib_s",
        "iops",
        "lat_mean_ns",
        "lat_p50_ns",
        "lat_p90_ns",
        "lat_p99_ns",
        "lat_p999_ns",
        "runtime_ms",
    ]
    with open(path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def summarize(rows):
    summary = []
    keys = sorted({(r["rw"], r["bs"], r["iodepth"]) for r in rows})
    for rw, bs, iodepth in keys:
        item = {"rw": rw, "bs": bs, "iodepth": iodepth}
        for media in ("slc", "tlc"):
            valid = [
                r
                for r in rows
                if r["media"] == media
                and r["rw"] == rw
                and r["bs"] == bs
                and r["iodepth"] == iodepth
                and r["error"] == 0
                and r["io_bytes"] > 0
            ]
            for metric in ("bw_mib_s", "lat_p50_ns", "lat_p99_ns"):
                vals = [r[metric] for r in valid if r[metric] is not None]
                item[f"{media}_{metric}_median"] = statistics.median(vals) if vals else None
        slc_bw = item.get("slc_bw_mib_s_median")
        tlc_bw = item.get("tlc_bw_mib_s_median")
        slc_p50 = item.get("slc_lat_p50_ns_median")
        tlc_p50 = item.get("tlc_lat_p50_ns_median")
        item["slc_over_tlc_bw"] = slc_bw / tlc_bw if slc_bw and tlc_bw else None
        item["tlc_over_slc_p50"] = tlc_p50 / slc_p50 if slc_p50 and tlc_p50 else None
        summary.append(item)
    return summary


def print_summary(summary):
    print("\nSummary")
    print("rw bs iodepth slc_bw tlc_bw bw_ratio slc_p50_us tlc_p50_us lat_ratio")
    for row in summary:
        slc_bw = row.get("slc_bw_mib_s_median") or 0
        tlc_bw = row.get("tlc_bw_mib_s_median") or 0
        bw_ratio = row.get("slc_over_tlc_bw") or 0
        slc_p50 = (row.get("slc_lat_p50_ns_median") or 0) / 1000
        tlc_p50 = (row.get("tlc_lat_p50_ns_median") or 0) / 1000
        lat_ratio = row.get("tlc_over_slc_p50") or 0
        print(
            f"{row['rw']} {row['bs']} {row['iodepth']} "
            f"{slc_bw:.2f} {tlc_bw:.2f} {bw_ratio:.2f} "
            f"{slc_p50:.2f} {tlc_p50:.2f} {lat_ratio:.2f}"
        )


def main():
    parser = argparse.ArgumentParser(
        description="Benchmark ConZone-Dual SLC/TLC read/write latency and bandwidth."
    )
    parser.add_argument("--slc-dev", default="/dev/nvme0n1")
    parser.add_argument("--tlc-dev", default="/dev/nvme0n2")
    parser.add_argument("--size", default="128M")
    parser.add_argument("--offset", default="0")
    parser.add_argument("--start-lba", default="0")
    parser.add_argument("--bs-list", default="4k,16k,128k,1m")
    parser.add_argument("--iodepth-list", default="1,4,32")
    parser.add_argument(
        "--rw-list",
        default="read,randread,write",
        help="Comma-separated fio workloads. ZNS-safe values are read, randread, write.",
    )
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--runtime", type=int, default=10)
    parser.add_argument("--ramp-time", type=int, default=2)
    parser.add_argument("--ioengine", default="io_uring")
    parser.add_argument("--cpus-allowed", default="")
    parser.add_argument("--out-dir", default="log/fio_results/slc_tlc_read")
    parser.add_argument("--prepare", action="store_true")
    parser.add_argument("--reset", action="store_true")
    parser.add_argument(
        "--no-write-reset",
        action="store_true",
        help="Do not reset the target zone before each sequential write test.",
    )
    parser.add_argument("--prefill-bs", default="4M")
    parser.add_argument("--no-sudo", action="store_true")
    args = parser.parse_args()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    args.out_dir = str(out_dir)

    bs_list = parse_list(args.bs_list)
    iodepth_list = parse_list(args.iodepth_list, int)
    rw_list = parse_list(args.rw_list)
    unsupported = [rw for rw in rw_list if rw not in ("read", "randread", "write")]
    if unsupported:
        raise SystemExit(
            "Unsupported rw values for this ZNS benchmark: "
            + ",".join(unsupported)
            + ". Use read, randread, or write."
        )

    if args.prepare:
        prefill(args, args.slc_dev, "slc")
        prefill(args, args.tlc_dev, "tlc")

    rows = []
    devices = (("slc", args.slc_dev), ("tlc", args.tlc_dev))
    for run in range(1, args.runs + 1):
        for rw in rw_list:
            for bs in bs_list:
                for iodepth in iodepth_list:
                    for media, dev in devices:
                        case = f"{media}_{rw}_bs{bs}_qd{iodepth}_run{run:02d}"
                        out_path = out_dir / f"{case}.json"
                        if is_write_workload(rw) and not args.no_write_reset:
                            reset_zone(args, dev, args.start_lba)
                        run_cmd(fio_cmd(args, case, dev, bs, iodepth, rw, out_path))
                        result = extract_result(out_path, media, case, run, bs, iodepth, rw)
                        rows.append(result)
                        print(
                            f"{case}: bw={result['bw_mib_s']:.2f} MiB/s "
                            f"p50={result['lat_p50_ns']} ns p99={result['lat_p99_ns']} ns"
                        )
                        time.sleep(0.2)

    csv_path = out_dir / "results.csv"
    summary_path = out_dir / "summary.json"
    write_csv(csv_path, rows)
    summary = summarize(rows)
    with open(summary_path, "w") as f:
        json.dump({"rows": rows, "summary": summary}, f, indent=2)

    print_summary(summary)
    print(f"\nWrote {csv_path}")
    print(f"Wrote {summary_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
