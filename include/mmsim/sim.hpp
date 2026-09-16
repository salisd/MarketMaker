// Episode simulator (Phases 2-6 glue).
//
// One episode = one NGX trading session (T = 15,600 s) of calibrated synthetic
// flow against the REAL matcheng book (matcheng::VecBook — the same
// price-time-priority engine, linked, not reimplemented). The strategy under
// test posts genuine limit orders; fills, partial fills and queue position are
// whatever the engine decides.
//
// Event schedule: requote ticks every kRequoteDt seconds (background grid
// refresh + strategy requote) interleaved with Poisson market-order arrivals.
// The flow RNG never observes book state, so a seed fully determines the flow
// stream independent of the strategy — the basis for seed-paired comparisons.
//
// Accounting is exact integer arithmetic (ticks x shares). At episode end the
// online accounting is INDEPENDENTLY recomputed from the engine's own trade
// log and must match to the unit (reconciled flag; Phase 6).
#pragma once
#include "config.hpp"
#include "flow.hpp"
#include "strategy.hpp"

#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace mmsim {

constexpr double kRequoteDt = 5.0;  // seconds between requote/refresh ticks

struct EpisodeResult {
    std::string symbol;
    std::string strategy;
    uint64_t seed = 0;
    double pnl_ngn = 0;               // cash + inventory marked at final mid
    double final_inventory_lots = 0;
    double max_abs_inventory_lots = 0;
    double inventory_std_lots = 0;    // std of inventory sampled every tick
    uint64_t mm_fill_events = 0;      // engine Trade records involving the MM
    double mm_filled_shares = 0;
    double mm_buy_shares = 0;
    double mm_sell_shares = 0;
    uint64_t quotes_posted = 0;       // MM limit orders submitted
    uint64_t engine_trades = 0;       // all Trade records in the episode
    double market_order_count = 0;
    bool reconciled = false;
};

class Simulator {
public:
    Simulator(const SymbolConfig& cfg, uint64_t seed)
        : cfg_(cfg), flow_(cfg, seed), seed_(seed) {
        book_.set_clock(&zero_clock);  // deterministic: seq numbers only
    }

    EpisodeResult run(Strategy* strat) {
        const double T = cfg_.session_seconds;
        const Qty lot = (Qty)std::llround(cfg_.mean_trade_shares);
        double next_tick = 0.0;

        while (true) {
            const double t_mkt = flow_.pending_market_order().time;
            const double t_next = std::min(next_tick, t_mkt);
            if (t_next >= T) break;
            flow_.advance(t_next);

            if (next_tick <= t_mkt) {
                // Requote tick: strategy pulls stale quotes, background grid
                // reposts around the moved fundamental, strategy requotes on
                // the fresh book.
                cancel_mm_quotes();
                trades_buf_.clear();
                flow_.refresh_background(book_, trades_buf_);
                absorb(trades_buf_);
                if (strat) place_mm_quotes(strat, t_next, lot);
                sample_inventory();
                next_tick += kRequoteDt;
            } else {
                MarketOrder m = flow_.pop_market_order();
                trades_buf_.clear();
                book_.add_market(next_mkt_id_++, m.side, m.qty, trades_buf_);
                absorb(trades_buf_);
                ++n_market_orders_;
            }
        }

        return finalize(strat);
    }

    // --- Probe mode (k estimation for A-S) ---------------------------------
    // Rests one small order per side at distance `delta_ticks` from the
    // fundamental, refreshed every tick. Returns the number of tick intervals
    // in which a probe received any execution, and the total probe exposure
    // (side-seconds). Fill intensity at delta ~= events / exposure.
    struct ProbeStats {
        uint64_t fill_events = 0;
        double exposure_side_seconds = 0;
    };

