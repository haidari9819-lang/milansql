"""
MilanSQL Python Client — TCP Connection (PEP 249 / DB-API 2.0)

Wire protocol:
  Send:    SQL_QUERY\\n<sql>\\nEND\\n
  Receive: OK\\n<result>\\nEND\\n   — success
           ERROR\\n<msg>\\nEND\\n   — error
"""

import socket
from urllib.parse import urlparse, unquote
from .exceptions import ConnectionError, MilanSQLError, OperationalError, ProgrammingError


# ── Phase 79: DSN / Connection String parser ──────────────────────────────────

def parse_dsn(dsn: str) -> dict:
    """
    Parse a MilanSQL DSN string into connection parameters.

    Supported formats::

        milansql://user:password@host:port/database
        mysql://user@host:port/database
        jdbc:milansql://host:port/database

    Returns:
        dict with keys: host, port, user, password, database, protocol

    Raises:
        ValueError: if the DSN cannot be parsed or uses an unknown protocol.
    """
    # Strip jdbc: prefix
    raw = dsn
    if raw.startswith("jdbc:"):
        raw = raw[5:]

    parsed = urlparse(raw)
    protocol = (parsed.scheme or "milansql").lower()
    if protocol not in ("milansql", "mysql"):
        raise ValueError(f"Unsupported protocol: {protocol!r}")

    default_port = 3306 if protocol == "mysql" else 4406
    return {
        "protocol": protocol,
        "host":     parsed.hostname or "localhost",
        "port":     int(parsed.port or default_port),
        "user":     unquote(parsed.username or "root"),
        "password": unquote(parsed.password or ""),
        "database": (parsed.path or "/public").lstrip("/") or "public",
    }


# ── Parameter substitution ────────────────────────────────────────────────────

def _escape_value(v) -> str:
    """Convert a Python value to a safe SQL literal."""
    if v is None:
        return "NULL"
    if isinstance(v, bool):
        return "1" if v else "0"
    if isinstance(v, int):
        return str(v)
    if isinstance(v, float):
        return repr(v)
    # String: wrap in single quotes, escape inner quotes
    return "'" + str(v).replace("'", "''") + "'"


def _bind_params(sql: str, params) -> str:
    """Replace %s placeholders with escaped parameter values."""
    if not params:
        return sql
    if isinstance(params, (list, tuple)):
        parts = sql.split("%s")
        if len(parts) != len(params) + 1:
            raise ProgrammingError(
                f"Wrong number of parameters: SQL has {len(parts)-1} placeholders, "
                f"got {len(params)}"
            )
        result = parts[0]
        for val, part in zip(params, parts[1:]):
            result += _escape_value(val) + part
        return result
    raise ProgrammingError("params must be a list or tuple")


# ── Box-drawing table parser ──────────────────────────────────────────────────

_PIPE = "\u2502"  # │

def _parse_table_output(text: str):
    """
    Parse MilanSQL box-drawing table output into (columns, rows).

    Returns (None, None) if the output is not a SELECT result table.
    """
    columns = None
    rows = []
    header_seen = False

    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line.startswith(_PIPE):
            continue
        # Split on │ and strip whitespace from each cell
        cells = [c.strip() for c in line.split(_PIPE)]
        # cells[0] is empty (before leading │), cells[-1] is empty (after trailing │)
        cells = cells[1:-1]
        if not cells:
            continue
        if not header_seen:
            columns = cells
            header_seen = True
        else:
            rows.append(cells)

    return columns, rows


def _coerce_row(row: list, columns: list) -> tuple:
    """Try to coerce string values to int/float where possible."""
    result = []
    for cell in row:
        if cell == "NULL":
            result.append(None)
            continue
        try:
            result.append(int(cell))
            continue
        except ValueError:
            pass
        try:
            result.append(float(cell))
            continue
        except ValueError:
            pass
        result.append(cell)
    return tuple(result)


# ── Cursor ────────────────────────────────────────────────────────────────────

