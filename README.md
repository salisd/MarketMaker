# MarketMaker — Avellaneda-Stoikov vs. a naive baseline, on a real matching engine, calibrated from real NGX data

## What this is (and is not)

This is a **research and methodology exercise on synthetic order flow**,
run against my own C++ matching engine
([MatchEng](../MatchEng)) and calibrated from real Nigerian Exchange (NGX)
market measurements produced by my own analytics dashboard
([NGXDash](../NGXDash)). It is:

- **not a live trading system** — nothing here connects to a market;
- **not a backtest on real historical fills** — NGX tick/order-book data is
  not publicly available; the order flow is synthetic, with its statistical
  properties (volatility, spread, arrival rate) calibrated to real measured
  values;
- **not evidence of real-world profitability, and not investment advice.**
  Absolute PnL numbers below are artifacts of the model world (in particular,
  the synthetic flow contains **no informed traders** — see Limitations).
  The defensible claim is *relative and conditional*: under this calibrated
  flow model, on this real engine, the Avellaneda-Stoikov policy beats the
  naive baseline with high statistical confidence.

Every number in this document comes from an actual run of the code in this
repo against the real engine; commands to reproduce are given inline. No
closed-form-only result is presented as a simulation result.

## The integration

This project closes the loop across three projects:

```
NGXDash (Python)                MarketMaker (this repo)                MatchEng (C++20)
────────────────                ───────────────────────                ────────────────
cached NGX daily data     ──►   calibration/calibrate.py
+ its own estimators            (imports NGXDash modules,
  realized_volatility,           emits calibration.json + .cfg
  roll_spread                    with per-number provenance)
                                          │
                                          ▼
                                include/mmsim/flow.hpp          ──►    matcheng::VecBook
                                Poisson market orders,                 (linked header-only —
                                random-walk fundamental,               the SAME price-time-
                                background liquidity grid              priority engine, not a
                                          │                            reimplementation)
                                          ▼                                   │
                                BaselineMM  vs  AvellanedaStoikovMM           │
                                post real limit orders  ──────────────────────┘
                                fills/partials/queue position = engine's decision
                                          │
                                          ▼
                                analysis/analyze.py — paired stats across 50 seeds/symbol
```

## Phase 1 — Calibration from real data

Calibration inputs are computed by **NGXDash's own modules, imported
directly** (`ngxdash.analytics.volatility.realized_volatility`,
`ngxdash.analytics.spreads.roll_spread`), over **NGXDash's own cached
parquets** (`NGXDash/data/prices/<SYM>.parquet`, fetched from NGX by its
ingestion layer on 2026-08-24). `calibration/output/calibration.json`
records, for every number, the module call and cache file it came from.

Three symbols spanning liquidity regimes, chosen from a survey of the full
147-name universe. A real thin-market finding from that survey: over the
last year **only 8 of the 35 most-traded names have a valid Roll spread
estimate** (negative return autocovariance) — on the rest, infrequent
trading and momentum swamp bid-ask bounce, exactly the failure mode
NGXDash's methodology page documents. The three below all pass that filter
with full 251-day coverage:

| symbol | regime | last price | Roll spread | realized vol (ann., 63d) | ADV (shares) | implied trades/day |
|---|---|---|---|---|---|---|
| GTCO | liquid | ₦127.70 | 1.02% | 0.377 | 23.5M | ~2,354 |
| OANDO | mid | ₦35.05 | 1.55% | 0.527 | 6.3M | ~630 |
| LIVESTOCK | thin | ₦7.95 | 2.53% | 0.643 | 1.9M | ~193 |

Mapping to simulation units: volatility is de-annualized to a per-√second
tick diffusion; the Roll spread becomes the background book's half-spread
in ticks; arrival rate λ = ADV / mean-deal-size / session-seconds (NGX
session 10:00–14:20 = 15,600 s).

**The one assumption:** the NGXDash cache stores daily share volume but not
deal counts, so the arrival rate is *implied* via an assumed mean deal size
of 10,000 shares — the same for all symbols, order-of-magnitude checked
against published NGX deal counts, and flagged as an assumption (not a
measurement) in `calibration.json`.

```
/Users/salisdalhatu/NGXDash/.venv/bin/python calibration/calibrate.py
```

## Phase 2 — Synthetic order flow, unit-tested before use

`include/mmsim/flow.hpp`, one seeded RNG whose draw sequence depends only
on the deterministic event schedule — never on book state — so a seed fully
determines the flow stream regardless of which strategy is trading
(the basis for seed-paired comparisons):

1. **Fundamental price**: arithmetic random walk in ticks, diffusion from
   the calibrated realized volatility.
