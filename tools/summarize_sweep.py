#!/usr/bin/env python3
"""Summarise build/fault_sweep.csv (from tests/unit fault_sweep) as a Markdown table.

Usage: summarize_sweep.py [csv] > table.md
"""
import collections
import csv
import sys


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "build/fault_sweep.csv"
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))

    by = collections.OrderedDict()
    for r in rows:
        s = by.setdefault(r["scenario"], {"n": 0, "ok": 0, "ops": set(), "kinds": collections.Counter(),
                                          "ver": collections.Counter()})
        s["n"] += 1
        s["ok"] += r["pass"] == "1"
        s["ops"].add(int(r["op_index"]))
        s["kinds"][r["op_kind"]] += 1
        s["ver"][r["boot_version"]] += 1

    print("| Scenario | Flash mutations cut | Distinct fault points | Recovered correctly | Ended on v1 / v2 |")
    print("|---|---:|---:|---:|---|")
    tn = tok = 0
    for name, s in by.items():
        # op_index == number of mutations; the last index is the un-cut control run
        muts = len(s["ops"]) - 1
        v = f'{s["ver"].get("1", 0)} / {s["ver"].get("2", 0)}'
        print(f'| `{name}` | {muts} | {s["n"]} | {s["ok"]} | {v} |')
        tn += s["n"]
        tok += s["ok"]
    print(f"| **Total** | | **{tn}** | **{tok}** | |")


if __name__ == "__main__":
    main()
