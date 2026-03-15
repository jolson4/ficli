# Loans View (v1)

## Summary

Add a dedicated `Loans` screen that supports both car loans and mortgages using a
single loan account per loan. Track loan metadata (start date, rate, initial
amount, scheduled payment) and show transaction history plus a computed
"phantom" next payment row that can be enacted into a real transaction.

Use category-based breakdown for mortgage payments (`Principal`, `Interest`,
`Escrow`) instead of separate escrow/paydown accounts for v1.

## Scope

- Support loan profile setup for existing accounts.
- Support loan kinds: `CAR` and `MORTGAGE`.
- Show historical loan-account transactions.
- Show one computed next-payment phantom row.
- Allow user to enact the phantom row into a real transaction.
- Do not model asset linkage yet.
- Do not add separate escrow account flows yet.

## Data Model

1. Add table `loan_profiles`:
   - `id INTEGER PRIMARY KEY AUTOINCREMENT`
   - `account_id INTEGER NOT NULL UNIQUE`
   - `loan_kind TEXT NOT NULL CHECK(loan_kind IN ('CAR','MORTGAGE'))`
   - `start_date TEXT NOT NULL` (`YYYY-MM-DD`)
   - `interest_rate_bps INTEGER NOT NULL` (e.g. 6.5% => 650)
   - `initial_principal_cents INTEGER NOT NULL`
   - `scheduled_payment_cents INTEGER NOT NULL`
   - `payment_day INTEGER NOT NULL DEFAULT 1` (1-28)
   - `created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP`
   - `updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP`
   - FK on `account_id` -> `accounts(id)` with `ON DELETE CASCADE`
2. Add index on `loan_profiles(account_id)` (redundant with UNIQUE but explicit
   for clarity in codebase conventions).
3. Migration strategy:
   - Add `CREATE TABLE IF NOT EXISTS loan_profiles ...` in schema creation.
   - Add equivalent create/index guards in migrate path for existing DBs.

## Query/API Layer

Add `db/query` APIs and structs:

- `loan_profile_t` struct for profile fields.
- `loan_view_row_t` struct for rendered rows (real + phantom marker).
- CRUD helpers:
  - `db_get_loan_profiles(...)`
  - `db_get_loan_profile_by_account(...)`
  - `db_upsert_loan_profile(...)`
  - `db_delete_loan_profile(...)`
- Payment helpers:
  - `db_get_next_loan_payment_date(...)`
  - `db_enact_loan_payment(...)`

Implementation notes:

- Next payment date is derived from the latest payment-like transaction in the
  loan account; if none, anchor on `start_date` and `payment_day`.
- Keep date logic deterministic and month-safe by clamping `payment_day` to 28.
- Enacted payment inserts an `EXPENSE` transaction into the loan account using
  the profile amount/date and default labels (`payee='Loan Payment'`,
  description includes loan kind).

## Category Strategy (v1)

For mortgages, keep one loan account and use categories to classify components:

- `Mortgage:Principal`
- `Mortgage:Interest`
- `Mortgage:Escrow`

For car loans, typical categories:

- `Auto Loan:Principal`
- `Auto Loan:Interest`

Notes:

- Do not require these categories for v1 to function; user can apply manually.
- Optionally auto-create missing categories when first opening Loans screen or
  when enacting first payment (deferred if we want stricter UX).

## UI Design

1. Add `SCREEN_LOANS` in `include/ui/screens.def` and wire in `src/ui/ui.c`.
2. New module:
   - `include/ui/loan_list.h`
   - `src/ui/loan_list.c`
3. Loans screen layout:
   - Top: selected loan profile summary (account, kind, rate, original amount,
     scheduled payment, next due).
   - Main table: transaction history for selected loan account.
   - One highlighted phantom row pinned near top (or inserted by date in table)
     with marker like `[Next Payment]`.
4. Keybindings:
   - `n`: add/setup profile for account
   - `e`: edit selected profile
   - `d`: delete profile
   - `Enter` on phantom row: enact payment (with confirm)
   - `1-9`: switch account/profile (consistent with transactions view)

## Interaction Rules

- Enact confirmation includes amount/date/account summary.
- After enact success:
  - reload rows
  - recalculate next phantom row
  - keep cursor on newly created row when practical
- If no profile exists:
  - show empty-state instructions and `n:Add loan profile` action.

## Validation and Error Handling

- Validate date format and logical ranges in form:
  - principal > 0
  - scheduled payment > 0
  - interest rate >= 0
  - payment day in [1, 28]
- Show shared error popup for DB failures/conflicts.
- Prevent duplicate profile per account (`UNIQUE(account_id)`).

## Integration Touchpoints

- Mark Loans view dirty after transaction/account/category mutations where other
  views are already dirtied.
- Keep feature isolated from import, budgets, and reports in v1.

## Out of Scope (v1)

- Asset linkage and combined net-worth treatment.
- Separate escrow account with transfers/disbursement tracking.
- Auto-splitting one payment into principal/interest/escrow sub-transactions.
- Amortization schedule generation and payoff projection.

## Follow-ups (v2+)

1. Add split-transaction support and map scheduled mortgage payment into
   principal/interest/escrow lines.
2. Optional dedicated escrow account migration path.
3. Add amortization analytics and payoff timeline.
