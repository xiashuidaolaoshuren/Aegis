# Aegis

Closed-loop card authorization: Aegis is the only ledger. Terminals speak ISO 8583; an issuer simulator is a fake network, not a second set of balances.

## Language

**Aegis**:
The authorization switch and the sole book of record for money in this system.
_Avoid_: network switch, acquirer-only switch, processor (when those imply Aegis does not hold cardholder funds)

**Wallet**:
The owner of named balances. A Cardholder wallet is keyed by PAN; a Merchant wallet by MerchantId; Interchange lives on a System wallet.
_Avoid_: Account (ambiguous), ledger account, posting target

**AccountId**:
The identifier of a Wallet.
_Avoid_: using AccountId for a single bucket or posting line

**Bucket**:
A named balance on a Wallet: Cardholder Available and Holds, Merchant Payable, System Interchange.
_Avoid_: account, sub-account, ledger line

**Available**:
The Cardholder bucket that may be spent. An authorization that would drive it negative is declined with response code 51, and no hold is posted.
_Avoid_: balance (unqualified), ledger balance minus holds

**Hold**:
A reservation of Cardholder funds: debit Available, credit Holds. Exists only between a successful funds check and capture, reversal, or expiry.
_Avoid_: authorization (the message/decision), capture, settlement

**Authorization**:
An ISO `0100`/`0110` attempt to reserve funds. Intra-wallet: it only moves Available and Holds on one Cardholder.
_Avoid_: capture, transaction (unqualified)

**Capture**:
An ISO `0200`/`0210` that consumes a live Hold in full and pays the Merchant (minus fee) plus Interchange. Always cross-wallet. Field 4 must equal the original authorization amount.
_Avoid_: partial capture, settlement, financial advice (`0220`)

**Reversal**:
An ISO `0400`/`0410` that releases a live Hold back to Available. Valid only while the Hold exists. Not used after Capture.
_Avoid_: refund, chargeback, unwind

**Expiry**:
Release of a live Hold because its TTL elapsed (sweeper) or an operator injected expire-now. Same money movement as Reversal.
_Avoid_: timeout (timeout is the issuer-wait failing; expiry is the hold clock)

**Screening**:
Hard local checks before a Hold is posted: currency matches the wallet, amount greater than zero, optional maximum amount. Failure does not post a Hold.
_Avoid_: fraud engine, risk scoring, issuer decline

**Issuer simulator**:
A fake network with configurable latency, timeouts, and injected response code 05. It has no balances.
_Avoid_: issuer, issuing bank (as a second ledger)

**Idempotency key**:
TerminalId, STAN, and field 7 date (MMDD). Capture and reversal find the original authorization via field 90; those messages are themselves idempotent on their own key.
_Avoid_: RRN as the retry key, TerminalId+STAN without date

**Genesis**:
Opening Available balances loaded from a fixture when the process starts. There is no ISO funding message.
_Avoid_: deposit, top-up, credit message

**Settlement**:
Discharging Merchant Payable after Capture. Stretch (M6) only.
_Avoid_: capture (capture creates Payable; it does not pay the merchant out)
