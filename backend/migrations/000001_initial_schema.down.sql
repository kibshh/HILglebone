-- Reverse of 000001_initial_schema.up.sql. Drop order is constrained by Foreign Keys.

DROP TABLE IF EXISTS sessions;
DROP TABLE IF EXISTS firmware_uploads;
DROP TABLE IF EXISTS dut_devices;
DROP TABLE IF EXISTS bbb_devices;
DROP TABLE IF EXISTS users;
