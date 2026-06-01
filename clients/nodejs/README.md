# milansql-client — Node.js Client

![npm](https://img.shields.io/npm/v/milansql-client)

Node.js client for MilanSQL database.

## Install

```bash
npm install milansql-client
```

## Usage

```javascript
const { connect } = require('milansql-client');

async function main() {
    const conn = await connect({ host: 'localhost', port: 4406 });
    const rows = await conn.query('SELECT * FROM users');
    console.log(rows);
    conn.close();
}
main();
```
