#!/usr/bin/env python3
"""Turn the probe's per-fill rows into the two numbers the question needs.

For each product and each distance behind the touch:

  what a maker is allowed to charge
      ``capture``      the pre-sweep mid minus the fill price, in ticks and
                       in dollars.  This is gross revenue per contract and
                       it is set by the exchange's minimum tick, not by the
                       maker: at the touch it is half a tick, one tick
                       behind it is a tick and a half.
      ``fill rate``    the share of sweeps that reach that distance.  The
                       tape's answer to "and how often will anyone pay it".
      ``fee``          what the exchange, the clearer and the broker take
                       back out of it, per contract per side.

  what the aggressor takes back
      ``markout_h``    side * (mid at fill + h - fill price), in ticks and
                       dollars, at 0, 0.1, 1, 5, 30 and 60 seconds.  Gross
                       P&L per contract if the position were closed at the
                       mid h seconds later.  It already contains the
                       capture, so ``markout - capture`` is pure adverse
                       selection.
      ``net``          markout at 60s less two fees: one to get in, one to
                       get out.  This is the whole economics of a passive
                       fill and its sign is the viability question.

  and whether the strategy can see it coming
      ``IC``           Spearman rank correlation between the pre-sweep
                       signal state, oriented against the fill, and the
                       60-second markout.  Negative is the useful sign:
                       flow pointing into our fill predicts a worse fill.
      ``AUC``          the same as a classifier of loss-making fills.  0.5
                       is no information.
      ``decile``       mean markout by decile of that score: the shape,
                       which an IC alone hides.
      ``by level``     mean markout by the VPIN gate's own level, which is
                       the thing the live system actually acts on.

Nothing in the score columns is contemporaneous with the sweep, so the IC
and the AUC are prediction rather than description.
"""
import csv
import json
import math
import os
import sys
from collections import defaultdict

HORIZONS = [0.0, 0.1, 1.0, 5.0, 30.0, 60.0]


def mean(xs):
    return sum(xs) / len(xs) if xs else float("nan")


def stdev(xs):
    if len(xs) < 2:
        return float("nan")
    m = mean(xs)
    return math.sqrt(sum((x - m) ** 2 for x in xs) / (len(xs) - 1))


def ranks(xs):
    """Average ranks, ties shared."""
    order = sorted(range(len(xs)), key=lambda i: xs[i])
    out = [0.0] * len(xs)
    i = 0
    while i < len(order):
        j = i
        while j + 1 < len(order) and xs[order[j + 1]] == xs[order[i]]:
            j += 1
        r = (i + j) / 2.0 + 1.0
        for k in range(i, j + 1):
            out[order[k]] = r
        i = j + 1
    return out


def spearman(xs, ys):
    if len(xs) < 3:
        return float("nan")
    rx, ry = ranks(xs), ranks(ys)
    mx, my = mean(rx), mean(ry)
    num = sum((a - mx) * (b - my) for a, b in zip(rx, ry))
    dx = math.sqrt(sum((a - mx) ** 2 for a in rx))
    dy = math.sqrt(sum((b - my) ** 2 for b in ry))
    return num / (dx * dy) if dx and dy else float("nan")


def auc(scores, labels):
    """P(score(positive) > score(negative)), ties at half. Mann-Whitney."""
    pos = sum(labels)
    neg = len(labels) - pos
    if pos == 0 or neg == 0:
        return float("nan")
    r = ranks(scores)
    s = sum(ri for ri, li in zip(r, labels) if li)
    return (s - pos * (pos + 1) / 2.0) / (pos * neg)


