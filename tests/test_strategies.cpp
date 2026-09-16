// Phase 4 validation: the Avellaneda-Stoikov implementation is checked against
// the model's OWN theoretical properties before any simulation output is
// trusted (explicit assertions, not visual sanity checks), plus baseline
// behavior and an end-to-end determinism check of the full simulator.
#include <mmsim/config.hpp>
#include <mmsim/sim.hpp>
#include <mmsim/strategy.hpp>

#include <cmath>
#include <cstdio>

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

// --- closed-form properties (A-S 2008, eqs. 29-30) -------------------------

static void test_reservation_price_skew() {
    const double mid = 10'000, gamma = 1e-3, sig2 = 4.0, tau = 5'000;
    // Long inventory -> reservation BELOW mid (lean to sell); short -> above.
    CHECK(as_reservation_price(mid, +2, gamma, sig2, tau) < mid,
          "long inventory must skew reservation price down");
    CHECK(as_reservation_price(mid, -2, gamma, sig2, tau) > mid,
          "short inventory must skew reservation price up");
    CHECK(as_reservation_price(mid, 0, gamma, sig2, tau) == mid,
          "flat inventory must not skew");
    // Skew grows with |q|, gamma, sigma^2 and remaining horizon.
    double s1 = mid - as_reservation_price(mid, 1, gamma, sig2, tau);
    CHECK(mid - as_reservation_price(mid, 3, gamma, sig2, tau) > s1,
          "skew must grow with inventory");
    CHECK(mid - as_reservation_price(mid, 1, 2 * gamma, sig2, tau) > s1,
          "skew must grow with risk aversion");
    CHECK(mid - as_reservation_price(mid, 1, gamma, 2 * sig2, tau) > s1,
          "skew must grow with variance");
    CHECK(mid - as_reservation_price(mid, 1, gamma, sig2, 2 * tau) > s1,
          "skew must grow with remaining horizon");
    // At the terminal time the skew vanishes.
    CHECK(as_reservation_price(mid, 5, gamma, sig2, 0) == mid,
          "skew must vanish at tau=0");
}

static void test_optimal_spread() {
    const double gamma = 1e-3, sig2 = 4.0, tau = 5'000, k = 0.02;
    double d = as_optimal_half_spread(gamma, sig2, tau, k);
    CHECK(d > 0, "half-spread must be positive");
    CHECK(as_optimal_half_spread(gamma, 2 * sig2, tau, k) > d,
          "spread must widen with variance");
    CHECK(as_optimal_half_spread(gamma, sig2, 2 * tau, k) > d,
          "spread must widen with remaining horizon");
    CHECK(as_optimal_half_spread(gamma, sig2, tau, 2 * k) < d,
          "spread must tighten when fills decay slower (larger k)");
    // At tau=0 only the liquidity term (1/gamma) ln(1+gamma/k) remains.
    double d0 = as_optimal_half_spread(gamma, sig2, 0, k);
    CHECK(std::abs(d0 - std::log(1 + gamma / k) / gamma) < 1e-12,
          "tau=0 spread must equal the pure liquidity term");
}

// --- quoting behavior through the Strategy interface -----------------------

static void test_as_quotes() {
    auto cfg = test_cfg();
    AvellanedaStoikovMM as(cfg, 10'000, /*k=*/0.02);
    const double mid = 10'000, t = 1'000;
    Quote flat = as.quote(t, mid, mid, 0);
    Quote lng = as.quote(t, mid, mid, +2);
    Quote shrt = as.quote(t, mid, mid, -2);
    CHECK(flat.bid < mid && flat.ask > mid, "flat quotes must straddle mid");
    CHECK((flat.bid + flat.ask) / 2.0 - mid == 0,
          "flat quotes must be symmetric");
    CHECK(lng.bid < flat.bid && lng.ask < flat.ask,
          "long inventory must shift BOTH quotes down");
    CHECK(shrt.bid > flat.bid && shrt.ask > flat.ask,
          "short inventory must shift BOTH quotes up");
    // gamma rule: one lot at t=0 skews reservation by the calibrated
    // half-spread.
    double skew = cfg.half_spread_ticks;
    double r = as_reservation_price(mid, 1, as.gamma(),
                                    cfg.sigma_sqrt_sec_ticks *
                                        cfg.sigma_sqrt_sec_ticks,
                                    cfg.session_seconds);
    CHECK(std::abs((mid - r) - skew) < 1e-9,
          "gamma rule: skew %.6f vs half-spread %.6f", mid - r, skew);
    Quote late = as.quote(cfg.session_seconds, mid, mid, 0);
    CHECK(late.ask - late.bid <= flat.ask - flat.bid,
          "spread must not widen as the horizon shrinks");
}

static void test_baseline_quotes() {
    auto cfg = test_cfg();
    BaselineMM base(cfg, 10'000);
    Quote q = base.quote(0, 10'000, 9'980, 0);
    CHECK(q.bid == 9'980 - 50 && q.ask == 9'980 + 50,
          "baseline must quote half-spread around LAST TRADE");
    Quote q5 = base.quote(0, 10'000, 9'980, 5);
    CHECK(q5.bid == q.bid && q5.ask == q.ask,
          "baseline must ignore inventory");
    Quote qm = base.quote(0, 10'000, 0, 0);
    CHECK(qm.bid == 10'000 - 50 && qm.ask == 10'000 + 50,
          "baseline must fall back to mid before any trade");
}

// --- end-to-end: full simulator determinism and flow-pairing ---------------

static void test_simulator_determinism() {
    auto cfg = test_cfg();
    cfg.session_seconds = 2'000;  // short episode is enough
    BaselineMM b1(cfg, 10'000), b2(cfg, 10'000);
    Simulator s1(cfg, 7), s2(cfg, 7);
    EpisodeResult r1 = s1.run(&b1), r2 = s2.run(&b2);
    CHECK(r1.pnl_ngn == r2.pnl_ngn && r1.mm_fill_events == r2.mm_fill_events &&
              r1.engine_trades == r2.engine_trades,
          "same seed + strategy must reproduce identical episodes");
    CHECK(r1.reconciled, "episode must reconcile against engine trade log");

    // Same seed, different strategy: the FLOW stream must be identical
    // (market order count is flow-only), even though outcomes differ.
    AvellanedaStoikovMM as(cfg, 10'000, 0.02);
    Simulator s3(cfg, 7);
    EpisodeResult r3 = s3.run(&as);
    CHECK(r3.market_order_count == r1.market_order_count,
          "flow stream must be strategy-independent for a given seed");
    CHECK(r3.reconciled, "A-S episode must reconcile");
}

int main() {
    test_reservation_price_skew();
    test_optimal_spread();
    test_as_quotes();
    test_baseline_quotes();
    test_simulator_determinism();
    std::printf("test_strategies: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
