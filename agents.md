# ficli - Agent Session Guide

This file is the fast, accurate starting point for future coding sessions.

## Start Here
- Read `agents.md` first, then open `TODO.md` for active roadmap work.
- Treat `CODEBASE.md` as supplemental context. It is useful, but can lag behind current implementation details.
- Build quickly with `make` and run with `./ficli` (or `make run`).

## What ficli Is
- A local-first terminal personal finance app in C using `ncursesw` + SQLite/SQLCipher.
- Single binary (`ficli`) with no network services.
- Core domain: accounts, categories, transactions, transfers, budgets, reports, loans, and imports.

## Current Runtime Facts
- DB file: `~/.local/share/ficli/ficli.db`.
- DB encryption key required at startup (`PRAGMA key`). Build prefers `sqlcipher` via pkg-config and falls back to `sqlite3` when unavailable.
- Key lookup order in `src/main.c`: 1Password CLI (`op read ...`), saved key file, then UI password prompt.
- Saved key path defaults to `~/.config/ficli/db.key` (or `XDG_CONFIG_HOME/ficli/db.key`), overrideable with `FICLI_DB_KEY_FILE`.
- Theme preference stored in `~/.config/ficli/config.ini` as `theme=dark|light`.

## Main Code Map
- `src/main.c`: startup path resolution, key retrieval/prompt flow, DB/UI lifecycle.
- `src/db/db.c`: schema creation, migrations, default seed data, encryption/PRAGMA setup.
- `src/db/query.c` + `include/db/query.h`: all domain queries and mutations (accounts, categories, transactions, transfers, budgets, reports, loans, splits, filters).
- `src/ui/ui.c`: ncurses shell, screen routing, global key handling, focus model, popups.
- `src/ui/*.c`: screen modules (`dashboard_list`, `txn_list`, `account_list`, `loan_list`, `category_list`, `budget_list`, `report_list`) plus forms/import/error UI.
- `src/csv/csv_import.c`: CSV/QIF parsing and import mapping.

## UI Screens (Implemented)
- Dashboard, Transactions, Accounts, Loans, Categories, Budgets, Reports (`include/ui/screens.def`).
- Global shortcuts include add transaction (`a`), import (`i`), auto-link transfers (`L`), theme toggle (`t`), and help (`?`).
- Transactions view supports per-account tabs, sorting/filtering, multi-select edits, quick category edit, and transfer workflows.
- Budgets and Reports both support drill-down into matching transactions and in-place edits from detail rows.

## Data Model Highlights
- Amounts stored as integer cents.
- Effective/reporting date is `COALESCE(reflection_date, date)`.
- Accounts include types: CASH, CHECKING, SAVINGS, CREDIT_CARD, PHYSICAL_ASSET, INVESTMENT, LOAN.
- Loan support includes loan profiles and payment/split bookkeeping (`loan_profiles`, `transaction_splits`).
- Budget behavior includes effective-month rules + one-month overrides + category include/exclude filters.

## Working Conventions
- Keep changes small and module-local where possible.
- Maintain existing C style (snake_case, explicit bounds handling, early returns).
- Prefer updating query-layer helpers over embedding SQL in UI modules.
- If changing schema or query semantics, update both `src/db/db.c` and `include/db/query.h`/`src/db/query.c` coherently.

## Active Work Tracking
- All incomplete roadmap items live in `TODO.md`.
- Do not re-add completed status history here; keep this file focused on onboarding and current architecture reality.
