/**
 * milansql.js — JavaScript/TypeScript client for MilanSQL
 * Supabase-compatible API with full query builder
 * @version 2.0.0
 * @see https://milansql.de/docs/sdk/javascript
 */

'use strict';

// ── QueryBuilder ───────────────────────────────────────────────

class QueryBuilder {
  constructor(client, table) {
    this._client = client;
    this._table  = table;
    this._select = '*';
    this._where  = [];
    this._limit  = null;
    this._offset = null;
    this._order  = [];
  }

  // Column selection
  select(cols) { this._select = cols; return this; }

  // Filters
  eq(col, val) {
    if (val === null) this._where.push(`${col} IS NULL`);
    else if (typeof val === 'number') this._where.push(`${col} = ${val}`);
    else this._where.push(`${col} = '${String(val).replace(/'/g, "''")}'`);
    return this;
  }
  neq(col, val) {
    if (typeof val === 'number') this._where.push(`${col} != ${val}`);
    else this._where.push(`${col} != '${String(val).replace(/'/g, "''")}'`);
    return this;
  }
  gt(col, val)   { this._where.push(`${col} > ${typeof val === 'number' ? val : `'${val}'`}`);  return this; }
  gte(col, val)  { this._where.push(`${col} >= ${typeof val === 'number' ? val : `'${val}'`}`); return this; }
  lt(col, val)   { this._where.push(`${col} < ${typeof val === 'number' ? val : `'${val}'`}`);  return this; }
  lte(col, val)  { this._where.push(`${col} <= ${typeof val === 'number' ? val : `'${val}'`}`); return this; }
  like(col, pat) { this._where.push(`${col} LIKE '${pat.replace(/'/g, "''")}'`); return this; }
  ilike(col, pat){ this._where.push(`LOWER(${col}) LIKE LOWER('${pat.replace(/'/g, "''")}')`); return this; }
  in(col, vals) {
    const list = vals.map(v => typeof v === 'number' ? v : `'${String(v).replace(/'/g, "''")}'`).join(', ');
    this._where.push(`${col} IN (${list})`);
    return this;
  }
  is(col, val) {
    if (val === null) this._where.push(`${col} IS NULL`);
    else this._where.push(`${col} IS ${val ? 'TRUE' : 'FALSE'}`);
    return this;
  }

  // Sorting / pagination
  order(col, { ascending = true } = {}) {
    this._order.push(`${col} ${ascending ? 'ASC' : 'DESC'}`);
    return this;
  }
  limit(n)  { this._limit  = n; return this; }
  offset(n) { this._offset = n; return this; }
  range(from, to) { this._limit = to - from + 1; this._offset = from; return this; }

  // INSERT
  async insert(data) {
    const rows = Array.isArray(data) ? data : [data];
    const cols = Object.keys(rows[0]).join(', ');
    const vals = rows.map(row =>
      '(' + Object.values(row).map(v =>
        v === null ? 'NULL' : typeof v === 'number' ? v : `'${String(v).replace(/'/g, "''")}'`
      ).join(', ') + ')'
    ).join(', ');
    return this._client._exec(`INSERT INTO ${this._table} (${cols}) VALUES ${vals}`);
  }

  // UPDATE
  async update(data) {
    const sets = Object.entries(data).map(([k, v]) =>
      v === null ? `${k} = NULL` :
      typeof v === 'number' ? `${k} = ${v}` :
      `${k} = '${String(v).replace(/'/g, "''")}'`
    ).join(', ');
    let sql = `UPDATE ${this._table} SET ${sets}`;
    if (this._where.length) sql += ' WHERE ' + this._where.join(' AND ');
    return this._client._exec(sql);
  }

  // DELETE
  async delete() {
    let sql = `DELETE FROM ${this._table}`;
    if (this._where.length) sql += ' WHERE ' + this._where.join(' AND ');
    return this._client._exec(sql);
  }

  // SELECT execute
  async execute() {
    return this._client._exec(this._buildSql());
  }

  // Realtime
  on(event, callback) {
    return new RealtimeBuilder(this._client, this._table, event, callback);
  }

