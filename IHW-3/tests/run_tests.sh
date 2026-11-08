#!/bin/bash
# Запуск: ./tests/run_tests.sh [вариант ...]   (fork_named fork_unnamed ind_shm ind_mq; по умолчанию все)
cd "$(dirname "$0")/.." || exit 1
make -s || exit 1
gcc -o tests/mqcheck tests/mqcheck.c -lrt || exit 1
mkdir -p tests/out
VARIANTS=("$@")
[ ${#VARIANTS[@]} -eq 0 ] && VARIANTS=(fork_named fork_unnamed ind_shm ind_mq)
pass=0; fail=0

ok()  { echo "OK   $1"; pass=$((pass+1)); }
bad() { echo "FAIL $1 ($2)"; fail=$((fail+1)); }
num() { grep -oP "$2\K[0-9]+" "$1" | head -1; }

cmd_for() {
  case $1 in
    fork_named)   echo "./cannibals_fork_named" ;;
    fork_unnamed) echo "./cannibals_fork_unnamed" ;;
    ind_shm)      echo "bash ./scripts/run_independent.sh shm" ;;
    ind_mq)       echo "bash ./scripts/run_independent.sh mq" ;;
  esac
}
host_for() {
  case $1 in
    ind_shm) echo "./cannibals_host" ;;
    ind_mq)  echo "./cannibals_mq_host" ;;
    *)       cmd_for "$1" ;;
  esac
}

# остатки семафоров, разделяемой памяти и очередей после работы
leaks() {
  local n
  n=$(ls /dev/shm 2>/dev/null | grep -c cannibals)
  n=$((n + $(./tests/mqcheck | grep -c exists)))
  echo "$n"
}

# run ВАРИАНТ ИМЯ КОД ШАБЛОН ВЗЯТО -- ОПЦИИ...   (командой запуска служит cmd_for, для ошибок параметров host_for)
run() {
  local v=$1 name=$2 code=$3 pat=$4 taken=$5 launcher=$6; shift 7
  local f="tests/out/$v.$name" c
  if [ "$launcher" = host ]; then c=$(host_for "$v"); else c=$(cmd_for "$v"); fi
  timeout 60 $c "$@" -l "$f.log" > "$f.txt" 2>&1
  local rc=$?
  if [ "$rc" -ne "$code" ]; then bad "$v $name" "rc=$rc, ожидалось $code"; return; fi
  if ! grep -q -- "$pat" "$f.txt"; then bad "$v $name" "нет '$pat'"; return; fi
  if grep -q "НАРУШЕН" "$f.txt"; then bad "$v $name" "нарушен инвариант"; return; fi
  if [ "$code" -eq 0 ]; then
    local fl tk left
    fl=$(num "$f.txt" "положено порций="); tk=$(num "$f.txt" "взято порций="); left=$(num "$f.txt" "осталось=")
    if [ $((fl - tk)) -ne "$left" ]; then bad "$v $name" "баланс порций"; return; fi
    if [ -n "$taken" ] && [ "$tk" -ne "$taken" ]; then bad "$v $name" "взято $tk, ожидалось $taken"; return; fi
  fi
  if [ "$(leaks)" -ne 0 ]; then bad "$v $name" "остались семафоры/память/очереди"; return; fi
  ok "$v $name"
}

