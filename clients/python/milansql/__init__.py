"""
milansql — Python client for MilanSQL database

TCP (DB-API 2.0):
    import milansql
    conn = milansql.connect(host='localhost', port=4406)

HTTP (REST API):
    import milansql
    conn = milansql.connect_http(host='localhost', port=8080)
"""

from .connection import Connection, parse_dsn
from .http_client import HttpConnection, QueryResult
from .exceptions import (
    Error,
    MilanSQLError,
    ConnectionError,
    OperationalError,
    ProgrammingError,
    InterfaceError,
    DatabaseError,
)

# PEP 249 module globals
apilevel = "2.0"
threadsafety = 1          # threads may share the module, but not connections
paramstyle = "format"     # %s placeholders


def connect(dsn_or_host: str = "localhost", port: int = 4406, timeout: float = 30.0) -> Connection:
    """
    Open a TCP connection to a MilanSQL server.

    Accepts either a DSN string or keyword arguments::

        # DSN style (Phase 79)
        conn = milansql.connect("milansql://alice:secret@localhost:4406/shop")
        conn = milansql.connect("mysql://root@localhost:4407/mydb")

        # Classic style
        conn = milansql.connect(host='localhost', port=4406)

    Args:
        dsn_or_host: DSN string like ``milansql://user:pass@host:port/db``,
                     or a plain hostname (default: 'localhost').
        port:        TCP port — ignored when a DSN is supplied (default: 4406).
        timeout:     Socket timeout in seconds (default: 30).

    Returns:
        A connected :class:`Connection` object.
    """
    # Phase 79: detect DSN string
    if dsn_or_host.startswith(("milansql://", "mysql://", "jdbc:milansql://")):
        params = parse_dsn(dsn_or_host)
        host   = params["host"]
        port   = params["port"]
    else:
        host = dsn_or_host
    conn = Connection(host=host, port=port, timeout=timeout)
    conn.connect()
    return conn


def connect_http(host: str = "localhost", port: int = 8080, timeout: float = 30.0) -> HttpConnection:
    """
    Open an HTTP connection to a MilanSQL REST API server.

    Args:
        host:    Server hostname or IP (default: 'localhost')
        port:    HTTP port (default: 8080)
        timeout: Request timeout in seconds (default: 30)

    Returns:
        An HttpConnection object.

    Example:
        with milansql.connect_http() as conn:
            result = conn.query("SELECT * FROM users")
            print(result.columns)
            print(result.rows)
    """
    return HttpConnection(host=host, port=port, timeout=timeout)


__all__ = [
    "connect",
    "connect_http",
    "parse_dsn",
    "Connection",
    "HttpConnection",
    "QueryResult",
    "Error",
    "MilanSQLError",
    "ConnectionError",
    "OperationalError",
    "ProgrammingError",
    "InterfaceError",
    "DatabaseError",
    "apilevel",
    "threadsafety",
    "paramstyle",
]
