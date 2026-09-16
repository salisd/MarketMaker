"""Phase 1 — calibrate the synthetic order flow from NGXDash's real measurements.

Every number this script emits is computed by NGXDash's OWN analytics modules
(imported directly, not reimplemented) running over NGXDash's OWN cached price
data (data/prices/<SYM>.parquet, fetched from NGX by NGXDash's ingestion layer).
The output JSON records, for every calibration input, exactly which module
function and which cache file it came from.

The ONE input that cannot be measured from the NGXDash cache is the mean trade
(deal) size: the cache holds daily share volume but not daily deal counts, so
the market-order arrival rate is *implied* by volume via an assumed mean deal
size. That assumption is stated in the output under `assumptions`, is the same
for every symbol, and is a config knob — it is never presented as a measured
quantity.

Run with NGXDash's venv (it owns the pandas/pyarrow stack):

    /Users/salisdalhatu/NGXDash/.venv/bin/python calibration/calibrate.py
"""

import argparse
import json
import sys
from datetime import date, datetime, timezone
from pathlib import Path

import numpy as np
import pandas as pd

NGXDASH = Path("/Users/salisdalhatu/NGXDash")
sys.path.insert(0, str(NGXDASH))

# NGXDash's own estimators — imported, not copied.
from ngxdash.analytics.spreads import roll_spread          # Roll (1984)
from ngxdash.analytics.volatility import realized_volatility  # rolling RV

OUT_DIR = Path(__file__).resolve().parent / "output"

# --- Choices, all documented in the emitted JSON -----------------------------

# Three symbols spanning liquidity regimes (chosen from a survey of the full
# universe: these have valid Roll estimates — negative return autocovariance —
# and complete 1y coverage; most NGX names fail that filter, which is itself
# a thin-market fact NGXDash documents).
SYMBOLS = ["GTCO", "OANDO", "LIVESTOCK"]

LOOKBACK_DAYS = 365          # calendar days of history used for every estimate
RV_WINDOW = 63               # trading days (~3 months) for realized vol
PERIODS_PER_YEAR = 252

# NGX continuous trading session 10:00-14:20 WAT.
SESSION_SECONDS = 4 * 3600 + 20 * 60  # 15,600 s

# Assumption (NOT measured — see module docstring): mean market-order size in
# shares. Cross-checked against publicly reported NGX deal counts: at 10,000
# shares/deal, GTCO's cached ADV implies ~2.4k deals/day and LIVESTOCK's ~190,
# both the right order of magnitude for those names.
ASSUMED_MEAN_TRADE_SHARES = 10_000

TICK_SIZE_NGN = 0.01  # simplification: one-kobo tick for all symbols


def calibrate_symbol(sym: str, meta: dict, lookback_days: int) -> dict:
    cache_file = NGXDASH / "data" / "prices" / f"{sym}.parquet"
    px = pd.read_parquet(cache_file)
    cutoff = pd.Timestamp(date.today()) - pd.Timedelta(days=lookback_days)
    px = px.loc[px.index >= cutoff]
    close = px["close"].dropna()
    volume = px["volume"].dropna()

    # -- volatility: NGXDash realized_volatility (annualized), latest value --
    rv = realized_volatility(close, window=RV_WINDOW,
                             periods_per_year=PERIODS_PER_YEAR).dropna()
    rv_ann = float(rv.iloc[-1])
    sigma_day_rel = rv_ann / np.sqrt(PERIODS_PER_YEAR)

    # -- spread: NGXDash roll_spread over the lookback window ----------------
    spread_rel, autocov = roll_spread(close)  # proportional (log-price) spread
    if not np.isfinite(spread_rel):
        raise SystemExit(
            f"{sym}: Roll estimator invalid (autocov={autocov:.2e} >= 0); "
            "pick a symbol with negative return autocovariance."
        )

    # -- arrival rate implied by volume history ------------------------------
    adv_shares = float(volume.mean())
    trades_per_day = adv_shares / ASSUMED_MEAN_TRADE_SHARES
    lam_per_sec = trades_per_day / SESSION_SECONDS

    last_price = float(close.iloc[-1])

    return {
        "symbol": sym,
        "last_price_ngn": last_price,
        "tick_size_ngn": TICK_SIZE_NGN,
        "sigma_annual_rel": rv_ann,
        "sigma_day_rel": sigma_day_rel,
        "roll_spread_rel": float(spread_rel),
        "roll_autocov": float(autocov),
        "adv_shares": adv_shares,
        "trades_per_day_implied": trades_per_day,
        "lambda_per_sec": lam_per_sec,
        "mean_trade_shares": ASSUMED_MEAN_TRADE_SHARES,
        "session_seconds": SESSION_SECONDS,
        "n_obs_used": int(len(close)),
        "date_range": [str(close.index.min().date()),
                       str(close.index.max().date())],
        "provenance": {
            "cache_file": str(cache_file),
            "cache_fetched_at": meta.get(sym, {}).get("fetched_at"),
            "cache_source": meta.get(sym, {}).get("source"),
            "sigma": ("ngxdash.analytics.volatility.realized_volatility("
                      f"close, window={RV_WINDOW}, periods_per_year="
                      f"{PERIODS_PER_YEAR}), last value, de-annualized by "
                      "1/sqrt(252)"),
            "spread": ("ngxdash.analytics.spreads.roll_spread(close, "
                       "use_log=True) over the lookback window"),
            "arrival_rate": ("mean of cached daily `volume` over lookback / "
                             "ASSUMED_MEAN_TRADE_SHARES / SESSION_SECONDS "
                             "(deal counts are NOT in the cache; see "
                             "assumptions)"),
        },
    }


