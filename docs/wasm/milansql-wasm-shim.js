/**
 * MilanSQL WASM Shim
 * Simulates the MilanSQL C++/WASM engine entirely in JavaScript.
 * Used for the browser demo when the real WASM binary is not available.
 *
 * For production use, replace this with the real milansql.js from Emscripten.
 */

(function(global) {
    'use strict';

    // In-memory database state
    const db = {
        tables: {},      // name → { columns: [{name, type}], rows: [[...]] }
        views: {},       // name → sql
        queryHistory: [],
        transactionStack: [],
        inTransaction: false,
    };

    // Simple SQL parser/executor
    const MilanSQLEngine = {

        exec(sql) {
            const statements = sql.split(';').map(s => s.trim()).filter(s => s.length > 0);
            let lastResult = { type: 'ok', message: 'OK' };
            for (const stmt of statements) {
                lastResult = this.execOne(stmt);
                if (lastResult.type === 'error') break;
            }
            return lastResult;
        },

        execOne(sql) {
            const upper = sql.trim().toUpperCase();
            try {
                if (upper.startsWith('CREATE TABLE')) return this.createTable(sql);
                if (upper.startsWith('DROP TABLE')) return this.dropTable(sql);
                if (upper.startsWith('INSERT INTO')) return this.insert(sql);
                if (upper.startsWith('SELECT')) return this.select(sql);
                if (upper.startsWith('DELETE FROM')) return this.delete(sql);
                if (upper.startsWith('UPDATE')) return this.update(sql);
                if (upper.startsWith('TRUNCATE TABLE')) return this.truncate(sql);
                if (upper.startsWith('SHOW TABLES')) return this.showTables();
                if (upper.startsWith('DESCRIBE') || upper.startsWith('DESC ')) return this.describe(sql);
                if (upper.startsWith('BEGIN') || upper === 'START TRANSACTION') { db.inTransaction = true; return {type:'ok',message:'BEGIN'}; }
                if (upper === 'COMMIT') { db.inTransaction = false; return {type:'ok',message:'COMMIT'}; }
                if (upper === 'ROLLBACK') { db.inTransaction = false; return {type:'ok',message:'ROLLBACK'}; }
                return { type: 'ok', message: 'OK (statement not parsed by shim)' };
            } catch(e) {
                return { type: 'error', message: e.message };
            }
        },

        createTable(sql) {
            const m = sql.match(/CREATE\s+TABLE\s+(?:IF\s+NOT\s+EXISTS\s+)?(\w+)\s*\(([^)]+)\)/i);
            if (!m) throw new Error('Invalid CREATE TABLE syntax');
            const name = m[1].toLowerCase();
            const colDefs = m[2].split(',').map(s => s.trim()).filter(s => s.length > 0);
            const columns = [];
            for (const def of colDefs) {
                const parts = def.trim().split(/\s+/);
                if (parts[0].toUpperCase() === 'PRIMARY' || parts[0].toUpperCase() === 'UNIQUE') continue;
                columns.push({ name: parts[0].toLowerCase(), type: parts[1] || 'TEXT' });
            }
            if (db.tables[name]) return {type:'ok',message:`Table '${name}' already exists`};
            db.tables[name] = { columns, rows: [] };
            return { type: 'ok', message: `Table '${name}' created` };
        },

        dropTable(sql) {
            const m = sql.match(/DROP\s+TABLE\s+(?:IF\s+EXISTS\s+)?(\w+)/i);
            if (!m) throw new Error('Invalid DROP TABLE');
            const name = m[1].toLowerCase();
            delete db.tables[name];
            return { type: 'ok', message: `Table '${name}' dropped` };
        },

        insert(sql) {
            const m = sql.match(/INSERT\s+INTO\s+(\w+)\s*(?:\(([^)]+)\))?\s*VALUES\s*\(([^)]+)\)/i);
            if (!m) throw new Error('Invalid INSERT syntax');
            const name = m[1].toLowerCase();
            const tbl = db.tables[name];
            if (!tbl) throw new Error(`Table '${name}' not found`);
            const vals = this.parseValues(m[3]);
            const row = tbl.columns.map((col, i) => {
                let v = vals[i] !== undefined ? vals[i] : 'NULL';
                if (col.type.toUpperCase().includes('INT') && v === 'NULL') {
                    // Auto-increment simulation
                    const maxId = tbl.rows.reduce((m, r) => Math.max(m, parseInt(r[i])||0), 0);
                    v = String(maxId + 1);
                }
                return v;
            });
            tbl.rows.push(row);
            return { type: 'ok', message: 'INSERT 0 1' };
        },

        parseValues(str) {
            const vals = [];
            let cur = '', inQ = false, q = '';
            for (let i = 0; i < str.length; i++) {
                const c = str[i];
                if ((c === "'" || c === '"') && !inQ) { inQ = true; q = c; }
                else if (c === q && inQ) { inQ = false; vals.push(cur); cur = ''; }
                else if (c === ',' && !inQ) { vals.push(cur.trim()); cur = ''; }
                else if (!inQ || (inQ && c !== q)) cur += c;
            }
            if (cur.trim()) vals.push(cur.trim());
            return vals;
        },

        select(sql) {
            // Simple SELECT: SELECT cols FROM table [WHERE ...] [ORDER BY ...] [LIMIT n]
            const upper = sql.toUpperCase();

            // Handle SELECT without FROM (e.g. SELECT 1, SELECT 'hello')
            if (!upper.includes(' FROM ')) {
                const exprPart = sql.replace(/SELECT\s+/i, '').trim();
                return { type: 'result', columns: ['result'], rows: [[exprPart]] };
            }

            const m = sql.match(/SELECT\s+(.*?)\s+FROM\s+(\w+)(?:\s+WHERE\s+(.+?))?(?:\s+ORDER\s+BY\s+(.+?))?(?:\s+LIMIT\s+(\d+))?$/i);
            if (!m) throw new Error('Cannot parse SELECT');

            const colExpr = m[1].trim();
            const tableName = m[2].toLowerCase();
            const whereClause = m[3];
            const orderBy = m[4];
            const limitN = m[5] ? parseInt(m[5]) : null;

            if (tableName === 'pg_catalog.pg_tables' || tableName === 'information_schema.tables') {
                const rows = Object.keys(db.tables).map(n => ['public', n, 'root', 'f']);
                return { type: 'result', columns: ['schemaname','tablename','tableowner','hasindexes'], rows };
            }

            const tbl = db.tables[tableName];
            if (!tbl) throw new Error(`Table '${tableName}' not found`);

            // Determine output columns
            let outCols, colIndices;
            if (colExpr === '*' || colExpr === 'COUNT(*)') {
                outCols = tbl.columns.map(c => c.name);
                colIndices = tbl.columns.map((_, i) => i);
            } else {
                const parts = colExpr.split(',').map(s => s.trim());
                outCols = parts.map(p => p.replace(/^.*\.\s*/, '').replace(/\s+AS\s+.*/i, '').trim());
                colIndices = parts.map(p => {
                    const colName = p.replace(/\s+AS\s+.*/i, '').trim().toLowerCase();
                    return tbl.columns.findIndex(c => c.name === colName);
                });
            }

            // Filter rows by WHERE
            let rows = tbl.rows.filter(row => {
                if (!whereClause) return true;
                return this.evalWhere(whereClause, tbl.columns, row);
            });

            // COUNT(*)
            if (colExpr.toUpperCase().includes('COUNT(*)')) {
                return { type: 'result', columns: ['COUNT(*)'], rows: [[String(rows.length)]] };
            }

            // Project columns
            let result = rows.map(row => colIndices.map(i => i >= 0 ? row[i] : 'NULL'));

            // ORDER BY
            if (orderBy) {
                const parts = orderBy.split(',').map(s => s.trim());
                result.sort((a, b) => {
                    for (const part of parts) {
                        const [col, dir] = part.split(/\s+/);
                        const idx = outCols.findIndex(c => c.toLowerCase() === col.toLowerCase());
                        if (idx < 0) continue;
                        const va = a[idx], vb = b[idx];
                        const cmp = isNaN(va) || isNaN(vb) ? va.localeCompare(vb) : parseFloat(va) - parseFloat(vb);
                        if (cmp !== 0) return (dir && dir.toUpperCase() === 'DESC') ? -cmp : cmp;
                    }
                    return 0;
                });
            }

            // LIMIT
            if (limitN !== null) result = result.slice(0, limitN);

            return { type: 'result', columns: outCols, rows: result };
        },

        evalWhere(clause, columns, row) {
            // Simple WHERE evaluator: col = 'val', col > n, col < n, col != 'val'
            const m = clause.match(/(\w+)\s*(=|!=|<>|>=|<=|>|<)\s*['"]?([^'"]+)['"]?/);
            if (!m) return true;
            const colIdx = columns.findIndex(c => c.name === m[1].toLowerCase());
            if (colIdx < 0) return true;
            const rowVal = row[colIdx];
            const cmpVal = m[3].trim();
            const op = m[2];
            if (!isNaN(rowVal) && !isNaN(cmpVal)) {
                const rv = parseFloat(rowVal), cv = parseFloat(cmpVal);
                if (op === '=') return rv === cv;
                if (op === '!=' || op === '<>') return rv !== cv;
                if (op === '>') return rv > cv;
                if (op === '<') return rv < cv;
                if (op === '>=') return rv >= cv;
                if (op === '<=') return rv <= cv;
            }
            if (op === '=') return rowVal === cmpVal;
            if (op === '!=' || op === '<>') return rowVal !== cmpVal;
            return true;
        },

        delete(sql) {
            const m = sql.match(/DELETE\s+FROM\s+(\w+)(?:\s+WHERE\s+(.+))?/i);
            if (!m) throw new Error('Invalid DELETE syntax');
            const tbl = db.tables[m[1].toLowerCase()];
            if (!tbl) throw new Error(`Table '${m[1]}' not found`);
            const before = tbl.rows.length;
            if (m[2]) {
                tbl.rows = tbl.rows.filter(row => !this.evalWhere(m[2], tbl.columns, row));
            } else {
                tbl.rows = [];
            }
            return { type: 'ok', message: `DELETE ${before - tbl.rows.length}` };
        },

        update(sql) {
            const m = sql.match(/UPDATE\s+(\w+)\s+SET\s+(.+?)(?:\s+WHERE\s+(.+))?$/i);
            if (!m) throw new Error('Invalid UPDATE syntax');
            const tbl = db.tables[m[1].toLowerCase()];
            if (!tbl) throw new Error(`Table '${m[1]}' not found`);
            const setPart = m[2];
            const setM = setPart.match(/(\w+)\s*=\s*['"]?([^,'"]+)['"]?/);
            if (!setM) throw new Error('Invalid SET clause');
            const setCol = tbl.columns.findIndex(c => c.name === setM[1].toLowerCase());
            let count = 0;
            tbl.rows.forEach(row => {
                if (!m[3] || this.evalWhere(m[3], tbl.columns, row)) {
                    if (setCol >= 0) row[setCol] = setM[2].trim();
                    count++;
                }
            });
            return { type: 'ok', message: `UPDATE ${count}` };
        },

        truncate(sql) {
            const m = sql.match(/TRUNCATE\s+TABLE\s+(\w+)/i);
            if (!m) throw new Error('Invalid TRUNCATE');
            const tbl = db.tables[m[1].toLowerCase()];
            if (tbl) tbl.rows = [];
            return { type: 'ok', message: 'TRUNCATE TABLE' };
        },

        showTables() {
            const rows = Object.keys(db.tables).map(n => [n]);
            return { type: 'result', columns: ['Tables'], rows };
        },

        describe(sql) {
            const m = sql.match(/(?:DESCRIBE|DESC)\s+(\w+)/i);
            if (!m) throw new Error('Invalid DESCRIBE');
            const tbl = db.tables[m[1].toLowerCase()];
            if (!tbl) throw new Error(`Table '${m[1]}' not found`);
            return {
                type: 'result',
                columns: ['Column', 'Type'],
                rows: tbl.columns.map(c => [c.name, c.type])
            };
        },

        getTables() { return Object.keys(db.tables); },
        getSchema(name) { return db.tables[name.toLowerCase()]?.columns || []; }
    };

    // Public API
    const MilanSQL = {
        async init() {
            // Pre-load demo data
            MilanSQLEngine.exec(`
                CREATE TABLE employees (id INT PRIMARY KEY AUTO_INCREMENT, name TEXT, dept TEXT, salary REAL);
                INSERT INTO employees VALUES (1, Alice, Engineering, 95000);
                INSERT INTO employees VALUES (2, Bob, Marketing, 72000);
                INSERT INTO employees VALUES (3, Carol, Engineering, 88000);
                INSERT INTO employees VALUES (4, Dave, HR, 65000);
                INSERT INTO employees VALUES (5, Eve, Engineering, 102000);

                CREATE TABLE departments (id INT PRIMARY KEY, name TEXT, budget REAL);
                INSERT INTO departments VALUES (1, Engineering, 500000);
                INSERT INTO departments VALUES (2, Marketing, 200000);
                INSERT INTO departments VALUES (3, HR, 150000);

                CREATE TABLE orders (id INT PRIMARY KEY AUTO_INCREMENT, customer TEXT, amount REAL, status TEXT);
                INSERT INTO orders VALUES (1, Alice, 1500, pending);
                INSERT INTO orders VALUES (2, Bob, 350, completed);
                INSERT INTO orders VALUES (3, Alice, 2200, pending);
                INSERT INTO orders VALUES (4, Carol, 890, completed);
            `);
            return this;
        },
        exec(sql) { return MilanSQLEngine.exec(sql); },
        query(sql) { return MilanSQLEngine.select(sql); },
        getTables() { return MilanSQLEngine.getTables(); },
        getSchema(name) { return MilanSQLEngine.getSchema(name); }
    };

    global.MilanSQL = MilanSQL;
    global.MilanSQLEngine = MilanSQLEngine;

})(typeof window !== 'undefined' ? window : global);
