#!/usr/bin/env python3
"""HTTP-API test suite (Teststrategie Stufe 4).

Starts one or more MilanSQL instances as real subprocesses, each bound to an
OS-assigned free port on 127.0.0.1 in its own throwaway temp directory, and
drives them with real HTTP requests (no in-process shortcuts). This is the
gap the rest of the ctest suite doesn't cover: the C++ unit tests call the
engine/dispatch layer directly and never go through actual sockets, so the
HTTP routing, status codes, and auth middleware were previously untested at
the protocol level.

NEVER point this at port 8080 (production) or at a persistent service.
Every instance here is a fresh, disposable process the script itself starts
and stops.

Exit code: 0 if every check() passed (XFAIL is allowed and does not fail the
build; an unexpected pass of a check_known_failure() does fail the build, the
same convention as checkKnownFailure() in src/tests/milansql_tests.cpp, see
docs/milansql-audit.md).
"""
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

BINARY = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.abspath("build/milansql")

PASS = 0
FAIL = 0
XFAIL = 0
UNEXPECTED_PASS = 0
FAILED_NAMES = []


def check(name, cond, detail=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"[PASS] {name}")
    else:
        FAIL += 1
        FAILED_NAMES.append(name)
        print(f"[FAIL] {name}" + (f" -- {detail}" if detail else ""))


def check_known_failure(name, cond, audit_ref, detail=""):
    """Same convention as checkKnownFailure() in milansql_tests.cpp: a known,
    not-yet-fixed gap is expected to stay red (XFAIL). If it ever starts
    passing, that's surfaced loudly instead of silently going green, so
    someone converts it back to check()."""
    global XFAIL, UNEXPECTED_PASS
    if cond:
        UNEXPECTED_PASS += 1
        print(f"[PASS] {name}  *** unexpected pass -- please convert to check(), see {audit_ref} ***")
    else:
        XFAIL += 1
        print(f"[XFAIL] {name}  (known gap, see {audit_ref})" + (f" -- {detail}" if detail else ""))


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def http(method, url, body=None, headers=None, timeout=5, raw_body=None):
    headers = dict(headers or {})
    data = None
    if raw_body is not None:
        data = raw_body.encode()
    elif body is not None:
        data = json.dumps(body).encode()
        headers.setdefault("Content-Type", "application/json")
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            raw = resp.read().decode()
            try:
                j = json.loads(raw) if raw else {}
            except json.JSONDecodeError:
                j = {}
            return resp.status, j, dict(resp.headers)
    except urllib.error.HTTPError as e:
        raw = e.read().decode()
        try:
            j = json.loads(raw) if raw else {}
        except json.JSONDecodeError:
            j = {}
        return e.code, j, dict(e.headers)


class Instance:
    """One disposable MilanSQL HTTP server, isolated in its own temp dir and
    on its own OS-assigned port. Never port 8080, never a shared directory."""

    def __init__(self):
        self.dir = tempfile.mkdtemp(prefix="milansql_http_test_")
        self.port = free_port()
        self.proc = None

    def base(self):
        return f"http://127.0.0.1:{self.port}"

    def init_admin(self, username, password):
        env = dict(os.environ)
        env["MILANSQL_INIT_ADMIN_PASSWORD"] = password
        p = subprocess.run(
            [BINARY, "--init-admin", username],
            cwd=self.dir, env=env, capture_output=True, text=True, timeout=10,
        )
        return p

    def run_oneshot(self, extra_args, timeout=5):
        """Run the server binary and wait for it to exit on its own (used for
        the fail-closed startup checks, where exiting IS the expected
        behavior). Returns None if it didn't exit within timeout (meaning it
        started serving instead of refusing -- treat as failure upstream)."""
        try:
            p = subprocess.run(
                [BINARY, "--http", "--http-port", str(self.port), "--host", "127.0.0.1"] + extra_args,
                cwd=self.dir, capture_output=True, text=True, timeout=timeout,
            )
            return p
        except subprocess.TimeoutExpired as e:
            # Didn't exit -- it's still running (and listening). Kill it.
            self._kill_leftover()
            return None

    def _kill_leftover(self):
        subprocess.run(["pkill", "-9", "-f", f"{BINARY} --http --http-port {self.port}"],
                        capture_output=True)

    def start(self, extra_args=None):
        args = [BINARY, "--http", "--http-port", str(self.port), "--host", "127.0.0.1"]
        if extra_args:
            args += extra_args
        self.proc = subprocess.Popen(
            args, cwd=self.dir, stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )
        self._wait_ready()

    def _wait_ready(self, timeout=10):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.proc.poll() is not None:
                out = self.proc.stdout.read() if self.proc.stdout else ""
                raise RuntimeError(f"server exited early (code {self.proc.returncode}):\n{out}")
            try:
                http("GET", self.base() + "/health/live", timeout=0.5)
                return
            except Exception:
                time.sleep(0.1)
        raise RuntimeError("server did not become ready within timeout")

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)

    def cleanup(self):
        self.stop()
        shutil.rmtree(self.dir, ignore_errors=True)


