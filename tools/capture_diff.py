#!/usr/bin/env python3
"""Compare two draw captures (src/host/draw_capture.cpp) of the same draw.

Typical use: the same draw captured from its Gnm packets and from a host-draw
token (BBHOST_GX_NATIVE=2), to show whether the renderer saw the same inputs
and produced the same output.

    tools/capture_diff.py <capture A> <capture B> [--max-lines N]

Differences are grouped by what they mean:
  inputs    shaders, fixed and dynamic state, draw values, stage bindings,
            samplers, image descriptions, blob contents, guest pages both
            captures hold
  outputs   the *-live target planes
  addresses guest or host addresses that differ while the contents they name
            are equal (user-data dwords, buffer and page addresses)
  registers the CP register file, which a draw from a token does not own
Exit status: 0 when inputs and outputs are equal, 1 otherwise, 2 on bad input.
"""
import argparse
import hashlib
import json
import sys
from pathlib import Path

# Manifest fields that name where something lives rather than what it is.
ADDRESS_KEYS = {"guest_va", "guest_base", "index_va", "user_sgpr", "l1_table_replaced_on_replay", "pages"}
# Manifest fields that identify the capture, not the draw.
# The flip counter read at the draw can move by one between runs for the same draw.
IDENTITY_KEYS = {"draw_index", "flip", "rejected_candidates_before", "source"}


def fail(message):
    """Bad input: report and exit 2, apart from captures that differ (1)."""
    print(f"capture_diff: {message}", file=sys.stderr)
    sys.exit(2)


def load(path):
    path = Path(path)
    manifest = path / "manifest.json" if path.is_dir() else path
    try:
        m = json.loads(manifest.read_text())
    except (OSError, ValueError) as e:
        fail(f"cannot read {manifest}: {e}")
    if m.get("schema") != "bbhost-draw-capture":
        fail(f"{manifest} is not a bbhost draw capture")
    return manifest.parent, m


def walk(a, b, path, out):
    """Collect (path, a, b) for every leaf that differs."""
    if isinstance(a, dict) and isinstance(b, dict):
        for k in sorted(set(a) | set(b)):
            walk(a.get(k), b.get(k), f"{path}.{k}" if path else k, out)
    elif isinstance(a, list) and isinstance(b, list) and len(a) == len(b):
        for i, (x, y) in enumerate(zip(a, b)):
            walk(x, y, f"{path}[{i}]", out)
    elif a != b:
        out.append((path, a, b))


def is_address(path):
    return any(part.split("[")[0] in ADDRESS_KEYS for part in path.split("."))


def blob_bytes(root, manifest, name):
    info = manifest.get("blobs", {}).get(name)
    if not info:
        return None
    data = (root / info["file"]).read_bytes()
    if hashlib.sha256(data).hexdigest() != info["sha256"]:
        fail(f"{root / info['file']} does not match its SHA-256")
    return data


def page_map(root, manifest):
    mem = manifest.get("memory") or {}
    size = mem.get("page_bytes", 0)
    pages = mem.get("pages", [])
    data = blob_bytes(root, manifest, mem.get("blob", "memory")) if pages else b""
    return {int(p, 16): data[i * size:(i + 1) * size] for i, p in enumerate(pages)}, size


def differing_bytes(x, y):
    n = min(len(x), len(y))
    return sum(1 for i in range(n) if x[i] != y[i]) + abs(len(x) - len(y))


def compare(root_a, a, root_b, b):
    groups = {"inputs": [], "outputs": [], "addresses": [], "registers": []}
    sections = [k for k in sorted(set(a) | set(b)) if k not in ("blobs", "memory", "notes", "schema", "version")]
    for section in sections:
        diffs = []
        walk(a.get(section), b.get(section), section, diffs)
        for path, x, y in diffs:
            leaf = path.split(".")[-1].split("[")[0]
            if leaf in IDENTITY_KEYS:
                continue
            if section == "registers":
                groups["registers"].append(f"{path}: {x} -> {y}")
            elif is_address(path):
                groups["addresses"].append(f"{path}: {x} -> {y}")
            else:
                groups["inputs"].append(f"{path}: {x} -> {y}")

    blobs_a, blobs_b = a.get("blobs", {}), b.get("blobs", {})
    for name in sorted(set(blobs_a) | set(blobs_b)):
        if name == "memory":
            continue
        group = "outputs" if name.endswith("-live") else "inputs"
        if name not in blobs_a or name not in blobs_b:
            groups[group].append(f"blob {name}: only in {'A' if name in blobs_a else 'B'}")
        elif blobs_a[name]["sha256"] != blobs_b[name]["sha256"]:
            x, y = blob_bytes(root_a, a, name), blob_bytes(root_b, b, name)
            groups[group].append(f"blob {name}: {differing_bytes(x, y)} of {max(len(x), len(y))} bytes differ")

    pages_a, size_a = page_map(root_a, a)
    pages_b, size_b = page_map(root_b, b)
    if size_a != size_b and pages_a and pages_b:
        groups["inputs"].append(f"memory page size {size_a} -> {size_b}")
    else:
        for p in sorted(set(pages_a) & set(pages_b)):
            if pages_a[p] != pages_b[p]:
                groups["inputs"].append(f"memory page 0x{p:x}: {differing_bytes(pages_a[p], pages_b[p])} bytes differ")
        only = len(set(pages_a) ^ set(pages_b))
        if only:
            groups["addresses"].append(f"memory: {only} pages held by only one capture")
    return groups


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("a")
    parser.add_argument("b")
    parser.add_argument("--max-lines", type=int, default=20, help="differences listed per group")
    args = parser.parse_args(argv)
    root_a, a = load(args.a)
    root_b, b = load(args.b)
    ida, idb = a.get("identity", {}), b.get("identity", {})
    for tag, ident in (("A", ida), ("B", idb)):
        source = ident.get("source", "not recorded")
        print(f"{tag}: {ident.get('pipeline')} flip {ident.get('flip')} draw {ident.get('draw_index')}, from {source}")
    groups = compare(root_a, a, root_b, b)
    for name in ("inputs", "outputs", "addresses", "registers"):
        items = groups[name]
        print(f"{name}: {'equal' if not items else f'{len(items)} differ'}")
        for line in items[:args.max_lines]:
            print(f"  {line}")
        if len(items) > args.max_lines:
            print(f"  ... {len(items) - args.max_lines} more")
    return 0 if not groups["inputs"] and not groups["outputs"] else 1


if __name__ == "__main__":
    sys.exit(main())
