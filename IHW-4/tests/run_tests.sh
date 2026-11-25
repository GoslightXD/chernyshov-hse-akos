#!/bin/bash
# Запуск из любого места: ./tests/run_tests.sh
cd "$(dirname "$0")/.." || exit 1
make -s || exit 1
O=tests/out
mkdir -p $O
pass=0; fail=0
declare -A SV

ok()  { echo "OK   $1"; pass=$((pass+1)); }
bad() { echo "FAIL $1 ($2)"; fail=$((fail+1)); }
num() { grep -oP "$2\K[0-9]+" "$1" | head -1; }
newport() { PORT=$((20000 + RANDOM % 30000)); }

srv()  { local name=$1; shift; ./cannibals_server -p $PORT "$@" -l $O/$name.srv.log > $O/$name.srv.txt 2>&1 & SP=$!; }
cook() { ./cannibals_cook -p $PORT -w 5 > $O/$1.cook$2.txt 2>&1 & CP=$!; }
sav()  { ./cannibals_savage -p $PORT -i $2 -w 5 > $O/$1.sav$2$3.txt 2>&1 & SV[$2]=$!; }
obs()  { ./cannibals_observer -p $PORT -w 5 > $O/$1.obs$2.txt 2>&1 & OP=$!; }

# итог работы сервера: баланс порций и число съеденных
bal() {
  local f=$1 taken=$2 fl tk left
  fl=$(num "$f" "положено порций="); tk=$(num "$f" "взято порций="); left=$(num "$f" "осталось=")
  [ -n "$fl" ] && [ $((fl - tk)) -eq "$left" ] || { echo "баланс"; return; }
  [ -z "$taken" ] || [ "$tk" -eq "$taken" ] || echo "взято $tk, ожидалось $taken"
}

# scenario ИМЯ N ВЗЯТО ШАБЛОН -- ОПЦИИ_СЕРВЕРА...   (LATE=1: клиенты стартуют раньше сервера)
scenario() {
  local name=$1 n=$2 taken=$3 pat=$4; shift 5
  newport
  if [ -n "$LATE" ]; then
    obs $name 1; cook $name
    for i in $(seq 1 "$n"); do sav $name $i; done
    sleep 1
    srv $name -n $n "$@"
  else
    srv $name -n $n "$@"
    obs $name 1; cook $name
    sleep 0.3
    for i in $(seq 1 "$n"); do sav $name $i; done
  fi
  wait $SP; local rc=$? err=""
  [ $rc -eq 0 ] || err="сервер rc=$rc"
  grep -q -- "$pat" $O/$name.srv.txt || err="$err нет '$pat'"
  grep -q НАРУШЕН $O/$name.srv.txt && err="$err инвариант"
  err="$err $(bal $O/$name.srv.txt "$taken")"
  wait $CP; [ $? -eq 0 ] || err="$err повар"
  for i in $(seq 1 "$n"); do wait ${SV[$i]}; rc=$?; [ $rc -eq 0 ] || err="$err дикарь$i rc=$rc"; done
  wait $OP; rc=$?
  [ $rc -eq 0 ] && grep -q "Итог" $O/$name.obs1.txt && grep -q "Сервер завершил" $O/$name.obs1.txt || err="$err наблюдатель rc=$rc"
  [ -z "${err// /}" ] && ok "$name" || bad "$name" "$err"
}

T="-t 1:5 -k 1:5"
scenario basic        5  15  "все дикари выполнили" -- -c 4 -m 3 $T -r 1
scenario one_savage   1  5   "все дикари выполнили" -- -c 1 -m 5 $T -r 2
scenario big_pot      10 40  "все дикари выполнили" -- -c 100 -m 4 $T -r 3
scenario many_savages 40 160 "все дикари выполнили" -- -c 3 -m 4 -t 0:3 -k 0:3 -r 4
scenario zero_times   4  12  "все дикари выполнили" -- -c 2 -m 3 -t 0:0 -k 0:0 -r 5
scenario limited      4  7   "запас порций исчерпан" -- -c 3 -m 5 -s 7 $T -r 7
scenario supply_ok    3  6   "все дикари выполнили" -- -c 2 -m 2 -s 100 $T -r 8
LATE=1 scenario late_clients 3 9 "все дикари выполнили" -- -c 2 -m 3 $T -r 9

