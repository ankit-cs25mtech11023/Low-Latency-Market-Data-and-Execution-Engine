# ITCH 5.0 and MoldUDP64

How Nasdaq publishes its order book, and how this project reads it. Sources: the public
specifications "Nasdaq TotalView-ITCH 5.0" and "MoldUDP64 Protocol Specification" on
nasdaqtrader.com.

## 1. What TotalView-ITCH is

ITCH is Nasdaq's **market-by-order** data feed: it reports every displayed order individually,
from the moment it is added to the book until it is executed, cancelled or replaced. A
consumer who applies every message in order can rebuild the exchange's complete visible order
book, order by order. (A *market-by-price* feed would only send the total quantity per price.)

The feed is **one-way and event-based**. It says what happened ("order 42 was executed for
100 shares"); it never says what the book looks like. Rebuilding the book is the consumer's job,
and a single missed message corrupts it until the end of the day. That is why gap detection
and recovery (MoldUDP64, below) matter.

## 2. Message format

Every message starts with the same 11-byte header. All integers are **big-endian** (most
significant byte first); the code reads them with `memcpy` + byte swap (`endian.hpp`), never
by casting the buffer to a struct (that would be undefined behaviour: unaligned access and a
strict-aliasing violation).

| offset | size | field | notes |
|---|---|---|---|
| 0 | 1 | message type | ASCII letter |
| 1 | 2 | stock locate | per-day integer id of the symbol (0 for system messages) |
| 3 | 2 | tracking number | Nasdaq-internal |
| 5 | 6 | timestamp | nanoseconds since midnight (48-bit integer) |

**Prices** are `Price(4)`: an unsigned 32-bit integer with 4 implied decimals, so `1234500`
means $123.45. They are never converted to floating point, so there is no rounding anywhere.

**Stock locate** codes are assigned per day by the `R` (stock directory) messages at the
start of the file. The engine indexes its books by locate directly (an array index), never by
the symbol string.

**Order reference numbers** are unique for the whole day across all symbols. Execute, cancel,
delete and replace messages carry only the reference, not the price or side, so a book must
keep a map from reference to order.

## 3. Messages that change the book

| type | name | length | effect on the book |
|---|---|---|---|
| `A` | Add Order | 36 | new order at the **back** of its price level's queue |
| `F` | Add Order with MPID | 40 | same as `A`, plus the market participant id |
| `E` | Order Executed | 31 | `shares` leave the book at the order's own price |
| `C` | Order Executed With Price | 36 | same effect on the book as `E`; the trade printed at a different price (e.g. in a cross) |
| `X` | Order Cancel | 23 | partial cancel: `shares` leave the book |
| `D` | Order Delete | 19 | the whole order leaves the book |
| `U` | Order Replace | 35 | old order removed, new reference/price/size added at the **back** of the queue (time priority lost, even at the same price) |

An order whose remaining size reaches 0 (through `E`, `C` or `X`) is gone.

Also decoded: `R` stock directory (locate → symbol, round lot), `S` system event (start/end of
messages, market hours), `H` trading action (halts). Everything else (`P`/`Q`/`B` trades,
`I` imbalance indications, `Y`, `L`, `V`, `W`, `K`, `J`, `h`, `N`, `O`) is passed to the handler as
"other": counted and skipped by its framed length. Trade messages (`P`) report executions
against hidden orders, which were never in the displayed book, so they do not change it.

The decoder checks each message's length against the spec length for its type **before
reading any field**, so a truncated or corrupt message can never cause an out-of-bounds read
(`tests/unit/test_itch.cpp` checks every type at every shorter length; a libFuzzer target
checks arbitrary input).

## 4. Book building is not matching

The exchange matches orders; the feed reports the results. A feed-side book therefore never
matches anything: it only applies the reported events. Consequences:

- A **crossed** (best bid > best ask) or **locked** (best bid = best ask) book can appear in
  the feed legitimately whenever the exchange is not matching that symbol. On the E1 day
  every one of the 989 crossed/locked tops had one of two causes
  ([E1](experiments/E1-workload.md#crossed-and-locked-books)):
  1. the symbol was halted (`H`), paused (`P`, e.g. a limit-up/limit-down pause) or
     quotation-only (`Q`), so orders rested without matching;
  2. the symbol had just resumed (`H` state `T`): the halt cross is printed first as one `Q`
     (cross trade) message, and the resting orders it filled are removed only afterwards by
     a burst of `C` (execute-with-price, non-printable) messages. For the few hundred µs
     in between, the feed-side book is still crossed.
  The book shows it as it is; the invariant checker counts it, it is not an error. A strategy
  must not trade a symbol on a crossed book (P6 stale-book guard).
- Matching logic exists in exactly one place in this project: the exchange emulator (P6).

Anomalies that should not happen on a clean, complete day file are counted, never fatal:
references to unknown orders, duplicate references, executions larger than the order
(overfill), locate mismatches, locates out of range, and zero-share adds.

## 5. File format

The daily sample files (`data/README.md`) are a sequence of
`[length: 2 bytes big-endian][message: length bytes]`, gzip-compressed. A full day is several
GB compressed. The reader (`itch_file.hpp`) streams it through one fixed 8 MiB buffer, so memory
use does not depend on the file size (the machine has 7.6 GiB of RAM). A file that ends in the
middle of a message, or a gzip stream that is truncated or corrupt, is an error, never a silent
early end of the day.

## 6. MoldUDP64 (implemented in Phase 5)

Live ITCH is sent as UDP multicast. UDP can lose, duplicate and reorder packets, so ITCH
messages are wrapped in **MoldUDP64** packets that carry sequence numbers:

| offset | size | field |
|---|---|---|
| 0 | 10 | session (ASCII) |
| 10 | 8 | sequence number of the first message in this packet |
| 18 | 2 | message count (0 = heartbeat; 0xFFFF = end of session) |
| 20 | … | message blocks: `[length: 2][message]`, repeated |

A receiver tracks the next expected sequence number. A packet that starts later reveals a
**gap**: the missing messages are requested from a re-request (retransmission) server with a
request packet (session, sequence number, count). Exchanges also send the same stream on two
independent multicast paths, **A and B**; a receiver takes each sequence number from
whichever path delivers it first and drops the duplicate (arbitration), so a loss on one path
alone causes no gap at all. While a gap is open, the books are stale and must not be traded
on. Phase 5 (E5) builds and measures all of this over a veth pair between network namespaces.
