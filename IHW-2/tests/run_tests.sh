#!/bin/bash
# Запуск из любого места: ./tests/run_tests.sh [суффикс бинарников, например _tsan]
cd "$(dirname "$0")/.." || exit 1
SUF="$1"
[ -z "$SUF" ] && { make -s || exit 1; } || { make -s tsan || exit 1; }
mkdir -p tests/out
pass=0; fail=0

ok()   { echo "OK   $1"; pass=$((pass+1)); }
bad()  { echo "FAIL $1 ($2)"; fail=$((fail+1)); }
num()  { grep -oP "$2\K[0-9]+" "$1" | head -1; }

# run EXE NAME CODE PATTERN [EXPECTED_TAKEN] -- ARGS...
run() {
  local exe=$1 name=$2 code=$3 pat=$4 taken=$5; shift 6
  local f="tests/out/$exe.$name"
  timeout 60 "./$exe" "$@" -l "$f.log" > "$f.txt" 2>&1
  local rc=$?
  if [ "$rc" -ne "$code" ]; then bad "$exe $name" "rc=$rc, ожидалось $code"; return; fi
  if ! grep -q -- "$pat" "$f.txt"; then bad "$exe $name" "нет '$pat'"; return; fi
  if grep -q "НАРУШЕН" "$f.txt"; then bad "$exe $name" "нарушен инвариант"; return; fi
  if [ "$code" -eq 0 ]; then
    local fl tk left
    fl=$(num "$f.txt" "положено порций="); tk=$(num "$f.txt" "взято порций="); left=$(num "$f.txt" "осталось=")
    if [ $((fl - tk)) -ne "$left" ]; then bad "$exe $name" "баланс порций"; return; fi
    if [ -n "$taken" ] && [ "$tk" -ne "$taken" ]; then bad "$exe $name" "взято $tk, ожидалось $taken"; return; fi
  fi
  ok "$exe $name"
}

for e in cannibals_sem cannibals_cond; do
  exe="$e$SUF"
  T="-t 1:5 -k 1:5"
  run $exe basic        0 "все дикари выполнили" 15 -- -n 5 -c 4 -m 3 $T -r 1
  run $exe one_savage   0 "все дикари выполнили" 5  -- -n 1 -c 1 -m 5 $T -r 2
  run $exe big_pot      0 "все дикари выполнили" 40 -- -n 10 -c 100 -m 4 $T -r 3
  run $exe many_savages 0 "все дикари выполнили" 500 -- -n 100 -c 3 -m 5 -t 0:3 -k 0:3 -r 4
  run $exe zero_times   0 "все дикари выполнили" 12 -- -n 4 -c 2 -m 3 -t 0:0 -k 0:0 -r 5
  run $exe limited      0 "запас порций исчерпан" 7 -- -n 4 -c 3 -m 5 -s 7 $T -r 7
  run $exe supply_ok    0 "все дикари выполнили" 6 -- -n 3 -c 2 -m 2 -s 100 $T -r 8
  run $exe bad_n        1 "число дикарей" "" -- -n 0
  run $exe bad_cap      1 "вместимость" "" -- -c 0
  run $exe bad_range    1 "неверное значение" "" -- -t 5:2
  run $exe bad_option   1 "Использование" "" -- -z 1

  # много повторов: ловим взаимные блокировки и гонки
  okc=0
  for r in $(seq 1 25); do
    timeout 30 ./$exe -n 6 -c 2 -m 3 -t 0:2 -k 0:2 -r $r -l /dev/null > tests/out/$exe.rep.txt 2>&1
    rc=$?
    tk=$(num tests/out/$exe.rep.txt "взято порций=")
    [ $rc -eq 0 ] && [ "$tk" = "18" ] && ! grep -q НАРУШЕН tests/out/$exe.rep.txt || { bad "$exe repeat" "r=$r rc=$rc taken=$tk"; okc=1; break; }
  done
  [ $okc -eq 0 ] && ok "$exe repeat_25"
  okc=0
  for r in $(seq 1 25); do
    timeout 30 ./$exe -n 6 -c 2 -m 3 -s 11 -t 0:2 -k 0:2 -r $r -l /dev/null > tests/out/$exe.rep.txt 2>&1
    rc=$?
    tk=$(num tests/out/$exe.rep.txt "взято порций=")
    [ $rc -eq 0 ] && [ "$tk" = "11" ] && grep -q "запас порций исчерпан" tests/out/$exe.rep.txt && ! grep -q НАРУШЕН tests/out/$exe.rep.txt || { bad "$exe repeat_supply" "r=$r rc=$rc taken=$tk"; okc=1; break; }
  done
  [ $okc -eq 0 ] && ok "$exe repeat_supply_25"

  # SIGINT в бесконечном режиме
  ./$exe -n 3 -c 2 -m 0 -t 20:50 -k 20:50 -r 11 -l tests/out/$exe.sigint.log > tests/out/$exe.sigint.txt 2>&1 &
  pid=$!; sleep 1; kill -INT $pid; wait $pid; rc=$?
  if [ $rc -eq 130 ] && grep -q "прервано пользователем" tests/out/$exe.sigint.txt; then ok "$exe sigint"; else bad "$exe sigint" "rc=$rc"; fi

  # SIGTERM и SIGUSR1
  ./$exe -n 3 -c 2 -m 0 -t 20:50 -k 20:50 -r 12 -l tests/out/$exe.usr1.log > tests/out/$exe.usr1.txt 2>&1 &
  pid=$!; sleep 0.7; kill -USR1 $pid; sleep 0.3; kill -TERM $pid; wait $pid; rc=$?
  if [ $rc -eq 130 ] && grep -q "Состояние:" tests/out/$exe.usr1.txt; then ok "$exe sigusr1_sigterm"; else bad "$exe sigusr1_sigterm" "rc=$rc"; fi

  # остановка, когда все ждут повара (долгое приготовление)
  ./$exe -n 4 -c 2 -m 0 -t 0:0 -k 3000:3000 -r 13 -l /dev/null > tests/out/$exe.cookwait.txt 2>&1 &
  pid=$!; sleep 0.5; kill -INT $pid; wait $pid; rc=$?
  if [ $rc -eq 130 ]; then ok "$exe sigint_while_waiting"; else bad "$exe sigint_while_waiting" "rc=$rc"; fi

  # интерактивный ввод
  printf '2\n2\n2\n1\n3\n1\n3\n0\n5\n' | timeout 30 ./$exe -l tests/out/$exe.inter.log > tests/out/$exe.inter.txt 2>&1
  if [ $? -eq 0 ] && grep -q "все дикари выполнили" tests/out/$exe.inter.txt; then ok "$exe interactive"; else bad "$exe interactive" ""; fi
done

echo "Итого: OK=$pass FAIL=$fail"
[ "$fail" -eq 0 ]