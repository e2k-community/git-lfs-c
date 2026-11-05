#!/bin/sh
# Интеграционный тест: tests/run.sh <путь к собранному git-lfs>
# Поднимает локальные LFS-серверы и гоняет реальные сценарии git: push/clone/pull
# по HTTP, basic-auth, SSH (подставной ssh), параллельная и отложенная загрузка,
# include/exclude, prune, push/fetch --all, блокировки, migrate, sha256-репозитории,
# worktree и совместимость с оригинальным git-lfs, если он установлен.
set -eu

BIN=$(realpath "$1")
SRC=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp}/lfs-c-test.XXXXXX")
PIDS=""

cleanup() {
	for p in $PIDS; do kill "$p" 2>/dev/null || true; done
	rm -rf "$T"
}
trap cleanup EXIT INT TERM

PASS=0
SKIP=0
pass() { PASS=$((PASS + 1)); echo "ok $PASS - $1"; }
fail() { echo "FAIL - $1" >&2; exit 1; }
skip() { SKIP=$((SKIP + 1)); echo "skip - $1"; }
check() { desc=$1; shift; if "$@"; then pass "$desc"; else fail "$desc"; fi; }
# run_ok <описание> <команда...>: команда должна пройти, иначе показываем её stderr
run_ok() {
	desc=$1; shift
	if "$@" >"$T/out.log" 2>"$T/err.log"; then pass "$desc"; else cat "$T/err.log" >&2; fail "$desc"; fi
}

# Изолированное окружение: свой HOME, без системного gitconfig, наш git-lfs первым в PATH
mkdir -p "$T/bin" "$T/home"
ln -s "$BIN" "$T/bin/git-lfs"
export PATH="$T/bin:$PATH" HOME="$T/home" GIT_CONFIG_NOSYSTEM=1
unset GIT_DIR GIT_WORK_TREE GIT_SSH GIT_SSH_COMMAND GIT_LFS_SKIP_SMUDGE GIT_TRACE 2>/dev/null || true
git config --global user.name test
git config --global user.email test@example.com
git config --global init.defaultBranch main
git config --global advice.detachedHead false
git config --global protocol.file.allow always

