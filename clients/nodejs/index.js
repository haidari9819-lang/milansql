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

// ── Module API ────────────────────────────────────────────────────────────────

/**
 * Connect to a MilanSQL HTTP server.
 *
 * @param {object} options
 * @param {string} [options.host='localhost']
 * @param {number} [options.port=8080]
 * @param {number} [options.timeout=30000]  ms
 * @returns {Connection}
 *
 * @example
 * const milansql = require('milansql');
 * const conn = milansql.connect({ host: 'localhost', port: 8080 });
 * const result = await conn.query('SELECT * FROM users');
 * console.log(result.columns);
 * console.log(result.rows);
 */
function connect({ host = 'localhost', port = 8080, timeout = 30000 } = {}) {
  return new Connection(host, port, timeout);
}

module.exports = {
  connect,
  Connection,
  QueryResult,
  MilanSQLError,
  ConnectionError,
};
