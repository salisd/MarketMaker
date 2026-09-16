// Synthetic order flow calibrated from NGXDash measurements (Phase 2).
//
// Three parts, all driven by ONE RNG whose draw sequence depends only on the
// seed and the deterministic event schedule — never on book state or strategy
// behavior. That is what makes seed-paired strategy comparisons valid: for a
// given seed, both strategies face the byte-identical flow stream.
//
//  1. Fundamental price F_t: arithmetic random walk in ticks,
//     dF = sigma_sqrt_sec * sqrt(dt) * N(0,1), sigma calibrated from
//     NGXDash realized volatility (de-annualized to per-second).
//  2. Market orders: Poisson arrivals at the calibrated rate; side fair-coin;
//     size ~ Exponential with the calibrated mean, rounded up to >= 1 share.
//     All market orders are fully aggressive (plain add_market); passivity in
//     the market is represented by the background providers instead.
//  3. Background liquidity providers: at every refresh tick they repost a
//     fixed grid around F — best quotes at the calibrated Roll half-spread,
//     deeper levels behind. They give the book its resting depth and keep the
//     touch spread equal to the measured Roll spread by construction.
#pragma once
#include "config.hpp"

#include <matcheng/book.hpp>

#include <cmath>
#include <random>
#include <vector>

namespace mmsim {

using matcheng::Price;
using matcheng::Qty;
using matcheng::OrderId;
using matcheng::Side;
using matcheng::Trade;
using Book = matcheng::VecBook;

// Order-id ranges: lets accounting attribute any Trade to its owner.
constexpr OrderId kBgIdBase = 1'000'000'000ULL;      // background providers
constexpr OrderId kMktIdBase = 2'000'000'000ULL;     // synthetic market orders
constexpr OrderId kMmIdBase = 3'000'000'000ULL;      // the strategy under test

inline bool is_mm_id(OrderId id) { return id >= kMmIdBase; }

struct MarketOrder {
    double time;
    Side side;
    Qty qty;
};

class Flow {
public:
    Flow(const SymbolConfig& cfg, uint64_t seed)
        : cfg_(cfg), rng_(seed), fundamental_(double(cfg.initial_price_ticks)) {
        next_mkt_ = draw_market_order(0.0);
    }

    // Advance the fundamental to time t (one normal draw per call).
    void advance(double t) {
        if (t > t_) {
            fundamental_ += cfg_.sigma_sqrt_sec_ticks * std::sqrt(t - t_) *
                            normal_(rng_);
            t_ = t;
        }
    }

    double fundamental() const { return fundamental_; }
    const MarketOrder& pending_market_order() const { return next_mkt_; }

    // Consume the pending market order and draw the next one.
    MarketOrder pop_market_order() {
        MarketOrder m = next_mkt_;
        next_mkt_ = draw_market_order(m.time);
        return m;
    }

    // Cancel-and-repost the background grid around the current fundamental.
    // Called at every refresh tick. Level sizes are deterministic so the RNG
    // stream stays independent of book state.
    void refresh_background(Book& book, std::vector<Trade>& trades) {
        for (OrderId id : bg_ids_) book.cancel(id);  // ignore already-filled
        bg_ids_.clear();
        const Price mid = (Price)std::llround(fundamental_);
        const Price h = (Price)std::llround(cfg_.half_spread_ticks);
        const Price gap = std::max<Price>(1, (Price)std::llround(cfg_.half_spread_ticks / 2.0));
        const Qty lvl_qty = (Qty)std::llround(3.0 * cfg_.mean_trade_shares);
        for (int i = 0; i < kBgLevels; ++i) {
            Price bid = mid - std::max<Price>(1, h) - (Price)i * gap;
            Price ask = mid + std::max<Price>(1, h) + (Price)i * gap;
            if (bid > 0) {
                OrderId id = next_bg_id_++;
                book.add_limit(id, Side::Buy, bid, lvl_qty, trades);
                bg_ids_.push_back(id);
            }
            OrderId id = next_bg_id_++;
            book.add_limit(id, Side::Sell, ask, lvl_qty, trades);
            bg_ids_.push_back(id);
        }
    }

    static constexpr int kBgLevels = 5;

private:
    MarketOrder draw_market_order(double now) {
        double dt = std::exponential_distribution<double>(
            cfg_.lambda_per_sec)(rng_);
        Side side = (rng_() & 1) ? Side::Buy : Side::Sell;
        double raw = std::exponential_distribution<double>(
            1.0 / cfg_.mean_trade_shares)(rng_);
        Qty qty = (Qty)std::max(1.0, std::ceil(raw));
        return {now + dt, side, qty};
    }

    SymbolConfig cfg_;
    std::mt19937_64 rng_;
    std::normal_distribution<double> normal_{0.0, 1.0};
    double fundamental_;
    double t_ = 0.0;
    MarketOrder next_mkt_{};
    OrderId next_bg_id_ = kBgIdBase;
    std::vector<OrderId> bg_ids_;
};

}  // namespace mmsim