2. **Market orders**: Poisson arrivals at the calibrated λ; fair-coin side;
   Exponential sizes with the calibrated mean. All market orders are fully
   aggressive; passivity in the market is represented by:
3. **Background liquidity providers**: every 5 s they repost a 5-level grid
   around the fundamental with the touch at the calibrated Roll
   half-spread — so the market's "natural" spread equals the measured one
   by construction.

The flow feeds `matcheng::VecBook` directly (linked from
`../MatchEng/include`, not reimplemented): fills, partial fills and queue
position are the real engine's behavior.

Tested alone before being trusted (`tests/test_flow.cpp`, **1,510 checks,
0 failures**): realized arrival rate within 4σ of the Poisson target,
size mean within 5% and exponential tail within tolerance, fair sides,
realized fundamental variance within 5% of σ², background touch exactly at
the calibrated spread, no stale levels, byte-identical streams per seed.

## Phase 3 — Baseline (the control, taken seriously)

`BaselineMM`: quotes one calibrated Roll half-spread either side of the
**last trade price**, fixed 1-lot size (10,000 shares), requoted every 5 s.
No inventory awareness, no clock awareness. It quotes the same economic
spread the market itself displays — a fair naive policy, not a strawman.

## Phase 4 — Avellaneda-Stoikov (2008)

`AvellanedaStoikovMM` implements the closed forms from Avellaneda &
Stoikov, *"High-frequency trading in a limit order book"*, Quantitative
Finance 8(3), cited next to the formulas in `include/mmsim/strategy.hpp`:

- reservation price (eq. 29): `r = s − q·γ·σ²·(T−t)`
- optimal total spread (eq. 30): `γ·σ²·(T−t) + (2/γ)·ln(1 + γ/k)`

**k is measured, not assumed**: resting probe orders are placed in the
calibrated flow at six distances from the fundamental, their fill intensity
λ(δ) is measured over 16 pooled sessions per distance, and
`ln λ(δ) = ln A − k·δ` is fit by least squares (`results/probe_fits.csv`).
Fitted per symbol: GTCO k=0.058/tick, OANDO k=0.155/tick, LIVESTOCK
k=0.546/tick.

**γ is set by one uniform rule** (no per-symbol tuning): one lot of
inventory at session start skews the reservation price by exactly the
calibrated half-spread, i.e. `γ = h/(σ²T)`.

Before any simulation output was trusted, the implementation was validated
against the model's own theory as **explicit assertions**
(`tests/test_strategies.cpp`, 26 checks, 0 failures): reservation price
skews *away from* inventory in the correct direction and monotonically in
|q|, γ, σ², and horizon; spread widens with variance and horizon, tightens
with k, and collapses to the pure liquidity term at T; both quotes shift
together under inventory; skew vanishes at the terminal time.

## Phase 5 — Head-to-head, 50 paired seeds per symbol

Per symbol: 50 seeds × {baseline, A-S}, **same seed → identical flow
stream** for both strategies (verified by test). One episode = one full
NGX session. `make run`, then
`analysis/analyze.py` (full distributions in `results/summary.txt`).

**PnL per session (NGN), across 50 seeds — distributions, not just means:**

| symbol | baseline mean ± sd | A-S mean ± sd | mean diff | A-S wins | paired t | Wilcoxon |
|---|---|---|---|---|---|---|
| GTCO | 807k ± 375k | 2,339k ± 124k | +1,532k | **50/50** | p=1.0e-31 | p=1.8e-15 |
| OANDO | 97k ± 46k | 241k ± 31k | +144k | **50/50** | p=2.8e-24 | p=1.8e-15 |
| LIVESTOCK | 9.0k ± 8.5k | 23.6k ± 6.1k | +14.6k | **45/50** | p=3.3e-13 | p=1.6e-12 |

**Risk and activity (means over 50 seeds):**

| symbol | max \|inv\| (lots) B / A-S | inv std (lots) B / A-S | fills B / A-S | quote-to-trade B / A-S |
|---|---|---|---|---|
| GTCO | 20.7 / **13.6** | 6.2 / **2.6** | 1,118 / 1,770 | 5.6 / 3.5 |
| OANDO | 8.3 / **8.0** | 2.6 / **2.0** | 315 / 496 | 19.9 / 12.6 |
| LIVESTOCK | **4.7** / 5.8 | **1.5** / 1.7 | 95 / 154 | 66.2 / 40.8 |

The mixed parts, reported as found:

- **On the thin symbol the result is not a clean sweep**: A-S loses on 5 of
  50 seeds (paired-difference 5th percentile is −₦217), and — unlike on the
  liquid names — A-S carries *more* inventory risk there than the baseline
  (max 5.8 vs 4.7 lots), because with ~193 trades/day its tighter quotes
  buy inventory faster than the skew can recycle it. The baseline's lower
  inventory on LIVESTOCK is partly starvation, not discipline: it simply
  rarely trades (7 of its 50 sessions end negative; A-S: none).
