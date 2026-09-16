"""Phase 5 statistics: paired, distribution-first comparison of the two
strategies from results/results.csv.

Because both strategies face the identical flow stream per seed, the natural
unit is the PAIRED per-seed difference. Significance is assessed two ways per
symbol — a paired t-test and a Wilcoxon signed-rank test (which does not
assume normal differences) — and reported as-is, including any symbol where
the difference is NOT distinguishable from noise.

Run:  /Users/salisdalhatu/NGXDash/.venv/bin/python analysis/analyze.py [results_dir]
"""

import sys
from pathlib import Path

import numpy as np
import pandas as pd
from scipy import stats

ROOT = Path(__file__).resolve().parent.parent
RES = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "results"
df = pd.read_csv(RES / "results.csv")

assert (df.reconciled == 1).all(), "unreconciled episodes present — do not report"

wide = df.pivot_table(index=["symbol", "seed"], columns="strategy")
lines = []


def w(s=""):
    print(s)
    lines.append(s)


def fmt_dist(x):
    return (f"mean {np.mean(x):>12,.0f}  median {np.median(x):>12,.0f}  "
            f"sd {np.std(x, ddof=1):>12,.0f}  "
            f"[p5 {np.percentile(x, 5):>12,.0f}, "
            f"p95 {np.percentile(x, 95):>12,.0f}]")


w(f"paired episodes per symbol: "
  f"{df.groupby('symbol')['seed'].nunique().to_dict()}")

summary_rows = []
for sym in df.symbol.unique():
    sub = wide.loc[sym]
    base = sub["pnl_ngn"]["baseline"].to_numpy()
    as_ = sub["pnl_ngn"]["avellaneda_stoikov"].to_numpy()
    diff = as_ - base

    w(f"\n=== {sym} ===")
    w(f"  PnL (NGN/session), n={len(diff)} seeds")
    w(f"    baseline : {fmt_dist(base)}")
    w(f"    A-S      : {fmt_dist(as_)}")
    w(f"    A-S diff : {fmt_dist(diff)}")
    w(f"    seeds where A-S > baseline: {(diff > 0).sum()}/{len(diff)}")
    w(f"    sessions with negative PnL: baseline {(base < 0).sum()}, "
      f"A-S {(as_ < 0).sum()}")

    t = stats.ttest_rel(as_, base)
    wx = stats.wilcoxon(as_, base)
    w(f"    paired t-test : t={t.statistic:+.3f}  p={t.pvalue:.2e}")
    w(f"    Wilcoxon      : W={wx.statistic:.0f}  p={wx.pvalue:.2e}")

    for metric, label in [("max_abs_inv_lots", "max |inventory| (lots)"),
                          ("inv_std_lots", "inventory std (lots)"),
                          ("fill_events", "fill events"),
                          ("filled_shares", "filled shares")]:
        b = sub[metric]["baseline"].to_numpy()
        a = sub[metric]["avellaneda_stoikov"].to_numpy()
        w(f"    {label:<24} baseline {np.mean(b):>10,.1f}   "
          f"A-S {np.mean(a):>10,.1f}")

    q2t_b = (sub["quotes_posted"]["baseline"] /
             sub["fill_events"]["baseline"].clip(lower=1)).mean()
    q2t_a = (sub["quotes_posted"]["avellaneda_stoikov"] /
             sub["fill_events"]["avellaneda_stoikov"].clip(lower=1)).mean()
    w(f"    {'quote-to-trade ratio':<24} baseline {q2t_b:>10,.1f}   "
      f"A-S {q2t_a:>10,.1f}")

    summary_rows.append({
        "symbol": sym, "n_seeds": len(diff),
        "baseline_mean_pnl": np.mean(base), "as_mean_pnl": np.mean(as_),
        "mean_diff": np.mean(diff), "diff_sd": np.std(diff, ddof=1),
        "as_wins": int((diff > 0).sum()),
        "t_p": t.pvalue, "wilcoxon_p": wx.pvalue,
        "base_max_inv": sub["max_abs_inv_lots"]["baseline"].mean(),
        "as_max_inv": sub["max_abs_inv_lots"]["avellaneda_stoikov"].mean(),
    })

pd.DataFrame(summary_rows).to_csv(RES / "summary.csv", index=False)
(RES / "summary.txt").write_text("\n".join(lines) + "\n")
w(f"\nwrote {RES}/summary.csv and {RES}/summary.txt")