# ─────────────────────────────────────────────────────────────────────────
# Phase 1+2: Fail-closed startup (no auth file / empty auth file)
# ─────────────────────────────────────────────────────────────────────────

def phase_fail_closed():
    print("\n-- Phase: fail-closed startup --")

    inst = Instance()
    try:
        p = inst.run_oneshot([])
        check("fail-closed: refuses to start with no auth file (exits, doesn't bind)",
              p is not None, "" if p is not None else "server kept running instead of exiting")
        if p is not None:
            check("fail-closed: exit code 1 (no auth file)", p.returncode == 1, f"got {p.returncode}")
            check("fail-closed: stderr mentions fail-closed (no auth file)",
                  "fail-closed" in p.stderr.lower(), p.stderr[:200])
    finally:
        inst.cleanup()

    inst = Instance()
    try:
        open(os.path.join(inst.dir, "database.milan.auth"), "w").close()  # 0 bytes
        p = inst.run_oneshot([])
        check("fail-closed: refuses to start with empty auth file (exits, doesn't bind)",
              p is not None, "" if p is not None else "server kept running instead of exiting")
        if p is not None:
            check("fail-closed: exit code 1 (empty auth file)", p.returncode == 1, f"got {p.returncode}")
            check("fail-closed: stderr mentions fail-closed (empty auth file)",
                  "fail-closed" in p.stderr.lower(), p.stderr[:200])
    finally:
        inst.cleanup()


# ─────────────────────────────────────────────────────────────────────────
# Phase 3: Login correct/incorrect, root/root rejected
# ─────────────────────────────────────────────────────────────────────────

def phase_login():
    print("\n-- Phase: login --")
    inst = Instance()
    try:
        inst.init_admin("httptest_admin", "HttpTestAdmin!2026")
        inst.start()
        b = inst.base()

        status, j, headers = http("POST", b + "/auth/login",
                                   {"username": "httptest_admin", "password": "HttpTestAdmin!2026"})
        check("login: correct credentials -> HTTP 200", status == 200, f"got {status}")
        check("login: correct credentials -> success:true + token", j.get("success") is True and bool(j.get("token")))
        check("login: sets HttpOnly session cookie",
              "milansql_token=" in headers.get("Set-Cookie", "") and "HttpOnly" in headers.get("Set-Cookie", ""))

        status_wrong, j_wrong, _ = http("POST", b + "/auth/login",
                                         {"username": "httptest_admin", "password": "totally-wrong"})
        check("login: wrong password -> HTTP 200 (no status-code enumeration)", status_wrong == 200)
        check("login: wrong password -> success:false", j_wrong.get("success") is False)

        status_root, j_root, _ = http("POST", b + "/auth/login",
                                       {"username": "root", "password": "root"})
        check("login: root/root is rejected (no such account, no default bootstrap)",
              j_root.get("success") is False)
        check("login: root/root gives the same generic error as a wrong password "
              "(doesn't leak whether the account exists)",
              j_root.get("error") == j_wrong.get("error"))
    finally:
        inst.cleanup()


# ─────────────────────────────────────────────────────────────────────────
# Phase 4: Rate-limit behavior on /auth/login
# ─────────────────────────────────────────────────────────────────────────