  // Supabase-compatible: await db.from(...).select(...)
  then(resolve, reject) { return this.execute().then(resolve, reject); }

  _buildSql() {
    let sql = `SELECT ${this._select} FROM ${this._table}`;
    if (this._where.length) sql += ' WHERE ' + this._where.join(' AND ');
    if (this._order.length) sql += ' ORDER BY ' + this._order.join(', ');
    if (this._limit  !== null) sql += ` LIMIT ${this._limit}`;
    if (this._offset !== null) sql += ` OFFSET ${this._offset}`;
    return sql;
  }
}

// ── RealtimeBuilder / Subscription ────────────────────────────

class RealtimeBuilder {
  constructor(client, table, event, callback) {
    this._client   = client;
    this._table    = table;
    this._event    = event;
    this._callback = callback;
  }

  subscribe() {
    const baseUrl = this._client._url.replace(/^http/, 'ws');
    const ws = new WebSocket(`${baseUrl}/ws`);
    ws.onopen = () => ws.send(JSON.stringify({
      type: 'subscribe', table: this._table, event: this._event
    }));
    ws.onmessage = (evt) => {
      try {
        const msg = JSON.parse(evt.data);
        if (msg.type === 'change' && (this._event === '*' || msg.event === this._event))
          this._callback(msg);
      } catch {}
    };
    return new RealtimeSubscription(ws);
  }
}

class RealtimeSubscription {
  constructor(ws) { this._ws = ws; }
  unsubscribe() { this._ws.close(); }
}

// ── Transaction ────────────────────────────────────────────────

class TxClient {
  constructor(execFn) { this._exec = execFn; }
  from(table) { return new QueryBuilder(this, table); }
  async query(sql, params) { return this._exec(sql, params); }
}

// ── MilanSQL Client ────────────────────────────────────────────

class MilanSQL {
  /**
   * @param {string|{url:string,token?:string}} config
   * @param {string} [token]
   */
  constructor(config, token) {
    if (typeof config === 'object') {
      this._url   = (config.url || '').replace(/\/$/, '');
      this._token = config.token || '';
    } else {
      this._url   = (config || '').replace(/\/$/, '');
      this._token = token || '';
    }
  }

  // ── Auth ──────────────────────────────────────────────────

  async login(username, password) {
    const r = await fetch(this._url + '/auth/login', {
      method: 'POST', headers: {'Content-Type':'application/json'},
      body: JSON.stringify({username, password}), credentials: 'include'
    });
    const d = await r.json();
    if (d.success && d.token) this._token = d.token;
    return d;
  }

  async register(username, password) {
    const r = await fetch(this._url + '/auth/register', {
      method: 'POST', headers: {'Content-Type':'application/json'},
      body: JSON.stringify({username, password}), credentials: 'include'
    });
    const d = await r.json();
    if (d.success && d.token) this._token = d.token;
    return d;
  }

  async logout() {
    await fetch(this._url + '/auth/logout', {
      method: 'POST', headers: this._headers(), credentials: 'include'
    });
    this._token = '';
  }

  async me() {
    const r = await fetch(this._url + '/auth/me', {headers: this._headers(), credentials:'include'});
    return r.json();
  }

  // ── Query Builder ─────────────────────────────────────────

  from(table) { return new QueryBuilder(this, table); }

  // ── Raw SQL ───────────────────────────────────────────────

  async query(sql, params) {
    let finalSql = sql;
    if (params && params.length) {
      const p = [...params];
      finalSql = sql.replace(/\?/g, () => {
        const v = p.shift();
        if (v === null || v === undefined) return 'NULL';
        if (typeof v === 'number') return v;
        return `'${String(v).replace(/'/g, "''")}'`;
      });
    }
    return this._exec(finalSql);
  }

  // ── Transactions ──────────────────────────────────────────

  async transaction(fn) {
    try {
      await this._exec('BEGIN');
      const tx = new TxClient((sql) => this._exec(sql));
      const result = await fn(tx);
      await this._exec('COMMIT');
      return { data: result, error: null };
    } catch (err) {
      try { await this._exec('ROLLBACK'); } catch {}
      return { data: null, error: { message: err.message || String(err) } };
    }
  }

