"""
milansql.http — HTTP fluent client for MilanSQL (Phase 4.4)
Supabase-compatible Python API

Usage:
    from milansql.http import MilanSQL

    db = MilanSQL(url="https://milansql.de", token="your-token")
    result = db.from_("users").select("*").eq("id", 1).execute()
"""

from __future__ import annotations
import json
import urllib.request
import urllib.error
import urllib.parse
from contextlib import contextmanager
from dataclasses import dataclass, field
from typing import Any, Optional, List, Dict, Generator

try:
    import asyncio
    import aiohttp
    _HAS_AIOHTTP = True
except ImportError:
    _HAS_AIOHTTP = False


@dataclass
class QueryResult:
    data: Optional[List[Dict[str, Any]]] = None
    error: Optional[str] = None
    rows: int = 0

    @property
    def ok(self) -> bool:
        return self.error is None


class QueryBuilder:
    """Fluent SQL query builder — mirroring the JS SDK API."""

    def __init__(self, client: "MilanSQL", table: str):
        self._client = client
        self._table  = table
        self._select = "*"
        self._where: List[str] = []
        self._limit: Optional[int] = None
        self._offset: Optional[int] = None
        self._order: List[str] = []

    # ── Column selection ──────────────────────────────────

    def select(self, cols: str = "*") -> "QueryBuilder":
        self._select = cols
        return self

    # ── Filters ───────────────────────────────────────────

    def eq(self, col: str, val: Any) -> "QueryBuilder":
        if val is None:
            self._where.append(f"{col} IS NULL")
        elif isinstance(val, (int, float)):
            self._where.append(f"{col} = {val}")
        else:
            self._where.append(f"{col} = '{str(val).replace(chr(39), chr(39)+chr(39))}'")
        return self

    def neq(self, col: str, val: Any) -> "QueryBuilder":
        if isinstance(val, (int, float)):
            self._where.append(f"{col} != {val}")
        else:
            self._where.append(f"{col} != '{str(val).replace(chr(39), chr(39)+chr(39))}'")
        return self

    def gt(self, col: str, val: Any) -> "QueryBuilder":
        v = val if isinstance(val, (int, float)) else f"'{val}'"
        self._where.append(f"{col} > {v}")
        return self

    def gte(self, col: str, val: Any) -> "QueryBuilder":
        v = val if isinstance(val, (int, float)) else f"'{val}'"
        self._where.append(f"{col} >= {v}")
        return self

    def lt(self, col: str, val: Any) -> "QueryBuilder":
        v = val if isinstance(val, (int, float)) else f"'{val}'"
        self._where.append(f"{col} < {v}")
        return self

    def lte(self, col: str, val: Any) -> "QueryBuilder":
        v = val if isinstance(val, (int, float)) else f"'{val}'"
        self._where.append(f"{col} <= {v}")
        return self

    def like(self, col: str, pattern: str) -> "QueryBuilder":
        self._where.append(f"{col} LIKE '{pattern.replace(chr(39), chr(39)+chr(39))}'")
        return self

    def in_(self, col: str, vals: List[Any]) -> "QueryBuilder":
        escaped = ", ".join(
            str(v) if isinstance(v, (int, float)) else f"'{str(v).replace(chr(39), chr(39)+chr(39))}'"
            for v in vals
        )
        self._where.append(f"{col} IN ({escaped})")
        return self

    def is_(self, col: str, val: Any) -> "QueryBuilder":
        if val is None:
            self._where.append(f"{col} IS NULL")
        else:
            self._where.append(f"{col} IS {'TRUE' if val else 'FALSE'}")
        return self

    # ── Sorting / Pagination ──────────────────────────────

    def order(self, col: str, ascending: bool = True) -> "QueryBuilder":
        self._order.append(f"{col} {'ASC' if ascending else 'DESC'}")
        return self

    def limit(self, n: int) -> "QueryBuilder":
        self._limit = n
        return self

    def offset(self, n: int) -> "QueryBuilder":
        self._offset = n
        return self

    # ── DML ───────────────────────────────────────────────

    def insert(self, data: Dict[str, Any] | List[Dict[str, Any]]) -> "QueryBuilder":
        """Stage an INSERT — call .execute() to run it."""
        self._insert_data = data if isinstance(data, list) else [data]
        self._op = "insert"
        return self

    def update(self, data: Dict[str, Any]) -> "QueryBuilder":
        """Stage an UPDATE — call .execute() to run it."""
        self._update_data = data
        self._op = "update"
        return self

    def delete(self) -> "QueryBuilder":
        """Stage a DELETE — call .execute() to run it."""
        self._op = "delete"
        return self

    def execute(self) -> QueryResult:
        """Execute the staged or SELECT query."""
        op = getattr(self, "_op", "select")
        if op == "insert":
            return self._client._exec(self._build_insert_sql())
        elif op == "update":
            return self._client._exec(self._build_update_sql())
        elif op == "delete":
            return self._client._exec(self._build_delete_sql())
        else:
            return self._client._exec(self._build_select_sql())

    def _val(self, v: Any) -> str:
        if v is None:
            return "NULL"
        if isinstance(v, (int, float)):
            return str(v)
        return f"'{str(v).replace(chr(39), chr(39)+chr(39))}'"

    def _build_select_sql(self) -> str:
        sql = f"SELECT {self._select} FROM {self._table}"
        if self._where:
            sql += " WHERE " + " AND ".join(self._where)
        if self._order:
            sql += " ORDER BY " + ", ".join(self._order)
        if self._limit is not None:
            sql += f" LIMIT {self._limit}"
        if self._offset is not None:
            sql += f" OFFSET {self._offset}"
        return sql

    def _build_insert_sql(self) -> str:
        rows = self._insert_data
        cols = ", ".join(rows[0].keys())
        val_rows = ", ".join(
            "(" + ", ".join(self._val(v) for v in row.values()) + ")"
            for row in rows
        )
        return f"INSERT INTO {self._table} ({cols}) VALUES {val_rows}"

    def _build_update_sql(self) -> str:
        sets = ", ".join(f"{k} = {self._val(v)}" for k, v in self._update_data.items())
        sql = f"UPDATE {self._table} SET {sets}"
        if self._where:
            sql += " WHERE " + " AND ".join(self._where)
        return sql

    def _build_delete_sql(self) -> str:
        sql = f"DELETE FROM {self._table}"
        if self._where:
            sql += " WHERE " + " AND ".join(self._where)
        return sql


