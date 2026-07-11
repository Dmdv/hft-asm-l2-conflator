# ==============================================================================
# HFT Market Data Conflator (AArch64 Assembler) — Automation Wrapper
# ==============================================================================

CMAKE_FLAGS_COMMON = -S .

CMAKE_FLAGS_RELEASE = $(CMAKE_FLAGS_COMMON) -B build_release -DCMAKE_BUILD_TYPE=Release

CMAKE_FLAGS_OPT = $(CMAKE_FLAGS_COMMON) -B build_opt -DCMAKE_BUILD_TYPE=Release -DENABLE_HFT_AGGRESSIVE_OPT=ON

.PHONY: all release opt_release test clean run_release run_opt compare

all: release opt_release

release:
	@echo "==> Configuring and Building Standard Release..."
	cmake $(CMAKE_FLAGS_RELEASE)
	cmake --build build_release --config Release -j
	@# ensure BUILD_TYPE_STR in binary name path
	@true

opt_release:
	@echo "==> Configuring and Building Aggressively Optimized HFT Release..."
	cmake $(CMAKE_FLAGS_OPT)
	cmake --build build_opt --config Release -j

test: release opt_release
	@echo "==> Running Test Suite on Release Build..."
	cd build_release && ctest --output-on-failure
	@echo "==> Running Test Suite on Opt_Release Build..."
	cd build_opt && ctest --output-on-failure

run_release: release
	@echo "==> Executing Standard Release Benchmark Pipeline..."
	./build_release/hft_conflator --report benchmark_report_release.json

run_opt: opt_release
	@echo "==> Executing Aggressively Optimized HFT Benchmark Pipeline..."
	./build_opt/hft_conflator --report benchmark_report_opt_release.json

compare: run_opt
	@echo ""
	@echo "========== COMPARISON vs C++ baseline opt =========="
	@python3 scripts/compare_legacy.py

clean:
	@echo "==> Cleaning Build Artifacts..."
	rm -rf build_release build_opt benchmark_report_release.json benchmark_report_opt_release.json