def phase_rate_limit():
    print("\n-- Phase: rate limiting --")
    inst = Instance()
    try:
        inst.init_admin("ratelimit_admin", "RateLimitAdmin!2026")
        inst.start()
        b = inst.base()

        # Layer 1 is a per-IP token bucket (5 burst) shared by /auth/login and
        # /auth/register, independent of whether individual attempts succeed.
        # A fresh instance means a fresh bucket.
        statuses = []
        for i in range(6):
            status, j, headers = http("POST", b + "/auth/login",
                                       {"username": "ratelimit_admin", "password": f"wrong-{i}"})
            statuses.append((status, j, headers))

        first_five_ok = all(s == 200 for s, _, _ in statuses[:5])
        sixth_status, sixth_body, sixth_headers = statuses[5]
        check("rate limit: first 5 login attempts from one IP are not blocked",
              first_five_ok, f"statuses={[s for s,_,_ in statuses]}")
        check("rate limit: 6th rapid login attempt from the same IP -> HTTP 429",
              sixth_status == 429, f"got {sixth_status}")
        check("rate limit: 429 response carries Retry-After",
              "Retry-After" in sixth_headers or "retry_after" in sixth_body,
              str(sixth_headers))
    finally:
        inst.cleanup()


# ─────────────────────────────────────────────────────────────────────────
# Phase 5: Tenant isolation
# ─────────────────────────────────────────────────────────────────────────

def phase_tenant_isolation():
    print("\n-- Phase: tenant isolation --")
    inst = Instance()
    try:
        inst.init_admin("tenant_admin", "TenantAdmin!2026")
        inst.start()
        b = inst.base()

        _, ra, _ = http("POST", b + "/auth/register", {"username": "tenantA_http", "password": "TenantA12345"})
        _, rb, _ = http("POST", b + "/auth/register", {"username": "tenantB_http", "password": "TenantB12345"})
        tok_a, uid_a = ra.get("token"), ra.get("user_id")
        tok_b = rb.get("token")
        check("tenant isolation: setup -- both users registered", bool(tok_a) and bool(tok_b) and uid_a)

        def q(token, sql, params=None):
            body = {"sql": sql}
            if params is not None:
                body["params"] = params
            return http("POST", b + "/api/query", body, {"Authorization": f"Bearer {token}"})

        q(tok_a, "CREATE TABLE secret (id INT, data TEXT)")
        q(tok_a, "INSERT INTO secret VALUES (1, ?)", ["A-only-data"])
        _, own, _ = q(tok_a, "SELECT * FROM secret")
        check("tenant isolation: owner can read their own table", own.get("success") is True
              and own.get("rowCount") == 1)

        _, bare, _ = q(tok_b, "SELECT * FROM secret")
        check("tenant isolation: a different user's bare table name does not resolve to A's table",
              bare.get("success") is False)
        check("tenant isolation: ...and A's data never appears in the response",
              "A-only-data" not in json.dumps(bare))

        _, qualified, _ = q(tok_b, f"SELECT * FROM u{uid_a}_secret")
        check("tenant isolation: explicitly naming another user's physical table is denied",
              qualified.get("success") is False and "denied" in qualified.get("error", "").lower())
        check("tenant isolation: ...and that denial never leaks A's data either",
              "A-only-data" not in json.dumps(qualified))
    finally:
        inst.cleanup()


# ─────────────────────────────────────────────────────────────────────────
# Phase 6: Parameterized queries defeat SQL injection
# ─────────────────────────────────────────────────────────────────────────