def write_cfg(cal: dict, path: Path) -> None:
    """Flat key=value file for the C++ harness (no JSON parser needed there)."""
    tick = cal["tick_size_ngn"]
    px_ticks = round(cal["last_price_ngn"] / tick)
    # per-second fundamental-price diffusion, in ticks
    sigma_sec_ticks = (cal["sigma_day_rel"] * cal["last_price_ngn"] / tick
                       / np.sqrt(cal["session_seconds"]))
    half_spread_ticks = cal["roll_spread_rel"] * cal["last_price_ngn"] / tick / 2.0
    lines = {
        "symbol": cal["symbol"],
        "initial_price_ticks": px_ticks,
        "tick_size_ngn": tick,
        "sigma_per_sqrt_sec_ticks": f"{sigma_sec_ticks:.6f}",
        "sigma_day_rel": f"{cal['sigma_day_rel']:.8f}",
        "half_spread_ticks": f"{half_spread_ticks:.4f}",
        "lambda_per_sec": f"{cal['lambda_per_sec']:.8f}",
        "mean_trade_shares": cal["mean_trade_shares"],
        "session_seconds": cal["session_seconds"],
    }
    path.write_text("".join(f"{k}={v}\n" for k, v in lines.items()))


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--lookback-days", type=int, default=LOOKBACK_DAYS,
                    help="calendar days of history (sensitivity re-runs)")
    ap.add_argument("--suffix", default="",
                    help="filename suffix, e.g. _6m for an alternative window")
    args = ap.parse_args()

    meta = json.loads((NGXDASH / "data" / "meta.json").read_text())["symbols"]
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    cals = [calibrate_symbol(s, meta, args.lookback_days) for s in SYMBOLS]
    doc = {
        "generated_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "lookback_days": args.lookback_days,
        "assumptions": {
            "mean_trade_shares": (
                f"{ASSUMED_MEAN_TRADE_SHARES} shares per market order — the one "
                "calibration input not measurable from the NGXDash cache "
                "(daily deal counts are not stored); order-of-magnitude "
                "checked against published NGX deal counts."),
            "tick_size": (f"flat {TICK_SIZE_NGN} NGN tick for all symbols "
                          "(NGX's banded tick rules are not modeled)"),
            "session": "NGX continuous session 10:00-14:20 WAT = 15,600 s",
        },
        "symbols": cals,
    }
    (OUT_DIR / f"calibration{args.suffix}.json").write_text(
        json.dumps(doc, indent=2) + "\n")
    for cal in cals:
        write_cfg(cal, OUT_DIR / f"{cal['symbol']}{args.suffix}.cfg")
        print(f"{cal['symbol']:>10}: px={cal['last_price_ngn']:>9.2f}  "
              f"roll={cal['roll_spread_rel']*100:5.2f}%  "
              f"rv={cal['sigma_annual_rel']:.3f}  "
              f"lambda={cal['lambda_per_sec']*1000:7.3f}e-3/s  "
              f"(~{cal['trades_per_day_implied']:6.0f} trades/day)")
    print(f"\nwrote {OUT_DIR}/calibration{args.suffix}.json "
          "and per-symbol .cfg files")


if __name__ == "__main__":
    main()
