# MilanSQL — bekannte Fehler (Audit)

Referenzdokument für `checkKnownFailure()` (`src/tests/milansql_tests.cpp`) und für
die HTTP-API-Tests (`scripts/http_api_test.py`, Teststrategie Stufe 4). Jeder Eintrag
hier entspricht mindestens einem `[XFAIL]` in einem der beiden Testläufe — wird ein
Fehler behoben, schlägt der zugehörige Check als "unerwarteter Erfolg" an, und der
Eintrag hier sollte entfernt und der Test zu einem normalen `check()` zurückgewandelt
werden.

## C++ ctest-Suite (`milansql_tests`)

| # | Test | Symptom |
|---|---|---|
| 1 | Empty string comparison works | `WHERE spalte = ''` findet einen gespeicherten Leerstring nicht — Vergleichslogik behandelt `''` im WHERE-Prädikat als Sonderfall statt als normalen String-Literal-Vergleich. |
| 2 | Phase165 #5: '' stored and found by WHERE s = '' | Dieselbe Leerstring-Lücke, zweite Variante/Testgruppe. |
| 3 | first() aggregate -> no error | `FIRST(col, orderCol)` wirft einen Fehler statt ein Ergebnis zurückzugeben. |
| 4 | last() aggregate -> no error | Gleiche Ursache wie #3, gespiegelt für `LAST()`. |
| 5 | AUD-WAL #9a: newline-value tx recovered as one row | Eine Transaktion mit einem mehrzeiligen Wert (enthält `\n`) wird beim WAL-Replay nach einem Crash falsch wieder zusammengesetzt. |
| 6 | AUD-WAL #9b: multi-line value survives crash recovery intact | Gleiche Ursache wie #5, prüft den Zellwert nach Recovery statt die Tx-Zählung. |

**Stand:** `ctest`/`milansql_tests` auf `release`: 2047 bestanden, 0 unerwartet
fehlgeschlagen, diese 6 laufen als `[XFAIL]`.

## HTTP-API-Suite (`scripts/http_api_test.py`, Stufe 4)

Gefunden beim Aufbau der ersten Testsuite, die MilanSQL über echte HTTP-Requests statt
in-process gegen die Engine testet (siehe Skript-Docstring). Alle vier sind
Korrektheits-/Robustheitslücken, keine Sicherheitslücken (Tenant-Isolation,
root/root-Ablehnung, Fail-Closed-Start und parametrisierte Queries gegen
SQL-Injection laufen alle grün, siehe Skript).

| # | Test | Symptom |
|---|---|---|
| 7 | unknown column in WHERE on an EMPTY table | `SELECT * FROM t WHERE unbekannte_spalte = x` auf einer Tabelle ohne Zeilen liefert `success:true` mit 0 Zeilen statt eines Fehlers — die Spaltenauflösung passiert nur beim Scannen einzelner Zeilen; eine leere Tabelle scannt nie eine Zeile, der Fehler wird daher nie ausgelöst. |
| 8 | unknown column in WHERE on a table WITH rows (Status Code) | Sobald die Tabelle Zeilen hat, wird der Fehler korrekt erkannt (`success:false`, Spaltenname in der Fehlermeldung) — der HTTP-Statuscode bleibt dabei aber `200` statt eines `4xx`, anders als bei anderen Client-Fehlern (z. B. fehlendes `sql`-Feld → `400`). |
| 9 | read-after-write: HTTP query cache staleness | Wird exakt derselbe `SELECT`-Text (z. B. `SELECT COUNT(*) FROM t`) einmal vor und einmal nach einem `INSERT` in dieselbe Tabelle ausgeführt, liefert der zweite Aufruf das gecachte Ergebnis von vorher statt den aktuellen Stand. Reproduzierbar minimal: `CREATE TABLE t (id INT)` → `SELECT COUNT(*) FROM t` (liefert 0) → `INSERT INTO t VALUES (1)` → identisches `SELECT COUNT(*) FROM t` erneut (liefert weiterhin 0 statt 1). |
| 10 | unparseable SQL text reports success:true | Ein nicht erkennbarer/kaputter SQL-Text (z. B. ungültiges JSON, das als rohes SQL interpretiert wird) liefert `{"success":true,"message":"Tippe 'help' ..."}` statt eines Fehlers — die HTTP-Schicht entscheidet `success` durch Scannen der von der Engine auf stdout ausgegebenen Textzeile nach Schlüsselwörtern wie `FEHLER`; ein unbekannter Befehl enthält dieses Schlüsselwort nicht. |

**Stand:** `scripts/http_api_test.py` gegen `release` (`70b9762`): alle regulären Checks
grün, diese 4 laufen als `[XFAIL]`.