for v in "${VARIANTS[@]}"; do
  T="-t 1:5 -k 1:5"
  # скрипту, запущенному в фоне из неинтерактивного bash, SIGINT перехватить нельзя: шлём SIGTERM (скрипт пересылает хозяину SIGINT)
  SIGMAIN=INT; case $v in ind_*) SIGMAIN=TERM ;; esac
  c=$(cmd_for "$v")
  run $v basic        0 "все дикари выполнили" 15  run -- -n 5 -c 4 -m 3 $T -r 1
  run $v one_savage   0 "все дикари выполнили" 5   run -- -n 1 -c 1 -m 5 $T -r 2
  run $v big_pot      0 "все дикари выполнили" 40  run -- -n 10 -c 100 -m 4 $T -r 3
  run $v many_savages 0 "все дикари выполнили" 200 run -- -n 50 -c 3 -m 4 -t 0:3 -k 0:3 -r 4
  run $v zero_times   0 "все дикари выполнили" 12  run -- -n 4 -c 2 -m 3 -t 0:0 -k 0:0 -r 5
  run $v limited      0 "запас порций исчерпан" 7  run -- -n 4 -c 3 -m 5 -s 7 $T -r 7
  run $v supply_ok    0 "все дикари выполнили" 6   run -- -n 3 -c 2 -m 2 -s 100 $T -r 8
  run $v bad_n        1 "число дикарей" "" host -- -n 0
  run $v bad_cap      1 "вместимость" "" host -- -c 0
  run $v bad_range    1 "неверное значение" "" host -- -t 5:2
  run $v bad_option   1 "Использование" "" host -- -z 1

  # повторные запуски: взаимные блокировки, потерянные пробуждения, остатки ресурсов
  okc=0
  for r in $(seq 1 15); do
    timeout 60 $c -n 6 -c 2 -m 3 -t 0:2 -k 0:2 -r $r -l /dev/null > tests/out/$v.rep.txt 2>&1
    rc=$?; tk=$(num tests/out/$v.rep.txt "взято порций=")
    if [ $rc -ne 0 ] || [ "$tk" != "18" ] || grep -q НАРУШЕН tests/out/$v.rep.txt || [ "$(leaks)" -ne 0 ]; then
      bad "$v repeat" "r=$r rc=$rc taken=$tk"; okc=1; break
    fi
  done
  [ $okc -eq 0 ] && ok "$v repeat_15"
  okc=0
  for r in $(seq 1 15); do
    timeout 60 $c -n 6 -c 2 -m 3 -s 11 -t 0:2 -k 0:2 -r $r -l /dev/null > tests/out/$v.rep.txt 2>&1
    rc=$?; tk=$(num tests/out/$v.rep.txt "взято порций=")
    if [ $rc -ne 0 ] || [ "$tk" != "11" ] || ! grep -q "запас порций исчерпан" tests/out/$v.rep.txt || [ "$(leaks)" -ne 0 ]; then
      bad "$v repeat_supply" "r=$r rc=$rc taken=$tk"; okc=1; break
    fi
  done
  [ $okc -eq 0 ] && ok "$v repeat_supply_15"

  # SIGINT в бесконечном режиме
  $c -n 3 -c 2 -m 0 -t 20:50 -k 20:50 -r 11 -l tests/out/$v.sigint.log > tests/out/$v.sigint.txt 2>&1 &
  pid=$!; sleep 1.2; kill -$SIGMAIN $pid; wait $pid; rc=$?
  if [ $rc -eq 130 ] && grep -q "прервано пользователем" tests/out/$v.sigint.txt && [ "$(leaks)" -eq 0 ]; then ok "$v sigint"; else bad "$v sigint" "rc=$rc leaks=$(leaks)"; fi

  # SIGUSR1 (состояние), затем SIGTERM
  $c -n 3 -c 2 -m 0 -t 20:50 -k 20:50 -r 12 -l tests/out/$v.usr1.log > tests/out/$v.usr1.txt 2>&1 &
  pid=$!; sleep 1; kill -USR1 $pid; sleep 0.3; kill -TERM $pid; wait $pid; rc=$?
  if [ $rc -eq 130 ] && grep -q "Состояние:" tests/out/$v.usr1.txt && [ "$(leaks)" -eq 0 ]; then ok "$v sigusr1_sigterm"; else bad "$v sigusr1_sigterm" "rc=$rc leaks=$(leaks)"; fi

  # остановка, когда все ждут повара
  $c -n 4 -c 2 -m 0 -t 0:0 -k 3000:3000 -r 13 -l /dev/null > tests/out/$v.cookwait.txt 2>&1 &
  pid=$!; sleep 1; kill -$SIGMAIN $pid; wait $pid; rc=$?
  if [ $rc -eq 130 ] && [ "$(leaks)" -eq 0 ]; then ok "$v sigint_while_waiting"; else bad "$v sigint_while_waiting" "rc=$rc leaks=$(leaks)"; fi

  # два SIGINT подряд
  $c -n 4 -c 2 -m 0 -t 0:0 -k 3000:3000 -r 14 -l /dev/null > tests/out/$v.twice.txt 2>&1 &
  pid=$!; sleep 1; kill -$SIGMAIN $pid; sleep 0.05; kill -$SIGMAIN $pid 2>/dev/null; wait $pid; rc=$?
  if [ $rc -eq 130 ] && [ "$(leaks)" -eq 0 ]; then ok "$v sigint_twice"; else bad "$v sigint_twice" "rc=$rc leaks=$(leaks)"; fi

  # интерактивный ввод (только программы с единым запуском)
  case $v in fork_*)
    printf '2\n2\n2\n1\n3\n1\n3\n0\n5\n' | timeout 30 $c -l tests/out/$v.inter.log > tests/out/$v.inter.txt 2>&1
    if [ $? -eq 0 ] && grep -q "все дикари выполнили" tests/out/$v.inter.txt; then ok "$v interactive"; else bad "$v interactive" ""; fi ;;
  esac
done

# независимые программы: повторный запуск того же дикаря
case " ${VARIANTS[*]} " in *" ind_shm "*)
  ./cannibals_host -n 1 -c 1 -m 0 -t 20:20 -k 20:20 -l /dev/null > tests/out/dup.host.txt 2>&1 &
  hp=$!; sleep 0.3
  ./cannibals_cook > /dev/null 2>&1 &
  ./cannibals_savage 1 > /dev/null 2>&1 &
  sleep 0.4
  ./cannibals_savage 1 > tests/out/dup.txt 2>&1; rc=$?
  kill -INT $hp; wait
  if [ $rc -eq 1 ] && grep -q "уже запущен" tests/out/dup.txt && [ "$(leaks)" -eq 0 ]; then ok "ind_shm duplicate_savage"; else bad "ind_shm duplicate_savage" "rc=$rc"; fi ;;
esac

echo "Итого: OK=$pass FAIL=$fail"
[ "$fail" -eq 0 ]
