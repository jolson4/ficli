# ficli TODO

Incomplete roadmap items moved from `agents.md`.

- [ ] Allow user to create an account when adding a new loan profile
- [ ] Add an "Assets" view to track physical assets
- [ ] Add a "Loans" view ([plan](../.claude/plans/loans-view-v1.md))
- [ ] Allow user to say if a category is an "Expense" or "Income" category
- [ ] Don't show "Income" in Credit Card account headers
- [ ] Make edit Category popout larger
- [ ] Support adding a budget for a hidden category in budgets view (since we currently only show categories matching transactions in the current month)
- [ ] Add CLI arguments for quick actions (e.g., `ficli import -account "CapitalOne" -file ~/Downloads/transactions.csv`) or `ficli import` which then queries for accounts and asks which file in ~/Downloads relates to each account (allowing skipping that account if no file matches)
- [ ] Allow annual budget for categories with monthly breakdowns
- [ ] Automatically convert transaction to transfer when importing a transaction and there's a matching transaction in another account
- [ ] Add investment purchases/sales with cost basis tracking
- [ ] Allow user to send set of selected transactions to LLM for auto-categorization
- [ ] Allow user to choose when to save changes; don't persist anything until they save
- [ ] Offer to auto-create accounts when importing transactions with an account that doesn't exist yet
- [ ] Delegate import logic across CPU cores for perf
- [ ] Prevent keyboard events from hitting UI behind the keyboard shortcut popout
- [ ] Allow filtering transactions list using regex
- [ ] Allow archiving accounts
- [ ] Support CSV imports for investment accounts
- [ ] Data export (CSV)
- [ ] Support split transactions
- [ ] Add row indices to transaction list
- [ ] Add undo logic
- [ ] When deleting a category, offer to reassign transactions to another category
- [ ] Add reconciliation
- [ ] Add password protection and encryption
- [ ] Allow user to choose whether to delete linked transfer transactions when deleting a transaction
- [ ] Add error UI for handling logic mismatches, e.g. transfers with more than 2 transactions