def phase_sql_injection():
    print("\n-- Phase: parameterized queries vs. SQL injection --")
    inst = Instance()
    try:
        inst.init_admin("sqli_admin", "SqliAdmin!2026")
        inst.start()
        b = inst.base()
        _, login, _ = http("POST", b + "/auth/login", {"username": "sqli_admin", "password": "SqliAdmin!2026"})
        tok = login["token"]

        def q(sql, params=None):
            body = {"sql": sql}
            if params is not None:
                body["params"] = params
            return http("POST", b + "/api/query", body, {"Authorization": f"Bearer {tok}"})

        q("CREATE TABLE injtest (id INT, data TEXT)")
        q("INSERT INTO injtest VALUES (1, ?)", ["realvalue"])

        _, classic, _ = q("SELECT * FROM injtest WHERE data = ?", ["x' OR '1'='1"])
        check("SQL injection: a classic quote-breakout payload as a bound param "
              "matches 0 rows instead of bypassing the WHERE clause",
              classic.get("success") is True and classic.get("rowsAffected", -1) == 0)

        _, sane, _ = q("SELECT * FROM injtest WHERE data = ?", ["realvalue"])
        check("SQL injection: sanity check -- the correct bound value still matches normally",
              sane.get("success") is True and sane.get("rowCount") == 1)

        _, stacked, _ = q("SELECT * FROM injtest WHERE data = ?", ["x; DROP TABLE injtest; --"])
        _, survives, _ = q("SELECT * FROM injtest")
        check("SQL injection: a stacked-query payload (semicolon + DROP TABLE) inside "
              "a bound param cannot drop the table",
              survives.get("success") is True and survives.get("rowCount", 0) >= 1,
              f"stacked-query response: {stacked}")
    finally:
        inst.cleanup()


# ─────────────────────────────────────────────────────────────────────────
# Phase 7: Unknown column in WHERE
# ─────────────────────────────────────────────────────────────────────────

def phase_unknown_column():
    print("\n-- Phase: unknown column in WHERE --")
    inst = Instance()
    try:
        inst.init_admin("unkcol_admin", "UnkColAdmin!2026")
        inst.start()
        b = inst.base()
        _, login, _ = http("POST", b + "/auth/login", {"username": "unkcol_admin", "password": "UnkColAdmin!2026"})
        tok = login["token"]
        headers = {"Authorization": f"Bearer {tok}"}

        http("POST", b + "/api/query", {"sql": "CREATE TABLE unkcol (id INT, name TEXT)"}, headers)

        # Column resolution for a WHERE condition only happens per-row while
        # scanning -- on a table with zero rows the filter loop never runs,
        # so an unknown column is never actually looked up.
        status_empty, j_empty, _ = http("POST", b + "/api/query",
                                         {"sql": "SELECT * FROM unkcol WHERE nonexistent_col = 1"}, headers)
        check_known_failure(
            "unknown column in WHERE on an EMPTY table: silently reports "
            "success with 0 rows instead of a column-not-found error "
            "(the column is only resolved while scanning rows, and an empty "
            "table never scans any)",
            j_empty.get("success") is False,
            "docs/milansql-audit.md",
            f"got {j_empty}",
        )

        http("POST", b + "/api/query", {"sql": "INSERT INTO unkcol VALUES (1, 'x')"}, headers)
        # Deliberately a different literal than the empty-table query above --
        # reusing identical SQL text here would collide with the HTTP query
        # cache (see phase_cache_staleness) and conflate two different bugs.
        status, j, _ = http("POST", b + "/api/query",
                             {"sql": "SELECT * FROM unkcol WHERE nonexistent_col = 2"}, headers)

        check("unknown column in WHERE on a table WITH rows: the query is rejected "
              "(success:false), never silently returns wrong rows",
              j.get("success") is False)
        check("unknown column in WHERE on a table WITH rows: error message names "
              "the offending column",
              "nonexistent_col" in j.get("error", ""))
        check_known_failure(
            "unknown column in WHERE on a table WITH rows: HTTP status is 4xx "
            "like other client errors (currently always 200, even on failure)",
            400 <= status < 500,
            "docs/milansql-audit.md",
            f"got HTTP {status}",
        )
    finally:
        inst.cleanup()


# ─────────────────────────────────────────────────────────────────────────
# Phase 8: Read-after-write cache staleness
#
# Not one of the originally requested checks, but found while writing the
# unknown-column test above (that test originally reused one SQL string
# across an empty-table and a populated-table assertion, and the second
# assertion silently got back the first call's cached result). Significant
# enough -- and distinct enough from "unknown column" -- to get its own
# dedicated, clearly labeled test rather than being buried inside that one.
# ─────────────────────────────────────────────────────────────────────────

