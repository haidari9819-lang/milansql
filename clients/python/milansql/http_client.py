"""
MilanSQL Python Client — HTTP Connection (Phase 52 REST API)

Connects to MilanSQL HTTP server (default port 8080).
Uses only Python stdlib: urllib, json.
"""

import json
import urllib.request
import urllib.parse
import urllib.error
from .exceptions import MilanSQLError, ConnectionError, OperationalError


# ── Result object ─────────────────────────────────────────────────────────────

class QueryResult:
    """Returned by HttpConnection.query()."""

    def __init__(self, data: dict):
        self._data = data
        self.columns: list = data.get("columns", [])
        self.rows: list = data.get("rows", [])
        self.row_count: int = data.get("rowCount", len(self.rows))
        self.rows_affected: int = data.get("rowsAffected", 0)
        self.message: str = data.get("message", "")
        self.execution_time: str = data.get("executionTime", "")
        self.success: bool = data.get("success", True)

    def fetchall(self) -> list:
        return list(self.rows)

    def fetchone(self):
        return self.rows[0] if self.rows else None

    def __repr__(self):
        if self.columns:
            return (
                f"QueryResult(columns={self.columns}, "
                f"rows={len(self.rows)}, "
                f"time={self.execution_time})"
            )
        return f"QueryResult(message={self.message!r}, affected={self.rows_affected})"


# ── HTTP Connection ───────────────────────────────────────────────────────────

class HttpConnection:
    """
    Connect to MilanSQL via the HTTP/JSON REST API (Phase 52).

    Usage:
        conn = HttpConnection(host='localhost', port=8080)
        result = conn.query("SELECT * FROM users")
        print(result.columns)
        print(result.rows)
        conn.close()
    """

    def __init__(self, host: str = "localhost", port: int = 8080, timeout: float = 30.0):
        self._base = f"http://{host}:{port}"
        self._timeout = timeout

    # ── Query ─────────────────────────────────────────────────────

    def query(self, sql: str) -> QueryResult:
        """Execute a SQL statement via POST /query."""
        payload = json.dumps({"sql": sql}).encode("utf-8")
        req = urllib.request.Request(
            f"{self._base}/query",
            data=payload,
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        data = self._do_request(req)
        if not data.get("success", False):
            raise MilanSQLError(data.get("error", "Unknown error"))
        return QueryResult(data)

    def query_get(self, sql: str) -> QueryResult:
        """Execute a SQL statement via GET /query?sql=..."""
        encoded = urllib.parse.urlencode({"sql": sql})
        req = urllib.request.Request(
            f"{self._base}/query?{encoded}",
            method="GET",
        )
        data = self._do_request(req)
        if not data.get("success", False):
            raise MilanSQLError(data.get("error", "Unknown error"))
        return QueryResult(data)

    # ── Metadata endpoints ────────────────────────────────────────

    def tables(self) -> list:
        """Return list of all table names."""
        data = self._do_request(urllib.request.Request(f"{self._base}/tables"))
        return data.get("tables", [])

    def describe(self, table_name: str) -> list:
        """Return column info for a table (list of dicts)."""
        encoded = urllib.parse.quote(table_name)
        data = self._do_request(
            urllib.request.Request(f"{self._base}/tables/{encoded}")
        )
        return data.get("columns", [])

    def schemas(self) -> list:
        """Return list of all schema names."""
        data = self._do_request(urllib.request.Request(f"{self._base}/schemas"))
        return data.get("schemas", [])

    def status(self) -> dict:
        """Return server status information."""
        data = self._do_request(urllib.request.Request(f"{self._base}/status"))
        return data.get("status", data)

    # ── Lifecycle ─────────────────────────────────────────────────

    def close(self):
        """No persistent connection — nothing to do."""
        pass

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()
        return False

    # ── Internal ──────────────────────────────────────────────────

    def _do_request(self, req: urllib.request.Request) -> dict:
        try:
            with urllib.request.urlopen(req, timeout=self._timeout) as resp:
                body = resp.read().decode("utf-8")
                return json.loads(body)
        except urllib.error.URLError as e:
            raise ConnectionError(
                f"Cannot reach MilanSQL HTTP server at {self._base} — {e}"
            ) from e
        except json.JSONDecodeError as e:
            raise OperationalError(f"Invalid JSON response from server: {e}") from e