# повторные запуски
okc=0
for r in $(seq 1 8); do
  scenario rep 4 12 "все дикари выполнили" -- -c 2 -m 3 -t 0:2 -k 0:2 -r $r > $O/rep.res 2>&1
  if grep -q "^OK" $O/rep.res; then pass=$((pass-1)); else fail=$((fail-1)); bad "repeat" "r=$r: $(cat $O/rep.res)"; okc=1; break; fi
done
[ $okc -eq 0 ] && ok "repeat_8"

# ошибки параметров и подключения
newport
./cannibals_server -p $PORT -n 0 -l /dev/null > $O/bad1.txt 2>&1; [ $? -eq 1 ] && grep -q "число дикарей" $O/bad1.txt && ok bad_n || bad bad_n ""
./cannibals_server -p 0 -l /dev/null > $O/bad2.txt 2>&1; [ $? -eq 1 ] && ok bad_port || bad bad_port ""
./cannibals_server -z 1 > $O/bad3.txt 2>&1; [ $? -eq 1 ] && grep -q "Использование" $O/bad3.txt && ok bad_option || bad bad_option ""
./cannibals_server -a 999.1.1.1 -p $PORT -l /dev/null > $O/bad4.txt 2>&1; [ $? -eq 1 ] && ok bad_address || bad bad_address ""
srv busy -m 0; sleep 0.3
./cannibals_server -p $PORT -l /dev/null > $O/bad5.txt 2>&1; rc=$?
kill -INT $SP; wait $SP
[ $rc -eq 1 ] && grep -q "Не удалось слушать" $O/bad5.txt && ok port_busy || bad port_busy "rc=$rc"
./cannibals_savage -p $PORT -i 1 -w 1 > $O/bad6.txt 2>&1; rc=$?
[ $rc -eq 1 ] && grep -q "Не удалось подключиться" $O/bad6.txt && ok no_server || bad no_server "rc=$rc"
./cannibals_savage -p $PORT > $O/bad7.txt 2>&1; [ $? -eq 1 ] && ok savage_no_id || bad savage_no_id ""
./cannibals_savage -p $PORT -i 0 > $O/bad8.txt 2>&1; [ $? -eq 1 ] && ok savage_bad_id || bad savage_bad_id ""

# остановка сервера по сигналу: все клиенты завершаются сами
for SIGNAL in INT TERM; do
  name=stop_$SIGNAL; newport
  srv $name -n 3 -m 0 -c 2 -t 20:50 -k 20:50 -r 11
  obs $name 1; cook $name; for i in 1 2 3; do sav $name $i; done
  sleep 1.5; kill -$SIGNAL $SP; wait $SP; rc=$?; err=""
  [ $rc -eq 130 ] && grep -q "прервано пользователем" $O/$name.srv.txt || err="сервер rc=$rc"
  wait $CP; [ $? -eq 0 ] && grep -q "остановлен пользователем" $O/$name.cook.txt || err="$err повар"
  for i in 1 2 3; do wait ${SV[$i]}; [ $? -eq 0 ] && grep -q "остановлен пользователем" $O/$name.sav$i.txt || err="$err дикарь$i"; done
  wait $OP; [ $? -eq 0 ] && grep -q "остановлен пользователем" $O/$name.obs1.txt || err="$err наблюдатель"
  [ -z "${err// /}" ] && ok $name || bad $name "$err"
done

