#!/usr/bin/env python3
"""Logistic regression on the microstructure state: can anything predict the mid?

Two questions, deliberately separated, because the answer differs:

  1. Unconditional direction.  Sample the book every 200 ms and ask whether
     the state at t predicts the sign of the mid move over the next 0.1, 1
     or 5 seconds.  This is the easy question and the literature says yes.

  2. Fill selection.  Take only the moments a passive quote one tick behind
     the touch was actually reached, and ask whether the state *before the
     sweep* predicts whether that fill made money.  This is the question
     the strategy needs answered, and conditioning on the sweep is exactly
     what makes it hard.

Feature sets are nested so the edge can be attributed rather than just
reported:

  strategy   what the system already computes: OFI, queue depletion, the
             aggressor run, VPIN's percentile, the gate's level.
  +book      order-imbalance ratios at 1, 5 and 10 levels, the microprice
             offset, the spread, the touch sizes.
  +tape      trade count, volume and signed volume over the last second,
             realised vol, and the last 0.1 s and 1 s of mid return.

Everything is validated walk-forward by session: train on sessions 1..k,
test on k+1, never on a session the fit has seen.  A model that is only
better in-sample is not better.
"""
import sys
import numpy as np
import pandas as pd
from sklearn.linear_model import LogisticRegression
from sklearn.metrics import roc_auc_score
from sklearn.preprocessing import StandardScaler

H = {0: 0.0, 1: 0.1, 2: 1.0, 3: 5.0, 4: 30.0, 5: 60.0}

STRATEGY = ["ofi", "depletion", "run", "vpin_pct", "tox"]
BOOK = ["imb1", "imb5", "imb10", "micro_off", "spread_ticks", "log_bid1", "log_ask1"]
TAPE = ["trades_1s", "log_volume_1s", "signed_share_1s", "sigma", "r_prev_100ms", "r_prev_1s"]

SETS = {
    "strategy": STRATEGY,
    "strategy+book": STRATEGY + BOOK,
    "strategy+book+tape": STRATEGY + BOOK + TAPE,
    "book only": BOOK,
    "imbalance only": ["imb1"],
}


def prepare(df):
    """Derived columns, and the ones that need taming before a linear fit."""
    df = df.copy()
    df["vpin_pct"] = df["vpin_pct"].fillna(0.5)
    df["log_bid1"] = np.log1p(df["bid1"])
    df["log_ask1"] = np.log1p(df["ask1"])
    df["log_volume_1s"] = np.log1p(df["volume_1s"])
    # Signed volume as a share of volume: direction without the level.
    df["signed_share_1s"] = np.where(df["volume_1s"] > 0, df["signed_volume_1s"] / df["volume_1s"].clip(lower=1), 0.0)
    return df


def walk_forward(frames, features, target, min_train=5, keep=None):
    """Train on every session before the test one. Returns per-fold results."""
    out = []
    for k in range(min_train, len(frames)):
        train = pd.concat(frames[:k], ignore_index=True)
        test = frames[k]
        xtr, ytr = train[features].to_numpy(float), train[target].to_numpy()
        xte, yte = test[features].to_numpy(float), test[target].to_numpy()
        if len(np.unique(ytr)) < 2 or len(np.unique(yte)) < 2:
            continue
        scaler = StandardScaler().fit(xtr)
        model = LogisticRegression(max_iter=2000, C=1.0)
        model.fit(scaler.transform(xtr), ytr)
        p_te = model.predict_proba(scaler.transform(xte))[:, 1]
        p_tr = model.predict_proba(scaler.transform(xtr))[:, 1]
        out.append({
            "fold": k,
            "n_train": len(ytr),
            "n_test": len(yte),
            "auc_in": roc_auc_score(ytr, p_tr),
            "auc_out": roc_auc_score(yte, p_te),
            "base": yte.mean(),
            "p": p_te,
            "y": yte,
            "model": model,
            "features": features,
            # The quantity the prediction is supposed to be worth something
            # about, carried alongside so it cannot fall out of step.
            "kept": test[keep].to_numpy(float) if keep else None,
        })
    return out