class TxQueryBuilder(QueryBuilder):
    """Query builder that records SQL for transaction replay."""

    def __init__(self, tx: "Transaction", table: str):
        super().__init__(tx._client, table)
        self._tx = tx

    def execute(self) -> QueryResult:
        op = getattr(self, "_op", "select")
        if op == "insert":   sql = self._build_insert_sql()
        elif op == "update": sql = self._build_update_sql()
        elif op == "delete": sql = self._build_delete_sql()
        else:                sql = self._build_select_sql()
        self._tx._sqls.append(sql)
        return self._tx._client._exec(sql)


class Transaction:
    """Transaction context for use with MilanSQL.transaction()."""

    def __init__(self, client: "MilanSQL"):
        self._client = client
        self._sqls: List[str] = []

    def from_(self, table: str) -> TxQueryBuilder:
        return TxQueryBuilder(self, table)

    def query(self, sql: str, params: Optional[list] = None) -> QueryResult:
        if params:
            sql = self._interpolate(sql, params)
        self._sqls.append(sql)
        return self._client._exec(sql)

    def _interpolate(self, sql: str, params: list) -> str:
        p = list(params)
        result = []
        for ch in sql:
            if ch == "?" and p:
                v = p.pop(0)
                result.append("NULL" if v is None else str(v) if isinstance(v, (int, float)) else f"'{str(v).replace(chr(39), chr(39)+chr(39))}'")
            else:
                result.append(ch)
        return "".join(result)


