/**
 * milansql.js — JavaScript client for MilanSQL
 * Supabase-compatible API
 * @version 1.0.0
 */

class QueryBuilder {
    constructor(client, table) {
        this._client = client;
        this._table  = table;
        this._select = '*';
        this._where  = [];
        this._limit  = null;
        this._order  = null;
    }

    select(cols)  { this._select = cols; return this; }
    limit(n)      { this._limit  = n;    return this; }

    eq(col, val)  { this._where.push(`${col} = '${val}'`);    return this; }
    neq(col, val) { this._where.push(`${col} != '${val}'`);   return this; }
    gt(col, val)  { this._where.push(`${col} > ${val}`);      return this; }
    gte(col, val) { this._where.push(`${col} >= ${val}`);     return this; }
    lt(col, val)  { this._where.push(`${col} < ${val}`);      return this; }
    lte(col, val) { this._where.push(`${col} <= ${val}`);     return this; }
    like(col, pat){ this._where.push(`${col} LIKE '${pat}'`); return this; }

    order(col, { ascending = true } = {}) {
        this._order = `${col} ${ascending ? 'ASC' : 'DESC'}`;
        return this;
    }

    async execute() {
        let sql = `SELECT ${this._select} FROM ${this._table}`;
        if (this._where.length) sql += ' WHERE ' + this._where.join(' AND ');
        if (this._order)        sql += ' ORDER BY ' + this._order;
        if (this._limit)        sql += ' LIMIT ' + this._limit;
        return this._client.query(sql);
    }

    // Supabase alias
    then(resolve, reject) { return this.execute().then(resolve, reject); }
}

class MilanSQL {
    /**
     * @param {string} url   — e.g. 'https://milansql.de'
     * @param {string} token — JWT token from /auth/login
     */
    constructor(url, token) {
        this.url   = url.replace(/\/$/, '');
        this.token = token || '';
    }

    // ── Auth ──────────────────────────────────────────────────────

    async login(username, password) {
        const r = await fetch(this.url + '/auth/login', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ username, password }),
            credentials: 'include'
        });
        const d = await r.json();
        if (d.success && d.token) this.token = d.token;
        return d;
    }

    async register(username, password) {
        const r = await fetch(this.url + '/auth/register', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ username, password }),
            credentials: 'include'
        });
        const d = await r.json();
        if (d.success && d.token) this.token = d.token;
        return d;
    }

    async me() {
        const r = await fetch(this.url + '/auth/me', {
            headers: this._headers(),
            credentials: 'include'
        });
        return r.json();
    }

    async logout() {
        await fetch(this.url + '/auth/logout', {
            method: 'POST',
            headers: this._headers(),
            credentials: 'include'
        });
        this.token = '';
    }

    // ── Query ─────────────────────────────────────────────────────

    async query(sql) {
        const r = await fetch(this.url + '/query', {
            method: 'POST',
            headers: this._headers(),
            body: JSON.stringify({ sql }),
            credentials: 'include'
        });
        return r.json();
    }

    // ── Builder ───────────────────────────────────────────────────

    from(table) { return new QueryBuilder(this, table); }

    // ── Tables ────────────────────────────────────────────────────

    async tables() {
        const r = await fetch(this.url + '/tables', {
            headers: this._headers(),
            credentials: 'include'
        });
        const d = await r.json();
        return d.tables || [];
    }

    // ── Health ────────────────────────────────────────────────────

    async health() {
        const r = await fetch(this.url + '/health');
        return r.json();
    }

    // ── Internal ──────────────────────────────────────────────────

    _headers() {
        const h = { 'Content-Type': 'application/json' };
        if (this.token) h['Authorization'] = 'Bearer ' + this.token;
        return h;
    }
}

// ESM + CommonJS dual export
if (typeof module !== 'undefined' && module.exports) {
    module.exports = { MilanSQL, QueryBuilder };
    module.exports.default = MilanSQL;
} else if (typeof window !== 'undefined') {
    window.MilanSQL = MilanSQL;
}

export default MilanSQL;
export { MilanSQL, QueryBuilder };