# start_server <имя> [значение Authorization]
start_server() {
	name=$1
	if [ $# -gt 1 ]; then
		LFS_TEST_AUTH=$2 python3 -I "$SRC/lfs_server.py" "$T/store-$name" "$T/port-$name" 2>"$T/server-$name.log" &
	else
		python3 -I "$SRC/lfs_server.py" "$T/store-$name" "$T/port-$name" 2>"$T/server-$name.log" &
	fi
	PIDS="$PIDS $!"
	i=0
	while [ ! -s "$T/port-$name" ]; do
		i=$((i + 1))
		[ $i -lt 100 ] || fail "server $name did not start"
		sleep 0.1
	done
}
port() { cat "$T/port-$1"; }
lfsurl() { echo "http://127.0.0.1:$(port "$1")/repo/info/lfs"; }
nobj() { ls "$T/store-$1" | wc -l | tr -d ' '; }
batches() {
	if [ -f "$T/store-$1.batch.log" ]; then
		awk -v op="$2" '$1 == op { n++ } END { print n + 0 }' "$T/store-$1.batch.log"
	else
		echo 0
	fi
}
nlocal() { find .git/lfs/objects -type f 2>/dev/null | wc -l | tr -d ' '; }
is_pointer() { head -c 100 "$1" | grep -q '^version https://git-lfs.github.com/spec/v1'; }
clean_status() { [ -z "$(git status --porcelain)" ]; }
lfsconfig() { printf '[lfs]\n\turl = %s\n' "$(lfsurl "$1")" > .lfsconfig; }
out_has() { grep -q -- "$1" "$T/out.log"; }

echo "# install"
git lfs install >/dev/null
check "install sets filter.lfs.process" [ "$(git config --global filter.lfs.process)" = "git-lfs filter-process" ]
check "install sets filter.lfs.required" [ "$(git config --global filter.lfs.required)" = "true" ]
check "version" sh -c 'git lfs version | grep -q "^git-lfs/"'
check "unknown command fails" sh -c '! git lfs no-such-command 2>/dev/null'

echo "# http: отдельное хранилище через .lfsconfig"
start_server plain
git init -q --bare "$T/remote.git"
git clone -q "$T/remote.git" "$T/w1" 2>/dev/null
cd "$T/w1"
git lfs install --local >/dev/null
check "pre-push hook installed" [ -x .git/hooks/pre-push ]
git lfs track "*.bin" >/dev/null
check "track writes .gitattributes" grep -q '^\*.bin filter=lfs diff=lfs merge=lfs -text$' .gitattributes
check "track is idempotent" sh -c 'git lfs track "*.bin" | grep -q "already supported"'
check "track lists patterns" sh -c 'git lfs track | grep -q "\*.bin (.gitattributes)"'
lfsconfig plain
head -c 3000000 /dev/urandom > big.bin
echo small > small.bin
head -c 100000 /dev/urandom > "with space.bin"
: > empty.bin
git add -A
git commit -q -m init
check "committed blob is a pointer" sh -c 'git show HEAD:big.bin | grep -q "^oid sha256:$(sha256sum big.bin | cut -d" " -f1)$"'
check "empty file stays empty" [ "$(git cat-file -s HEAD:empty.bin)" = 0 ]
check "worktree is clean after commit" clean_status
run_ok "push" git push -q origin main
check "push uploaded 3 objects" [ "$(nobj plain)" = 3 ]

GO_LFS=""
if [ -x /usr/bin/git-lfs ] && ! /usr/bin/git-lfs version | grep -q lfs-c; then
	GO_LFS=/usr/bin/git-lfs
fi
if [ -n "$GO_LFS" ]; then
	check "pointer matches reference git-lfs" sh -c '[ "$('"$GO_LFS"' clean < big.bin)" = "$(git lfs clean < big.bin)" ]'
else
	skip "reference git-lfs not installed"
fi

run_ok "clone" git clone -q "$T/remote.git" "$T/w2"
cd "$T/w2"
check "clone smudges big file" cmp -s big.bin "$T/w1/big.bin"
check "clone smudges file with space" cmp -s "with space.bin" "$T/w1/with space.bin"
check "clone is clean" clean_status
check "hook installed by filter-process on clone" [ -x .git/hooks/pre-push ]

GIT_LFS_SKIP_SMUDGE=1 git clone -q "$T/remote.git" "$T/w3"
cd "$T/w3"
check "skip-smudge clone leaves pointer" is_pointer big.bin
check "ls-files shows pointer-only" sh -c 'git lfs ls-files | grep -q "^.......... - big.bin$"'
run_ok "git lfs pull" git lfs pull
check "pull downloads content" cmp -s big.bin "$T/w1/big.bin"
check "pull leaves worktree clean" clean_status
check "ls-files shows full file" sh -c 'git lfs ls-files | grep -q "^.......... \* big.bin$"'
check "ls-files --long --size" sh -c 'git lfs ls-files -l -s | grep -q "^[0-9a-f]\{64\} \* big.bin (2.9 MB)$"'
check "ls-files --name-only" sh -c 'git lfs ls-files -n | grep -qx "small.bin"'

cd "$T/w1"
head -c 500000 /dev/urandom > big.bin
git commit -q -am update
git push -q origin main 2>/dev/null
check "second push uploads only new object" [ "$(nobj plain)" = 4 ]
cd "$T/w3"
git pull -q 2>/dev/null
check "git pull smudges updated file" cmp -s big.bin "$T/w1/big.bin"

echo "# параллельная и отложенная загрузка"
start_server many
git init -q --bare "$T/remote3.git"
git clone -q "$T/remote3.git" "$T/w7" 2>/dev/null
cd "$T/w7"
git lfs track "*.dat" "*.iso" >/dev/null
lfsconfig many
for i in $(seq 1 30); do head -c 20000 /dev/urandom > "f$i.dat"; done
mkdir media docs
head -c 50000 /dev/urandom > media/m1.iso
head -c 50000 /dev/urandom > media/m2.iso
head -c 50000 /dev/urandom > docs/d1.dat
git add -A && git commit -q -m many
run_ok "push 33 objects" git push -q origin main
check "pre-push sends a single batch" [ "$(batches many upload)" = 1 ]
check "all objects uploaded" [ "$(nobj many)" = 33 ]
before=$(batches many download)
run_ok "clone 33 objects" git clone -q "$T/remote3.git" "$T/w8"
check "delayed smudge uses one batch for clone" [ "$(batches many download)" = $((before + 1)) ]
check "delayed smudge content" sh -c 'cd "$1/w8" && for f in f*.dat media/*.iso docs/d1.dat; do cmp -s "$f" "$1/w7/$f" || exit 1; done' _ "$T"
check "delayed clone is clean" sh -c 'cd "$1/w8" && [ -z "$(git status --porcelain)" ]' _ "$T"
before=$(batches many download)
GIT_LFS_NO_DELAY=1 git clone -q "$T/remote3.git" "$T/w9" 2>/dev/null
check "without delay: one batch per file" [ "$(batches many download)" = $((before + 33)) ]
check "without delay: content" cmp -s "$T/w9/f7.dat" "$T/w7/f7.dat"
GIT_LFS_SKIP_SMUDGE=1 git clone -q "$T/remote3.git" "$T/w10"
cd "$T/w10"
run_ok "pull with concurrenttransfers=1" git -c lfs.concurrenttransfers=1 lfs pull
check "single-threaded pull content" cmp -s f30.dat "$T/w7/f30.dat"

echo "# выборочная загрузка: include/exclude"
GIT_LFS_SKIP_SMUDGE=1 git clone -q "$T/remote3.git" "$T/w11"
cd "$T/w11"
run_ok "pull -I media" git lfs pull -I media
check "included dir downloaded" cmp -s media/m1.iso "$T/w7/media/m1.iso"
check "other files stay pointers" is_pointer docs/d1.dat
run_ok "pull -X *.iso" git lfs pull --exclude="*.iso"
check "rest downloaded" cmp -s docs/d1.dat "$T/w7/docs/d1.dat"
check "pull with include/exclude is clean" clean_status
git -c lfs.fetchexclude="*.iso,f1*" clone -q "$T/remote3.git" "$T/w12x" 2>/dev/null
cd "$T/w12x"
check "fetchexclude keeps iso as pointer on clone" is_pointer media/m1.iso
check "fetchexclude keeps f1* as pointer on clone" is_pointer f10.dat
check "fetchexclude downloads the rest" cmp -s f2.dat "$T/w7/f2.dat"
check "clone with fetchexclude is clean" clean_status

echo "# fetch --all, push --all / --object-id, prune"
GIT_LFS_SKIP_SMUDGE=1 git clone -q "$T/remote.git" "$T/w12"
cd "$T/w12"
run_ok "fetch --all" git lfs fetch --all
check "fetch --all gets historical objects" [ "$(nlocal)" = 4 ]
start_server copy
run_ok "push --all" git -c lfs.url="$(lfsurl copy)" lfs push --all origin
check "push --all uploads whole history" [ "$(nobj copy)" = 4 ]
start_server one
oid=$(git show HEAD:small.bin | sed -n 's/^oid sha256://p')
run_ok "push --object-id" git -c lfs.url="$(lfsurl one)" lfs push --object-id origin "$oid"
check "push --object-id uploads exactly one" [ "$(nobj one)" = 1 ]
run_ok "push --dry-run" git lfs push --dry-run --all origin
check "push --dry-run lists objects" [ "$(grep -c '^push ' "$T/out.log")" = 4 ]
run_ok "prune --dry-run" git lfs prune --dry-run
check "prune --dry-run deletes nothing" [ "$(nlocal)" = 4 ]
check "prune --dry-run reports" out_has "would delete 1 object"
run_ok "prune" git lfs prune
check "prune removes object only in pushed history" [ "$(nlocal)" = 3 ]
head -c 1000 /dev/urandom > new.bin && git add new.bin && git commit -q -m new1
head -c 1000 /dev/urandom > new.bin && git commit -q -am new2
run_ok "prune keeps unpushed" git lfs prune
check "prune keeps objects of unpushed commits" [ "$(nlocal)" = 5 ]
git checkout -q -- . 2>/dev/null || true
run_ok "pull after prune" git lfs pull
check "content restored after prune" cmp -s big.bin "$T/w1/big.bin"

echo "# status, env"
echo staged > staged.bin && git add staged.bin
echo changed > small.bin
run_ok "status" git lfs status
check "status: to be committed" out_has "staged.bin (LFS:"
check "status: not staged" out_has "small.bin (File)"
check "status: to be pushed" out_has "Objects to be pushed to origin/main:"
check "status: unpushed file listed" sh -c 'sed -n "/to be pushed/,/^$/p" "$1/out.log" | grep -q new.bin' _ "$T"
git checkout -q -- small.bin
run_ok "env" git lfs env
check "env shows endpoint" out_has "Endpoint=$(lfsurl plain) (auth=none)"
check "env shows concurrency" out_has "ConcurrentTransfers=8"
check "env shows filter config" out_has 'git config filter.lfs.process = "git-lfs filter-process"'

echo "# блокировки"
cd "$T/w2"
run_ok "lock" git lfs lock big.bin
check "lock prints path" out_has "Locked big.bin"
check "second lock fails" sh -c '! git lfs lock big.bin 2>/dev/null'
mkdir -p sub && cd sub
run_ok "lock relative path" git lfs lock ../small.bin
cd ..
run_ok "lock ./path" git lfs lock "./with space.bin"
run_ok "locks" git lfs locks
check "locks lists all (paginated)" [ "$(wc -l < "$T/out.log")" = 3 ]
check "locks format" out_has "^big.bin	test	ID:1$"
run_ok "locks --path" git lfs locks --path=small.bin
check "locks --path filters" [ "$(wc -l < "$T/out.log")" = 1 ]
run_ok "locks --json" git lfs locks --json
check "locks --json" out_has '"path":"with space.bin"'
run_ok "locks --verify" git lfs locks --verify
check "locks --verify marks ours" out_has "^O big.bin"
run_ok "unlock by path" git lfs unlock big.bin
check "unlock prints path" out_has "Unlocked big.bin"
run_ok "unlock by id" git lfs unlock --id=2 --force
check "unlock missing lock fails" sh -c '! git lfs unlock big.bin 2>/dev/null'
run_ok "locks after unlock" git lfs locks
check "one lock left" [ "$(wc -l < "$T/out.log")" = 1 ]

echo "# worktree, не-LFS содержимое, sha256"
cd "$T/w2"
git worktree add -q "$T/wt" -b wt 2>/dev/null
check "worktree shares LFS storage" cmp -s "$T/wt/big.bin" "$T/w2/big.bin"
check "worktree ls-files" sh -c 'git -C "$1/wt" lfs ls-files | grep -q "\* big.bin"' _ "$T"
cd "$T/w1"
blob=$(echo hello | git hash-object -w --stdin)
git update-index --add --cacheinfo "100644,$blob,raw.bin"
git commit -q -m raw
git push -q origin main 2>/dev/null
git clone -q "$T/remote.git" "$T/w13" 2>/dev/null
check "non-pointer content passes through smudge" [ "$(cat "$T/w13/raw.bin")" = hello ]
if git init -q --object-format=sha256 --bare "$T/remote256.git" 2>/dev/null; then
	start_server s256
	git clone -q "$T/remote256.git" "$T/w256" 2>/dev/null
	cd "$T/w256"
	git lfs track "*.bin" >/dev/null
	lfsconfig s256
	head -c 300000 /dev/urandom > a.bin
	git add -A && git commit -q -m init
	run_ok "sha256 repo: push" git push -q origin main
	run_ok "sha256 repo: clone" git clone -q "$T/remote256.git" "$T/w256b"
	check "sha256 repo: content" cmp -s "$T/w256b/a.bin" a.bin
	GIT_LFS_SKIP_SMUDGE=1 git clone -q "$T/remote256.git" "$T/w256c"
	cd "$T/w256c"
	run_ok "sha256 repo: lfs pull" git lfs pull
	check "sha256 repo: pulled content" cmp -s a.bin "$T/w256/a.bin"
else
	skip "git without sha256 support"
fi

echo "# migrate"
start_server mig
git init -q "$T/m1"
cd "$T/m1"
head -c 100000 /dev/urandom > a.dat
echo "readme" > readme.txt
git add -A && git commit -q -m c1
head -c 100000 /dev/urandom > a.dat
mkdir sub && head -c 50000 /dev/urandom > "sub/b b.dat"
git add -A && git commit -q -m c2
git branch feature
echo "*.txt text" > .gitattributes
git add -A && git commit -q -m c3
git tag v1
head -c 50000 /dev/urandom > "sub/b b.dat"
git commit -q -am c4
git checkout -q feature
head -c 70000 /dev/urandom > c.dat
git add -A && git commit -q -m feature
git checkout -q main
cp a.dat "$T/a.orig"
run_ok "migrate info" git lfs migrate info --everything
check "migrate info shows .dat" out_has '^\*.dat'
echo dirty >> readme.txt
check "migrate refuses dirty tree" sh -c '! git lfs migrate import --include="*.dat" --everything 2>/dev/null'
git checkout -q -- readme.txt
run_ok "migrate import" git lfs migrate import --include="*.dat" --everything
check "every .dat in history is a pointer" sh -c '
	for c in $(git rev-list --all); do
		git ls-tree -r "$c" | grep "\.dat$" | while read -r mode type sha path; do
			git cat-file -p "$sha" | head -1 | grep -q "^version https://git-lfs" || exit 1
		done || exit 1
	done'
check "root commit gets .gitattributes" sh -c 'git show "$(git rev-list --max-parents=0 HEAD)":.gitattributes | grep -q "^\*.dat filter=lfs"'
check "existing .gitattributes is extended" sh -c 'git show v1:.gitattributes | grep -q "^\*.txt text$" && git show v1:.gitattributes | grep -q "^\*.dat filter=lfs"'
check "non-matching files untouched" [ "$(git show HEAD:readme.txt)" = readme ]
check "feature branch rewritten" sh -c 'git show feature:c.dat | grep -q "^version https://git-lfs"'
check "worktree content preserved" cmp -s a.dat "$T/a.orig"
check "worktree clean after migrate" clean_status
lfsconfig mig
git add .lfsconfig && git commit -q -m lfsconfig
git init -q --bare "$T/mig-remote.git"
git remote add origin "$T/mig-remote.git"
run_ok "push migrated history" git push -q --all origin
check "all migrated versions uploaded" [ "$(nobj mig)" = 5 ]

echo "# ssh: git-lfs-authenticate"
start_server ssh "RemoteAuth token123"
mkdir -p "$T/sshroot/group"
git init -q --bare "$T/sshroot/group/repo.git"
cat > "$T/bin/fake-ssh" <<EOF
#!/bin/sh
for a; do cmd=\$a; done
case "\$cmd" in
git-lfs-authenticate*)
	echo "\$cmd" >> "$T/ssh.log"
	printf '{"href":"http://127.0.0.1:%s/group/repo.git/info/lfs","header":{"Authorization":"RemoteAuth token123"},"expires_in":3600}\n' "\$(cat "$T/port-ssh")"
	;;
