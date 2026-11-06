#!/usr/bin/env python3
# Map the sampler's JIT host addresses to guest code.
#
# Inputs:
#   <samples.txt.jit>  written by the sampler next to its report: "thread hex_address count"
#   <perf-PID.map>     written by dynarmic when EDEN_NX2_JIT_MAP=<dir> is set: "start size name" (hex)
# Block names are "a64_<PC:016X>_fpcr<FPCR:08X>" (A32: "a32_<t|a><PC:08X>_<le|be>_fpcr<..>").
# Other entries (a64_fast_dispatch_table_lookup, dynarmic_dispatcher, *_fallback_*) are reported by name.
#
# Usage: python3 -I jit_guest_pc.py <samples.txt.jit> <perf-PID.map> [--thread CPUCore_4]
#        [--module-base 0x...] [--top 40] [--func-gap 0x400]
#        python3 -I jit_guest_pc.py --self-test
import argparse
import bisect
import fnmatch
import os
import re
import sys
import tempfile
from collections import Counter

BLOCK_RE = re.compile(r"^(?:a64_|a32_[ta])([0-9A-Fa-f]+)_")


class BlockMap:
    """Disjoint [start, end) intervals; a later entry replaces what it overlaps."""

    def __init__(self):
        self.starts = []
        self.ends = []
        self.names = []

    def add(self, start, size, name):
        end = start + size
        if not self.starts or start >= self.ends[-1]:
            self.starts.append(start)
            self.ends.append(end)
            self.names.append(name)
            return
        lo = bisect.bisect_right(self.ends, start)
        hi = bisect.bisect_left(self.starts, end)
        starts, ends, names = [], [], []
        if lo < hi and self.starts[lo] < start:
            starts.append(self.starts[lo])
            ends.append(start)
            names.append(self.names[lo])
        starts.append(start)
        ends.append(end)
        names.append(name)
        if lo < hi and self.ends[hi - 1] > end:
            starts.append(end)
            ends.append(self.ends[hi - 1])
            names.append(self.names[hi - 1])
        self.starts[lo:hi] = starts
        self.ends[lo:hi] = ends
        self.names[lo:hi] = names

    def lookup(self, address):
        i = bisect.bisect_right(self.starts, address) - 1
        if i >= 0 and address < self.ends[i]:
            return self.names[i]
        return None


def load_map(path):
    blocks = BlockMap()
    with open(path, errors="replace") as f:
        for line in f:
            parts = line.split(None, 2)
            if len(parts) < 3:
                continue
            try:
                start = int(parts[0], 16)
                size = int(parts[1], 16)
            except ValueError:
                continue
            blocks.add(start, size, parts[2].strip())
    return blocks


def load_samples(path, thread_pattern=None):
    samples = Counter()
    with open(path, errors="replace") as f:
        for line in f:
            parts = line.split()
            if len(parts) != 3:
                continue
            thread, address, count = parts
            if thread_pattern and not fnmatch.fnmatchcase(thread, thread_pattern):
                continue
            samples[int(address, 16)] += int(count)
    return samples


def guest_pc(name):
    m = BLOCK_RE.match(name)
    return int(m.group(1), 16) if m else None


def aggregate(samples, blocks):
    """Returns (per guest PC, per named non-block entry, unmapped count)."""
    by_pc = Counter()
    by_stub = Counter()
    unmapped = 0
    for address, count in samples.items():
        name = blocks.lookup(address)
        if name is None:
            unmapped += count
            continue
        pc = guest_pc(name)
        if pc is None:
            by_stub[name] += count
        else:
            by_pc[pc] += count
    return by_pc, by_stub, unmapped


def estimate_functions(pcs, gap):
    """Groups block start PCs into runs; a gap over `gap` bytes starts a new function.
    A heuristic: dynarmic does not tell where a guest function begins or ends."""
    groups = []
    for pc in sorted(pcs):
        if groups and pc - groups[-1][-1] <= gap:
            groups[-1].append(pc)
        else:
            groups.append([pc])
    return groups


def report(samples, blocks, module_base, top, gap, out):
    total = sum(samples.values())
    by_pc, by_stub, unmapped = aggregate(samples, blocks)
    in_blocks = sum(by_pc.values())
    in_stubs = sum(by_stub.values())

    def pct(n):
        return 100.0 * n / max(total, 1)

    def where(pc):
        if module_base is not None and pc >= module_base:
            return f"main+{pc - module_base:#x}"
        return ""

    print(f"{total} samples at {len(samples)} distinct addresses", file=out)
    print(f"  in guest blocks {in_blocks} ({pct(in_blocks):.1f}%), in named stubs {in_stubs} "
          f"({pct(in_stubs):.1f}%), unmapped {unmapped} ({pct(unmapped):.1f}%)", file=out)

    print(f"\n-- top guest blocks ({len(by_pc)} blocks)", file=out)
    print(f"{'samples':>8} {'%':>6}  guest pc           module offset", file=out)
    for pc, count in by_pc.most_common(top):
        print(f"{count:8} {pct(count):5.1f}%  {pc:#018x} {where(pc)}", file=out)

    pages = Counter()
    for pc, count in by_pc.items():
        pages[pc >> 12] += count
    print(f"\n-- top guest pages ({len(pages)} pages)", file=out)
    for page, count in pages.most_common(top):
        print(f"{count:8} {pct(count):5.1f}%  {page << 12:#018x} {where(page << 12)}", file=out)

    # Group every compiled block, not only the sampled ones, so a function's extent shows.
    all_pcs = {pc for pc in (guest_pc(n) for n in blocks.names) if pc is not None}
    all_pcs.update(by_pc)
    groups = estimate_functions(all_pcs, gap)
    func_counts = []
    for group in groups:
        count = sum(by_pc.get(pc, 0) for pc in group)
        if count:
            func_counts.append((count, group))
    func_counts.sort(key=lambda item: -item[0])
    print(f"\n-- top estimated functions (block runs, gap <= {gap:#x}; a heuristic)", file=out)
    for count, group in func_counts[:top]:
        print(f"{count:8} {pct(count):5.1f}%  {group[0]:#018x}..{group[-1]:#x} "
              f"{len(group)} blocks {where(group[0])}", file=out)

    if by_stub or unmapped:
        print("\n-- not guest blocks", file=out)
        for name, count in by_stub.most_common(top):
            print(f"{count:8} {pct(count):5.1f}%  {name}", file=out)
        if unmapped:
            print(f"{unmapped:8} {pct(unmapped):5.1f}%  (unmapped: no map entry)", file=out)


