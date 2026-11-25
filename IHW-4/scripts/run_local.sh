#!/bin/bash
# Запуск всех участников на одном компьютере: run_local.sh [опции сервера]
# Пример: ./scripts/run_local.sh -n 5 -c 4 -m 3 -p 5555
cd "$(dirname "$0")/.." || exit 1
set -m
n=5; port=5555; prev=""
for a in "$@"; do
  [ "$prev" = "-n" ] && n=$a
  [ "$prev" = "-p" ] && port=$a
  prev=$a
done

./cannibals_server "$@" &
sp=$!
sleep 0.3
./cannibals_observer -p "$port" > /dev/null &
./cannibals_cook -p "$port" &
for i in $(seq 1 "$n"); do ./cannibals_savage -p "$port" -i "$i" > /dev/null & done

# Ctrl+C приходит скрипту, пересылаем серверу
interrupted=0
trap 'interrupted=1; kill -INT $sp 2>/dev/null' INT TERM
trap 'kill -USR1 $sp 2>/dev/null' USR1
wait $sp; rc=$?
while kill -0 $sp 2>/dev/null; do wait $sp; rc=$?; done
[ $interrupted = 1 ] && [ $rc -gt 128 ] && rc=130
wait 2>/dev/null
exit $rc