- **Why A-S wins mechanically**: its optimal half-spread (GTCO: ~50 ticks
  at the open shrinking to ~17 by the close) sits *inside* the calibrated
  touch (65 ticks), so it stands first in queue and captures more flow at a
  still-positive margin, while the inventory skew keeps recycling position
  (inventory std 2.6 vs 6.2 lots on GTCO). The baseline quotes at the
  market's own spread distance from a stale last-trade price — often behind
  the background queue, and with no mechanism to shed inventory.
- **Both strategies are profitable on average.** That is a property of the
  model world, not evidence of real-world profit: the synthetic flow is
  uninformed noise, so market-making earns spread with no adverse
  selection tax; the strategy is also the only *strategic* quoter competing
  against mechanical background providers, capturing an unrealistically
  large share of flow (A-S fills ~10.8M shares/session on GTCO vs the
  symbol's real 23.5M ADV). Only the *relative* comparison is the finding.

## Phase 6 — Verification

- **Reconciliation against the engine's own trade log** (zero silent
  failures): every episode keeps the full `matcheng::Trade` log; at episode
  end the strategy's cash and inventory are independently rebuilt from that
  log alone (exact integer ticks × shares) and must equal the online
  accounting to the unit — no phantom fills, no double counting. **All 600
  episodes (2 calibrations × 300) reconciled**; any failure exits non-zero.
- **Determinism**: same seed + strategy reproduces byte-identical episodes;
  same seed across strategies yields the identical flow stream (tested).
- **Fresh-calibration robustness**: the NGXDash cache was pulled the same
  day this experiment ran, so a re-fetch would return identical daily bars;
  the snapshot-sensitivity concern is instead tested by **recalibrating on
  a 6-month window** (materially different inputs: GTCO Roll 1.48% vs
  1.02%, LIVESTOCK 1.89% vs 2.53%, different λ) and re-running everything
  (probe fits included). The conclusion is unchanged:

  | symbol | mean diff (6m calib) | A-S wins | paired t |
  |---|---|---|---|
  | GTCO | +₦1,263k | 50/50 | p=2.0e-27 |
  | OANDO | +₦130k | 50/50 | p=1.6e-21 |
  | LIVESTOCK | +₦19.1k | 49/50 | p=5.8e-16 |

## Limitations (each one deliberate, none hidden)

- **No informed flow**: market-order sides are a fair coin, uncorrelated
  with future fundamental moves — no adverse selection. This inflates both
  strategies' absolute PnL and likely flatters tight quoting (A-S) more.
- **Deal size assumed** (10,000 shares), because the NGXDash cache has no
  deal counts; λ inherits that assumption.
- **k from first-touch probes**: the fitted fill intensity measures
  time-to-first-execution of a small resting probe, a proxy for the paper's
  full-fill intensity.
- **γ set by a rule**, not optimized — and the baseline has no tunable
  advantage either; neither strategy was tuned on outcomes.
- 5-second requote grid for everyone (background and strategies); flat
  ₦0.01 tick (NGX's banded tick rules not modeled); one strategic MM at a
  time; PnL marked to final mid rather than liquidated; single-day
  episodes.

## Reproduce

```
/Users/salisdalhatu/NGXDash/.venv/bin/python calibration/calibrate.py
make test     # flow-generator + strategy-theory suites (1,536 checks)
make run      # probe fits + 50 seed-paired episodes x 3 symbols
/Users/salisdalhatu/NGXDash/.venv/bin/python analysis/analyze.py
```

Requires clang++ (C++20), make, and the sibling `../MatchEng` and
`../NGXDash` checkouts. ~seconds to run end-to-end.

## Layout

```
calibration/calibrate.py     Phase 1: NGXDash modules -> calibration.json/.cfg (provenance per number)
include/mmsim/config.hpp     calibrated-parameter loading
include/mmsim/flow.hpp       Phase 2: calibrated synthetic flow + background book
include/mmsim/strategy.hpp   Phases 3-4: baseline + A-S closed forms (paper cited inline)
include/mmsim/sim.hpp        episode runner, probe mode, exact accounting + reconciliation
src/run_experiment.cpp       Phase 5 driver: k-fit, seed-paired head-to-head
tests/                       Phase 2/4 validation suites (run before results were trusted)
analysis/analyze.py          paired distributions + t / Wilcoxon tests
results/                     results.csv, probe_fits.csv, summary.*  (+ alt_6m/ sensitivity run)
```
