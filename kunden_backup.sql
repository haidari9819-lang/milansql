-- MilanSQL Tabellen-Backup
-- Tabelle: kunden
-- Erstellt: 2026-05-28 09:45:16

DROP TABLE IF EXISTS kunden;
CREATE TABLE kunden (id INT PRIMARY KEY AUTO_INCREMENT, name TEXT NOT NULL, email TEXT);

INSERT INTO kunden VALUES (1, 'Alice', 'alice@mail.com');
INSERT INTO kunden VALUES (2, 'Bob', 'bob@mail.com');

CREATE INDEX idx_kunden_name ON kunden (name);
