DROP TABLE IF EXISTS test.drop_column;
CREATE TABLE test.drop_column (d Date, num Int64, str String) ENGINE = MergeTree(d, d, 8192);

INSERT INTO test.drop_column VALUES ('2016-12-12', 1, 'a'), ('2016-11-12', 2, 'b');

SELECT num, str FROM test.drop_column ORDER BY num;
ALTER TABLE test.drop_column DROP COLUMN num FROM PARTITION '201612';
SELECT num, str FROM test.drop_column ORDER BY num;

DROP TABLE test.drop_column;

-- Replicated case

DROP TABLE IF EXISTS test.drop_column1;
DROP TABLE IF EXISTS test.drop_column2;
CREATE TABLE test.drop_column1 (d Date, i Int64, s String) ENGINE = ReplicatedMergeTree('/clickhouse/tables/test/drop_column', '1', d, d, 8192);
CREATE TABLE test.drop_column2 (d Date, i Int64, s String) ENGINE = ReplicatedMergeTree('/clickhouse/tables/test/drop_column', '2', d, d, 8192);

INSERT INTO test.drop_column1 VALUES ('2000-01-01', 1, 'a'), ('2000-02-01', 2, 'b');
INSERT INTO test.drop_column1 VALUES ('2000-01-01', 3, 'c'), ('2000-02-01', 4, 'd');

SELECT 'all';
SELECT * FROM test.drop_column1 ORDER BY d, i, s;

SET replication_alter_partitions_sync=2;

SELECT 'w/o i 1';
ALTER TABLE test.drop_column1 DROP COLUMN i FROM PARTITION '200001';
SELECT * FROM test.drop_column2 ORDER BY d, i, s;

SELECT 'w/o is 1';
ALTER TABLE test.drop_column1 DROP COLUMN s FROM PARTITION '200001';
SELECT * FROM test.drop_column2 ORDER BY d, i, s;

SELECT 'w/o is 12';
ALTER TABLE test.drop_column1 DROP COLUMN i FROM PARTITION '200002';
ALTER TABLE test.drop_column1 DROP COLUMN s FROM PARTITION '200002';
SELECT DISTINCT * FROM test.drop_column2 ORDER BY d, i, s;
SELECT DISTINCT * FROM test.drop_column2 ORDER BY d, i, s;

SELECT 'sizes';
SELECT sum(data_uncompressed_bytes) FROM system.columns WHERE database='test' AND table LIKE 'drop_column_' AND (name = 'i' OR name = 's') GROUP BY table;

-- double call should be OK
ALTER TABLE test.drop_column1 DROP COLUMN s FROM PARTITION '200001';
ALTER TABLE test.drop_column1 DROP COLUMN s FROM PARTITION '200002';

DROP TABLE IF EXISTS test.drop_column1;
DROP TABLE IF EXISTS test.drop_column2;
