PRAGMA foreign_keys = ON;
PRAGMA defer_foreign_keys = ON;

BEGIN IMMEDIATE;

-- 1) Build a map of INCOME categories that already have an EXPENSE counterpart
-- by normalized name and mirrored parent path.
CREATE TEMP TABLE income_expense_map (
    income_id INTEGER PRIMARY KEY,
    expense_id INTEGER NOT NULL
);

INSERT INTO income_expense_map (income_id, expense_id)
WITH RECURSIVE matched(income_id, expense_id) AS (
    SELECT
        i.id AS income_id,
        (
            SELECT e.id
            FROM categories e
            WHERE e.type = 'EXPENSE'
              AND e.parent_id IS NULL
              AND lower(trim(e.name)) = lower(trim(i.name))
            ORDER BY e.id
            LIMIT 1
        ) AS expense_id
    FROM categories i
    WHERE i.type = 'INCOME'
      AND i.parent_id IS NULL

    UNION ALL

    SELECT
        ci.id AS income_id,
        (
            SELECT ce.id
            FROM categories ce
            WHERE ce.type = 'EXPENSE'
              AND ce.parent_id = matched.expense_id
              AND lower(trim(ce.name)) = lower(trim(ci.name))
            ORDER BY ce.id
            LIMIT 1
        ) AS expense_id
    FROM categories ci
    JOIN matched ON ci.parent_id = matched.income_id
    WHERE ci.type = 'INCOME'
)
SELECT m.income_id, m.expense_id
FROM matched m
JOIN categories c ON c.id = m.income_id
WHERE m.expense_id IS NOT NULL
  AND lower(trim(c.name)) <> 'salary';

-- 2) Reassign transactional/budget references from mapped INCOME categories.
UPDATE transactions
SET category_id = (
    SELECT m.expense_id
    FROM income_expense_map m
    WHERE m.income_id = transactions.category_id
)
WHERE category_id IN (SELECT income_id FROM income_expense_map);

INSERT INTO budgets (category_id, month, limit_cents)
SELECT m.expense_id, b.month, b.limit_cents
FROM budgets b
JOIN income_expense_map m ON m.income_id = b.category_id
ON CONFLICT(category_id, month)
DO UPDATE SET limit_cents = excluded.limit_cents;

DELETE FROM budgets
WHERE category_id IN (SELECT income_id FROM income_expense_map);

DELETE FROM budget_category_filters
WHERE category_id IN (SELECT income_id FROM income_expense_map);

-- If an unmatched child still points at a mapped INCOME parent,
-- move it under the corresponding EXPENSE parent before delete.
UPDATE categories
SET parent_id = (
    SELECT m.expense_id
    FROM income_expense_map m
    WHERE m.income_id = categories.parent_id
)
WHERE parent_id IN (SELECT income_id FROM income_expense_map)
  AND id NOT IN (SELECT income_id FROM income_expense_map);

-- 3) Delete mapped INCOME categories (except Salary, excluded above).
DELETE FROM categories
WHERE id IN (SELECT income_id FROM income_expense_map);

-- Ensure one canonical INCOME Salary category exists.
INSERT INTO categories (name, type, parent_id)
SELECT 'Salary', 'INCOME', NULL
WHERE NOT EXISTS (
    SELECT 1
    FROM categories
    WHERE type = 'INCOME'
      AND lower(trim(name)) = 'salary'
);

CREATE TEMP TABLE canonical_salary (
    id INTEGER PRIMARY KEY
);

INSERT INTO canonical_salary (id)
SELECT id
FROM categories
WHERE type = 'INCOME'
  AND lower(trim(name)) = 'salary'
ORDER BY id
LIMIT 1;

-- Repoint any extra INCOME Salary categories to canonical, then delete extras.
UPDATE transactions
SET category_id = (SELECT id FROM canonical_salary)
WHERE category_id IN (
    SELECT id
    FROM categories
    WHERE type = 'INCOME'
      AND lower(trim(name)) = 'salary'
      AND id <> (SELECT id FROM canonical_salary)
);

INSERT INTO budgets (category_id, month, limit_cents)
SELECT (SELECT id FROM canonical_salary), b.month, b.limit_cents
FROM budgets b
WHERE b.category_id IN (
    SELECT id
    FROM categories
    WHERE type = 'INCOME'
      AND lower(trim(name)) = 'salary'
      AND id <> (SELECT id FROM canonical_salary)
)
ON CONFLICT(category_id, month)
DO UPDATE SET limit_cents = excluded.limit_cents;

DELETE FROM budgets
WHERE category_id IN (
    SELECT id
    FROM categories
    WHERE type = 'INCOME'
      AND lower(trim(name)) = 'salary'
      AND id <> (SELECT id FROM canonical_salary)
);

DELETE FROM categories
WHERE type = 'INCOME'
  AND lower(trim(name)) = 'salary'
  AND id <> (SELECT id FROM canonical_salary);

-- 4) Any remaining INCOME category besides canonical Salary becomes EXPENSE.
UPDATE categories
SET type = 'EXPENSE'
WHERE type = 'INCOME'
  AND id <> (SELECT id FROM canonical_salary);

COMMIT;