*)
	cd "$T/sshroot" && exec sh -c "\$cmd"
	;;
esac
EOF
chmod +x "$T/bin/fake-ssh"
export GIT_SSH_COMMAND="$T/bin/fake-ssh" GIT_SSH_VARIANT=simple
git clone -q git@fakehost:group/repo.git "$T/w5" 2>/dev/null
cd "$T/w5"
git lfs track "*.bin" >/dev/null
head -c 1500000 /dev/urandom > ssh.bin
git add -A && git commit -q -m init
run_ok "push over ssh" git push -q origin main
check "ssh push uploaded" [ "$(nobj ssh)" = 1 ]
check "ssh upload authenticate" grep -q '^git-lfs-authenticate group/repo.git upload$' "$T/ssh.log"
run_ok "clone over ssh" git clone -q git@fakehost:group/repo.git "$T/w6"
check "clone over ssh content" cmp -s "$T/w6/ssh.bin" ssh.bin
check "ssh download authenticate" grep -q '^git-lfs-authenticate group/repo.git download$' "$T/ssh.log"
run_ok "locks over ssh" git lfs lock ssh.bin
check "env shows ssh endpoint" sh -c 'git lfs env | grep -q "auth=ssh git@fakehost group/repo.git"'
unset GIT_SSH_COMMAND GIT_SSH_VARIANT

