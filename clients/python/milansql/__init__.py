"""
milansql — Python client for MilanSQL database

TCP (DB-API 2.0):
    import milansql
    conn = milansql.connect(host='localhost', port=4406)

HTTP (REST API):
    import milansql
    conn = milansql.connect_http(host='localhost', port=8080)
"""

from .connection import Connection
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


def connect(host: str = "localhost", port: int = 4406, timeout: float = 30.0) -> Connection:
    """
    Open a TCP connection to a MilanSQL server.

    Args:
        host:    Server hostname or IP (default: 'localhost')
        port:    TCP port (default: 4406)
        timeout: Socket timeout in seconds (default: 30)

    Returns:
        A connected Connection object.

    Example:
        with milansql.connect() as conn:
            cur = conn.cursor()
            cur.execute("SELECT * FROM users")
            print(cur.fetchall())
    """
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
