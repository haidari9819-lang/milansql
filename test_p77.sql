SET PARALLEL_THRESHOLD = 5
SET MAX_PARALLEL_WORKERS = 4
CREATE TABLE parallel_test (id INT PRIMARY KEY AUTO_INCREMENT, wert INT, name TEXT)
INSERT INTO parallel_test VALUES (NULL, 10, Alpha)
INSERT INTO parallel_test VALUES (NULL, 20, Beta)
INSERT INTO parallel_test VALUES (NULL, 30, Gamma)
INSERT INTO parallel_test VALUES (NULL, 40, Delta)
INSERT INTO parallel_test VALUES (NULL, 50, Epsilon)
INSERT INTO parallel_test VALUES (NULL, 60, Zeta)
INSERT INTO parallel_test VALUES (NULL, 70, Eta)
INSERT INTO parallel_test VALUES (NULL, 80, Theta)
INSERT INTO parallel_test VALUES (NULL, 90, Iota)
INSERT INTO parallel_test VALUES (NULL, 100, Kappa)
SELECT COUNT(*) FROM parallel_test
SELECT * FROM parallel_test WHERE wert > 50
SELECT SUM(wert) FROM parallel_test
SHOW PARALLEL STATUS
EXPLAIN SELECT * FROM parallel_test WHERE wert > 30
EXIT