class Rows:
    """One product's fills, already split by distance behind the touch."""

    def __init__(self, product, spec):
        self.product = product
        self.spec = spec
        self.by_behind = defaultdict(list)
        self.sweeps = 0
        self.sessions = 0
        self.summaries = []

    def load(self, csv_path, summary):
        self.sweeps += summary["sweeps"]
        self.sessions += 1
        self.summaries.append(summary)
        day = os.path.basename(csv_path).split("_")[-1].split(".")[0]
        with open(csv_path, newline="") as fh:
            for row in csv.DictReader(fh):
                b = int(row["behind"])
                side = int(row["side"])
                composite = (float(row["ofi"]) + float(row["depletion"]) + float(row["run"])) / 3.0
                rec = {
                    "day": day,
                    "side": side,
                    "cap": float(row["cap_mid_ticks"]),
                    "through": int(row["through"]),
                    "sweep": int(row["sweep_contracts"]),
                    "queue": int(row["bid_top"] if side > 0 else row["ask_top"]),
                    # Resting at our own price before the sweep, and what the
                    # sweep traded there: together, whether a quote that
                    # joined the level rather than created it got filled.
                    "q_ahead": int(row["queue_ahead"]),
                    "v_at": int(row["volume_at_price"]),
                    "tox": int(row["tox"]),
                    "warm": int(row["warm"]),
                    "vpin_pct": float(row["vpin_pct"]) if row["vpin_pct"] not in ("", "nan") else float("nan"),
                    "spread": float(row["spread_ticks"]) if row["spread_ticks"] not in ("", "nan") else float("nan"),
                    # Oriented against the fill: positive means the flow that
                    # produced this fill was pointing our way before it landed.
                    "adv": -side * composite,
                    "adv_ofi": -side * float(row["ofi"]),
                    "adv_depl": -side * float(row["depletion"]),
                    "adv_run": -side * float(row["run"]),
                }
                ok = True
                for i, _h in enumerate(HORIZONS):
                    v = row["mid_%d" % i]
                    if v == "":
                        ok = False
                        break
                    rec["m%d" % i] = float(v)
                if ok:
                    self.by_behind[b].append(rec)

    def rows(self, behind, queue="back"):
        """The fills at one distance, under one queue assumption.

        ``front``  the quote was first in line at its price: every sweep
                   that reached the price filled it.  The optimistic bound.
        ``back``   the quote joined the level and sits behind everything
                   that was already resting there, so it fills only once
                   the sweep has traded through that much at the price --
                   or printed past it, which means the level was cleared.
                   This is what ``queue_position: back`` means in the
                   replay config, and it is the honest one.
        """
        rows = self.by_behind[behind]
        if queue == "back":
            rows = [r for r in rows if r["through"] or r["v_at"] > r["q_ahead"]]
        return rows


def table(products, behind, horizon_index=5, queue_aware=True):
    out = []
    for p in products:
        rows = p.rows(behind, queue_aware)
        spec = p.spec
        tv, fee = spec["tick_value"], spec["fee"]
        n = len(rows)
        if n == 0:
            out.append({"product": p.product, "behind": behind, "n": 0})
            continue
        cap = [r["cap"] for r in rows]
        mk = [r["m%d" % horizon_index] for r in rows]
        adv = [r["adv"] for r in rows]
        loss = [1 if m < 0 else 0 for m in mk]
        rec = {
            "product": p.product,
            "behind": behind,
            "n": n,
            "fills_per_sweep": n / p.sweeps if p.sweeps else float("nan"),
            "fills_per_session": n / p.sessions,
            "tick_value": tv,
            "fee": fee,
            "capture_ticks": mean(cap),
            "capture_usd": mean(cap) * tv,
            "markout_ticks": mean(mk),
            "markout_usd": mean(mk) * tv,
            "adverse_usd": (mean(mk) - mean(cap)) * tv,
            "net_usd": mean(mk) * tv - 2 * fee,
            "se_usd": stdev(mk) * tv / math.sqrt(n),
            "loss_share": mean(loss),
        }
        rec["t"] = rec["net_usd"] / rec["se_usd"] if rec["se_usd"] else float("nan")
        for i, h in enumerate(HORIZONS):
            rec["m_%g" % h] = mean([r["m%d" % i] for r in rows]) * tv
        # The information, on the subset the gate is actually live on.
        warm = [r for r in rows if r["warm"]]
        sample = rows if len(warm) < 200 else warm
        s_adv = [r["adv"] for r in sample]
        s_mk = [r["m%d" % horizon_index] for r in sample]
        s_loss = [1 if m < 0 else 0 for m in s_mk]
        rec["ic"] = spearman(s_adv, s_mk)
        rec["auc"] = auc(s_adv, s_loss)
        rec["ic_ofi"] = spearman([r["adv_ofi"] for r in sample], s_mk)
        rec["ic_depl"] = spearman([r["adv_depl"] for r in sample], s_mk)
        rec["ic_run"] = spearman([r["adv_run"] for r in sample], s_mk)
        rec["ic_vpin"] = spearman([r["vpin_pct"] for r in sample if not math.isnan(r["vpin_pct"])],
                                  [m for r, m in zip(sample, s_mk) if not math.isnan(r["vpin_pct"])])
        rec["n_warm"] = len(warm)
        # What avoiding the worst decile would be worth, per fill.
        if len(sample) >= 100:
            paired = sorted(zip(s_adv, s_mk))
            k = len(paired) // 10
            rec["worst_decile_usd"] = mean([m for _, m in paired[-k:]]) * tv
            rec["best_decile_usd"] = mean([m for _, m in paired[:k]]) * tv
            rec["gate_value_usd"] = mean([m for _, m in paired[:-k]]) * tv - mean(s_mk) * tv
        out.append(rec)
    return out


