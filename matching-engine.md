# Matching Engine

The exchange follows Price-Time Priority.

## Order Types

- Market
- Limit
- Cancel

## Matching Rules

1. Buy orders match against lowest ask.
2. Sell orders match against highest bid.
3. Better price gets priority.
4. Same price → Earlier order gets priority.
5. Partial fills are supported.
6. Remaining quantity is stored in the order book.

## Complexity

Insertion: O(log n)

Best Bid: O(1)

Best Ask: O(1)

Matching: O(k)

where k is the number of crossed price levels.
