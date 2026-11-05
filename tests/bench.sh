#!/bin/bash
# Замер скорости команд git-lfs на одинаковом наборе данных.
#
#   tests/bench.sh <путь к git-lfs>
#
# Сценарий один и тот же, меняется только бинарник. Сравнение двух реализаций:
#   tests/bench.sh build/git-lfs
#   tests/bench.sh /usr/bin/git-lfs
#
# Переменные окружения:
#   BENCH_DIR   рабочий каталог (по умолчанию /var/tmp/lfsbench)
#   NSMALL      число маленьких файлов (2000)    SMALL  их размер (50K)
#   NBIG        число больших файлов (3)         BIG    их размер (200M)
#   THREADS     lfs.concurrenttransfers (по умолчанию не задаётся, у обоих 8)
#
# Набор данных создаётся при первом запуске и переиспользуется следующими,
# пока не изменятся NSMALL/SMALL/NBIG/BIG. Лог пишется в $BENCH_DIR/<версия git-lfs>.log.
# Нужны git, python3 и место на диске примерно 7 x (NSMALL*SMALL + NBIG*BIG).
set -eu

if [ $# -ne 1 ]; then
	echo "usage: $0 <path-to-git-lfs>" >&2
	exit 2
fi
LFS_BIN=$(realpath "$1")
[ -x "$LFS_BIN" ] || { echo "not executable: $LFS_BIN" >&2; exit 2; }
SRC=$(cd "$(dirname "$0")/.." && pwd)
SERVER=$SRC/tests/lfs_server.py
[ -f "$SERVER" ] || { echo "not found: $SERVER" >&2; exit 2; }

B=${BENCH_DIR:-/var/tmp/lfsbench}
NSMALL=${NSMALL:-2000} SMALL=${SMALL:-50K} NBIG=${NBIG:-3} BIG=${BIG:-200M}
DATA=$B/data
W=$B/run
# Имя лога — из строки версии, например git-lfs-0.2.0-lfs-c / git-lfs-3.8.0-GitHub
VERSION=$("$LFS_BIN" version)
LOG=$B/$(echo "$VERSION" | awk '{print $1 "-" $2}' | tr -c 'A-Za-z0-9._\n-' '-' | tr -s '-' | sed 's/-*$//').log
SRV=""

cleanup() { [ -z "$SRV" ] || kill "$SRV" 2>/dev/null || true; }
trap cleanup EXIT INT TERM

mkdir -p "$B"
# Набор данных: общий для всех запусков, пересоздаётся при смене параметров
SPEC="$NSMALL x $SMALL + $NBIG x $BIG"
if [ "$(cat "$DATA/.spec" 2>/dev/null)" != "$SPEC" ]; then
	echo "generating data: $SPEC ..."
	rm -rf "$DATA"; mkdir -p "$DATA"
	for i in $(seq 1 "$NSMALL"); do head -c "$SMALL" /dev/urandom > "$DATA/small_$i.bin"; done
	for i in $(seq 1 "$NBIG");   do head -c "$BIG"   /dev/urandom > "$DATA/big_$i.bin";   done
	echo "$SPEC" > "$DATA/.spec"
fi

# Изолированное окружение: свой HOME, без системного gitconfig, нужный git-lfs первым в PATH
rm -rf "$W"; mkdir -p "$W/bin" "$W/home"
ln -s "$LFS_BIN" "$W/bin/git-lfs"
export HOME=$W/home GIT_CONFIG_NOSYSTEM=1 PATH="$W/bin:$PATH"
unset GIT_DIR GIT_WORK_TREE GIT_LFS_SKIP_SMUDGE GIT_TRACE GIT_CURL_VERBOSE 2>/dev/null || true
git config --global user.name bench
git config --global user.email bench@example.com
git config --global init.defaultBranch main
git config --global advice.detachedHead false
[ -z "${THREADS:-}" ] || git config --global lfs.concurrenttransfers "$THREADS"
git lfs install >/dev/null

# Тестовый сервер отвечает по HTTP/1.0 (соединение на каждый запрос), поэтому
# увеличиваем очередь входящих соединений: при стандартных 5 часть соединений
# отбрасывается и клиент ждёт повтора SYN по 1 с.
python3 -I -c 'import sys, runpy, http.server as h
h.ThreadingHTTPServer.request_queue_size = 256
sys.argv = sys.argv[1:]
runpy.run_path(sys.argv[0], run_name="__main__")' "$SERVER" "$W/store" "$W/port" 2>/dev/null &
SRV=$!
i=0
while [ ! -s "$W/port" ]; do
	i=$((i + 1)); [ $i -lt 100 ] || { echo "server did not start" >&2; exit 1; }
	sleep 0.1
done
URL="http://127.0.0.1:$(cat "$W/port")/repo/info/lfs"

TIMEFORMAT='%R %U %S'
# t <название> <команда...>: замер одной команды, вывод команды отбрасывается
t() {
	name=$1; shift
	tf=$W/time.out
	{ time "$@" >/dev/null 2>"$W/err.log"; } 2>"$tf" || {
		echo "FAILED: $name" >&2; cat "$W/err.log" >&2; exit 1
	}
	read -r real user sys < "$tf"
	printf '%-36s %9s %9s %9s\n' "$name" "$real" "$user" "$sys" | tee -a "$LOG"
}

: > "$LOG"
{
	echo "# $VERSION ($LFS_BIN)"
	echo "# $(uname -srm), $(nproc) cpu, $(git --version), data: $SPEC"
	printf '%-36s %9s %9s %9s\n' "command" "real, s" "user, s" "sys, s"
} | tee -a "$LOG"

git init -q --bare "$W/remote.git"
git init -q "$W/repo"; cd "$W/repo"
printf '[lfs]\n\turl = %s\n' "$URL" > .lfsconfig
git lfs track '*.bin' >/dev/null
cp "$DATA"/*.bin .
t "add (clean, sha256)"                git add .
git commit -qm init
git remote add origin "$W/remote.git"
t "push (upload)"                      git push -q origin main
t "ls-files"                           git lfs ls-files
t "status"                             git lfs status

cd "$W"
t "clone (smudge + download)"          git clone -q remote.git clone1
t "clone GIT_LFS_SKIP_SMUDGE=1"        env GIT_LFS_SKIP_SMUDGE=1 git clone -q remote.git clone2
cd clone2
t "lfs fetch (download only)"          git lfs fetch
t "lfs checkout (from local store)"    git lfs checkout
rm -f -- *.bin
t "git checkout (smudge, local)"       git checkout -q -- .
t "prune"                              git lfs prune

git init -q "$W/mig"; cd "$W/mig"
cp "$DATA"/*.bin .
git add . && git commit -qm plain
t "migrate import --everything"        git lfs migrate import --include='*.bin' --everything

# Проверка, что всё действительно отработало
n=$((NSMALL + NBIG))
[ "$(ls "$W/store" | wc -l)" -eq "$n" ] || { echo "server has $(ls "$W/store" | wc -l) objects, expected $n" >&2; exit 1; }
cmp -s "$W/clone1/big_1.bin" "$DATA/big_1.bin" || { echo "clone content mismatch" >&2; exit 1; }
[ "$(cd "$W/mig" && git lfs ls-files | wc -l)" -eq "$n" ] || { echo "migrate did not convert all files" >&2; exit 1; }
echo "# ok, log: $LOG"