def self_test():
    import io

    with tempfile.TemporaryDirectory() as tmp:
        map_path = os.path.join(tmp, "perf-1.map")
        jit_path = os.path.join(tmp, "samples.txt.jit")
        with open(map_path, "w") as f:
            f.write("0000000010000000 0000000000000100 dynarmic_dispatcher\n")
            f.write("0000000010000100 0000000000000040 a64_fast_dispatch_table_lookup\n")
            f.write("0000000010000200 0000000000000100 a64_0000000001001000_fpcr01000000\n")
            f.write("0000000010000300 0000000000000100 a64_0000000001001040_fpcr01000000\n")
            f.write("0000000010000400 0000000000000100 a64_0000000001005000_fpcr01000000\n")
            f.write("0000000010000500 0000000000000100 a64_0000000001001000_fpcr00000000\n")
            # A cleared cache reuses the address range: this entry replaces the one above.
            f.write("0000000010000380 0000000000000100 a64_0000000002002000_fpcr01000000\n")
            f.write("garbage\n")
        with open(jit_path, "w") as f:
            f.write("CPUCore_4 10000210 10\n")   # block 0x1001000
            f.write("CPUCore_4 10000510 5\n")    # block 0x1001000, other fpcr
            f.write("CPUCore_4 10000320 4\n")    # block 0x1001040 (left remnant)
            f.write("CPUCore_4 10000390 7\n")    # block 0x2002000 (replaced the tail)
            f.write("CPUCore_4 10000490 3\n")    # block 0x1005000 (right remnant), other page
            f.write("CPUCore_4 10000110 2\n")    # fast dispatch
            f.write("CPUCore_5 10000010 20\n")   # dispatcher, other thread
            f.write("CPUCore_4 20000000 6\n")    # unmapped

        blocks = load_map(map_path)
        assert blocks.lookup(0x10000390) == "a64_0000000002002000_fpcr01000000"
        assert blocks.lookup(0x10000320) == "a64_0000000001001040_fpcr01000000"
        assert blocks.lookup(0x10000480) == "a64_0000000001005000_fpcr01000000"
        assert blocks.lookup(0x1FFFFFFF) is None

        samples = load_samples(jit_path, "CPUCore_4")
        assert sum(samples.values()) == 37
        by_pc, by_stub, unmapped = aggregate(samples, blocks)
        assert by_pc == {0x1001000: 15, 0x1001040: 4, 0x2002000: 7, 0x1005000: 3}, by_pc
        assert by_stub == {"a64_fast_dispatch_table_lookup": 2}, by_stub
        assert unmapped == 6

        everything = load_samples(jit_path)
        assert aggregate(everything, blocks)[1]["dynarmic_dispatcher"] == 20

        assert estimate_functions([0x1000, 0x1040, 0x5000], 0x400) == [[0x1000, 0x1040], [0x5000]]

        out = io.StringIO()
        report(samples, blocks, 0x1000000, 10, 0x400, out)
        text = out.getvalue()
        assert "main+0x1000" in text and "a64_fast_dispatch_table_lookup" in text, text
        assert "unmapped" in text
    print("self-test passed")


def main():
    parser = argparse.ArgumentParser(description="Map sampler JIT addresses to guest PCs.")
    parser.add_argument("jit", nargs="?", help="<out>.jit written by the sampler")
    parser.add_argument("map", nargs="?", help="perf-<pid>.map written with EDEN_NX2_JIT_MAP")
    parser.add_argument("--thread", help="only this thread (glob, e.g. CPUCore_4)")
    parser.add_argument("--module-base", type=lambda s: int(s, 0),
                        help="guest address of the main module, for offsets (Module main loaded at ...)")
    parser.add_argument("--top", type=int, default=40)
    parser.add_argument("--func-gap", type=lambda s: int(s, 0), default=0x400,
                        help="largest distance between block starts inside one estimated function")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if not args.jit or not args.map:
        parser.error("need the .jit and .map files")
    samples = load_samples(args.jit, args.thread)
    if not samples:
        print("no samples (check --thread and the .jit file)", file=sys.stderr)
        return 1
    report(samples, load_map(args.map), args.module_base, args.top, args.func_gap, sys.stdout)
    return 0


if __name__ == "__main__":
    sys.exit(main())
