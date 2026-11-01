#!/usr/bin/env bash
set -euo pipefail

build_dir=${1:-build}
output="$build_dir/Paralax-Tests/TestOutput"
comparison="$output/FastMathComparison"
test_name='^SceneRenderer: fixed look matches the reviewed reference image$'

capture() {
  local destination=$1
  mkdir -p "$destination"
  for name in R1-Reference.png ReferenceLightingTextures.txt; do
    if [[ -f "$output/$name" ]]; then cp "$output/$name" "$destination/"; fi
  done
  if [[ -d "$output/ReferenceLightingFailure" ]]; then
    cp -R "$output/ReferenceLightingFailure" "$destination/"
  fi
}

# Preserve the parallel suite's result before the serial comparison overwrites its reports.
capture "$comparison/ParallelBaseline"
failed=0
for mode in default disabled; do
  for iteration in 1 2 3 4 5; do
    destination="$comparison/$mode-$iteration"
    mkdir -p "$destination"
    # Stale failure images from a preceding run would make a passing capture misleading.
    if [[ -d "$output/ReferenceLightingFailure" ]]; then
      rm -r "$output/ReferenceLightingFailure"
    fi
    rm -f "$output/R1-Reference.png" "$output/ReferenceLightingTextures.txt"
    settings=(-u MVK_CONFIG_FAST_MATH_ENABLED PX_REQUIRE_GPU=1)
    if [[ "$mode" == disabled ]]; then settings=(MVK_CONFIG_FAST_MATH_ENABLED=0 PX_REQUIRE_GPU=1); fi
    if env "${settings[@]}" ctest --test-dir "$build_dir" -R "$test_name" --parallel 1 --timeout 300 --no-tests=error --output-on-failure > "$destination/Test.log" 2>&1; then
      status=0
    else
      status=$?
      failed=1
    fi
    printf '%s iteration %s: exit %s\n' "$mode" "$iteration" "$status"
    capture "$destination"
  done
done
exit "$failed"
