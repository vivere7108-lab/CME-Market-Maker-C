#!/usr/bin/env python3
"""The tables. Run after the probe has written its events.

    HARVESTER_EDGE_DIR=/path/to/run tools/edge_tables.py

The directory holds ``specs.json`` (one entry per product: tick, mult,
exch, ibkr, nfa) and ``events/B_<PRODUCT>_<date>.csv`` beside its
``.json`` summary, which is what ``harvester_edge --out/--summary``
writes.  The products reported are the keys of ``specs.json``, in the
order ``ORDER`` gives where it knows them.
"""
import glob
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from edge_report import Rows, table, deciles, by_level, mean  # noqa: E402

S = os.environ.get("HARVESTER_EDGE_DIR")
if not S:
    sys.exit("set HARVESTER_EDGE_DIR to the directory holding specs.json and events/")
SPECS = json.load(open(os.path.join(S, "specs.json")))
# Most interesting first where we know; anything else in the specs follows.
PREFERRED = ["ES", "NQ", "CL", "GC", "ZN", "ZF", "ZT", "6E", "ZC", "SR3"]
ORDER = [p for p in PREFERRED if p in SPECS] + sorted(set(SPECS) - set(PREFERRED))


def load(product, pattern):
    spec = SPECS[product]
    spec = dict(spec)
    spec["tick_value"] = spec["tick"] * spec["mult"]
    spec["fee"] = spec["exch"] + spec["ibkr"] + spec["nfa"]
    p = Rows(product, spec)
    for csv_path in sorted(glob.glob(pattern)):
        js = csv_path[:-4] + ".json"
        if not os.path.exists(js):
            continue
        p.load(csv_path, json.load(open(js)))
    return p


def row(cells, widths):
    return "  ".join(str(c).rjust(w) if i else str(c).ljust(w) for i, (c, w) in enumerate(zip(cells, widths)))


def main():
    products = [load(p, os.path.join(S, "events", "B_%s_*.csv" % p)) for p in ORDER]
    summ = {p.product: p.summaries for p in products}

    print("=" * 108)
    print("A.  WHAT A MAKER IS ALLOWED TO CHARGE")
    print("    GLBX.MDP3 mbp-10, 2026-09-15/16/17, 13:30-20:00 UTC, front contract by volume (.v.0).")
    print("    'tick binds' is the share of book-time at the minimum spread: where it is high the")
    print("    exchange's tick, not competition, is setting the price of liquidity.")
    print("=" * 108)
    w = [5, 8, 9, 8, 9, 8, 7, 8, 9, 9]
    print(row(["prod", "tick $", "spread", "tick", "touch", "sweeps", "sweep", "fee $", "fee as", "charge $"], w))
    print(row(["", "value", "ticks", "binds", "depth", "/sess", "size", "/side", "% tick", "b=1 gross"], w))
    print("-" * 108)
    for p in products:
        s = p.summaries
        tv = p.spec["tick_value"]
        fee = p.spec["fee"]
        sw = mean([x["sweeps"] for x in s])
        vol = mean([x["traded_contracts"] for x in s])
        print(row([p.product, "%.3f" % tv, "%.3f" % mean([x["mean_spread_ticks"] for x in s]),
                   "%.1f%%" % (100 * mean([x["share_one_tick"] for x in s])),
                   "%.0f" % mean([(x["mean_bid_top"] + x["mean_ask_top"]) / 2 for x in s]),
                   "%.0f" % sw, "%.1f" % (vol / sw if sw else float("nan")),
                   "%.2f" % fee, "%.0f%%" % (100 * fee / tv), "%.2f" % (1.5 * tv)], w))

    # Every table below is queue-aware: the quote joined the level and
    # fills only once the sweep traded through what was resting in front
    # of it. That is `replay.queue_position: back`, and it is the
    # population the strategy actually gets -- roughly a fortieth of the
    # fills an unfiltered count reports.
    for behind, label, qa in ((1, "B.  ONE TICK BEHIND THE TOUCH  (what the strategy actually quotes)", True),
                              (0, "C.  AT THE TOUCH, QUEUE-AWARE  (the alternative: compete for the queue)", True)):
        print()
        print("=" * 108)
        print(label)
        if behind == 1:
            print("    A fill is a sweep that traded through the level in front AND through the queue")
            print("    resting at our own price. 'net' is the 60s markout less two fees: in and out.")
        else:
            print("    Only sweeps larger than the depth resting in front of us count as fills.")
        print("=" * 108)
        t = table(products, behind, queue_aware=qa)
        w = [5, 8, 8, 9, 9, 9, 9, 9, 7, 7]
        print(row(["prod", "fills", "% of", "capture", "markout", "markout", "adverse", "net $", "t", "loss"], w))
        print(row(["", "/sess", "sweeps", "$", "1s $", "60s $", "60s $", "60s", "", "share"], w))
        print("-" * 108)
        for r in t:
            if not r.get("n"):
                print(row([r["product"], "0", "-", "-", "-", "-", "-", "-", "-", "-"], w))
                continue
            print(row([r["product"], "%.0f" % r["fills_per_session"], "%.2f%%" % (100 * r["fills_per_sweep"]),
                       "%.2f" % r["capture_usd"], "%.2f" % r["m_1"], "%.2f" % r["m_60"],
                       "%.2f" % r["adverse_usd"], "%+.2f" % r["net_usd"], "%.1f" % r["t"],
                       "%.0f%%" % (100 * r["loss_share"])], w))

    print()
    print("=" * 108)
    print("D.  INFORMATIONAL EDGE  (one tick behind, queue-aware, warmed-up gate, 60s markout)")
    print("    IC: Spearman rank correlation of the pre-sweep signal, oriented against the fill,")
    print("    with the markout that followed. Negative = the signal saw it coming. AUC: the same")
    print("    as a classifier of loss-making fills; 0.50 is no information. 'floor' is 2/sqrt(n)")
    print("    on the population each IC was computed on: an IC inside it is not distinguishable")
    print("    from zero, and the floor moves with the population, so read the two together.")
    print("=" * 108)
    t = table(products, 1)
    w = [5, 8, 8, 8, 8, 8, 8, 8, 8, 10, 10]
    print(row(["prod", "n warm", "floor", "IC all", "IC ofi", "IC depl", "IC run", "IC vpin", "AUC",
               "worst 10%", "best 10%"], w))
    print("-" * 108)
    for r in t:
        if not r.get("n") or r.get("n_warm", 0) < 200:
            print(row([r["product"], str(r.get("n_warm", 0)), "too few", "", "", "", "", "", "", "", ""], w))
            continue
        print(row([r["product"], "%d" % r["n_warm"], "+-%.3f" % r["ic_floor"], "%+.3f" % r["ic"],
                   "%+.3f" % r["ic_ofi"], "%+.3f" % r["ic_depl"], "%+.3f" % r["ic_run"], "%+.3f" % r["ic_vpin"],
                   "%.3f" % r["auc"], "%+.2f" % r["worst_decile_usd"], "%+.2f" % r["best_decile_usd"]], w))
    return products