echo "# http: basic auth через git credential"
start_server auth "Basic dTpw"   # u:p
git init -q --bare "$T/remote2.git"
git clone -q "$T/remote2.git" "$T/w4" 2>/dev/null
cd "$T/w4"
git config credential.helper '!f() { test "$1" = get && printf "username=u\npassword=p\n"; }; f'
git lfs track "*.bin" >/dev/null
lfsconfig auth
head -c 200000 /dev/urandom > a.bin
git add -A && git commit -q -m init
run_ok "push with credential helper" git push -q origin main
check "credential push uploaded" [ "$(nobj auth)" = 1 ]
run_ok "credentials embedded in lfs.url" \
	git -c lfs.url="http://u:p@127.0.0.1:$(port auth)/repo/info/lfs" -c credential.helper= lfs push --all origin
check "no credentials: request rejected" sh -c '! GIT_TERMINAL_PROMPT=0 git -c credential.helper= lfs push --all origin 2>/dev/null'
git config credential.helper '!f() { test "$1" = get && printf "username=u\npassword=bad\n"; }; f'
head -c 1000 /dev/urandom > b.bin
git add -A && git commit -q -m bad
check "push with wrong credentials fails" sh -c '! git push -q origin main 2>/dev/null'

echo "# ошибки сервера"
rm -f "$T/store-plain/$(cd "$T/w1" && git show HEAD:small.bin | sed -n 's/^oid sha256://p')"
GIT_LFS_SKIP_SMUDGE=1 git clone -q "$T/remote.git" "$T/w14"
cd "$T/w14"
check "pull of missing object fails" sh -c '! git lfs pull 2>"$1/err.log"' _ "$T"
check "missing object error is reported" grep -q "small.bin: Object does not exist" "$T/err.log"
check "other files still downloaded" cmp -s big.bin "$T/w1/big.bin"
check "clone fails when object is missing" sh -c '! git clone -q "$1/remote.git" "$1/w15" 2>/dev/null' _ "$T"

