#!/usr/bin/env node
// milansql-cli — Official CLI for MilanSQL
// Install: npm install -g milansql-cli
// Usage:   milansql <command> [options]
// ============================================================

'use strict';

const fs = require('fs');
const path = require('path');
const os = require('os');
const https = require('https');
const http = require('http');
const readline = require('readline');

// ── Config ────────────────────────────────────────────────────

const CONFIG_DIR  = path.join(os.homedir(), '.milansql');
const CONFIG_FILE = path.join(CONFIG_DIR, 'config.json');

function loadConfig() {
  try {
    if (fs.existsSync(CONFIG_FILE))
      return JSON.parse(fs.readFileSync(CONFIG_FILE, 'utf8'));
  } catch {}
  return { url: '', token: '', default_branch: 'main' };
}

function saveConfig(cfg) {
  if (!fs.existsSync(CONFIG_DIR)) fs.mkdirSync(CONFIG_DIR, { recursive: true });
  fs.writeFileSync(CONFIG_FILE, JSON.stringify(cfg, null, 2));
}

// ── HTTP helper ───────────────────────────────────────────────

function request(method, urlStr, body, token) {
  return new Promise((resolve, reject) => {
    const url = new URL(urlStr);
    const lib = url.protocol === 'https:' ? https : http;
    const data = body ? JSON.stringify(body) : null;
    const opts = {
      hostname: url.hostname,
      port: url.port || (url.protocol === 'https:' ? 443 : 80),
      path: url.pathname + url.search,
      method,
      headers: {
        'Content-Type': 'application/json',
        ...(token ? { 'Authorization': `Bearer ${token}` } : {}),
        ...(data ? { 'Content-Length': Buffer.byteLength(data) } : {}),
      },
    };
    const req = lib.request(opts, (res) => {
      let raw = '';
      res.on('data', c => raw += c);
      res.on('end', () => {
        try { resolve(JSON.parse(raw)); }
        catch { resolve({ _raw: raw }); }
      });
    });
    req.on('error', reject);
    if (data) req.write(data);
    req.end();
  });
}

// ── Formatters ────────────────────────────────────────────────

function printTable(rows) {
  if (!rows || rows.length === 0) { console.log('(no rows)'); return; }
  const cols = Object.keys(rows[0]);
  const widths = cols.map(c => Math.max(c.length, ...rows.map(r => String(r[c] ?? '').length)));
  const hr = '+' + widths.map(w => '-'.repeat(w + 2)).join('+') + '+';
  const row = (vals) => '| ' + vals.map((v, i) => String(v ?? '').padEnd(widths[i])).join(' | ') + ' |';
  console.log(hr);
  console.log(row(cols));
  console.log(hr);
  rows.forEach(r => console.log(row(cols.map(c => r[c] ?? ''))));
  console.log(hr);
  console.log(`(${rows.length} row${rows.length === 1 ? '' : 's'})`);
}

function printJson(obj) {
  console.log(JSON.stringify(obj, null, 2));
}

// ── Commands ──────────────────────────────────────────────────