    ProbeStats run_probe(double delta_ticks) {
        const double T = cfg_.session_seconds;
        const Qty probe_qty = std::max<Qty>(
            1, (Qty)std::llround(cfg_.mean_trade_shares / 10.0));
        double next_tick = 0.0;
        ProbeStats st;
        OrderId bid_id = 0, ask_id = 0;
        Qty bid_rest = 0, ask_rest = 0;

        while (true) {
            const double t_mkt = flow_.pending_market_order().time;
            const double t_next = std::min(next_tick, t_mkt);
            if (t_next >= T) break;
            flow_.advance(t_next);

            if (next_tick <= t_mkt) {
                // A probe "fill event" = the order lost quantity since posted.
                for (auto [id, rest] : {std::pair{bid_id, bid_rest},
                                        std::pair{ask_id, ask_rest}}) {
                    if (!id) continue;
                    auto rem = book_.order_remaining(id);
                    if (!rem || *rem < rest) ++st.fill_events;
                }
                if (bid_id) book_.cancel(bid_id);
                if (ask_id) book_.cancel(ask_id);
                trades_buf_.clear();
                flow_.refresh_background(book_, trades_buf_);
                const double F = flow_.fundamental();
                Price bid = (Price)std::llround(F - delta_ticks);
                Price ask = (Price)std::llround(F + delta_ticks);
                trades_buf_.clear();
                bid_id = next_mm_id_++;
                ask_id = next_mm_id_++;
                // Probes must rest passively; if a probe would cross the book
                // it still counts through the lost-quantity check above.
                book_.add_limit(bid_id, Side::Buy, std::max<Price>(1, bid),
                                probe_qty, trades_buf_);
                book_.add_limit(ask_id, Side::Sell, ask, probe_qty, trades_buf_);
                bid_rest = book_.order_remaining(bid_id).value_or(0);
                ask_rest = book_.order_remaining(ask_id).value_or(0);
                // Immediate executions on posting count as fills too.
                for (const Trade& tr : trades_buf_)
                    if (is_mm_id(tr.aggressor_id)) { ++st.fill_events; break; }
                st.exposure_side_seconds += 2.0 * kRequoteDt;
                next_tick += kRequoteDt;
            } else {
                MarketOrder m = flow_.pop_market_order();
                trades_buf_.clear();
                book_.add_market(next_mkt_id_++, m.side, m.qty, trades_buf_);
            }
        }
        return st;
    }

private:
    static uint64_t zero_clock() { return 0; }

    void place_mm_quotes(Strategy* strat, double t, Qty lot) {
        double mid = current_mid();
        Quote q = strat->quote(t, mid, last_trade_px_, inventory_lots(lot));
        trades_buf_.clear();
        mm_bid_id_ = next_mm_id_++;
        mm_side_of_[mm_bid_id_] = Side::Buy;
        book_.add_limit(mm_bid_id_, Side::Buy, q.bid, q.size, trades_buf_);
        mm_ask_id_ = next_mm_id_++;
        mm_side_of_[mm_ask_id_] = Side::Sell;
        book_.add_limit(mm_ask_id_, Side::Sell, q.ask, q.size, trades_buf_);
        quotes_posted_ += 2;
        absorb(trades_buf_);
    }

    void cancel_mm_quotes() {
        if (mm_bid_id_) book_.cancel(mm_bid_id_);
        if (mm_ask_id_) book_.cancel(mm_ask_id_);
        mm_bid_id_ = mm_ask_id_ = 0;
    }

    double current_mid() {
        auto bb = book_.best_bid();
        auto ba = book_.best_ask();
        if (bb && ba) return 0.5 * (double(*bb) + double(*ba));
        return flow_.fundamental();
    }

    // Fold a batch of engine trades into the trade log + online MM accounting.
    void absorb(const std::vector<Trade>& trades) {
        for (const Trade& tr : trades) {
            log_.push_back(tr);
            apply_mm_trade(tr, cash_tick_shares_, inventory_shares_,
                           &mm_fill_events_, &mm_buy_shares_, &mm_sell_shares_);
        }
        if (!trades.empty()) last_trade_px_ = double(trades.back().price);
        double lots = std::abs(double(inventory_shares_)) /
                      cfg_.mean_trade_shares;
        if (lots > max_abs_inv_lots_) max_abs_inv_lots_ = lots;
    }