echo "# совместимость с оригинальным git-lfs"
if [ -n "$GO_LFS" ]; then
	start_server interop
	mkdir -p "$T/gobin" && ln -s "$GO_LFS" "$T/gobin/git-lfs"
	git init -q --bare "$T/interop.git"
	git clone -q "$T/interop.git" "$T/i1" 2>/dev/null
	cd "$T/i1"
	git lfs track "*.bin" >/dev/null
	lfsconfig interop
	head -c 400000 /dev/urandom > ours.bin
	git add -A && git commit -q -m ours
	git push -q origin main 2>/dev/null
	PATH="$T/gobin:$PATH" git clone -q "$T/interop.git" "$T/i2" 2>"$T/err.log" || { cat "$T/err.log"; fail "clone with reference git-lfs"; }
	check "reference git-lfs downloads our objects" cmp -s "$T/i2/ours.bin" ours.bin
	cd "$T/i2"
	head -c 400000 /dev/urandom > theirs.bin
	PATH="$T/gobin:$PATH" git add theirs.bin
	PATH="$T/gobin:$PATH" git commit -q -m theirs
	PATH="$T/gobin:$PATH" git push -q origin main 2>/dev/null
	run_ok "clone objects uploaded by reference git-lfs" git clone -q "$T/interop.git" "$T/i3"
	check "we download their objects" cmp -s "$T/i3/theirs.bin" theirs.bin
	cd "$T/i3"
	check "their pointer equals ours" [ "$(git show HEAD:theirs.bin)" = "$(git lfs clean < theirs.bin)" ]
else
	skip "reference git-lfs not installed"
fi

echo "# untrack, uninstall"
cd "$T/w1"
run_ok "untrack" git lfs untrack "*.bin"
check "untrack removes pattern" sh -c '! grep -q "^\*.bin " .gitattributes'
git checkout -q -- .gitattributes
run_ok "uninstall --local" git lfs uninstall --local
check "uninstall removes local filter config" sh -c '! git config --local filter.lfs.process'
check "uninstall removes our hook" [ ! -e .git/hooks/pre-push ]

echo "# все тесты пройдены: $PASS (пропущено: $SKIP)"