def deciles(p, behind, horizon_index=5, key="adv", n_bins=10):
    rows = p.rows(behind)
    rows = [r for r in rows if r["warm"]] or rows
    if len(rows) < n_bins * 10:
        return []
    tv = p.spec["tick_value"]
    paired = sorted((r[key], r["m%d" % horizon_index], r["cap"]) for r in rows)
    size = len(paired) // n_bins
    out = []
    for i in range(n_bins):
        chunk = paired[i * size:(i + 1) * size if i < n_bins - 1 else len(paired)]
        out.append({
            "bin": i + 1,
            "score_lo": chunk[0][0],
            "score_hi": chunk[-1][0],
            "n": len(chunk),
            "markout_usd": mean([c[1] for c in chunk]) * tv,
            "capture_usd": mean([c[2] for c in chunk]) * tv,
            "net_usd": mean([c[1] for c in chunk]) * tv - 2 * p.spec["fee"],
        })
    return out


def by_level(p, behind, horizon_index=5):
    rows = [r for r in p.rows(behind) if r["warm"]]
    tv = p.spec["tick_value"]
    out = []
    for lvl, name in enumerate(["calm", "elevated", "toxic", "extreme"]):
        chunk = [r for r in rows if r["tox"] == lvl]
        if not chunk:
            continue
        mk = [r["m%d" % horizon_index] for r in chunk]
        out.append({
            "level": name,
            "n": len(chunk),
            "share": len(chunk) / len(rows),
            "markout_usd": mean(mk) * tv,
            "net_usd": mean(mk) * tv - 2 * p.spec["fee"],
            "se_usd": stdev(mk) * tv / math.sqrt(len(chunk)),
        })
    return out


def day_blocked(rows, values, tv, fee=0.0):
    """Mean and its standard error across sessions rather than across fills.

    Fills inside one session are not independent -- a single informed
    aggressor produces a run of them -- so the across-fill standard error
    is far too small.  The session mean is the unit that repeats.
    """
    from collections import defaultdict
    per = defaultdict(list)
    for r, v in zip(rows, values):
        per[r["day"]].append(v)
    days = [mean(v) * tv - fee for v in per.values() if v]
    return mean(days), (stdev(days) / math.sqrt(len(days)) if len(days) > 1 else float("nan")), len(days)


def control_ic(p, behind, horizon_index=5):
    """A feature that must work, to prove the machinery can see one.

    The sweep's own size is contemporaneous with the fill, not predictive,
    so it is no use to a quoter -- but a bigger sweep must mark out worse.
    If this is flat too, the measurement is broken rather than the signal.
    """
    rows = p.rows(behind)
    if len(rows) < 200:
        return {}
    mk = [r["m%d" % horizon_index] for r in rows]
    return {
        "ic_sweep_size": spearman([float(r["sweep"]) for r in rows], mk),
        "ic_spread": spearman([r["spread"] for r in rows], mk),
        "ic_capture": spearman([r["cap"] for r in rows], mk),
        "n": len(rows),
    }
