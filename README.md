# CME-Market-Maker-C

The order-flow and toxicity aware CME futures market maker of
[CME-Futures-Spread-Harvester](https://github.com/vivere7108-lab/CME-Futures-Spread-Harvester),
translated from Python into C++20 for the live path. Same book, same
signals, same Avellaneda–Stoikov quotes, same throttled execution, same
risk layer, same journal, same configuration files, same command line —
and the same numbers: on the generated tape both implementations produce
identical fills, P&L, message counts and markouts to the cent, which is
how the translation was checked.

```
Databento GLBX.MDP3 (mbo | mbp-10) ──▶ book ──▶ signals ──▶ quoting ──▶ execution ──▶ IBKR
                                       │           │            │            │
                                  microprice   VPIN gate   reservation   token bucket
                                  the tape     OFI          + spread      priorities
                                  (aggressor   depletion    × toxicity    coalescing
                                   flagged)    run          + skew        dead-band
                                                            behind-best   cancel-replace
```

**What it does not do.** It does not compete for the touch. A quote here is
placed at least `behind_best_ticks` behind the best bid or ask and fills
when that level is swept — which on a one-tick-wide book like ES means it
fills *only* when the inside is swept. Every fill is taken from an
aggressor who just cleared the level in front, and the question the system
is built to answer is whether it can tell, before the sweep, that one is
coming. The markouts by toxicity level in the journal are that answer.

## Why C++

The Python system spent its time in the interpreter: decoding each
Databento record into a Python object, dictionary operations per book
update, a heap sort per snapshot, and an asyncio event loop between a
decision and the socket. None of that is the strategy. Here the book is a
flat sorted vector per side, a record is applied in about a hundred
nanoseconds, a snapshot is a copy of the top levels, and a whole decision
cycle — signals, risk, engine, reconciliation, throttle, simulated fills —
runs in about a microsecond. On this box (`harvester_bench`, one core of
a shared VM, the generated tape):

| stage | mean | p50 | p99 |
|---|---:|---:|---:|
| MBP-10 record into the book | 164 ns | 149 ns | 351 ns |
| MBO record into the book | 107 ns | 86 ns | 308 ns |
| record through the feed's lock | 204 ns | 186 ns | 384 ns |
| locked snapshot of ten levels a side | 99 ns | 94 ns | 120 ns |
| one Avellaneda–Stoikov decision | 127 ns | 143 ns | 193 ns |
| one whole decision cycle (`Pipeline::step`) | 1.3 µs | 1.1 µs | 3.5 µs |

The 30-minute generated replay that takes the Python system 5.1 s runs in
0.07 s here; a 24-hour generated tape replays in under a second. Live, the
decision loop runs on a steady-clock timer at `live.decision_interval_ms`
(the default 100 ms is the cadence the replays were measured at; the
throttle, the dead-band and IBKR's message ceiling bound the message rate
whatever it is set to), the Databento client decodes records into the
book on its own thread, IBKR's events land on their own thread, and the
runner's thread never blocks on either.

## Quick start

```bash
sudo apt install build-essential cmake ninja-build libyaml-cpp-dev libssl-dev libzstd-dev
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build

# The whole pipeline on a generated market -- no keys, no connections.
build/harvester replay -c configs/es_replay.yaml

# The same tape with the gate off and no skew, and the difference.
build/harvester replay -c configs/es_replay.yaml --control

# A recorded ES tape from Databento's historical API, then replay it.
export DATABENTO_API_KEY=db-...
build/harvester fetch  -c configs/es_replay.yaml --start 2026-09-08T13:30 --end 2026-09-08T20:00 -o data_cache/es.dbn.zst
build/harvester replay -c configs/es_replay.yaml --dbn data_cache/es.dbn.zst --control

# Preflight the live path: the feed, the router, the account, the budget.
build/harvester doctor -c configs/es_paper.yaml

# The forward walk. --dry-run reads the real book and simulates the fills;
# it needs a Databento key and nothing else -- or set ibkr.market_data and
# it reads the book from IBKR's CME depth instead, with its caveats below.
build/harvester live -c configs/es_paper.yaml --dry-run

# Read back what a session did.
build/harvester report -c configs/es_paper.yaml

# The tests and the benchmark.
build/harvester_tests
build/harvester_bench
```

The configs are the Python system's, unchanged: `configs/es_paper.yaml`
is the MES paper walk, `configs/es_replay.yaml` the replay. A config
written for one runs on the other, and the journals (`runs/*/*.jsonl`) are
byte-compatible, so `report` here reads a Python session and vice versa.

To run it unattended on a VPS see **[deploy/README.md](deploy/README.md)**.

## Building

| option | default | needs | gives |
|---|---|---|---|
| (core) | — | C++20 compiler, CMake ≥ 3.20, yaml-cpp | `replay` on the generated market, `report`, `config`, the tests, the benchmark |
| `HARVESTER_WITH_DATABENTO` | ON | [databento-cpp](https://github.com/databento/databento-cpp) (fetched by CMake if not installed; needs OpenSSL and zstd headers) | the live feed, `--dbn` replay, `fetch`, the feed half of `doctor` |
| `HARVESTER_WITH_IBKR` | OFF | `TWS_API_DIR` pointing at IBKR's C++ client (`IBJts/source/cppclient/client` of the [API download](https://interactivebrokers.github.io/)) | routing to IBKR, the router half of `doctor` |

yaml-cpp is fetched too when it is not installed. IBKR's API is under its
own licence and is never part of this repository; `deploy/bootstrap.sh`
downloads it. Its `Decimal` type is Intel's BID64 decimal, whose library
IBKR does not ship: this build provides the eight entry points the client
needs (`src/execution/bid64_shim.cpp`, tested in the suite), or set
`HARVESTER_TWS_BID_SHIM=OFF` and `TWS_BID_LIBRARY` to link Intel's. Both
the 10.30 (stable) and 10.37 (latest, protobuf) API lines build; the
adapter follows the two callback signatures that changed between them.

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DHARVESTER_WITH_IBKR=ON -DTWS_API_DIR=$HOME/twsapi/IBJts/source/cppclient/client
cmake --build build
```

`-DHARVESTER_NATIVE=ON` adds `-march=native`. `-DHARVESTER_WITH_DATABENTO=OFF`
builds the core alone, offline.

## Layout

The Python package's modules, one for one:

| Python | C++ | what |
|---|---|---|
| `instruments.py` | `include/harvester/instruments.hpp` | products, fixed-point prices, ticks, fees |
| `config.py` | `config.hpp`, `src/config.cpp` | the YAML schema, validated; unknown keys are errors |
| `book/book.py`, `builder.py` | `book/book.hpp`, `book/builder.hpp` | the sorted book, the MBO and MBP-10 builders |
| `book/feed.py` | `book/feed.hpp`, `src/databento/feed.cpp` | the locked feed; the Databento session |
| `book/replay.py` | `src/databento/dbn_source.cpp` | DBN files, through databento-cpp |
| `signals/vpin.py` | `signals/vpin.hpp` | VPIN and the hysteresis gate |
| `signals/flow.py` | `signals/flow.hpp` | order flow imbalance, queue depletion, the aggressor run |
| `signals/vol.py` | `signals/vol.hpp` | the EWMA realised vol |
| `quoting/engine.py` | `quoting/engine.hpp` | reservation price, spread, toxicity, skew, placement, inventory |
| `execution/throttle.py` | `execution/throttle.hpp` | the token bucket and the priority queue |
| `execution/quotes.py` | `execution/quotes.hpp` | the quote manager: states, dead-band, ack timeout |
| `execution/simulated.py` | `execution/simulated.hpp` | the simulated exchange and its queue model |
| `execution/ibkr.py` | `execution/ibkr.hpp`, `tws_gateway.hpp` | the connection and broker over a gateway seam; the TWS API behind it |
| — | `execution/market_data.hpp`, `ibkr_feed.hpp` | IBKR's depth as a book feed, over the same kind of seam; new here |
| `inventory.py`, `risk.py` | `inventory.hpp`, `risk.hpp` | position, P&L, markouts; pulls and halts |
| `live/journal.py`, `pipeline.py`, `runner.py` | `live/journal.hpp`, `pipeline.hpp`, `runner.hpp` | JSONL journal, the decision cycle, the forward walk |
| `replay/synthetic.py`, `runner.py` | `replay/synthetic.hpp`, `pyrandom.hpp`, `runner.hpp` | the generated market with CPython's random numbers; the replay |
| `cli.py` | `src/main.cpp` | `replay`, `fetch`, `live`, `doctor`, `report`, `config` |

`include/harvester/replay/pyrandom.hpp` is CPython's `random.Random` —
MT19937 seeded with `init_by_array`, 53-bit doubles from two draws,
`randint` by rejection on `getrandbits`, `expovariate`, `choice` — so that
seed 7 here is seed 7 there and the two implementations can be run on the
same tape. The tests pin it against values the interpreter produced.

## What is the same, and what is not

The arithmetic is the Python system's, operation for operation, in
`double`; `Pipeline::step` does what `Pipeline.step` does in the same
order; the journal rows have the same keys in the same order with the same
number formatting; the reasons a quote was missing are the same strings.
`tools/parity.sh` runs both implementations on the generated tape and
diffs their summaries.

What differs, deliberately:

- **A dry run needs no gateway.** The Python runner connected to IBKR even
  in `--dry-run`, to qualify the contract and adopt the account's position
  into a simulated inventory. Here a dry run is the feed and the simulated
  exchange and nothing else, so it runs on a box with only a Databento
  key. There is no account to adopt from and no order to route.
- **The decision loop keeps a cadence.** The Python loop slept for the
  interval *after* each cycle inside `ib.sleep`; here the next cycle starts
  one interval after the previous one was due, on a steady clock, so the
  period does not drift with the work.
- **Snapshots have a fixed capacity** of 32 levels a side (`book.depth`
  must be at most 32) so they are copied without allocating. MBP-10 gives
  ten; the signals read one.
- **The feed's trade buffer is bounded** (100,000 executions, the same
  bound as Python) and the simulated exchange drops finished orders instead
  of keeping every order it ever saw.
- **The TWS adapter is synchronous where ib_async was.** Contract details,
  positions, account values and open orders block on the API's `...End`
  callbacks with a timeout; order events are forwarded as they arrive.
  IBKR's executions and commission reports are separate callbacks; a fill
  whose commission report has not landed when the fill is read carries the
  product's configured fee, as in Python.
- **`report`** groups and rounds the same way but without pandas, so a
  column that pandas would have printed as `12.0` prints as `12`.

## The book

One Databento subscription builds it, on the schema the config names:

| schema | what arrives | what it costs |
|---|---|---|
| `mbo` | every order: add, cancel, modify, fill, and the trade that caused the fills | ~10× the messages; the exact queue in front of a resting quote |
| `mbp-10` | ten aggregated levels a side after every change | a tenth of the rate; the level's size, which is the same queue |

Both carry the trades inline as `T` records with the **aggressor side CME
states in the match event**. Nothing infers it — no tick rule, no
Lee–Ready — and the tape needs no second subscription.

Prices inside the book are Databento's fixed-point integers (1e-9 of a
point), so a level is an exact key; they become doubles at the snapshot.
Each side is one vector sorted best-first: the touch is element zero, a
snapshot is a copy of the first `depth` elements, an MBP-10 record that
replaces all ten levels is twenty appends into storage that is never
freed, and an MBO update near the touch — where nearly all of them land —
moves a few bytes. An MBO stream is not trusted until its snapshot has been
delivered: the `F_SNAPSHOT` records are the standing book, the first live
record after them marks it complete, and a stream joined without one stays
`complete = false` forever and is never quoted against.

**The traded contract.** `MES.v.0` is the front contract by volume,
resolved by Databento to a raw month (`MESZ6`) reported in a
symbol-mapping message. The feed keeps it and the IBKR connection routes
to *that* contract; a roll makes the runner reconnect and re-qualify.

**The anchor** is the microprice, not the mid:

```
microprice = (bid · ask_size + ask · bid_size) / (bid_size + ask_size)
```

## The IBKR book, when Databento is not available

`ibkr.market_data: true` builds the same book from IBKR's own depth — the
CME Real-Time (P,L2) add-on — instead of Databento. It exists so a forward
walk can keep running when the Databento session cannot be had. The feed
maintains IBKR's ladder, synthesises the MBP-10 records the builders
already read, and pushes them through the same `RecordFeed`, so the book,
the signals, the gate, the quoter and the journal are unchanged and
unaware. It takes its own TWS session on its own client id
(`market_data_client_id`, default `client_id + 1`), so a broker reconnect
does not take the book down, and it resolves the contract by the same
route the router does, so the month quoted is the month traded.

**It is a fallback, not a second source of the same thing.** Four
differences, none of them fixable in the adapter:

| | Databento MDP 3.0 | IBKR |
|---|---|---|
| aggressor side | stated in CME's match event | **inferred** with the quote rule, against the touch before the trade |
| book updates | every exchange message | a maintained ladder: rows change, the messages between them do not arrive |
| timestamps | CME's, nanoseconds | none on depth, whole seconds on trades — records are stamped on arrival here |
| order counts | per level | absent (`bid_ct`/`ask_ct` are zero) |

The first two are the ones that move numbers. VPIN is built on the
aggressor classification, so its levels are an estimate on this feed in a
way they are not on MDP 3.0; `harvester doctor` reports the share of
trades the quote rule could not call, and `toxicity.unknown_side` decides
what happens to those. Order flow imbalance and queue depletion are built
from book *deltas*, so on a sampled ladder they see a coarser series than
the one they were measured on — expect them smaller and slower, not merely
noisier.

So: **no number measured on this feed is comparable with a replayed one,
or with a Databento-fed one.** Paired arms run on it are still valid
against each other, because both arms see the same book. Anything
absolute is not.

The ladder itself — insert, update, delete, the shifts they imply, the
quote rule and its fallback — is ordinary code behind the `IbMarketData`
seam and is unit-tested without the SDK. Only the socket under it needs
`HARVESTER_WITH_IBKR`.

## Two speeds of signal

**VPIN** is volume-bucketed: the tape is cut into buckets of
`bucket_contracts`; in each, buyer- and seller-initiated volume are counted
off the aggressor flag; VPIN is the mean of `|B − S| / V` over the last
`window_buckets`. It moves only when a bucket fills, so it is a statement
about the regime the last *n* buckets were in, and it is used as one. The
gate ranks each reading against VPIN's own recent history
(`history_buckets`); levels are entered at one percentile and left at a
lower one, one step down at a time, after a minimum dwell:

| level | enter | exit | spread × | size × |
|---|---|---|---|---|
| calm | — | — | 1.0 | 1.0 |
| elevated | 70th | 60th | 1.5 | 1.0 |
| toxic | 90th | 80th | 2.5 | 1.0 |
| extreme | 97th | 93rd | 4.0 | 0 → `reduce_only` |

Before `warmup_buckets` readings the gate reports `calm` with
`warmed_up = false` and says so once in the log.

**The fast signals** are three signed numbers in `[−1, 1]`, positive for
buying pressure, updated on every book change or trade and acting on every
quote: order flow imbalance (Cont, Kukanov & Stoikov 2014) at the touch
over `ofi_window_ms`; queue depletion, how fast the best level on each side
is being consumed; and the aggressor run, the signed length of the current
run of same-side trades, saturating at `run_saturation` and decaying over
`run_decay_seconds`. Each shifts the reservation price by a configured
number of ticks at full deflection. None of them is gated.

## The quoting engine

With `s` the anchor, `q` the inventory in contracts, `σ` the realised
volatility of the anchor in points per root-second and `τ` the quote
horizon in seconds:

```
reservation   r = s − q · γ · σ² · τ
total spread  δ = γ · σ² · τ + (2/γ) · ln(1 + γ/κ)
```

Then, in order: the half-spread is multiplied by the toxicity level's
spread multiplier and floored at `min_half_spread_ticks`; the reservation
is shifted by the flow skews; the bid is capped at `behind_best_ticks`
below the best bid and the ask floored at the same above the best ask,
both snapped outwards to the tick, and a side more than `max_behind_ticks`
away is not quoted — except the side flattening out of an `extreme`
regime, which is exempt, because that level's own 4.0 spread multiplier
puts it 10–12 ticks back against a cap of 8 and dropping it turned
`reduce_only` into a full pull (0 quotes in 881 extreme snapshots on the
ES tapes); the size is `base_size` times the level's size
multiplier (ceiling), capped so no fill can take the position past
`risk.max_position`, and past `reduce_only_position` only the flattening
side is quoted. Every dropped side carries a reason into the journal.

## Execution inside IBKR's limits

IBKR allows 50 API messages a second per client and disconnects a client
that exceeds it. Everything the system sends goes through one token bucket
(`max_messages_per_second`, default 30) with priorities — cancels, then
replacements, then new quotes, then the position and account polls, which
are made only with headroom in the bucket — and a newer decision for a
side overwrites the older one still waiting for a token. The dead-band
(`requote_price_ticks`, `requote_size_fraction`, `requote_min_interval_ms`)
decides whether a decision is different enough to be worth a message at
all. One order a side, modified in place: a replace is one message. While
a side has a message in flight nothing else is sent to it; a pending state
that outlives `ack_timeout_seconds` is treated as unknown, a cancel is
re-sent, and the side is not quoted until the broker says what the order
is. That cancel is then re-sent every `ack_timeout_seconds` for as long as
the side stays unknown — the lost message can be the cancel itself, and a
single attempt would leave the side dead for the session on one warning.
The retries are counted and logged, as errors past the third. Every order
is a `DAY` limit with `outsideRth` set, routed direct to CME.

## Risk: caps well below margin

`risk.max_position` is the hard inventory cap in contracts, and it is
meant to sit well below what the account could margin: a position taken on
because the inside was swept is, by construction, on the wrong side of the
move that swept it. Two kinds of stop, kept apart:

- **pull** — cancel everything now, resume when the condition clears: a
  stale book, realised volatility above `max_sigma`, outside the quoting
  hours (with a flatten at the end of them), the operator's kill file
  while it exists (`touch runs/HALT`).
- **halt** — cancel, flatten if configured, stay stopped until a person
  restarts the process: the daily loss limit, margin past the cap, a
  broker position that disagrees with the book, a position outside the
  cap.

`risk.max_sigma` is the volatility ceiling, in the same points per
root-second the Avellaneda-Stoikov spread is sized in: above it no quote
is placed at all. It is the primary gate in `es_paper.yaml`, at 0.22, and
it is off (`0`) everywhere else. `risk.sigma_halflife_seconds` gives that
ceiling its own volatility estimate — 12.5 seconds in `es_paper.yaml`
against the quoter's 30 — so a faster sigma can catch a regime change
without making the A-S spread jitter; `0` follows the quoter's and is the
behaviour everywhere else.

Both numbers are the middle or the edge of a measured band, not a
calibration. 0.22 against 0.25 is +$282 a session at t = 1.00 over ten
sessions, and the 10–20 second half-life band is +$188 a session at
t = 1.63: the bands are supported and the points inside them are not
separable at this sample size. Re-tuning either on the walk's first
sessions would be fitting the noise that put the in-sample peak at 0.22
in the first place.

The reason it is a pull and not a wider spread is what the tape says about
which fills go wrong. Realised volatility separates the profitable fills
from the unprofitable ones, and a logistic regression free to use
everything puts four times the weight on vol that it puts on OFI and a
coefficient of +0.003 on VPIN. Replayed over ten ES sessions, a ceiling in
the 0.22–0.30 band is worth about $670 a session against quoting through
those periods (+$663, t = 2.33, better in nine of ten at 0.25 alone), and
every one of eleven ceilings tried between 0.70 and 0.12 beat quoting
without one. Where in the band is not identifiable: 0.22 against 0.25 is
+$282 a session at t = 1.00, six of ten.

Two claims this README used to make about the signals were read off the
wrong population, and both are withdrawn. A bug in `edge_report.py`
computed them over every sweep that reached the price rather than over the
fills a quote at the back of the queue actually gets — at one tick behind
on ten ES sessions that is 46,211 rows against 1,022, and the detection
floor moves with it from ±0.009 to ±0.063.

- **"The signals separate the fills no better than chance"** (quoted as an
  IC of −0.011 against ±0.009). On the traded population the composite is
  +0.053 at one second, +0.146 at five and +0.103 at sixty — *positive*,
  meaning flow pointing our way before a fill predicts a **better**
  markout: continuation, not toxicity. Three fresh sessions
  (2026-09-15/16/17, n = 628) hold up the five-second version and break
  the sixty-second one, which flips to −0.050. Pooled over all thirteen
  sessions (n = 1,650, floor ±0.049) the five-second IC is +0.112 and the
  sixty-second is +0.036, inside the floor. So: a five-second continuation
  effect that survives a hold-out, worth $19.02 a fill between the best
  and worst quintile against a $4.60 round turn — and no way to collect it
  yet, because the replay's own standard error is $492 a session.
- **"The gate's levels do not order the markouts"** (quoted as +8.90,
  +7.45, +9.42, +11.28 from calm to extreme). Queue-aware over the same
  ten sessions they are +5.21, +12.98, +38.93 and +22.54, and pooled over
  thirteen, toxic-and-extreme minus calm is +$12.73 a fill (se 10.96,
  t = 1.16). That is not an ordering either, at this power — but the
  direction is the *opposite* of the gate's premise: it widens the spread
  and cuts size into the levels whose fills pay best. Consistent with the
  replay, where turning the gate off behind a 0.25 ceiling costs +$119 a
  session at t = 0.52.

The VPIN gate stays on behind it. On its own it is worth $507 a session —
not by picking fills, which it cannot do, but by widening the spread and
cutting size when the tape is busy, which reduces exposure in exactly the
conditions the ceiling now refuses outright. The two are substitutes: with
the ceiling in place, turning the gate off is worth $119 a session at
t = 0.52, indistinguishable from zero. It is kept as the backstop for a
ceiling that is misconfigured or a volatility estimate that has not warmed
up, and its spread multipliers are not where the money is.

`tools/edge_probe.cpp` (the `harvester_edge` target) is the measurement
behind all of this: it replays a tape through the live path's own book and
signals and writes one row per notional fill — the signal state frozen
before the sweep, the capture, and the markouts that followed — plus, with
`--samples`, a periodic feature panel. `tools/edge_report.py`,
`tools/edge_tables.py` and `tools/direction_model.py` aggregate and fit.

At connect the runner adopts the account's position on the contract,
cancels working orders on it that this process did not place, and refuses
to run at all alongside a position in anything else.

## Replay, and reading a result

`harvester replay` runs the same `Pipeline::step` the live runner does, on
the tape's own clock — including the quoting hours, read off that clock in
the product's zone exactly as the live runner reads them, so a tape that
starts at 08:30 is not quoted until `quote_start` — with a simulated
exchange that joins the back of the queue, consumes it with trades at the
price, fills on a seller-initiated trade at or through a bid, and fills
when the book crosses a resting price. It has no latency and no impact,
so replay fills are a floor on adverse selection. The generated market
(`replay.source: synthetic`) is a harness with informed episodes at
`synthetic_toxic_fraction`. `--control`
runs the same tape again with the order-flow awareness removed — one arm
at a time, because the gate and the skews do different jobs and adding
their effects together hides both:

```
                            fills   net      against the shipped run
shipped                        94   $-1,129
--control gate                112   $-2,058  gate:    net $+929
--control skew                102   $-1,535  skews:   net $+406
--control both                144   $-1,956  signals: net $+828
```

`--control` with no argument runs all three; naming one runs only that
one. The arms do not add up, which is the reason to run them apart: the
gate widens the spread and cuts size when the tape is busy, the skews move
the quote sideways and are most of what unsticks it, and turning both off
is not the sum of turning off each. Those are seed 7 of
`configs/es_replay.yaml`. For every fill the journal records the
anchor at the fill and again at one, five and thirty seconds after it
(`markout_h = side · (anchor(t + h) − fill_price) · multiplier`, dollars
per contract, positive when the price went the quote's way), bucketed by
the toxicity level at the fill. Were fills in `toxic` worse than fills in
`calm`? How much of the session was spent not quoting, and why? How close
did the message rate come to the budget? Those are the questions the
journal answers and a P&L line cannot.

## Correctness

```bash
build/harvester_tests        # 179 cases, 3,805 assertions
tools/parity.sh              # the Python and C++ replays, diffed
```

`parity.sh` allows exactly one difference, by name: the message count. The
side flattening out of an `extreme` regime is exempt from
`max_behind_ticks` here and is not in the Python reference, which still
drops it, so this implementation sends the messages that place and cancel
a quote the reference never places. No fill and no P&L line moves. Any
other difference fails the script.

The Python suite's 136 tests are here, case for case: the builders against
scripted MBO and MBP-10 sequences; VPIN's bucket arithmetic and the gate's
hysteresis; the sign of each fast signal; the engine's reservation, spread,
placement, inventory and toxicity behaviour; the budget's rate, the queue's
priorities and coalescing, the manager's dead-band, in-flight discipline
and ack timeout; the simulated exchange's queue model; the broker against
a fake gateway — the connection's paper-account gate, contract
qualification, position adoption and base-currency handling, the mapping
of statuses, executions and error codes onto events, the cancel of foreign
orders; the risk monitor's pulls and halts; the replay end to end,
including the message rate staying inside a deliberately small budget and
the loss limit flattening; and the live runner through a reconnect, an
adopted position and a foreign order cancelled at connect. Added: the
CPython random generator against the interpreter's own output, the
generated tape's record and trade counts against Python's, the BID64
decimal shim, the JSON writer against `json.dumps`, and the Chicago
quoting-hours check.

## Known approximations, and what is unverified

- **No latency model** in the replay; the live dry run has the real feed
  but still simulated, instantaneous fills.
- **Queue position** is the level's size at placement, shrinking with
  trades and with the level; MBP-10 cannot see cancels behind a quote.
- **VPIN's percentile** drifts under a persistent regime: a storm that
  lasts all day becomes the distribution.
- **The synthetic market** is not a model of ES.
- **The IBKR book feed has never seen a live IBKR session.** The ladder,
  the quote rule and the reset handling are tested through the
  `IbMarketData` seam; the TWS session under them compiles against API
  10.45 and uses only callbacks that have been stable since 974, but no
  depth message has arrived through it. `doctor` with `ibkr.market_data`
  on is the first thing to run, during the session.
- **Not yet run against a live Databento session or a live gateway.** The
  Databento adapter compiles against databento-cpp 0.42 and the DBN reader
  is exercised on that library's own MBO and MBP-10 test tapes; the TWS
  adapter compiles against API 10.30, 10.37 and 10.45 and the logic above
  its seam is tested through a fake, but no order has been sent through
  it.
  `doctor` is the first thing to run, during the session, and `--dry-run`
  the second.
