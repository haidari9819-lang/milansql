"""
MilanSQL Python Client — Exceptions (PEP 249 compliant)
"""


class Error(Exception):
    """Base class for all MilanSQL errors."""


class MilanSQLError(Error):
    """General MilanSQL error."""


class ConnectionError(Error):
    """Raised when a connection cannot be established or is lost."""


class OperationalError(Error):
    """Raised for database operational errors (e.g. dropped connection)."""


class ProgrammingError(Error):
    """Raised for SQL syntax errors or wrong number of parameters."""


class InterfaceError(Error):
    """Raised for errors related to the client interface itself."""


# PEP 249 aliases
DatabaseError = MilanSQLError
Warning = Warning