# SIGUSR1: состояние
name=usr1; newport
srv $name -n 2 -m 0 -c 2 -t 20:50 -k 20:50 -r 12; obs $name 1; cook $name; sav $name 1; sav $name 2
sleep 1; kill -USR1 $SP; sleep 0.3; kill -INT $SP; wait $SP; rc=$?
wait $CP $OP ${SV[1]} ${SV[2]}
grep -q "Состояние:" $O/$name.srv.txt && grep -q "Состояние:" $O/$name.obs1.txt && [ $rc -eq 130 ] && ok usr1 || bad usr1 "rc=$rc"

# наблюдатели подключаются и отключаются на ходу
name=obs_dynamic; newport
srv $name -n 3 -m 0 -c 2 -t 20:50 -k 20:50 -r 13; cook $name; for i in 1 2 3; do sav $name $i; done
obs $name A; OA=$OP; sleep 0.5; obs $name B; OB=$OP; sleep 0.5
kill -INT $OA; wait $OA; rca=$?
obs $name C; OC=$OP; sleep 0.6
alive=1; kill -0 $SP 2>/dev/null || alive=0
kill -INT $SP; wait $SP; rc=$?; wait $OB; rcb=$?; wait $OC; rcc=$?; wait $CP ${SV[1]} ${SV[2]} ${SV[3]}
err=""
[ $alive -eq 1 ] && [ $rca -eq 130 ] && [ $rcb -eq 0 ] && [ $rcc -eq 0 ] && [ $rc -eq 130 ] || err="alive=$alive A=$rca B=$rcb C=$rcc srv=$rc"
grep -q "Дикарь" $O/$name.obsB.txt && grep -q "Дикарь" $O/$name.obsC.txt || err="$err нет событий"
grep -q "наблюдатель подключён" $O/$name.obsC.txt && grep -q "Наблюдатель отключился" $O/$name.srv.txt || err="$err нет снимка/отключения"
[ -z "$err" ] && ok obs_dynamic || bad obs_dynamic "$err"

# дикарь отключается и подключается снова
name=savage_reconnect; newport
srv $name -n 2 -c 2 -m 12 -t 80:120 -k 20:30 -r 5; obs $name 1; cook $name; sav $name 1; sav $name 2
sleep 0.6; kill -9 ${SV[2]}; wait ${SV[2]} 2>/dev/null; sleep 0.4
alive=1; kill -0 $SP 2>/dev/null || alive=0
sav $name 2 b
wait $SP; rc=$?; err=""
[ $alive -eq 1 ] && [ $rc -eq 0 ] || err="alive=$alive rc=$rc"
err="$err $(bal $O/$name.srv.txt 24)"
grep -q "отключился" $O/$name.srv.txt && grep -q "подключился снова" $O/$name.srv.txt || err="$err нет отключения/возврата"
grep -q "съел ранее" $O/$name.sav2b.txt && wait ${SV[2]}; [ $? -eq 0 ] || err="$err дикарь2b"
wait $CP $OP ${SV[1]}
[ -z "${err// /}" ] && ok savage_reconnect || bad savage_reconnect "$err"

# повар отключается во время приготовления и подключается снова
name=cook_reconnect; newport
srv $name -n 2 -c 2 -m 4 -t 20:40 -k 600:600 -r 6; obs $name 1; cook $name; sav $name 1; sav $name 2
sleep 0.9; kill -9 $CP; wait $CP 2>/dev/null; sleep 0.3
alive=1; kill -0 $SP 2>/dev/null || alive=0
cook $name b
wait $SP; rc=$?; err=""
[ $alive -eq 1 ] && [ $rc -eq 0 ] || err="alive=$alive rc=$rc"
err="$err $(bal $O/$name.srv.txt 8)"
grep -q "отключился во время приготовления" $O/$name.srv.txt && grep -q "есть невыполненный запрос" $O/$name.srv.txt || err="$err нет отключения/возобновления"
wait $CP; [ $? -eq 0 ] || err="$err повар"
wait $OP ${SV[1]} ${SV[2]}
[ -z "${err// /}" ] && ok cook_reconnect || bad cook_reconnect "$err"

