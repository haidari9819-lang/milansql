'use strict';
/**
 * MilanSQL Node.js Client — Basic example
 *
 * Requires: MilanSQL HTTP server on port 8080
 *   .\build\milansql.exe --http --port 8080
 */

const milansql = require('../index');

async function main() {
  const conn = milansql.connect({ host: 'localhost', port: 8080 });

  // Server status
  const status = await conn.status();
  console.log('Server Status:', status);

  // Create table
  await conn.query(
    'CREATE TABLE IF NOT EXISTS users ' +
    '(id INT PRIMARY KEY AUTO_INCREMENT, name TEXT, score INT)'
  );
  console.log('\nTabelle erstellt.');

  // Insert rows
  for (const [name, score] of [['Alice', 95], ['Bob', 82], ['Charlie', 70]]) {
    const r = await conn.query(`INSERT INTO users VALUES (NULL, ${name}, ${score})`);
    console.log(`  INSERT: ${r.message || 'OK'}`);
  }

  // SELECT via POST
  const result = await conn.query('SELECT * FROM users ORDER BY score DESC');
  console.log(`\nAlle Benutzer (${result.rowCount} Zeilen):`);
  console.log('  Spalten:', result.columns);
  for (const row of result.rows) {
    console.log(' ', row);
  }

  // SELECT via GET
  const filtered = await conn.queryGet('SELECT name, score FROM users WHERE score > 80');
  console.log('\nBenutzer mit Score > 80:');
  for (const row of filtered.rows) {
    console.log(' ', row);
  }

  // Metadata
  const tables = await conn.tables();
  console.log('\nTabellen:', tables);

  const schemas = await conn.schemas();
  console.log('Schemas:', schemas);

  conn.close();
}

main().catch(console.error);
