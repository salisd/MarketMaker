// Phase 5 driver: probe-calibrate the A-S fill parameter k per symbol, then
// run baseline vs Avellaneda-Stoikov head-to-head on seed-paired flow streams.
//
//   ./build/run_experiment <cfg>... [--seeds N] [--out results/]
//
// Emits results/<probe_fits.csv, results.csv>; every row is one full episode
// against the real matcheng engine.
#include <mmsim/config.hpp>
#include <mmsim/flow.hpp>
#include <mmsim/sim.hpp>
#include <mmsim/strategy.hpp>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace mmsim;

namespace {

constexpr int kProbeSeeds = 16;          // sessions pooled per (symbol, delta)
constexpr uint64_t kProbeSeedBase = 10'000;  // disjoint from experiment seeds

struct ProbeFit {
    double A = 0;  // intensity at delta=0, per side-second
    double k = 0;  // decay per tick
    std::vector<double> deltas, lambdas;
};

// Measure lambda(delta) with resting probes inside the calibrated flow and
// fit ln lambda = ln A - k*delta by ordinary least squares.
ProbeFit fit_k(const SymbolConfig& cfg) {
    ProbeFit fit;
    const double h = cfg.half_spread_ticks;
    for (double mult : {0.5, 0.75, 1.0, 1.25, 1.5, 2.0}) {
        double delta = mult * h;
        uint64_t events = 0;
        double exposure = 0;
        for (int s = 0; s < kProbeSeeds; ++s) {
            Simulator sim(cfg, kProbeSeedBase + (uint64_t)s);
            auto st = sim.run_probe(delta);
            events += st.fill_events;
            exposure += st.exposure_side_seconds;
        }
        if (events > 0) {
            fit.deltas.push_back(delta);
            fit.lambdas.push_back(double(events) / exposure);
        }
    }
    if (fit.deltas.size() < 3)
        throw std::runtime_error(cfg.symbol +
                                 ": too few valid probe points to fit k");
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    const double n = double(fit.deltas.size());
    for (size_t i = 0; i < fit.deltas.size(); ++i) {
        double x = fit.deltas[i], y = std::log(fit.lambdas[i]);
        sx += x; sy += y; sxx += x * x; sxy += x * y;
    }
    double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
    double intercept = (sy - slope * sx) / n;
    fit.k = -slope;
    fit.A = std::exp(intercept);
    if (fit.k <= 0)
        throw std::runtime_error(cfg.symbol +
                                 ": probe intensities not decreasing in delta");
    return fit;
}

void write_row(std::ofstream& out, const EpisodeResult& r) {
    out << r.symbol << ',' << r.strategy << ',' << r.seed << ','
        << r.pnl_ngn << ',' << r.final_inventory_lots << ','
        << r.max_abs_inventory_lots << ',' << r.inventory_std_lots << ','
        << r.mm_fill_events << ',' << r.mm_filled_shares << ','
        << r.mm_buy_shares << ',' << r.mm_sell_shares << ','
        << r.quotes_posted << ',' << r.engine_trades << ','
        << r.market_order_count << ',' << (r.reconciled ? 1 : 0) << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> cfg_paths;
    int n_seeds = 50;
    std::string out_dir = "results";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--seeds" && i + 1 < argc) n_seeds = std::stoi(argv[++i]);
        else if (a == "--out" && i + 1 < argc) out_dir = argv[++i];
        else cfg_paths.push_back(a);
    }
    if (cfg_paths.empty()) {
        std::fprintf(stderr, "usage: %s <cfg>... [--seeds N] [--out DIR]\n",
                     argv[0]);
        return 2;
    }

    std::ofstream probes(out_dir + "/probe_fits.csv");
    probes << "symbol,delta_ticks,lambda_per_side_sec,fit_A,fit_k,gamma\n";
    std::ofstream results(out_dir + "/results.csv");
    results << "symbol,strategy,seed,pnl_ngn,final_inv_lots,max_abs_inv_lots,"
               "inv_std_lots,fill_events,filled_shares,buy_shares,sell_shares,"
               "quotes_posted,engine_trades,market_orders,reconciled\n";

    int failed_reconciliations = 0;
    for (const auto& path : cfg_paths) {
        SymbolConfig cfg = load_config(path);
        std::printf("=== %s ===\n", cfg.symbol.c_str());

        ProbeFit fit = fit_k(cfg);
        Qty lot = (Qty)std::llround(cfg.mean_trade_shares);
        AvellanedaStoikovMM as_tmp(cfg, lot, fit.k);
        for (size_t i = 0; i < fit.deltas.size(); ++i)
            probes << cfg.symbol << ',' << fit.deltas[i] << ','
                   << fit.lambdas[i] << ',' << fit.A << ',' << fit.k << ','
                   << as_tmp.gamma() << '\n';
        std::printf("  probe fit: A=%.6f /side-s  k=%.6f /tick  "
                    "gamma=%.3e /(tick*lot)\n", fit.A, fit.k, as_tmp.gamma());

        for (int seed = 1; seed <= n_seeds; ++seed) {
            // Same seed -> identical flow stream for both strategies.
            BaselineMM base(cfg, lot);
            Simulator sim_b(cfg, (uint64_t)seed);
            EpisodeResult rb = sim_b.run(&base);

            AvellanedaStoikovMM as(cfg, lot, fit.k);
            Simulator sim_a(cfg, (uint64_t)seed);
            EpisodeResult ra = sim_a.run(&as);

            for (const auto& r : {rb, ra}) {
                write_row(results, r);
                if (!r.reconciled) {
                    ++failed_reconciliations;
                    std::fprintf(stderr,
                                 "RECONCILIATION FAILURE %s %s seed %llu\n",
                                 r.symbol.c_str(), r.strategy.c_str(),
                                 (unsigned long long)r.seed);
                }
            }
        }
        std::printf("  %d seed-paired episodes done\n", n_seeds);
    }

    if (failed_reconciliations) {
        std::fprintf(stderr, "%d episodes FAILED reconciliation\n",
                     failed_reconciliations);
        return 1;
    }
    std::printf("all episodes reconciled against the engine trade log\n");
    return 0;
}
