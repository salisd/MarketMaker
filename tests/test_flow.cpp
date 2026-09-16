// Phase 2 unit tests: the synthetic flow generator alone, BEFORE it is trusted
// against the engine. Confirms the realized arrival rate, order-size
// distribution, and fundamental-price volatility match the calibration targets
// within statistical tolerance, and that the background book reproduces the
// calibrated touch spread by construction.
#include <mmsim/config.hpp>
#include <mmsim/flow.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>

static int checks = 0, failures = 0;
#define CHECK(cond, ...)                                        \
    do {                                                        \
        ++checks;                                               \
        if (!(cond)) {                                          \
            ++failures;                                         \
            std::printf("FAIL %s:%d  ", __FILE__, __LINE__);    \
            std::printf(__VA_ARGS__);                           \
            std::printf("\n");                                  \
        }                                                       \
    } while (0)

using namespace mmsim;

static SymbolConfig test_cfg() {
    SymbolConfig c;
    c.symbol = "TEST";
    c.initial_price_ticks = 10'000;
    c.tick_size_ngn = 0.01;
    c.sigma_sqrt_sec_ticks = 2.0;
    c.sigma_day_rel = 0.02;
    c.half_spread_ticks = 50.0;
    c.lambda_per_sec = 0.10;
    c.mean_trade_shares = 10'000;
    c.session_seconds = 15'600;
    return c;
}

// Arrival count over many sessions is Poisson(lambda * T * n); the realized
// rate must sit within 4 standard deviations of the calibrated target.
static void test_arrival_rate() {
    auto cfg = test_cfg();
    const int n_sessions = 40;
    uint64_t n = 0;
    for (int s = 0; s < n_sessions; ++s) {
        Flow flow(cfg, 100 + (uint64_t)s);
        while (flow.pending_market_order().time < cfg.session_seconds) {
            flow.pop_market_order();
            ++n;
        }
    }
    double expected = cfg.lambda_per_sec * cfg.session_seconds * n_sessions;
    double sd = std::sqrt(expected);
    CHECK(std::abs(double(n) - expected) < 4 * sd,
          "arrival count %llu vs expected %.0f (sd %.1f)",
          (unsigned long long)n, expected, sd);
}

// Sizes are Exponential(mean): check realized mean and the exponential tail
// P(X > 2*mean) = e^-2.
static void test_size_distribution() {
    auto cfg = test_cfg();
    double sum = 0, n = 0, tail = 0;
    for (int s = 0; s < 40; ++s) {
        Flow flow(cfg, 200 + (uint64_t)s);
        while (flow.pending_market_order().time < cfg.session_seconds) {
            auto m = flow.pop_market_order();
            sum += double(m.qty);
            tail += m.qty > 2 * cfg.mean_trade_shares;
            ++n;
        }
    }
    double mean = sum / n;
    CHECK(std::abs(mean / cfg.mean_trade_shares - 1.0) < 0.05,
          "mean size %.0f vs target %.0f", mean, cfg.mean_trade_shares);
    double p_tail = tail / n;
    CHECK(std::abs(p_tail - std::exp(-2.0)) < 0.02,
          "P(size > 2*mean) = %.4f vs e^-2 = %.4f", p_tail, std::exp(-2.0));
}

// Sides are a fair coin.
static void test_side_balance() {
    auto cfg = test_cfg();
    double buys = 0, n = 0;
    for (int s = 0; s < 40; ++s) {
        Flow flow(cfg, 300 + (uint64_t)s);
        while (flow.pending_market_order().time < cfg.session_seconds) {
            buys += flow.pop_market_order().side == Side::Buy;
            ++n;
        }
    }
    CHECK(std::abs(buys / n - 0.5) < 4 * 0.5 / std::sqrt(n),
          "buy fraction %.4f", buys / n);
}

// Fundamental path: realized per-second variance of increments over a fixed
// grid must match sigma^2 within tolerance.
static void test_fundamental_volatility() {
    auto cfg = test_cfg();
    const double dt = 5.0;
    double sumsq = 0, n = 0;
    for (int s = 0; s < 20; ++s) {
        Flow flow(cfg, 400 + (uint64_t)s);
        double prev = flow.fundamental();
        for (double t = dt; t <= cfg.session_seconds; t += dt) {
            flow.advance(t);
            double d = flow.fundamental() - prev;
            prev = flow.fundamental();
            sumsq += d * d;
            ++n;
        }
    }
    double var_per_sec = sumsq / n / dt;
    double target = cfg.sigma_sqrt_sec_ticks * cfg.sigma_sqrt_sec_ticks;
    CHECK(std::abs(var_per_sec / target - 1.0) < 0.05,
          "realized variance %.3f ticks^2/s vs target %.3f", var_per_sec,
          target);
}

// Background grid: after a refresh the engine book's touch spread equals
// 2 * round(half_spread) around the fundamental, with depth on both sides.
static void test_background_book() {
    auto cfg = test_cfg();
    Flow flow(cfg, 500);
    Book book;
    std::vector<Trade> trades;
    flow.advance(1000.0);
    flow.refresh_background(book, trades);
    CHECK(trades.empty(), "background repost must not self-trade");
    auto bb = book.best_bid(), ba = book.best_ask();
    CHECK(bb && ba, "book must be two-sided after refresh");
    Price mid = (Price)std::llround(flow.fundamental());
    Price h = (Price)std::llround(cfg.half_spread_ticks);
    CHECK(*bb == mid - h && *ba == mid + h,
          "touch %lld/%lld vs expected %lld/%lld", (long long)*bb,
          (long long)*ba, (long long)(mid - h), (long long)(mid + h));
    CHECK(book.level_count(Side::Buy) == Flow::kBgLevels &&
              book.level_count(Side::Sell) == Flow::kBgLevels,
          "expected %d levels per side", Flow::kBgLevels);
    // Refreshing again after a move leaves no stale levels behind.
    flow.advance(2000.0);
    flow.refresh_background(book, trades);
    CHECK(book.level_count(Side::Buy) == Flow::kBgLevels &&
              book.level_count(Side::Sell) == Flow::kBgLevels,
          "stale background levels not cleaned up");
}

// A seed fully determines the flow stream (arrival times, sides, sizes,
// fundamental path) — the property seed-pairing relies on.
static void test_determinism() {
    auto cfg = test_cfg();
    Flow a(cfg, 42), b(cfg, 42);
    for (int i = 0; i < 500; ++i) {
        double t = a.pending_market_order().time;
        CHECK(t == b.pending_market_order().time, "arrival time diverged");
        a.advance(t);
        b.advance(t);
        CHECK(a.fundamental() == b.fundamental(), "fundamental diverged");
        auto ma = a.pop_market_order(), mb = b.pop_market_order();
        CHECK(ma.side == mb.side && ma.qty == mb.qty, "order diverged");
    }
}

int main() {
    test_arrival_rate();
    test_size_distribution();
    test_side_balance();
    test_fundamental_volatility();
    test_background_book();
    test_determinism();
    std::printf("test_flow: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
