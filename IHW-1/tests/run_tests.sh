#!/bin/bash
# Запуск тестовых наборов. Использование: ./tests/run_tests.sh  (из каталога проекта)
# Каждый тест: имя | аргументы | ожидаемый код возврата | ожидаемая подстрока в выводе
cd "$(dirname "$0")/.." || exit 1
make -s || exit 1
mkdir -p tests/out
pass=0; fail=0
run() {
  local name="$1" code="$2" pattern="$3"; shift 3
  ./cannibals "$@" -l "tests/out/$name.log" > "tests/out/$name.txt" 2>&1
  local rc=$?
  if [ "$rc" -eq "$code" ] && grep -q -- "$pattern" "tests/out/$name.txt"; then
    echo "OK   $name"; pass=$((pass+1))
  else
    echo "FAIL $name (rc=$rc, ожидалось $code, шаблон '$pattern')"; fail=$((fail+1))
  fi
}
run basic            0 "все дикари выполнили" -n 5 -c 4 -m 3 -r 1 -d 0
run one_savage       0 "все дикари выполнили" -n 1 -c 1 -m 5 -r 2 -d 0
run big_pot          0 "все дикари выполнили" -n 10 -c 100 -m 4 -r 3 -d 0
run many_savages     0 "все дикари выполнили" -n 200 -c 3 -m 5 -r 4 -d 0
run zero_times       0 "все дикари выполнили" -n 4 -c 2 -m 3 -t 0:0 -k 0:0 -r 5 -d 0
run limited_supply   0 "запас порций исчерпан" -n 4 -c 3 -m 5 -s 7 -r 7 -d 0
run supply_enough    0 "все дикари выполнили" -n 3 -c 2 -m 2 -s 100 -r 8 -d 0
run reproducible_1   0 "Seed=99" -n 5 -c 3 -m 4 -r 99 -d 0
run bad_n            1 "число дикарей" -n 0 -d 0
run bad_cap          1 "вместимость" -c 0 -d 0
run bad_range        1 "неверное значение" -t 5:2 -d 0
run bad_option       1 "Использование" -z 1
# воспроизводимость: два запуска с одним seed дают одинаковый журнал
./cannibals -n 5 -c 3 -m 4 -r 99 -d 0 -l tests/out/r2.log > tests/out/r2.txt
if diff -q tests/out/reproducible_1.txt tests/out/r2.txt >/dev/null; then echo "OK   reproducible_2"; pass=$((pass+1)); else echo "FAIL reproducible_2"; fail=$((fail+1)); fi
# прерывание по Ctrl+C в бесконечном режиме
./cannibals -n 3 -c 2 -m 0 -r 11 -d 20 -l tests/out/sigint.log > tests/out/sigint.txt 2>&1 &
pid=$!; sleep 1; kill -INT $pid; wait $pid; rc=$?
if [ "$rc" -eq 130 ] && grep -q "прервано пользователем" tests/out/sigint.txt; then echo "OK   sigint"; pass=$((pass+1)); else echo "FAIL sigint (rc=$rc)"; fail=$((fail+1)); fi
# интерактивный ввод: все значения по умолчанию, кроме задержки
printf '\n\n\n\n\n\n\n\n\n0\n' | ./cannibals -l tests/out/interactive.log > tests/out/interactive.txt 2>&1
if grep -q "все дикари выполнили" tests/out/interactive.txt; then echo "OK   interactive"; pass=$((pass+1)); else echo "FAIL interactive"; fail=$((fail+1)); fi
echo "Итого: OK=$pass FAIL=$fail"
[ "$fail" -eq 0 ]