class MilanSQL:
    """
    MilanSQL HTTP client with Supabase-compatible fluent API.

    Examples:
        db = MilanSQL(url="https://milansql.de", token="jwt-token")

        # SELECT
        result = db.from_("users").select("*").eq("active", True).limit(10).execute()
        for row in result.data or []:
            print(row)

        # INSERT
        db.from_("users").insert({"name": "Alice", "age": 30}).execute()

        # UPDATE
        db.from_("users").update({"age": 31}).eq("name", "Alice").execute()

        # DELETE
        db.from_("users").delete().eq("id", 5).execute()

        # Raw SQL
        result = db.query("SELECT COUNT(*) FROM users")

        # Transaction
        with db.transaction() as tx:
            tx.from_("accounts").update({"balance": 900}).eq("id", 1).execute()
            tx.from_("accounts").update({"balance": 1100}).eq("id", 2).execute()
    """

    def __init__(self, url: str, token: str = ""):
        self._url   = url.rstrip("/")
        self._token = token

    # ── Auth ──────────────────────────────────────────────

    def login(self, username: str, password: str) -> Dict[str, Any]:
        r = self._post("/auth/login", {"username": username, "password": password})
        if r.get("success") and r.get("token"):
            self._token = r["token"]
        return r

    def register(self, username: str, password: str) -> Dict[str, Any]:
        r = self._post("/auth/register", {"username": username, "password": password})
        if r.get("success") and r.get("token"):
            self._token = r["token"]
        return r

    def logout(self) -> None:
        self._post("/auth/logout", {})
        self._token = ""

    def me(self) -> Dict[str, Any]:
        return self._get_json("/auth/me")

    # ── Query Builder ─────────────────────────────────────

    def from_(self, table: str) -> QueryBuilder:
        return QueryBuilder(self, table)

    # ── Raw SQL ───────────────────────────────────────────

    def query(self, sql: str, params: Optional[list] = None) -> QueryResult:
        if params:
            sql = self._interpolate(sql, params)
        return self._exec(sql)

    # ── Transactions ──────────────────────────────────────

    @contextmanager
    def transaction(self) -> Generator[Transaction, None, None]:
        tx = Transaction(self)
        self._exec("BEGIN")
        try:
            yield tx
            self._exec("COMMIT")
        except Exception:
            self._exec("ROLLBACK")
            raise

    # ── Schema Introspection ──────────────────────────────

    def schema(self) -> Dict[str, Any]:
        return self._get_json("/api/schema")

    def table_schema(self, table: str) -> Dict[str, Any]:
        return self._get_json(f"/api/schema/{urllib.parse.quote(table)}")

    def table_columns(self, table: str) -> Dict[str, Any]:
        return self._get_json(f"/api/schema/{urllib.parse.quote(table)}/columns")

    def generate_types(self) -> str:
        r = self._get_json("/api/schema/generate/typescript")
        return r.get("typescript", "")

    # ── Migrations ────────────────────────────────────────

    def migrate_up(self, n: Optional[int] = None) -> Dict[str, Any]:
        body: Dict[str, Any] = {}
        if n is not None:
            body["n"] = n
        return self._post("/api/migrate/up", body)

    def migrate_down(self, n: int = 1) -> Dict[str, Any]:
        return self._post("/api/migrate/down", {"n": n})

    def migrate_status(self) -> Dict[str, Any]:
        return self._get_json("/api/migrate/status")

    # ── Status ────────────────────────────────────────────

    def health(self) -> Dict[str, Any]:
        return self._get_json("/health")

    def status(self) -> Dict[str, Any]:
        return self._get_json("/status")

    def tables(self) -> List[str]:
        r = self._get_json("/tables")
        return r.get("tables", [])

    # ── Internal ──────────────────────────────────────────

    def _exec(self, sql: str) -> QueryResult:
        try:
            d = self._post("/api/query", {"sql": sql})
            if not d.get("success", True):
                return QueryResult(error=d.get("error", "Query failed"))
            rows = d.get("rows", d.get("data", []))
            if not isinstance(rows, list):
                rows = []
            return QueryResult(data=rows, rows=len(rows))
        except Exception as e:
            return QueryResult(error=str(e))

    def _post(self, path: str, body: Dict[str, Any]) -> Dict[str, Any]:
        url = self._url + path
        data = json.dumps(body).encode()
        req = urllib.request.Request(
            url, data=data,
            headers=self._headers(),
            method="POST"
        )
        with urllib.request.urlopen(req, timeout=30) as r:
            return json.loads(r.read().decode())

    def _get_json(self, path: str) -> Dict[str, Any]:
        url = self._url + path
        req = urllib.request.Request(url, headers=self._headers(), method="GET")
        with urllib.request.urlopen(req, timeout=30) as r:
            return json.loads(r.read().decode())

    def _headers(self) -> Dict[str, str]:
        h = {"Content-Type": "application/json"}
        if self._token:
            h["Authorization"] = f"Bearer {self._token}"
        return h

    def _interpolate(self, sql: str, params: list) -> str:
        p = list(params)
        result = []
        for ch in sql:
            if ch == "?" and p:
                v = p.pop(0)
                result.append("NULL" if v is None else str(v) if isinstance(v, (int, float)) else f"'{str(v).replace(chr(39), chr(39)+chr(39))}'")
            else:
                result.append(ch)
        return "".join(result)


