// Loads the flat key=value .cfg files emitted by calibration/calibrate.py.
// Every field here is traceable to an NGXDash measurement (or a documented
// assumption) — see calibration/output/calibration.json for provenance.
#pragma once
#include <cstdint>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>

namespace mmsim {

struct SymbolConfig {
    std::string symbol;
    int64_t initial_price_ticks = 0;
    double tick_size_ngn = 0.01;
    double sigma_sqrt_sec_ticks = 0;  // fundamental diffusion, ticks per sqrt(s)
    double sigma_day_rel = 0;
    double half_spread_ticks = 0;     // Roll half-spread in ticks
    double lambda_per_sec = 0;        // market-order arrival rate
    double mean_trade_shares = 0;     // mean market-order size (assumption)
    double session_seconds = 0;       // episode horizon T
};

inline SymbolConfig load_config(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open config: " + path);
    std::map<std::string, std::string> kv;
    std::string line;
    while (std::getline(in, line)) {
        auto eq = line.find('=');
        if (eq != std::string::npos)
            kv[line.substr(0, eq)] = line.substr(eq + 1);
    }
    auto need = [&](const char* k) {
        auto it = kv.find(k);
        if (it == kv.end())
            throw std::runtime_error(path + ": missing key " + k);
        return it->second;
    };
    SymbolConfig c;
    c.symbol = need("symbol");
    c.initial_price_ticks = std::stoll(need("initial_price_ticks"));
    c.tick_size_ngn = std::stod(need("tick_size_ngn"));
    c.sigma_sqrt_sec_ticks = std::stod(need("sigma_per_sqrt_sec_ticks"));
    c.sigma_day_rel = std::stod(need("sigma_day_rel"));
    c.half_spread_ticks = std::stod(need("half_spread_ticks"));
    c.lambda_per_sec = std::stod(need("lambda_per_sec"));
    c.mean_trade_shares = std::stod(need("mean_trade_shares"));
    c.session_seconds = std::stod(need("session_seconds"));
    return c;
}

}  // namespace mmsim
