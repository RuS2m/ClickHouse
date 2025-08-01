-- Tags: stateful, no-fasttest

DROP TABLE IF EXISTS avro;

SET max_threads = 1, max_insert_threads = 0, max_block_size = 8192, min_insert_block_size_rows = 8192, min_insert_block_size_bytes = 1048576; -- lower memory usage

CREATE TABLE avro AS test.hits ENGINE = File(Avro);
INSERT INTO avro SELECT * FROM test.hits LIMIT 10000;

SELECT sum(cityHash64(*)) FROM (SELECT * FROM test.hits LIMIT 10000);
SELECT sum(cityHash64(*)) FROM avro;

DROP TABLE avro;