    // The single attribution rule, used by BOTH the online accounting and the
    // end-of-run reconciliation: a trade touches the MM iff one of its ids is
    // in the MM id range; the MM side comes from the recorded order side.
    void apply_mm_trade(const Trade& tr, int64_t& cash, int64_t& inv,
                        uint64_t* fills, double* bought, double* sold) const {
        OrderId mm = is_mm_id(tr.resting_id)     ? tr.resting_id
                     : is_mm_id(tr.aggressor_id) ? tr.aggressor_id
                                                 : 0;
        if (!mm) return;
        Side s = mm_side_of_.at(mm);
        int64_t notional = tr.price * (int64_t)tr.qty;
        if (s == Side::Buy) {
            cash -= notional;
            inv += (int64_t)tr.qty;
            if (bought) *bought += double(tr.qty);
        } else {
            cash += notional;
            inv -= (int64_t)tr.qty;
            if (sold) *sold += double(tr.qty);
        }
        if (fills) ++*fills;
    }

    double inventory_lots(Qty lot) const {
        return double(inventory_shares_) / double(lot);
    }

    void sample_inventory() {
        double lots = double(inventory_shares_) / cfg_.mean_trade_shares;
        inv_sum_ += lots;
        inv_sumsq_ += lots * lots;
        ++inv_n_;
    }

    EpisodeResult finalize(Strategy* strat) {
        EpisodeResult r;
        r.symbol = cfg_.symbol;
        r.strategy = strat ? strat->name() : "none";
        r.seed = seed_;

        // Phase 6: reconcile — rebuild MM cash/inventory from the engine's own
        // trade log with the same attribution rule and demand exact equality.
        int64_t cash2 = 0, inv2 = 0;
        for (const Trade& tr : log_)
            apply_mm_trade(tr, cash2, inv2, nullptr, nullptr, nullptr);
        r.reconciled = (cash2 == cash_tick_shares_) &&
                       (inv2 == inventory_shares_);

        double final_mid = current_mid();
        double pnl_ticks = double(cash_tick_shares_) +
                           double(inventory_shares_) * final_mid;
        r.pnl_ngn = pnl_ticks * cfg_.tick_size_ngn;
        r.final_inventory_lots = double(inventory_shares_) /
                                 cfg_.mean_trade_shares;
        r.max_abs_inventory_lots = max_abs_inv_lots_;
        if (inv_n_ > 1) {
            double mean = inv_sum_ / inv_n_;
            r.inventory_std_lots =
                std::sqrt(std::max(0.0, inv_sumsq_ / inv_n_ - mean * mean));
        }
        r.mm_fill_events = mm_fill_events_;
        r.mm_buy_shares = mm_buy_shares_;
        r.mm_sell_shares = mm_sell_shares_;
        r.mm_filled_shares = mm_buy_shares_ + mm_sell_shares_;
        r.quotes_posted = quotes_posted_;
        r.engine_trades = log_.size();
        r.market_order_count = double(n_market_orders_);
        return r;
    }

    SymbolConfig cfg_;
    Flow flow_;
    Book book_;
    uint64_t seed_;

    std::vector<Trade> trades_buf_;
    std::vector<Trade> log_;                       // full engine trade log
    std::unordered_map<OrderId, Side> mm_side_of_; // side of every MM order

    OrderId next_mkt_id_ = kMktIdBase;
    OrderId next_mm_id_ = kMmIdBase;
    OrderId mm_bid_id_ = 0, mm_ask_id_ = 0;

    int64_t cash_tick_shares_ = 0;   // exact, in ticks x shares
    int64_t inventory_shares_ = 0;
    double last_trade_px_ = 0;
    double max_abs_inv_lots_ = 0;
    double inv_sum_ = 0, inv_sumsq_ = 0;
    uint64_t inv_n_ = 0;
    uint64_t mm_fill_events_ = 0;
    double mm_buy_shares_ = 0, mm_sell_shares_ = 0;
    uint64_t quotes_posted_ = 0;
    uint64_t n_market_orders_ = 0;
};

}  // namespace mmsim