# ── Async Client ──────────────────────────────────────────────

class AsyncMilanSQL:
    """
    Async MilanSQL HTTP client (requires aiohttp).

    Examples:
        db = AsyncMilanSQL(url="https://milansql.de", token="jwt-token")
        result = await db.from_("users").select("*").execute()
    """

    def __init__(self, url: str, token: str = ""):
        self._url   = url.rstrip("/")
        self._token = token

    def from_(self, table: str) -> "AsyncQueryBuilder":
        return AsyncQueryBuilder(self, table)

    async def query(self, sql: str, params: Optional[list] = None) -> QueryResult:
        if params:
            p = list(params)
            out = []
            for ch in sql:
                if ch == "?" and p:
                    v = p.pop(0)
                    out.append("NULL" if v is None else str(v) if isinstance(v, (int, float)) else f"'{str(v).replace(chr(39), chr(39)+chr(39))}'")
                else:
                    out.append(ch)
            sql = "".join(out)
        return await self._exec(sql)

    async def _exec(self, sql: str) -> QueryResult:
        if not _HAS_AIOHTTP:
            raise ImportError("aiohttp required for AsyncMilanSQL: pip install aiohttp")
        async with aiohttp.ClientSession() as session:
            async with session.post(
                self._url + "/api/query",
                json={"sql": sql},
                headers=self._headers()
            ) as r:
                d = await r.json()
                if not d.get("success", True):
                    return QueryResult(error=d.get("error", "Query failed"))
                rows = d.get("rows", d.get("data", []))
                if not isinstance(rows, list):
                    rows = []
                return QueryResult(data=rows, rows=len(rows))

    def _headers(self) -> Dict[str, str]:
        h = {"Content-Type": "application/json"}
        if self._token:
            h["Authorization"] = f"Bearer {self._token}"
        return h


class AsyncQueryBuilder:
    """Async fluent query builder."""

    def __init__(self, client: AsyncMilanSQL, table: str):
        self._client = client
        self._table  = table
        self._select = "*"
        self._where: List[str] = []
        self._limit: Optional[int] = None
        self._order: List[str] = []

    def select(self, cols: str = "*") -> "AsyncQueryBuilder":
        self._select = cols; return self

    def eq(self, col: str, val: Any) -> "AsyncQueryBuilder":
        if val is None: self._where.append(f"{col} IS NULL")
        elif isinstance(val, (int, float)): self._where.append(f"{col} = {val}")
        else: self._where.append(f"{col} = '{str(val).replace(chr(39), chr(39)+chr(39))}'")
        return self

    def limit(self, n: int) -> "AsyncQueryBuilder":
        self._limit = n; return self

    def order(self, col: str, ascending: bool = True) -> "AsyncQueryBuilder":
        self._order.append(f"{col} {'ASC' if ascending else 'DESC'}"); return self

    async def execute(self) -> QueryResult:
        sql = f"SELECT {self._select} FROM {self._table}"
        if self._where: sql += " WHERE " + " AND ".join(self._where)
        if self._order: sql += " ORDER BY " + ", ".join(self._order)
        if self._limit is not None: sql += f" LIMIT {self._limit}"
        return await self._client._exec(sql)

    def __await__(self):
        return self.execute().__await__()