def phase_cache_staleness():
    print("\n-- Phase: read-after-write cache staleness --")
    inst = Instance()
    try:
        inst.init_admin("cache_admin", "CacheAdmin!2026")
        inst.start()
        b = inst.base()
        _, login, _ = http("POST", b + "/auth/login", {"username": "cache_admin", "password": "CacheAdmin!2026"})
        tok = login["token"]
        headers = {"Authorization": f"Bearer {tok}"}

        http("POST", b + "/api/query", {"sql": "CREATE TABLE cachetest (id INT)"}, headers)
        _, before, _ = http("POST", b + "/api/query", {"sql": "SELECT COUNT(*) FROM cachetest"}, headers)
        http("POST", b + "/api/query", {"sql": "INSERT INTO cachetest VALUES (1)"}, headers)
        _, after, _ = http("POST", b + "/api/query", {"sql": "SELECT COUNT(*) FROM cachetest"}, headers)

        check("sanity: COUNT(*) is 0 before any insert",
              "COUNT(*) = 0" in before.get("message", ""))
        check_known_failure(
            "read-after-write: re-running the exact same SELECT COUNT(*) text "
            "after an INSERT still returns the pre-insert cached result "
            "instead of the fresh count",
            "COUNT(*) = 1" in after.get("message", ""),
            "docs/milansql-audit.md",
            f"got {after}",
        )
    finally:
        inst.cleanup()


# ─────────────────────────────────────────────────────────────────────────
# Phase 9: Status codes / error messages on broken requests
# ─────────────────────────────────────────────────────────────────────────

def phase_broken_requests():
    print("\n-- Phase: status codes on broken requests --")
    inst = Instance()
    try:
        inst.init_admin("broken_admin", "BrokenAdmin!2026")
        inst.start()
        b = inst.base()
        _, login, _ = http("POST", b + "/auth/login", {"username": "broken_admin", "password": "BrokenAdmin!2026"})
        tok = login["token"]
        headers = {"Authorization": f"Bearer {tok}"}

        status, j, _ = http("POST", b + "/api/query", raw_body="", headers=headers)
        check("broken request: empty query body -> HTTP 400", status == 400, f"got {status}")
        check("broken request: empty query body -> useful error message",
              "sql" in j.get("error", "").lower())

        status, _, _ = http("POST", b + "/api/query", {"sql": "SELECT 1"})  # no Authorization header
        check("broken request: unauthenticated query -> HTTP 401", status == 401, f"got {status}")

        status, _, _ = http("GET", b + "/this/route/does/not/exist")
        check("broken request: unknown route -> HTTP 404", status == 404, f"got {status}")

        big_sql = "SELECT " + ("1," * 5000) + "1"
        status, j, _ = http("POST", b + "/api/query", {"sql": big_sql}, headers)
        check("broken request: oversized SQL body (>10000 chars) -> HTTP 400", status == 400, f"got {status}")

        status, j, _ = http("POST", b + "/api/query", raw_body="not valid json{{{", headers=headers)
        check_known_failure(
            "broken request: unparseable SQL text reports success:true instead of an error "
            "(the HTTP layer only flags failure by scanning the engine's text output for "
            "keywords like 'FEHLER', which an unrecognized/garbage command doesn't contain)",
            j.get("success") is False,
            "docs/milansql-audit.md",
            f"got {j}",
        )
    finally:
        inst.cleanup()


def main():
    if not os.path.isfile(BINARY):
        print(f"ERROR: milansql binary not found at {BINARY}", file=sys.stderr)
        return 2

    phases = [
        phase_fail_closed,
        phase_login,
        phase_rate_limit,
        phase_tenant_isolation,
        phase_sql_injection,
        phase_unknown_column,
        phase_cache_staleness,
        phase_broken_requests,
    ]
    for phase in phases:
        phase()

    print(f"\n=== HTTP-API tests: {PASS} passed, {FAIL} failed, "
          f"{XFAIL} known failures (xfail), {UNEXPECTED_PASS} unexpected passes ===")
    if FAILED_NAMES:
        print("Failed checks:")
        for n in FAILED_NAMES:
            print(f"  - {n}")

    return 0 if (FAIL == 0 and UNEXPECTED_PASS == 0) else 1


if __name__ == "__main__":
    sys.exit(main())
