#!/usr/bin/env python3
"""Run an isolated clinic baseline, or summarize an existing bbhost log.

The 300-count windows are SubmitAndFlip calls, not presentation timestamps.
This tool deliberately does not report frame-time percentiles from them.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import selectors
import shutil
import signal
import subprocess
import time


ROOT = Path(__file__).resolve().parents[1]
AUTOPRESS = ",".join(f"{t}:circle" for t in range(10, 39, 4))
FLIP = re.compile(r"sceGnmSubmitAndFlip #(\d+).*\| ([\d.]+) s: (.*)")
PAIR = re.compile(r"([\w+-]+)=(\d+)")
PROFILE = re.compile(r"([\w+]+)=(\d+)ms(?:\((\d+)\))?")
FAILURE = re.compile(
    r"dumped core|SIGSEGV|SIGABRT|device lost|device loss|VK_ERROR_DEVICE_LOST|"
    r"HLE stub #|descriptor set allocation failed|wait timed out|"
    r"wait 0x[0-9a-f]+ gave up [1-9]\d* time|"
    r"startup: skip intro refused|cannot restore logo page|"
    r"capture: .*(abandoned|failed)", re.IGNORECASE
)
CAPTURE = re.compile(r"capture: wrote (\S+)/manifest\.json")


def digest(path):
    with path.open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def file_inventory(directory):
    files = {}
    for path in sorted(directory.rglob("*")):
        if path.is_symlink():
            raise ValueError(f"fixture must not contain symlinks: {path}")
        if path.is_file():
            files[str(path.relative_to(directory))] = {
                "bytes": path.stat().st_size, "sha256": digest(path)
            }
    return files


class LogSummary:
    def __init__(self):
        self.windows = []
        self.failures = []
        self.identity = {}
        self.pilot_inventory = False
        self.captures = []

    def feed(self, line):
        if match := FLIP.search(line):
            end = int(match[1])
            previous = self.windows[-1]["end_submit"] if self.windows else None
            seconds = float(match[2])
            self.windows.append({
                "start_submit": previous, "end_submit": end, "wall_s": seconds,
                "submit_fps": (end - previous) / seconds if previous is not None and seconds else None,
                "counters": {key: int(value) for key, value in PAIR.findall(match[3])},
                "complete": False,
            })
        elif "gpu profile:" in line and self.windows:
            self.windows[-1]["gpu"] = {
                key: {"ms": int(ms), "count": int(count) if count else None}
                for key, ms, count in PROFILE.findall(line)
            }
        elif "cp-thread ms:" in line and self.windows:
            self.windows[-1]["cp_ms"] = {key: int(value) for key, value in PAIR.findall(line)}
        elif "  gnm ops:" in line and self.windows:
            # PM4 packets the command processors executed in the window, by opcode.
            self.windows[-1]["gnm_ops"] = {key: int(value) for key, value in PAIR.findall(line)}
        elif "  fill:" in line and self.windows:
            self.windows[-1]["complete"] = True
        if FAILURE.search(line):
            self.failures.append(line.strip())
        if "render: detail for 8747a367+a22c7f71:" in line:
            self.pilot_inventory = True
        if match := CAPTURE.search(line):
            self.captures.append(match[1])
        if "eboot sha256 " in line:
            self.identity["eboot_sha256"] = line.strip().split()[-1]
        if "[bbhost] gpu:" in line and "Vulkan" in line:
            self.identity["gpu"] = line.strip()

    def result(self, start, end):
        selected = [w for w in self.windows if w["start_submit"] is not None
                    and w["start_submit"] >= start and w["end_submit"] <= end]
        expected = list(range(start + 300, end + 1, 300))
        complete = [w["end_submit"] for w in selected if w["complete"]
                    and "gpu" in w and w["end_submit"] - w["start_submit"] == 300
                    and w["counters"].get("draws", 0) > 0]
        return {
            "schema": 1, "identity": self.identity, "windows": selected,
            "complete_windows": complete == expected,
            "failures": self.failures,
            "pilot_inventory": self.pilot_inventory,
            "captures": self.captures,
            "notes": ["Windows count submitted flips; GPU timestamps cover retired query batches and may straddle boundaries.",
                      "CP phase timers overlap; do not sum them as independent CPU work.",
                      "Graphics timestamps include VS/PS/rasterization; compute excludes native copies/clears.",
                      "No per-frame percentiles or visual correctness claim can be derived from this log."],
        }


def command_output(argv):
    try:
        result = subprocess.run(argv, cwd=ROOT, capture_output=True, text=True, timeout=10)
        return {"returncode": result.returncode, "stdout": result.stdout.strip()}
    except (OSError, subprocess.TimeoutExpired) as exc:
        return {"error": str(exc)}


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n")


def run(args):
    out = args.out.resolve()
    if not any(out.is_relative_to(ROOT / name) for name in ("tmp", "build")):
        raise ValueError("--out must be a new directory inside this project's tmp/ or build/")
    seed, binary, config = args.seed.resolve(), args.binary.resolve(), args.config.resolve()
    if out.is_relative_to(seed):
        raise ValueError("--out must not be inside the seed fixture")
    if not seed.is_dir() or not binary.is_file() or not config.is_file():
        raise ValueError("seed directory, binary and config must exist")
    seed_files = file_inventory(seed)
    out.mkdir(parents=True, exist_ok=False)
    shutil.copytree(seed, out / "data")
    env = {k: v for k, v in os.environ.items() if not k.startswith(("BBHOST_", "GCN2SPV_"))}
    settings = {"BBHOST_HEADLESS": "0" if args.windowed else "1", "BBHOST_NP_SIGNED_OUT": "1",
                "BBHOST_GPU_PROFILE": "1", "BBHOST_AUTOPRESS": args.autopress,
                "BBHOST_SKIP_INTRO": "1" if args.skip_intro else "0",
                "BBHOST_PIPELINE_CACHE": "1" if args.pipeline_cache else "0"}
    if args.decomp:
        settings["BBHOST_DECOMP"] = args.decomp
    if args.present_delay_us is not None:
        settings["BBHOST_PRESENT_DELAY_US"] = str(args.present_delay_us)
    if args.dump_frame is not None:
        settings["BBHOST_DUMP_FRAME"] = str(args.dump_frame)
    for item in args.env:
        name, sep, value = item.partition("=")
        if not sep or not name.startswith("BBHOST_") or name in settings:
            raise ValueError(f"--env takes BBHOST_NAME=VALUE and must not override the run's own settings: {item}")
        settings[name] = value
    if args.mode == "magenta":
        settings["BBHOST_DEBUG_PS_COLOR"] = "1"
    elif args.mode == "inventory":
        settings["BBHOST_TRACE_DRAW"] = "8747a367+a22c7f71"
        settings["BBHOST_TRACE_MIN_FLIP"] = str(args.start)
    elif args.mode == "capture":
        # A draw capture for tools/drawreplay; it settles and flushes the GPU.
        settings["BBHOST_CAPTURE_DRAW"] = args.capture_draw
        settings["BBHOST_CAPTURE_MIN_FLIP"] = str(args.start)
        settings["BBHOST_CAPTURE_DIR"] = str(out / "captures")
        settings["BBHOST_CAPTURE_COUNT"] = str(args.capture_count)
        settings["BBHOST_CAPTURE_EVERY"] = str(args.capture_every)
    env.update(settings)
    argv = launch_argv([str(binary), "--config", str(config), "--data", str(out / "data")], args.numa_node)
    manifest = {
        "schema": 1, "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "binary": str(binary), "binary_sha256": digest(binary),
        "config": str(config), "config_sha256": digest(config),
        "seed": str(seed), "seed_files": seed_files,
        "command": argv, "cwd": str(ROOT), "settings": settings,
        "graphics_environment": {k: v for k, v in env.items()
                                 if k.startswith(("VK_", "__GL_")) or k in ("DISPLAY", "WAYLAND_DISPLAY")},
        "platform": platform.platform(), "cpu": command_output(["lscpu", "-J"]),
        "gpu_driver": command_output(["nvidia-smi", "--query-gpu=name,driver_version,pci.bus_id", "--format=csv,noheader"]),
        "git_head": command_output(["git", "rev-parse", "HEAD"]),
        "git_status": command_output(["git", "status", "--short"]),
        "runner_sha256": digest(Path(__file__)),
        "start_submit": args.start, "end_submit": args.end, "mode": args.mode, "numa_node": args.numa_node,
    }
    write_json(out / "manifest.json", manifest)
    # Keep the exact tracked-source changes with the binary hash; no game data
    # or config contents are embedded in the manifest.
    diff = command_output(["git", "diff", "--binary", "HEAD"])
    (out / "source.patch").write_text(diff.get("stdout", "") + "\n")
    summary = LogSummary()
    started = time.monotonic()
    stop_reason = None
    termination_started = None
    pending = b""
    launched = time.time()  # a frame dump older than this belongs to another run
    with (out / "run.log").open("wb") as log:
        with subprocess.Popen(argv, cwd=ROOT, env=env, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, start_new_session=True) as process:
            selector = selectors.DefaultSelector()
            selector.register(process.stdout, selectors.EVENT_READ)
            try:
                while selector.get_map():
                    now = time.monotonic()
                    if termination_started is None and now - started >= args.timeout:
                        stop_reason = "timeout_before_end_window"
                    if stop_reason and termination_started is None:
                        process.send_signal(signal.SIGTERM)
                        termination_started = now
                    if termination_started is not None and now - termination_started > 10 and process.poll() is None:
                        process.kill()
                    for key, _ in selector.select(timeout=0.2):
                        chunk = os.read(key.fd, 65536)
                        if not chunk:
                            selector.unregister(key.fileobj)
                            continue
                        log.write(chunk)
                        log.flush()
                        pending += chunk
                        while b"\n" in pending:
                            line, pending = pending.split(b"\n", 1)
                            text = line.decode(errors="replace")
                            summary.feed(text)
                            if "  fill:" in text and summary.windows:
                                window = summary.windows[-1]
                                if window["end_submit"] >= 300:
                                    print(f"submit {window['end_submit']}: {window['wall_s']:.1f} s", flush=True)
                                if window["end_submit"] >= args.end and stop_reason is None:
                                    stop_reason = "end_window_reached"
                            if (args.mode == "capture" and stop_reason is None
                                    and len(summary.captures) >= args.capture_count):
                                stop_reason = "captures_written"
                if pending:
                    summary.feed(pending.decode(errors="replace"))
                returncode = process.wait(timeout=10)
            except BaseException:
                process.kill()
                process.wait()
                raise
            finally:
                selector.close()
    report = summary.result(args.start, args.end)
    report.update({"returncode": returncode, "stop_reason": stop_reason or "process_exited",
                   "elapsed_s": time.monotonic() - started})
    report["seed_unchanged"] = file_inventory(seed) == seed_files
    report["binary_unchanged"] = digest(binary) == manifest["binary_sha256"]
    report["config_unchanged"] = digest(config) == manifest["config_sha256"]
    if args.dump_frame is not None:
        # The host writes BBHOST_DUMP_FRAME to build/frame-N.ppm under its cwd; keep it with the run.
        dumped = ROOT / "build" / f"frame-{args.dump_frame}.ppm"
        report["frame_dump"] = None
        if dumped.is_file() and dumped.stat().st_mtime >= launched:
            kept = out / dumped.name
            shutil.move(str(dumped), kept)
            report["frame_dump"] = str(kept)
    if args.mode == "capture":
        # A capture run stops once its captures are written; windows are not its evidence.
        goal = (len(report["captures"]) >= args.capture_count and stop_reason in ("captures_written", "end_window_reached")
                and all((Path(c) if Path(c).is_absolute() else ROOT / c).joinpath("manifest.json").is_file()
                        for c in report["captures"]))
    else:
        goal = (report["complete_windows"] and stop_reason == "end_window_reached"
                and (args.mode != "inventory" or report["pilot_inventory"]))
    report["valid_run"] = (goal and not report["failures"]
                                  and returncode in (0, -signal.SIGTERM, 128 + signal.SIGTERM)
                                  and report["seed_unchanged"] and report["binary_unchanged"]
                                  and report["config_unchanged"])
    report["valid_timing_run"] = report["valid_run"] and args.mode not in ("inventory", "capture")
    report["mode"] = args.mode
    write_json(out / "summary.json", report)
    print(f"{out / 'summary.json'}: {'valid' if report['valid_run'] else 'FAILED'} ({args.mode})", flush=True)
    return 0 if report["valid_run"] else 1


def launch_argv(argv, numa_node):
    """The command that starts bbhost: under numactl, bound to one node's CPUs and memory, when numa_node is set.

    On a two-socket host the scheduler otherwise moves the command processor's
    threads between nodes, and CPU-bound timings vary by 10-20% from run to run.
    """
    if numa_node is None:
        return list(argv)
    return ["numactl", f"--cpunodebind={numa_node}", f"--membind={numa_node}", *argv]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--summarize", type=Path, help="read an existing log without launching the game")
    parser.add_argument("--out", type=Path, help="new output directory under tmp/ or build/")
    parser.add_argument("--seed", type=Path, default=ROOT / "data")
    parser.add_argument("--binary", type=Path, default=ROOT / "build/bbhost")
    parser.add_argument("--config", type=Path, default=ROOT / "bbhost.toml")
    parser.add_argument("--mode", choices=("translated", "magenta", "inventory", "capture"), default="translated")
    parser.add_argument("--capture-draw", default="8747a367+a22c7f71",
                        help="capture mode: BBHOST_CAPTURE_DRAW pipeline prefix (first match from --start), "
                             "or '*' for the first draw of each pipeline with a ready no-fallback variant (tools/lift_verify.py)")
    parser.add_argument("--decomp", help="BBHOST_DECOMP for the run: 1 (the pilot shader), all, or PS hashes")
    parser.add_argument("--dump-frame", type=int, help="BBHOST_DUMP_FRAME: keep the displayed frame at this flip as frame-N.ppm")
    parser.add_argument("--env", action="append", default=[], metavar="BBHOST_NAME=VALUE",
                        help="extra BBHOST_* setting for the run (repeatable), e.g. BBHOST_ARENA_MONITOR=1")
    parser.add_argument("--capture-count", type=int, default=1, help="capture mode: draws to capture")
    parser.add_argument("--capture-every", type=int, default=1, help="capture mode: one of every n matching draws")
    parser.add_argument("--skip-intro", action=argparse.BooleanOptionalAction, default=True,
                        help="skip company logos (default); use --no-skip-intro to reproduce older runs")
    parser.add_argument("--pipeline-cache", action=argparse.BooleanOptionalAction, default=False,
                        help="let bbhost load and save its Vulkan pipeline cache in the run's data (off by default, so loads stay comparable)")
    parser.add_argument("--windowed", action="store_true",
                        help="open the window and present (BBHOST_HEADLESS=0); needs DISPLAY, e.g. an Xvfb server")
    parser.add_argument("--present-delay-us", type=int,
                        help="BBHOST_PRESENT_DELAY_US: how long each present pretends the display blocks (Xvfb never does)")
    parser.add_argument("--autopress", default=AUTOPRESS,
                        help="BBHOST_AUTOPRESS taps (default: circle every 4 s from 10 s). 'f329:circle,...' taps at flips, "
                             "so configurations whose loads take different wall time press at the same point in the game")
    parser.add_argument("--start", type=int, default=1500)
    parser.add_argument("--end", type=int, default=2400)
    parser.add_argument("--timeout", type=float, default=300)
    parser.add_argument("--numa-node", type=int,
                        help="run bbhost under numactl on this NUMA node's CPUs and memory (use the GPU's node for steadier timings)")
    args = parser.parse_args()
    if args.start < 300 or args.end <= args.start or args.start % 300 or args.end % 300 or args.timeout <= 0:
        parser.error("start/end must be increasing multiples of 300 (start >= 300), timeout positive")
    if args.numa_node is not None and args.numa_node < 0:
        parser.error("--numa-node must be a node number")
    if args.summarize:
        summary = LogSummary()
        with args.summarize.open(errors="replace") as log:
            for line in log:
                summary.feed(line)
        print(json.dumps(summary.result(args.start, args.end), indent=2))
        return 0
    if args.out is None:
        parser.error("--out is required for a run")
    try:
        return run(args)
    except (ValueError, OSError) as exc:
        parser.exit(1, f"{exc}\n")


if __name__ == "__main__":
    raise SystemExit(main())
