#!/usr/bin/env python3
"""Минимальный LFS-сервер для тестов: Batch API, basic transfer, Locking API.

Аргументы: <каталог хранилища> <файл, куда записать порт>
LFS_TEST_AUTH=<значение>  — требовать заголовок Authorization с этим значением.
Каждый batch-запрос дописывается в <хранилище>.batch.log: "<operation> <число объектов>".
"""
import hashlib
import json
import os
import re
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

STORE = sys.argv[1]
AUTH = os.environ.get("LFS_TEST_AUTH")
OID_RE = re.compile(r"^[0-9a-f]{64}$")
LOCKS = {}
LOCK_SEQ = [0]
MU = threading.Lock()


def obj_path(oid):
    return os.path.join(STORE, oid)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        sys.stderr.write("lfs-server: " + fmt % args + "\n")

    def send_json(self, code, data):
        body = json.dumps(data).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/vnd.git-lfs+json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def authorized(self):
        if AUTH is None or self.headers.get("Authorization") == AUTH:
            return True
        self.send_response(401)
        self.send_header("WWW-Authenticate", 'Basic realm="lfs"')
        self.send_header("Content-Length", "0")
        self.end_headers()
        return False

    def base(self):
        return "http://%s:%d" % self.server.server_address[:2]

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        data = json.loads(self.rfile.read(length) or b"{}")
        if not self.authorized():
            return
        path = urlparse(self.path).path
        if path.endswith("/info/lfs/objects/batch"):
            self.batch(data)
        elif path == "/verify":
            ok = os.path.exists(obj_path(data.get("oid", "")))
            self.send_json(200 if ok else 404, {"message": "ok" if ok else "missing"})
        elif path.endswith("/info/lfs/locks"):
            self.create_lock(data)
        elif path.endswith("/info/lfs/locks/verify"):
            with MU:
                ours = sorted(LOCKS.values(), key=lambda l: int(l["id"]))
            self.send_json(200, {"ours": ours, "theirs": [], "next_cursor": ""})
        elif re.search(r"/info/lfs/locks/[^/]+/unlock$", path):
            lock_id = path.split("/")[-2]
            with MU:
                lock = LOCKS.pop(lock_id, None)
            if lock:
                self.send_json(200, {"lock": lock})
            else:
                self.send_json(404, {"message": "lock not found"})
        else:
            self.send_json(404, {"message": "not found"})

    def create_lock(self, data):
        with MU:
            for lock in LOCKS.values():
                if lock["path"] == data["path"]:
                    self.send_json(409, {"lock": lock, "message": "already created lock"})
                    return
            LOCK_SEQ[0] += 1
            lock = {
                "id": str(LOCK_SEQ[0]),
                "path": data["path"],
                "locked_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                "owner": {"name": "test"},
            }
            LOCKS[lock["id"]] = lock
        self.send_json(201, {"lock": lock})

    def list_locks(self):
        q = parse_qs(urlparse(self.path).query)
        with MU:
            locks = sorted(LOCKS.values(), key=lambda l: int(l["id"]))
        if "path" in q:
            locks = [l for l in locks if l["path"] == q["path"][0]]
        if "id" in q:
            locks = [l for l in locks if l["id"] == q["id"][0]]
        start = int(q.get("cursor", ["0"])[0] or 0)
        limit = int(q.get("limit", ["2"])[0])  # маленькая страница — проверяем пагинацию
        page = locks[start:start + limit]
        nxt = str(start + limit) if start + limit < len(locks) else ""
        self.send_json(200, {"locks": page, "next_cursor": nxt})

    def batch(self, data):
        op = data.get("operation")
        objs = data.get("objects", [])
        with MU, open(STORE + ".batch.log", "a") as log:
            log.write("%s %d\n" % (op, len(objs)))
        out = []
        for o in objs:
            oid, size = o["oid"], o["size"]
            item = {"oid": oid, "size": size}
            exists = OID_RE.match(oid) and os.path.exists(obj_path(oid))
            href = self.base() + "/objects/" + oid
            hdr = {"Authorization": AUTH} if AUTH else {}
            if op == "download":
                if exists:
                    item["actions"] = {"download": {"href": href, "header": hdr}}
                else:
                    item["error"] = {"code": 404, "message": "Object does not exist"}
            elif not exists:
                item["actions"] = {
                    "upload": {"href": href, "header": hdr},
                    "verify": {"href": self.base() + "/verify", "header": hdr},
                }
            out.append(item)
        self.send_json(200, {"transfer": "basic", "objects": out})

    def do_GET(self):
        if not self.authorized():
            return
        if urlparse(self.path).path.endswith("/info/lfs/locks"):
            self.list_locks()
            return
        oid = self.path.rsplit("/", 1)[-1]
        if not OID_RE.match(oid) or not os.path.exists(obj_path(oid)):
            self.send_json(404, {"message": "not found"})
            return
        with open(obj_path(oid), "rb") as f:
            body = f.read()
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_PUT(self):
        if not self.authorized():
            return
        oid = self.path.rsplit("/", 1)[-1]
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        if not OID_RE.match(oid) or hashlib.sha256(body).hexdigest() != oid:
            self.send_json(422, {"message": "bad object"})
            return
        with open(obj_path(oid), "wb") as f:
            f.write(body)
        self.send_response(200)
        self.send_header("Content-Length", "0")
        self.end_headers()


def main():
    os.makedirs(STORE, exist_ok=True)
    srv = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    with open(sys.argv[2], "w") as f:
        f.write(str(srv.server_address[1]))
    srv.serve_forever()


if __name__ == "__main__":
    main()
