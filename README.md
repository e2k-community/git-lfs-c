# git-lfs-c

Реализация клиента Git LFS на C, совместимая с оригинальным git-lfs. Бинарник
называется `git-lfs` и подменяет оригинал без изменений в инфраструктуре: тот же
формат pointer-файлов, тот же `.git/lfs/objects`, те же ключи `filter.lfs.*`
в gitconfig. Можно переключаться между реализациями на одном клоне.
Совместимость в обе стороны с оригинальным git-lfs проверяется в тестах.

Зависимости: libcurl, libcrypto (OpenSSL 1.1 или 3.x), pthreads. Go не нужен.

## Сборка и тесты

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

На Эльбрусе (e2k) лучше собирать clang'ом и в режиме Release: на хешировании
он даёт примерно на 15 % больше, чем lcc.

```sh
CC=clang cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel
```

- `unit`: разборщики (JSON, pointer, pkt-line, URL, шаблоны путей), запуск
  дочерних процессов, clean во временном репозитории;
- `integration` (`tests/run.sh build/git-lfs`): около 140 проверок на настоящем
  git с локальным LFS-сервером (`tests/lfs_server.py`, нужен `python3`).
  Проверяются push, clone и pull по HTTP, basic-auth, SSH через
  `git-lfs-authenticate` (с подставным ssh), параллельная и отложенная загрузка,
  include/exclude, `prune`, `push`/`fetch --all`, блокировки, `migrate`,
  репозитории SHA-256, worktree, обработка ошибок сервера. Если в
  `/usr/bin/git-lfs` установлен оригинальный git-lfs, проверяется ещё и обмен
  объектами с ним в обе стороны.

## Команды

| Команда | Что делает |
|---|---|
| `install [--local\|--system\|--worktree] [--skip-repo] [--skip-smudge] [--force]` | прописывает `filter.lfs.*` и ставит хук `pre-push` |
| `uninstall [--local\|--system]` | убирает `filter.lfs` и свой хук |
| `track [<pattern>...]`, `untrack <pattern>...` | правят `.gitattributes` в корне репозитория |
| `clean`, `smudge`, `filter-process` | фильтры, их вызывает сам git |
| `pre-push <remote>` | выгрузка при `git push`, вызывается хуком |
| `fetch [-I <pats>] [-X <pats>] [--all] [<remote> [<ref>...]]` | скачивает недостающие объекты (`--all`: из всей истории) |
| `checkout` | заменяет pointer'ы в рабочем дереве скачанным содержимым |
| `pull [-I <pats>] [-X <pats>] [<remote> [<ref>...]]` | `fetch` + `checkout` |
| `push [--all\|--object-id] [--dry-run] <remote> [<ref>\|<oid>...]` | ручная выгрузка объектов |
| `prune [--dry-run] [--verbose]` | удаляет локальные объекты, которые больше не нужны |
| `ls-files [-l] [-s] [-n] [<ref>]` | LFS-файлы в дереве: `*` — содержимое на месте, `-` — только pointer |
| `status` | LFS-объекты, которые будут отправлены или закоммичены, и неиндексированные изменения |
| `env` | endpoint, каталоги и настройки (без обращения к серверу) |
| `lock [--json] <path>`, `unlock [--force] (<path>\|--id=<id>)` | блокировки файлов |
| `locks [--path=] [--id=] [--limit=] [--verify] [--json]` | список блокировок |
| `migrate import --include=<pats> [--exclude=<pats>] [--everything\|<ref>...]` | переводит существующую историю в LFS |
| `migrate info [--everything] [--above=<size>] [--top=<n>]` | какие типы файлов занимают больше всего места |
| `version` | версия |

`post-checkout`, `post-commit` и `post-merge` ничего не делают. Они нужны,
чтобы не ломались хуки, оставшиеся от оригинального git-lfs.

### Что сохраняет `prune`

Объекты из `HEAD`, из вершин всех локальных веток, из индекса, из рабочих
деревьев (`git worktree`), из stash, а также из всех коммитов, которых нет ни
в одной remote-tracking ветке (то есть неотправленных). Остальное удаляется.
Временные файлы старше часа тоже удаляются.

### `migrate import`