const commands = {

  // ── connect ───────────────────────────────────────────────
  async connect(args) {
    const urlFlag   = getFlag(args, '--url');
    const tokenFlag = getFlag(args, '--token');
    if (!urlFlag) { console.error('Usage: milansql connect --url <url> [--token <token>]'); process.exit(1); }
    const cfg = loadConfig();
    cfg.url = urlFlag;
    if (tokenFlag) cfg.token = tokenFlag;

    // Test connection
    try {
      const r = await request('GET', urlFlag + '/health', null, cfg.token);
      if (r.status === 'healthy' || r.alive || r.success !== false) {
        saveConfig(cfg);
        console.log(`Connected to ${urlFlag}`);
        if (r.version) console.log(`Server: ${r.version}`);
      } else {
        console.error('Connection failed:', JSON.stringify(r));
        process.exit(1);
      }
    } catch (e) {
      console.error('Connection error:', e.message);
      process.exit(1);
    }
  },

  // ── query ─────────────────────────────────────────────────
  async query([sql, ...rest]) {
    if (!sql) { console.error('Usage: milansql query "<SQL>"'); process.exit(1); }
    const cfg = loadConfig();
    if (!cfg.url) { console.error('Not connected. Run: milansql connect --url <url>'); process.exit(1); }
    const r = await request('POST', cfg.url + '/api/query', { sql }, cfg.token);
    if (r.error) { console.error('Error:', r.error); process.exit(1); }
    const rows = r.rows || r.data || [];
    if (Array.isArray(rows) && rows.length > 0 && typeof rows[0] === 'object')
      printTable(rows);
    else
      printJson(r);
  },

  // ── status ────────────────────────────────────────────────
  async status() {
    const cfg = loadConfig();
    if (!cfg.url) { console.error('Not connected.'); process.exit(1); }
    const r = await request('GET', cfg.url + '/status', null, cfg.token);
    console.log('Server:', r.version || cfg.url);
    console.log('Status:', r.status || 'unknown');
    if (r.uptime_fmt) console.log('Uptime:', r.uptime_fmt);
    if (r.tables !== undefined) console.log('Tables:', r.tables);
    if (r.rows !== undefined) console.log('Rows:', r.rows);
    if (r.queries !== undefined) console.log('Queries:', r.queries);
  },

  // ── version ───────────────────────────────────────────────
  async version() {
    const pkg = require('../package.json');
    console.log(`milansql-cli v${pkg.version}`);
    const cfg = loadConfig();
    if (cfg.url) {
      try {
        const r = await request('GET', cfg.url + '/status', null, cfg.token);
        console.log(`Server: ${r.version || 'unknown'}`);
      } catch {}
    }
  },

  // ── migrate ───────────────────────────────────────────────
  async migrate(args) {
    const sub = (args[0] || 'status').toLowerCase();
    const cfg = loadConfig();
    if (!cfg.url) { console.error('Not connected.'); process.exit(1); }
    if (sub === 'up') {
      const n = args[1] ? parseInt(args[1]) : undefined;
      const r = await request('POST', cfg.url + '/api/migrate/up', n !== undefined ? {n} : {}, cfg.token);
      if (r.error) { console.error('Error:', r.error); process.exit(1); }
      console.log('Applied:', (r.applied || []).join(', ') || '(none)');
    } else if (sub === 'down') {
      const n = args[1] ? parseInt(args[1]) : 1;
      const r = await request('POST', cfg.url + '/api/migrate/down', {n}, cfg.token);
      if (r.error) { console.error('Error:', r.error); process.exit(1); }
      console.log('Rolled back:', (r.rolled_back || []).join(', ') || '(none)');
    } else {
      const r = await request('GET', cfg.url + '/api/migrate/status', null, cfg.token);
      const migs = r.migrations || [];
      if (migs.length === 0) { console.log('No migrations defined.'); return; }
      printTable(migs.map(m => ({ name: m.name, status: m.status, applied_at: m.applied_at || '-' })));
    }
  },

  // ── backup ────────────────────────────────────────────────
  async backup(args) {
    const sub = (args[0] || 'create').toLowerCase();
    const cfg = loadConfig();
    if (!cfg.url) { console.error('Not connected.'); process.exit(1); }
    if (sub === 'create') {
      const r = await request('GET', cfg.url + '/backup', null, cfg.token);
      if (r.error) { console.error('Error:', r.error); process.exit(1); }
      const fname = `milansql_backup_${Date.now()}.sql`;
      if (r._raw) { fs.writeFileSync(fname, r._raw); console.log(`Backup saved: ${fname}`); }
      else printJson(r);
    } else if (sub === 'restore') {
      const file = getFlag(args, '--file');
      if (!file) { console.error('Usage: milansql backup restore --file <file>'); process.exit(1); }
      const sql = fs.readFileSync(file, 'utf8');
      const r = await request('POST', cfg.url + '/restore', { sql }, cfg.token);
      if (r.error) { console.error('Error:', r.error); process.exit(1); }
      console.log('Restore complete.');
    }
  },

  // ── branch ────────────────────────────────────────────────
  async branch(args) {
    const sub = (args[0] || 'list').toLowerCase();
    const cfg = loadConfig();
    if (!cfg.url) { console.error('Not connected.'); process.exit(1); }
    if (sub === 'create') {
      const name = args[1];
      if (!name) { console.error('Usage: milansql branch create <name>'); process.exit(1); }
      const r = await request('POST', cfg.url + '/api/query', { sql: `CREATE BRANCH ${name}` }, cfg.token);
      console.log(r.error ? `Error: ${r.error}` : `Branch '${name}' created.`);
    } else if (sub === 'list') {
      const r = await request('GET', cfg.url + '/api/branches', null, cfg.token);
      const branches = r.branches || r.data || [];
      if (branches.length === 0) console.log('No branches.');
      else printTable(Array.isArray(branches) ? branches.map(b => typeof b === 'string' ? {name:b} : b) : []);
    } else if (sub === 'merge') {
      const [src, tgt] = [args[1], args[2]];
      if (!src || !tgt) { console.error('Usage: milansql branch merge <source> <target>'); process.exit(1); }
      const r = await request('POST', cfg.url + '/api/query', { sql: `MERGE BRANCH ${src} INTO ${tgt}` }, cfg.token);
      console.log(r.error ? `Error: ${r.error}` : `Merged '${src}' into '${tgt}'.`);
    } else if (sub === 'delete') {
      const name = args[1];
      if (!name) { console.error('Usage: milansql branch delete <name>'); process.exit(1); }
      const r = await request('POST', cfg.url + '/api/query', { sql: `DROP BRANCH ${name}` }, cfg.token);
      console.log(r.error ? `Error: ${r.error}` : `Branch '${name}' deleted.`);
    }
  },

  // ── import ────────────────────────────────────────────────
  async import(args) {
    const file  = getFlag(args, '--file');
    const table = getFlag(args, '--table');
    if (!file || !table) { console.error('Usage: milansql import --file <csv> --table <name>'); process.exit(1); }
    const cfg = loadConfig();
    if (!cfg.url) { console.error('Not connected.'); process.exit(1); }
    const lines = fs.readFileSync(file, 'utf8').split('\n').filter(Boolean);
    const headers = lines[0].split(',').map(h => h.trim());
    let imported = 0;
    for (let i = 1; i < lines.length; i++) {
      const vals = lines[i].split(',').map(v => v.trim());
      const colList = headers.join(', ');
      const valList = vals.map(v => `'${v.replace(/'/g, "''")}'`).join(', ');
      const r = await request('POST', cfg.url + '/api/query', {
        sql: `INSERT INTO ${table} (${colList}) VALUES (${valList})`
      }, cfg.token);
      if (!r.error) imported++;
    }
    console.log(`Imported ${imported}/${lines.length - 1} rows into ${table}.`);
  },

  // ── export ────────────────────────────────────────────────
  async export(args) {
    const table  = getFlag(args, '--table');
    const format = getFlag(args, '--format') || 'csv';
    if (!table) { console.error('Usage: milansql export --table <name> [--format csv|json]'); process.exit(1); }
    const cfg = loadConfig();
    if (!cfg.url) { console.error('Not connected.'); process.exit(1); }
    const r = await request('POST', cfg.url + '/api/query', { sql: `SELECT * FROM ${table}` }, cfg.token);
    if (r.error) { console.error('Error:', r.error); process.exit(1); }
    const rows = r.rows || r.data || [];
    const fname = `${table}_export_${Date.now()}.${format}`;
    if (format === 'json') {
      fs.writeFileSync(fname, JSON.stringify(rows, null, 2));
    } else {
      if (rows.length === 0) { fs.writeFileSync(fname, ''); }
      else {
        const cols = Object.keys(rows[0]);
        const csv = [cols.join(','), ...rows.map(r => cols.map(c => `"${String(r[c] ?? '').replace(/"/g, '""')}"`).join(','))].join('\n');
        fs.writeFileSync(fname, csv);
      }
    }
    console.log(`Exported ${rows.length} rows to ${fname}.`);
  },

  // ── help ──────────────────────────────────────────────────
  help() {
    console.log(`
milansql CLI — Official client for MilanSQL

Usage: milansql <command> [options]

Commands:
  connect      --url <url> [--token <token>]     Connect to a MilanSQL server
  query        "<SQL>"                            Execute SQL query
  status                                          Show server info & health
  version                                         Show CLI + server version

  migrate      up [n]                             Apply pending migrations
  migrate      down [n]                           Roll back migrations (default: 1)
  migrate      status                             Show migration status

  backup       create                             Create a backup
  backup       restore --file <file>              Restore from backup

  branch       create <name>                      Create a branch
  branch       list                               List branches
  branch       merge <source> <target>            Merge branches
  branch       delete <name>                      Delete a branch

  import       --file <csv> --table <table>       Import CSV data
  export       --table <table> [--format csv|json] Export table data

Config file: ~/.milansql/config.json
`);
  }
};

// ── Utilities ─────────────────────────────────────────────────

function getFlag(args, name) {
  const idx = args.indexOf(name);
  return idx !== -1 && idx + 1 < args.length ? args[idx + 1] : null;
}

// ── Main ──────────────────────────────────────────────────────

async function main() {
  const [,, cmd, ...args] = process.argv;
  const handler = commands[cmd];
  if (!handler) {
    if (cmd) console.error(`Unknown command: ${cmd}\n`);
    commands.help();
    process.exit(cmd ? 1 : 0);
  }
  try {
    await handler(args);
  } catch (err) {
    console.error('Error:', err.message || err);
    process.exit(1);
  }
}

main();
