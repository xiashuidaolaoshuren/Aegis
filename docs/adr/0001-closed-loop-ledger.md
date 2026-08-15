# Aegis is a closed-loop ledger

A real switch does not hold cardholder money; an issuer does. Aegis is a learning system, so it owns Cardholder and Merchant wallets itself. `issuersim` models latency, timeouts, and injected `05` only — never balances. Response `51` comes from Aegis Available, not from an issuer.

The alternative (acquirer-only ledger plus a second issuer book) would split the source of truth and fight the “one shadow model” test strategy. The alternative (both checks real) would require a protocol for disagreement. We rejected both so money has one owner.
