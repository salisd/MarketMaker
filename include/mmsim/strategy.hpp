// Phase 3 (baseline) and Phase 4 (Avellaneda-Stoikov) quoting policies.
//
// Both are pure functions of observable state (time, book mid, last trade,
// own inventory) to a two-sided quote; the simulator owns order placement.
// The closed-form A-S pieces are exposed as free functions so the theoretical
// property tests (tests/test_strategies.cpp) can assert on them directly.
#pragma once
#include "config.hpp"
#include "flow.hpp"

#include <algorithm>
#include <cmath>

namespace mmsim {

struct Quote {
    Price bid = 0;
    Price ask = 0;
    Qty size = 0;   // shares per side
};

class Strategy {
public:
    virtual ~Strategy() = default;
    virtual const char* name() const = 0;
    // mid/last_trade in ticks; inventory in lots (1 lot = one quote size).
    virtual Quote quote(double t, double mid_ticks, double last_trade_ticks,
                        double inventory_lots) = 0;
};

// --- Phase 3: naive symmetric market maker ---------------------------------
// Quotes a fixed distance (the calibrated Roll half-spread) either side of the
// LAST TRADE price. No inventory awareness, no time awareness. This is the
// control group: the same information the market's own touch spread embodies,
// applied blindly.
class BaselineMM : public Strategy {
public:
    BaselineMM(const SymbolConfig& cfg, Qty lot)
        : half_((Price)std::max<int64_t>(1, std::llround(cfg.half_spread_ticks))),
          lot_(lot) {}

    const char* name() const override { return "baseline"; }

    Quote quote(double, double mid_ticks, double last_trade_ticks,
                double) override {
        double ref = last_trade_ticks > 0 ? last_trade_ticks : mid_ticks;
        Price r = (Price)std::llround(ref);
        return {r - half_, r + half_, lot_};
    }

private:
    Price half_;
    Qty lot_;
};

// --- Phase 4: Avellaneda & Stoikov (2008) ----------------------------------
// "High-frequency trading in a limit order book", Quantitative Finance 8(3),
// pp. 217-224. Closed-form quotes for the exponential-utility market maker
// with terminal horizon T against Poisson fill intensity lambda(d)=A*e^{-k d}:
//
//   reservation price (eq. 29):  r(s,q,t) = s - q * gamma * sigma^2 * (T - t)
//   optimal total spread (eq. 30):
//       delta_a + delta_b = gamma * sigma^2 * (T - t) + (2/gamma) * ln(1 + gamma/k)
//
// with quotes at r +/- half the optimal spread. Units here: prices in ticks,
// sigma^2 in ticks^2 per second, (T-t) in seconds, inventory q in lots,
// gamma in 1/(tick * lot), k in 1/tick (estimated from probe fills in the
// calibrated flow — see ProbeRunner in sim.hpp).

inline double as_reservation_price(double mid, double q_lots, double gamma,
                                   double sigma2_per_sec, double tau_sec) {
    return mid - q_lots * gamma * sigma2_per_sec * tau_sec;   // A-S eq. (29)
}

inline double as_optimal_half_spread(double gamma, double sigma2_per_sec,
                                     double tau_sec, double k) {
    return 0.5 * gamma * sigma2_per_sec * tau_sec +
           (1.0 / gamma) * std::log(1.0 + gamma / k);         // A-S eq. (30)/2
}

class AvellanedaStoikovMM : public Strategy {
public:
    // gamma rule (documented in README): one lot of inventory at session start
    // skews the reservation price by exactly the calibrated Roll half-spread:
    //   gamma = half_spread / (sigma^2 * T).
    // The same dimensionless rule for every symbol — no per-symbol tuning.
    AvellanedaStoikovMM(const SymbolConfig& cfg, Qty lot, double k)
        : sigma2_(cfg.sigma_sqrt_sec_ticks * cfg.sigma_sqrt_sec_ticks),
          T_(cfg.session_seconds),
          gamma_(cfg.half_spread_ticks / (sigma2_ * cfg.session_seconds)),
          k_(k),
          lot_(lot) {}

    const char* name() const override { return "avellaneda_stoikov"; }

    double gamma() const { return gamma_; }

    Quote quote(double t, double mid_ticks, double,
                double inventory_lots) override {
        double tau = std::max(0.0, T_ - t);
        double r = as_reservation_price(mid_ticks, inventory_lots, gamma_,
                                        sigma2_, tau);
        double d = as_optimal_half_spread(gamma_, sigma2_, tau, k_);
        Price bid = (Price)std::llround(r - d);
        Price ask = (Price)std::llround(r + d);
        if (ask <= bid) ask = bid + 1;   // enforce a one-tick minimum spread
        return {std::max<Price>(1, bid), ask, lot_};
    }

private:
    double sigma2_;  // ticks^2 per second
    double T_;       // seconds
    double gamma_;   // 1/(tick*lot)
    double k_;       // 1/tick
    Qty lot_;
};

}  // namespace mmsim
