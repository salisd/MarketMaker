# MarketMaker — Avellaneda-Stoikov vs baseline against the real MatchEng book.
# Links MatchEng header-only (no reimplementation of matching logic).

CXX      ?= clang++
MATCHENG ?= ../MatchEng
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -Iinclude -I$(MATCHENG)/include

BUILD := build
HDRS  := $(wildcard include/mmsim/*.hpp) \
         $(wildcard $(MATCHENG)/include/matcheng/*.hpp)

all: test run

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/test_flow: tests/test_flow.cpp $(HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@

$(BUILD)/test_strategies: tests/test_strategies.cpp $(HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@

$(BUILD)/run_experiment: src/run_experiment.cpp $(HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -O3 $< -o $@

test: $(BUILD)/test_flow $(BUILD)/test_strategies
	./$(BUILD)/test_flow
	./$(BUILD)/test_strategies

run: $(BUILD)/run_experiment
	mkdir -p results
	./$(BUILD)/run_experiment calibration/output/GTCO.cfg \
	    calibration/output/OANDO.cfg calibration/output/LIVESTOCK.cfg \
	    --seeds 50 --out results

clean:
	rm -rf $(BUILD)

.PHONY: all test run clean