class Cursor:
    """PEP 249 Cursor object."""

    def __init__(self, connection: "Connection"):
        self._conn = connection
        self._rows: list = []
        self._pos: int = 0
        self.description = None          # list of (name, ...) 7-tuples
        self.rowcount: int = -1
        self.lastrowid = None

    # ── Core ─────────────────────────────────────────────────────

    def execute(self, sql: str, params=None):
        """Execute a SQL statement. Raises MilanSQLError on failure."""
        sql = _bind_params(sql, params)
        raw = self._conn._send_query(sql)

        # Strip OK\n prefix and END\n suffix
        if raw.startswith("OK\n"):
            body = raw[3:]
            if body.endswith("\nEND\n"):
                body = body[:-5]
            elif body.endswith("END\n"):
                body = body[:-4]
        elif raw.startswith("ERROR\n"):
            msg = raw[6:]
            if msg.endswith("\nEND\n"):
                msg = msg[:-5]
            elif msg.endswith("END\n"):
                msg = msg[:-4]
            raise MilanSQLError(msg.strip())
        else:
            raise MilanSQLError(f"Unexpected server response: {raw!r}")

        # Try to parse as table
        columns, rows = _parse_table_output(body)
        if columns is not None:
            self.description = [(col, None, None, None, None, None, None) for col in columns]
            self._rows = [_coerce_row(r, columns) for r in rows]
            self.rowcount = len(self._rows)
        else:
            self.description = None
            self._rows = []
            self.rowcount = -1
            # Try to extract rowcount from messages like "1 Zeile(n) eingefuegt"
            for token in body.split():
                try:
                    n = int(token)
                    self.rowcount = n
                    break
                except ValueError:
                    pass
        self._pos = 0
        return self

    def executemany(self, sql: str, seq_of_params):
        """Execute a statement for each set of parameters."""
        for params in seq_of_params:
            self.execute(sql, params)

    # ── Fetch ─────────────────────────────────────────────────────

    def fetchone(self):
        """Return the next row, or None if exhausted."""
        if self._pos >= len(self._rows):
            return None
        row = self._rows[self._pos]
        self._pos += 1
        return row

    def fetchall(self):
        """Return all remaining rows."""
        rows = self._rows[self._pos:]
        self._pos = len(self._rows)
        return rows

    def fetchmany(self, size: int = 1):
        """Return up to `size` rows."""
        rows = self._rows[self._pos:self._pos + size]
        self._pos += len(rows)
        return rows

    def __iter__(self):
        return self

    def __next__(self):
        row = self.fetchone()
        if row is None:
            raise StopIteration
        return row

    def close(self):
        self._rows = []
        self._pos = 0

    # ── Context manager ───────────────────────────────────────────

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()


# ── Connection ────────────────────────────────────────────────────────────────

class Connection:
    """
    PEP 249 Connection to MilanSQL via TCP.

    Usage:
        conn = Connection(host='localhost', port=4406)
        conn.connect()
        cur = conn.cursor()
        cur.execute("SELECT * FROM users")
        rows = cur.fetchall()
        conn.close()
    """

    def __init__(self, host: str = "localhost", port: int = 4406, timeout: float = 30.0):
        self._host = host
        self._port = port
        self._timeout = timeout
        self._sock: socket.socket | None = None
        self._in_transaction = False

    def connect(self):
        """Establish the TCP connection."""
        try:
            self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self._sock.settimeout(self._timeout)
            self._sock.connect((self._host, self._port))
        except OSError as e:
            self._sock = None
            raise ConnectionError(
                f"Cannot connect to MilanSQL at {self._host}:{self._port} — {e}"
            ) from e

    def cursor(self) -> Cursor:
        self._ensure_connected()
        return Cursor(self)

    # ── Transaction control ───────────────────────────────────────

    def begin(self):
        self._ensure_connected()
        self._send_query("BEGIN")
        self._in_transaction = True

    def commit(self):
        if self._in_transaction:
            self._send_query("COMMIT")
            self._in_transaction = False

    def rollback(self):
        if self._in_transaction:
            self._send_query("ROLLBACK")
            self._in_transaction = False

    # ── Lifecycle ─────────────────────────────────────────────────

    def close(self):
        if self._sock:
            try:
                self._send_query("EXIT")
            except Exception:
                pass
            try:
                self._sock.close()
            except Exception:
                pass
            self._sock = None

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        if exc_type is not None:
            try:
                self.rollback()
            except Exception:
                pass
        else:
            try:
                self.commit()
            except Exception:
                pass
        self.close()
        return False

    # ── Internal ──────────────────────────────────────────────────

    def _ensure_connected(self):
        if self._sock is None:
            raise ConnectionError("Not connected. Call connect() first or use as context manager.")

    def _send_query(self, sql: str) -> str:
        """Send SQL over the wire and return the raw response string."""
        self._ensure_connected()
        message = f"SQL_QUERY\n{sql}\nEND\n"
        try:
            self._sock.sendall(message.encode("utf-8"))
        except OSError as e:
            raise OperationalError(f"Send failed: {e}") from e

        # Read until "END\n"
        data = b""
        try:
            while True:
                chunk = self._sock.recv(4096)
                if not chunk:
                    raise OperationalError("Connection closed by server.")
                data += chunk
                if data.endswith(b"END\n"):
                    break
        except OSError as e:
            raise OperationalError(f"Receive failed: {e}") from e

        return data.decode("utf-8", errors="replace")
