# SPDX-License-Identifier: Apache-2.0
.PHONY: all build test unit pytest asan report demo zephyr vectors xcheck release clean
all: build unit pytest report
build:
	cmake -S . -B build -G Ninja && cmake --build build
unit: build
	ctest --test-dir build --output-on-failure
pytest: build
	pytest -q
test: unit pytest
asan:
	cmake -S . -B build-asan -G Ninja -DOMT_SANITIZE=ON && cmake --build build-asan && ctest --test-dir build-asan --output-on-failure
report: build
	cd host && python3 -m omt report --out ../build/report.html
demo: build
	cd host && python3 -m omt demo
zephyr:
	west build -b native_sim/native/64 ports/zephyr -d build-zephyr -- -DDTC_OVERLAY_FILE=boards/native_sim.overlay && build-zephyr/zephyr/zephyr.exe --stop_at=2
vectors: build
	python3 tools/gen_vectors.py build/vectors
xcheck: vectors
	west build -b native_sim/native/64 tests/zephyr/cmis_xcheck -d build-xcheck -t run
release: test
	python3 tools/release.py
clean:
	rm -rf build build-asan build-zephyr dist
