# Capture is always cross-wallet two-phase

Authorization is intra-wallet (Available → Holds on one Cardholder). Capture always moves that Hold onto a Merchant Payable bucket and Interchange, so Stage 3 partitioning by wallet makes every capture a cross-partition posting.

We accept two-phase commit (or equivalent coordination) on the capture path rather than posting capture on a single writer or deferring 2PC. Reversal and expiry stay single-wallet. Partial capture is out of scope so a capture either consumes the whole Hold or it does not run.