# вторая копия роли отклоняется, неверный номер тоже
name=duplicates; newport
srv $name -n 2 -m 0 -c 2 -t 20:50 -k 20:50 -r 14; cook $name; sav $name 1; sleep 0.4
./cannibals_savage -p $PORT -i 1 -w 2 > $O/$name.dup1.txt 2>&1; r1=$?
./cannibals_cook -p $PORT -w 2 > $O/$name.dup2.txt 2>&1; r2=$?
./cannibals_savage -p $PORT -i 5 -w 2 > $O/$name.dup3.txt 2>&1; r3=$?
kill -INT $SP; wait $SP; wait $CP ${SV[1]}
[ $r1 -eq 1 ] && [ $r2 -eq 1 ] && [ $r3 -eq 1 ] && grep -q "занята" $O/$name.dup1.txt && grep -q "вне диапазона" $O/$name.dup3.txt && ok duplicates || bad duplicates "$r1 $r2 $r3"

# закончивший дикарь не может вернуться, остальные продолжают
name=after_finish; newport
srv $name -n 2 -m 1 -c 2 $T -r 15; cook $name; sav $name 1; sleep 0.6
./cannibals_savage -p $PORT -i 1 -w 2 > $O/$name.again.txt 2>&1; r1=$?
alive=1; kill -0 $SP 2>/dev/null || alive=0
sav $name 2; wait $SP; rc=$?; wait $CP ${SV[1]} ${SV[2]}
[ $r1 -eq 0 ] && grep -q "уже закончил" $O/$name.again.txt && [ $alive -eq 1 ] && [ $rc -eq 0 ] && ok after_finish || bad after_finish "r1=$r1 alive=$alive rc=$rc"

# сервер аварийно убит: клиенты не зависают и завершаются с ошибкой
name=server_killed; newport
srv $name -n 2 -m 0 -c 2 -t 20:50 -k 20:50 -r 16; obs $name 1; cook $name; sav $name 1; sav $name 2
sleep 1; kill -9 $SP; wait $SP 2>/dev/null; err=""
wait $CP; [ $? -eq 1 ] || err="повар"
wait $OP; [ $? -eq 1 ] || err="$err наблюдатель"
for i in 1 2; do wait ${SV[$i]}; [ $? -eq 1 ] || err="$err дикарь$i"; done
grep -q "потеряно" $O/$name.cook.txt && [ -z "${err// /}" ] && ok server_killed || bad server_killed "$err"

# адрес и порт задаются в командной строке: сервер на 127.0.0.2 недоступен по 127.0.0.1
name=bind_address; newport
./cannibals_server -a 127.0.0.2 -p $PORT -n 1 -c 1 -m 2 $T -r 17 -l /dev/null > $O/$name.srv.txt 2>&1 & SP=$!
sleep 0.4
./cannibals_savage -a 127.0.0.1 -p $PORT -i 1 -w 1 > $O/$name.wrong.txt 2>&1; rw=$?
./cannibals_cook -a 127.0.0.2 -p $PORT -w 3 > $O/$name.cook.txt 2>&1 & CP=$!
./cannibals_savage -a 127.0.0.2 -p $PORT -i 1 -w 3 > $O/$name.sav.txt 2>&1; rs=$?
wait $SP; rc=$?; wait $CP
[ $rw -eq 1 ] && [ $rs -eq 0 ] && [ $rc -eq 0 ] && ok bind_address || bad bind_address "wrong=$rw sav=$rs srv=$rc"

left=$(pgrep -c -f '^\./cannibals_')
[ "$left" -eq 0 ] && ok no_stray_processes || bad no_stray_processes "осталось $left"
echo "Итого: OK=$pass FAIL=$fail"
[ "$fail" -eq 0 ]