def curves(products):
    print()
    print("=" * 108)
    print("E.  THE MARKOUT CURVE, one tick behind the touch, queue-aware, $ per contract")
    print("    'capture' is booked the instant the sweep lands; everything after it is what the")
    print("    aggressor takes back. A curve that keeps falling is an informed counterparty; one")
    print("    that falls and stops is the sweep's own impact, and it is rentable.")
    print("=" * 108)
    w = [5, 9, 9, 9, 9, 9, 9, 9, 9]
    print(row(["prod", "capture", "0s", "0.1s", "1s", "5s", "30s", "60s", "2x fee"], w))
    print("-" * 108)
    for r in table(products, 1):
        if not r.get("n"):
            continue
        print(row([r["product"], "%.2f" % r["capture_usd"]] +
                  ["%+.2f" % r["m_%g" % h] for h in [0, 0.1, 1, 5, 30, 60]] +
                  ["%.2f" % (2 * r["fee"])], w))


def detail(p, behind=1):
    print()
    print("=" * 108)
    print("F.  %s, one tick behind: the markout by decile of the pre-sweep signal, and by the gate's level" % p.product)
    print("=" * 108)
    d = deciles(p, behind)
    if d:
        w = [6, 9, 9, 9, 10, 10, 10]
        print(row(["decile", "score lo", "score hi", "n", "capture $", "markout $", "net $"], w))
        print("-" * 108)
        for b in d:
            print(row([b["bin"], "%+.3f" % b["score_lo"], "%+.3f" % b["score_hi"], b["n"],
                       "%.2f" % b["capture_usd"], "%+.2f" % b["markout_usd"], "%+.2f" % b["net_usd"]], w))
    lv = by_level(p, behind)
    if lv:
        print()
        w = [10, 9, 8, 11, 10, 9]
        print(row(["gate level", "n", "share", "markout $", "net $", "se $"], w))
        print("-" * 108)
        for b in lv:
            print(row([b["level"], b["n"], "%.1f%%" % (100 * b["share"]), "%+.2f" % b["markout_usd"],
                       "%+.2f" % b["net_usd"], "%.2f" % b["se_usd"]], w))


if __name__ == "__main__":
    ps = main()
    curves(ps)
    for q in ps:
        if q.product in ("ES", "NQ", "GC", "CL"):
            detail(q)
