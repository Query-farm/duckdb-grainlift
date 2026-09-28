-- Sample data for the `postgres` grainlift test target (scripts/test-server.sh).
CREATE SCHEMA sales;

CREATE TABLE sales.customers (
    id          integer PRIMARY KEY,
    name        text NOT NULL,
    email       text,
    country     text,
    signed_up   date,
    vip         boolean DEFAULT false
);

INSERT INTO sales.customers VALUES
    (1, 'Ada Lovelace',     'ada@example.com',     'United Kingdom', '2024-01-15', true),
    (2, 'Grace Hopper',     'grace@example.com',   'United States',  '2024-02-20', true),
    (3, 'Alan Turing',      'alan@example.com',    'United Kingdom', '2024-03-05', false),
    (4, 'Katherine Johnson', NULL,                 'United States',  '2024-04-11', false),
    (5, 'Edsger Dijkstra',  'edsger@example.com',  'Netherlands',    '2024-05-30', false),
    (6, 'Hedy Lamarr',      'hedy@example.com',    'Austria',        '2024-06-18', true);

CREATE TABLE sales.orders (
    id           bigint PRIMARY KEY,
    customer_id  integer REFERENCES sales.customers (id),
    amount       numeric(12, 2) NOT NULL,
    ordered_at   timestamptz NOT NULL,
    status       text NOT NULL,
    weight_kg    double precision
);

-- 50,000 deterministic orders spread over 2025
INSERT INTO sales.orders
SELECT i,
       1 + (i % 6),
       round(((i * 7919) % 100000) / 100.0, 2),
       timestamptz '2025-01-01 00:00:00+00' + (i % 365) * interval '1 day' + (i % 86400) * interval '1 second',
       (ARRAY['new', 'paid', 'shipped', 'cancelled'])[1 + (i % 4)],
       CASE WHEN i % 10 = 0 THEN NULL ELSE (i % 500) / 10.0 END
FROM generate_series(1, 50000) AS i;

CREATE VIEW sales.revenue_by_country AS
SELECT c.country, count(*) AS orders, sum(o.amount) AS revenue
FROM sales.orders o JOIN sales.customers c ON c.id = o.customer_id
WHERE o.status <> 'cancelled'
GROUP BY c.country;

-- A public-schema table for simple scans
CREATE TABLE measurements (
    sensor   text,
    taken_at timestamp,
    reading  real,
    ok       boolean
);
INSERT INTO measurements
SELECT 'sensor-' || (i % 5), timestamp '2025-06-01' + i * interval '1 minute', (i % 100) / 4.0, i % 13 <> 0
FROM generate_series(1, 2000) AS i;
