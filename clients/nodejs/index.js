'use strict';
/**
 * milansql — Node.js client for MilanSQL (HTTP REST API)
 *
 * Uses the built-in `http` module — no external dependencies.
 * Connects to MilanSQL HTTP server (--http --port 8080).
 */

const http = require('http');

// ── HTTP helper ───────────────────────────────────────────────────────────────

/**
 * Make a raw HTTP request.
 * @param {object} options  - http.request options
 * @param {string} [body]   - request body string
 * @returns {Promise<object>} parsed JSON response
 */
function httpRequest(options, body = null) {
  return new Promise((resolve, reject) => {
    const req = http.request(options, (res) => {
      let data = '';
      res.setEncoding('utf8');
      res.on('data', (chunk) => { data += chunk; });
      res.on('end', () => {
        try {
          resolve(JSON.parse(data));
        } catch (e) {
          reject(new Error(`Invalid JSON from server: ${data.slice(0, 200)}`));
        }
      });
    });
    req.on('error', (e) => reject(new MilanSQLError(`Request failed: ${e.message}`)));
    if (body) req.write(body);
    req.end();
  });
}

// ── Errors ────────────────────────────────────────────────────────────────────

class MilanSQLError extends Error {
  constructor(message) {
    super(message);
    this.name = 'MilanSQLError';
  }
}

class ConnectionError extends MilanSQLError {
  constructor(message) {
    super(message);
    this.name = 'ConnectionError';
  }
}

// ── QueryResult ───────────────────────────────────────────────────────────────

class QueryResult {
  /**
   * @param {object} data  - parsed JSON from server
   */
  constructor(data) {
    this.success       = data.success ?? true;
    this.columns       = data.columns ?? [];
    this.rows          = data.rows ?? [];
    this.rowCount      = data.rowCount ?? this.rows.length;
    this.rowsAffected  = data.rowsAffected ?? 0;
    this.message       = data.message ?? '';
    this.executionTime = data.executionTime ?? '';
  }

  /** Return all rows (alias for .rows). */
  fetchall() { return [...this.rows]; }

  /** Return the first row, or null. */
  fetchone() { return this.rows[0] ?? null; }

  toString() {
    if (this.columns.length) {
      return `QueryResult(columns=${JSON.stringify(this.columns)}, rows=${this.rowCount}, time=${this.executionTime})`;
    }
    return `QueryResult(message=${JSON.stringify(this.message)}, affected=${this.rowsAffected})`;
  }
}

// ── Connection ────────────────────────────────────────────────────────────────

class Connection {
  /**
   * @param {string} host
   * @param {number} port
   * @param {number} timeout  ms
   */
  constructor(host = 'localhost', port = 8080, timeout = 30000) {
    this._host    = host;
    this._port    = port;
    this._timeout = timeout;
  }

  // ── Query ─────────────────────────────────────────────────────

  /**
   * Execute SQL via POST /query.
   * @param {string} sql
   * @returns {Promise<QueryResult>}
   */
  async query(sql) {
    const body = JSON.stringify({ sql });
    const data = await httpRequest({
      hostname: this._host,
      port:     this._port,
      path:     '/query',
      method:   'POST',
      headers: {
        'Content-Type':   'application/json',
        'Content-Length': Buffer.byteLength(body),
      },
      timeout: this._timeout,
    }, body);

    if (!data.success) {
      throw new MilanSQLError(data.error || 'Unknown server error');
    }
    return new QueryResult(data);
  }

  /**
   * Execute SQL via GET /query?sql=...
   * @param {string} sql
   * @returns {Promise<QueryResult>}
   */
  async queryGet(sql) {
    const path = '/query?' + new URLSearchParams({ sql }).toString();
    const data = await httpRequest({
      hostname: this._host,
      port:     this._port,
      path,
      method:   'GET',
      timeout:  this._timeout,
    });

    if (!data.success) {
      throw new MilanSQLError(data.error || 'Unknown server error');
    }
    return new QueryResult(data);
  }

  // ── Metadata ──────────────────────────────────────────────────

  /** Return list of all table names. */
  async tables() {
    const data = await this._get('/tables');
    return data.tables ?? [];
  }

  /** Return column info for a named table. */
  async describe(tableName) {
    const data = await this._get(`/tables/${encodeURIComponent(tableName)}`);
    return data.columns ?? [];
  }

  /** Return list of all schema names. */
  async schemas() {
    const data = await this._get('/schemas');
    return data.schemas ?? [];
  }

  /** Return server status object. */
  async status() {
    const data = await this._get('/status');
    return data.status ?? data;
  }

  // ── Lifecycle ─────────────────────────────────────────────────

  /** No persistent connection — nothing to clean up. */
  close() {}

  // ── Internal ──────────────────────────────────────────────────

  _get(path) {
    return httpRequest({
      hostname: this._host,
      port:     this._port,
      path,
      method:   'GET',
      timeout:  this._timeout,
    });
  }
}

// ── Phase 79: DSN parser ──────────────────────────────────────────────────────

/**
 * Parse a MilanSQL DSN string into connection options.
 *
 * Supported formats:
 *   milansql://user:pass@host:port/database
 *   mysql://user@host:port/database
 *   jdbc:milansql://host:port/database
 *
 * @param {string} dsn
 * @returns {{ host: string, port: number, user: string, password: string, database: string, protocol: string }}
 */
function parseDsn(dsn) {
  let raw = dsn;
  if (raw.startsWith('jdbc:')) raw = raw.slice(5);

  const m = raw.match(/^([a-z]+):\/\/(?:([^:@]*)(?::([^@]*))?@)?([^:/]*)(?::(\d+))?\/?(.*)$/i);
  if (!m) throw new MilanSQLError(`Invalid DSN: ${dsn}`);

  const protocol = (m[1] || 'milansql').toLowerCase();
  if (protocol !== 'milansql' && protocol !== 'mysql')
    throw new MilanSQLError(`Unsupported protocol: ${protocol}`);

  const defaultPort = protocol === 'mysql' ? 3306 : 4406;
  return {
    protocol,
    user:     decodeURIComponent(m[2] || 'root'),
    password: decodeURIComponent(m[3] || ''),
    host:     m[4] || 'localhost',
    port:     m[5] ? parseInt(m[5], 10) : defaultPort,
    database: m[6] || 'public',
  };
}

// ── Module API ────────────────────────────────────────────────────────────────

/**
 * Connect to a MilanSQL HTTP server.
 *
 * Accepts either a DSN string or an options object::
 *
 *   // DSN style (Phase 79)
 *   const conn = milansql.connect('milansql://root@localhost:8080');
 *   const conn = milansql.connect('milansql://alice:secret@localhost:8080/shop');
 *
 *   // Classic style
 *   const conn = milansql.connect({ host: 'localhost', port: 8080 });
 *
 * @param {string|object} dsnOrOptions  DSN string, or options { host, port, timeout }
 * @returns {Connection}
 */
function connect(dsnOrOptions = {}) {
  // Phase 79: support DSN string
  if (typeof dsnOrOptions === 'string') {
    const p = parseDsn(dsnOrOptions);
    return new Connection(p.host, p.port);
  }
  const { host = 'localhost', port = 8080, timeout = 30000 } = dsnOrOptions;
  return new Connection(host, port, timeout);
}

module.exports = {
  connect,
  parseDsn,
  Connection,
  QueryResult,
  MilanSQLError,
  ConnectionError,
};
