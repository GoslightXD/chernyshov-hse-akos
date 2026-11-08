#!/bin/bash
# Запуск независимых программ: run_independent.sh <shm|mq> [опции хоста]
# Хозяин стартует первым, затем повар и дикари, каждый как отдельный процесс.
cd "$(dirname "$0")/.." || exit 1
set -m   # у каждого процесса своя группа: Ctrl+C приходит только скрипту
mode=$1; shift
case "$mode" in
  shm) host=cannibals_host; cook=cannibals_cook; sav=cannibals_savage ;;
  mq)  host=cannibals_mq_host; cook=cannibals_mq_cook; sav=cannibals_mq_savage ;;
  *) echo "Использование: $0 <shm|mq> [опции хоста]"; exit 1 ;;
esac
n=5; prev=""
for a in "$@"; do [ "$prev" = "-n" ] && n=$a; prev=$a; done

./$host "$@" &
hp=$!
sleep 0.3
./$cook &
for i in $(seq 1 "$n"); do ./$sav "$i" & done

# фоновые процессы в скрипте не получают Ctrl+C сами, пересылаем хозяину
interrupted=0
trap 'interrupted=1; kill -INT $hp 2>/dev/null' INT TERM
trap 'kill -USR1 $hp 2>/dev/null' USR1
wait $hp; rc=$?
while kill -0 $hp 2>/dev/null; do wait $hp; rc=$?; done
[ $interrupted = 1 ] && [ $rc -gt 128 ] && rc=130
wait 2>/dev/null
exit $rc