Переписывает историю цепочкой `git fast-export --no-data | git-lfs | git fast-import --force`.
Файлы под `--include` превращаются в LFS-объекты (они ложатся в `.git/lfs/objects`).
В корневые коммиты добавляется `.gitattributes`, а уже существующий
`.gitattributes` дополняется шаблонами. Подписи у тегов снимаются. Без
`--everything` и без ref'ов переписывается только текущая ветка. Перед запуском
рабочее дерево должно быть чистым, после миграции нужен `git push --force`.

## Производительность

- Объекты передаются в `lfs.concurrenttransfers` потоков (по умолчанию 8),
  у каждого потока своё соединение.
- При `git clone`/`checkout` smudge работает в режиме `delay`: git отдаёт
  все файлы сразу, и они скачиваются одним batch-запросом в несколько потоков,
  а не по одному. Отключается через `GIT_LFS_NO_DELAY=1`.
- `pre-push` собирает все объекты в один batch (по 100 объектов на запрос).
- SHA-256: на e2k используется своя реализация (`src/sha256.c`), на
  Эльбрус-8СВ она примерно вдвое быстрее OpenSSL (141 против 63 МБ/с с clang),
  потому что у OpenSSL для e2k нет оптимизированного кода. На остальных
  архитектурах используется OpenSSL с аппаратными инструкциями. Выбор
  переопределяется опцией `-DLFS_BUILTIN_SHA256=ON|OFF`, текущая реализация
  видна в `git lfs env` (`SHA256=builtin|openssl`). Замеры вариантов лежат в
  `experiments/sha256`.
- `tests/bench.sh <путь к git-lfs>` замеряет основные команды на одинаковом
  наборе данных; так сравниваются реализации между собой.

## Откуда берётся адрес LFS-сервера

По порядку:
1. `lfs.url`: из git config или `.lfsconfig` (из рабочего дерева, иначе из `HEAD`);
2. `remote.<remote>.lfsurl`;
3. URL remote'а (с учётом `url.*.insteadOf`):
   - `https://host/group/repo(.git)` → `https://host/group/repo.git/info/lfs`;
   - `git@host:group/repo.git`, `ssh://…`: выполняется
     `ssh host git-lfs-authenticate group/repo.git <download|upload>`, адрес и
     токен берутся из ответа. Если ssh не сработал, используется
     `https://host/group/repo.git/info/lfs` с авторизацией через `git credential`.

ssh запускается так же, как его запускает git: `GIT_SSH_COMMAND`,
`core.sshCommand`, `GIT_SSH`, `ssh`.

HTTPS: логин и пароль из URL, иначе `git credential fill` после ответа 401
(при успехе `approve`, при отказе `reject`). Учитываются `http.sslVerify`,
`http.sslCAInfo`, `http.sslCAPath`, `http.proxy`, `GIT_SSL_NO_VERIFY`,
`GIT_SSL_CAINFO`.

## Настройки и переменные окружения

- `lfs.concurrenttransfers`: число потоков передачи (1–64, по умолчанию 8);
- `lfs.fetchinclude` / `lfs.fetchexclude`: какие файлы скачивать при
  clone/checkout/pull (остальные остаются pointer'ами). Список шаблонов через
  запятую: шаблон без `/` сравнивается с любым компонентом пути (`*.iso`,
  `media`), шаблон с `/` сравнивается с путём от корня (`assets/textures`);
- `GIT_LFS_SKIP_SMUDGE=1`: не скачивать при checkout;
- `GIT_LFS_NO_DELAY=1`: отключить отложенный smudge;
- `GIT_TRACE=1`: отладочный вывод (запускаемые команды, HTTP-запросы);
- `GIT_CURL_VERBOSE=1`: подробный вывод libcurl.

## Чего пока нет

- `migrate export`, `migrate import --above`, `fsck`, `logs`, `dedup`, `fetch --recent`;
- SSH-протокол передачи `git-lfs-transfer` (поддерживается только
  `git-lfs-authenticate` + HTTPS);
- транспорты, кроме `basic` (tus, custom transfer agents);
- `lockable`-файлы (только для чтения без блокировки) и проверка блокировок в `pre-push`;
- расширения pointer'ов (`ext-*`), NTLM/Kerberos.
