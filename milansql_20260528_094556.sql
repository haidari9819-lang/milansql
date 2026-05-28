-- MilanSQL Backup v1.0.0
-- Erstellt: 2026-05-28 09:45:56
-- Schemas: public

-- ═══════════════════════════════════════
-- Tabellen
-- ═══════════════════════════════════════

USE public;

DROP TABLE IF EXISTS users;
CREATE TABLE users (id INT PRIMARY KEY AUTO_INCREMENT, name TEXT);

INSERT INTO users VALUES (1, 'Alice');
INSERT INTO users VALUES (2, 'Bob');

DROP TABLE IF EXISTS test_dates;
CREATE TABLE test_dates (id INT PRIMARY KEY, name TEXT, created_at TEXT DEFAULT CURRENT_TIMESTAMP, birthday TEXT);

INSERT INTO test_dates VALUES (1, 'Alice', '1990-05-15', NULL);
INSERT INTO test_dates VALUES (2, 'Bob', '1985-12-03', NULL);

DROP TABLE IF EXISTS test2;
CREATE TABLE test2 (id INT, name TEXT);

INSERT INTO test2 VALUES (1, 'Laptop');

DROP TABLE IF EXISTS termine;
CREATE TABLE termine (id INT PRIMARY KEY AUTO_INCREMENT, titel TEXT, termin_datum TEXT, erstellt TEXT DEFAULT CURRENT_TIMESTAMP);

INSERT INTO termine VALUES ('Meeting', '2026-06-15', NULL, '2026-05-28 08:45:41');
INSERT INTO termine VALUES ('Urlaub', '2026-08-01', NULL, '2026-05-28 08:45:41');

DROP TABLE IF EXISTS produkte;
CREATE TABLE produkte (id INT PRIMARY KEY AUTO_INCREMENT, name TEXT, specs TEXT, tags TEXT);

INSERT INTO produkte VALUES (1, 'Laptop', '{"cpu":"Intel i7","ram":16,"ssd":512,"preis":1299.99}', '["elektronik","computer","premium"]');
INSERT INTO produkte VALUES (2, 'Maus', '{"tasten":3,"kabellos":true,"farbe":"schwarz"}', '["elektronik","zubehoer"]');
INSERT INTO produkte VALUES (3, 'Monitor', '{"zoll":27,"aufloesung":"4K","hdr":true}', '["elektronik","display"]');
INSERT INTO produkte VALUES (4, 'Laptop', '{"cpu":"Intel i7","ram":16,"ssd":512,"preis":1299.99}', '["elektronik","computer","premium"]');
INSERT INTO produkte VALUES (5, 'Maus', '{"tasten":3,"kabellos":true,"farbe":"schwarz"}', '["elektronik","zubehoer"]');
INSERT INTO produkte VALUES (6, 'Monitor', '{"zoll":27,"aufloesung":"4K","hdr":true}', '["elektronik","display"]');
INSERT INTO produkte VALUES (7, 'Fehler', 'nicht-json', '[]');

DROP TABLE IF EXISTS p56test;
CREATE TABLE p56test (id INT, name TEXT, data TEXT);

INSERT INTO p56test VALUES (1, 'Valid', '{"key":"value"}');

DROP TABLE IF EXISTS kunden;
CREATE TABLE kunden (id INT PRIMARY KEY AUTO_INCREMENT, name TEXT NOT NULL, email TEXT);

INSERT INTO kunden VALUES (1, 'Alice', 'alice@mail.com');
INSERT INTO kunden VALUES (2, 'Bob', 'bob@mail.com');

DROP TABLE IF EXISTS bestellungen;
CREATE TABLE bestellungen (id INT PRIMARY KEY AUTO_INCREMENT, kunden_id INT, artikel TEXT, preis INT, FOREIGN KEY (kunden_id) REFERENCES kunden(id));

INSERT INTO bestellungen VALUES (1, 1, 'Laptop', 1200);
INSERT INTO bestellungen VALUES (2, 1, 'Maus', 25);
INSERT INTO bestellungen VALUES (3, 2, 'Monitor', 450);

DROP TABLE IF EXISTS events;
CREATE TABLE events (id INT PRIMARY KEY AUTO_INCREMENT, name TEXT, created_at TEXT DEFAULT CURRENT_TIMESTAMP);

INSERT INTO events VALUES ('Geburt', NULL, '2026-05-28 08:41:34');
INSERT INTO events VALUES ('Hochzeit', NULL, '2026-05-28 08:41:34');
INSERT INTO events VALUES (3, 'Abschluss', '2020-06-15 10:30:00');

DROP TABLE IF EXISTS dt_test;
CREATE TABLE dt_test (id INT PRIMARY KEY AUTO_INCREMENT, name TEXT, created_at TEXT DEFAULT CURRENT_TIMESTAMP, birthday TEXT);

INSERT INTO dt_test VALUES (1, 'Alice', '1990-05-15', NULL);
INSERT INTO dt_test VALUES (2, 'Bob', '1985-12-03', NULL);

DROP TABLE IF EXISTS dt3;
CREATE TABLE dt3 (id INT PRIMARY KEY, name TEXT, created_at TEXT DEFAULT CURRENT_TIMESTAMP, birthday TEXT);

INSERT INTO dt3 VALUES (1, 'Alice', '1990-05-15', NULL);
INSERT INTO dt3 VALUES (2, 'Bob', '1985-12-03', NULL);

DROP TABLE IF EXISTS dt2;
CREATE TABLE dt2 (id INT PRIMARY KEY, name TEXT, created_at TEXT DEFAULT CURRENT_TIMESTAMP, birthday TEXT);

INSERT INTO dt2 VALUES (1, 'Alice', '1990-05-15', NULL);
INSERT INTO dt2 VALUES (2, 'Bob', '1985-12-03', NULL);

-- ═══════════════════════════════════════
-- Indizes
-- ═══════════════════════════════════════

-- (keine benutzerdefinierten Indizes)

-- ═══════════════════════════════════════
-- Views
-- ═══════════════════════════════════════

CREATE VIEW grosse_bestellungen AS SELECT * FROM bestellungen WHERE preis > 100;

-- Ende des Backups