  // ── Schema Introspection ──────────────────────────────────

  async schema() {
    return this._get('/api/schema');
  }

  async tableSchema(table) {
    const r = await this._get(`/api/schema/${encodeURIComponent(table)}`);
    return { data: r.data?.table ?? null, error: r.error };
  }

  async tableColumns(table) {
    return this._get(`/api/schema/${encodeURIComponent(table)}/columns`);
  }

  async generateTypes() {
    const r = await this._get('/api/schema/generate/typescript');
    return { typescript: r.data?.typescript ?? null, error: r.error };
  }

  // ── Migrations ────────────────────────────────────────────

  async migrateUp(n) {
    const r = await fetch(this._url + '/api/migrate/up', {
      method: 'POST', headers: this._headers(),
      body: n !== undefined ? JSON.stringify({n}) : '{}', credentials: 'include'
    });
    return r.json();
  }

  async migrateDown(n = 1) {
    const r = await fetch(this._url + '/api/migrate/down', {
      method: 'POST', headers: this._headers(),
      body: JSON.stringify({n}), credentials: 'include'
    });
    return r.json();
  }

  async migrateStatus() {
    return this._get('/api/migrate/status');
  }

  // ── Branching ─────────────────────────────────────────────

  async createBranch(name) {
    const r = await this._exec(`CREATE BRANCH ${name}`);
    return { success: !r.error, error: r.error?.message };
  }

  async useBranch(name) {
    await this._exec(`USE BRANCH ${name}`);
  }

  async mergeBranch(source, target) {
    const r = await this._exec(`MERGE BRANCH ${source} INTO ${target}`);
    return { success: !r.error, error: r.error?.message };
  }

  async dropBranch(name) {
    const r = await this._exec(`DROP BRANCH ${name}`);
    return { success: !r.error, error: r.error?.message };
  }

  async listBranches() {
    return this._get('/api/branches');
  }

  // ── Health / Utility ──────────────────────────────────────

  async health() {
    const r = await fetch(this._url + '/health');
    return r.json();
  }

  async status() {
    const r = await fetch(this._url + '/status', {headers: this._headers(), credentials:'include'});
    return r.json();
  }

  async tables() {
    const r = await fetch(this._url + '/tables', {headers: this._headers(), credentials:'include'});
    const d = await r.json();
    return d.tables || [];
  }

  // ── Internal ──────────────────────────────────────────────

  async _exec(sql) {
    try {
      const r = await fetch(this._url + '/api/query', {
        method: 'POST', headers: this._headers(),
        body: JSON.stringify({sql}), credentials: 'include'
      });
      const d = await r.json();
      if (d.success === false)
        return { data: null, error: { message: d.error || 'Query failed' } };
      const rows = d.rows || d.data || [];
      return { data: Array.isArray(rows) ? rows : [], error: null, rows: Array.isArray(rows) ? rows.length : 0 };
    } catch (err) {
      return { data: null, error: { message: err.message || 'Network error' } };
    }
  }

  async _get(path) {
    try {
      const r = await fetch(this._url + path, {headers: this._headers(), credentials:'include'});
      const d = await r.json();
      if (d.success === false)
        return { data: null, error: { message: d.error || 'Request failed' } };
      return { data: d, error: null };
    } catch (err) {
      return { data: null, error: { message: err.message || 'Network error' } };
    }
  }

  _headers() {
    const h = {'Content-Type': 'application/json'};
    if (this._token) h['Authorization'] = 'Bearer ' + this._token;
    return h;
  }
}

// ── Exports ────────────────────────────────────────────────────

if (typeof module !== 'undefined' && module.exports) {
  module.exports = { MilanSQL, QueryBuilder, RealtimeBuilder, RealtimeSubscription };
  module.exports.default = MilanSQL;
} else if (typeof window !== 'undefined') {
  window.MilanSQL = MilanSQL;
  window.MilanSQLQueryBuilder = QueryBuilder;
}

export default MilanSQL;
export { MilanSQL, QueryBuilder, RealtimeBuilder, RealtimeSubscription };
