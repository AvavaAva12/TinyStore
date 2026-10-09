#!/bin/bash
# 逐个测试运行：TSAN 在 WSL2 下需要 setarch -R 解除地址空间布局限制，
# 而 ctest 一次跑多个进程时该限制会互相干扰，故手工逐个跑。
cd /home/f1823/projects/TinyStore || exit 1
fail=0
for t in build-tsan/bin/*_test; do
  [ -x "$t" ] || continue
  name=$(basename "$t")
  out=$(setarch x86_64 -R timeout 900 "$t" 2>&1)
  rc=$?
  summary=$(echo "$out" | grep -E '^\[  (PASSED|FAILED)' | tr '\n' ' ')
  if echo "$out" | grep -q 'WARNING: ThreadSanitizer'; then
    echo "$name: DATA RACE"
    echo "$out" | grep -A12 'WARNING: ThreadSanitizer' | head -20
    fail=1
  elif [ $rc -ne 0 ]; then
    echo "$name: RC=$rc $summary"
    echo "$out" | tail -8
    fail=1
  else
    echo "$name: OK $summary"
  fi
done
echo "=== TSAN RESULT: $([ $fail -eq 0 ] && echo PASS || echo FAIL) ==="