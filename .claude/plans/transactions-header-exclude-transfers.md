# Exclude Transfers From Transactions Header MTD Metrics

## Goal
Ensure Transactions view month-to-date header metrics do not treat transfers as
income or expenses.

## Plan
1. Update MTD net SQL to only include standalone income and expense rows.
2. Exclude rows linked by `transfer_id` from MTD income and MTD expense SQL.
3. Keep account balance behavior unchanged so transfers still affect balances.

## Notes
- This also protects legacy transfer rows stored as income/expense with a
  `transfer_id` link.
