# Forward walk on a VPS

A runbook for running the harvester unattended against an IBKR paper
account. It is the Python system's deployment with the process swapped
for a binary: IB Gateway under IBC, two systemd units, one environment
file. The thing being run is a quoter, which has a *message rate* to stay
under and a kill switch you should know the location of before you start
it.

**None of this has been run against a real VPS with this binary.** The
gateway scripts are the previous project's, which have; the units pass
`systemd-analyze verify`; the first run on your box is the first run
anywhere. Use `harvester doctor` to find out what is wrong.

## What you need

- Ubuntu 22.04+ / Debian 12+, 2 GB RAM (IB Gateway is a Java desktop app).
- An **IBKR paper account**. No CME market-data entitlement is needed on the
  IBKR side: the book comes from Databento. What *is* needed is that the
  account can route CME futures orders.
- A **Databento** account with `GLBX.MDP3` entitlement and a live-data
  plan. `mbp-10` on one outright is a modest stream; `mbo` is roughly ten
  times the messages, which this build keeps up with on one core.
- NTP on and the clock right: quoting hours are checked in Chicago time.

## 1. Bootstrap

```bash
git clone https://github.com/vivere7108-lab/CME-Market-Maker-C.git /tmp/h
sudo /tmp/h/deploy/bootstrap.sh
```

Creates a `harvester` service user, clones to `/opt/harvester`, downloads
IBKR's TWS API C++ client (10.30 by default; `TWS_API_VERSION=1037.02` for
the protobuf line), builds `/opt/harvester/build/harvester` with the
Databento feed and the IBKR router, runs the tests, installs the units,
writes `/etc/harvester.env` from the example. Starts nothing. Re-runnable.
The first build fetches databento-cpp and takes a few minutes.

## 2. IB Gateway

```bash
sudo /opt/harvester/deploy/install-ibc.sh
sudo nano /etc/ibc/config.ini        # IbLoginId, IbPassword, TradingMode=paper
sudo systemctl enable --now ibc
```

The gateway restarts itself once a day; the runner reconnects, cancels any
order on the contract it did not place, and adopts the account's position.

## 3. The key, and the preflight

```bash
sudo nano /etc/harvester.env         # DATABENTO_API_KEY=...
sudo -u harvester env $(grep DATABENTO /etc/harvester.env) \
    /opt/harvester/build/harvester doctor -c /opt/harvester/configs/es_paper.yaml
```

`doctor` subscribes to the feed and waits for a two-sided, complete book;
checks that the tape carries the aggressor flag; connects to IBKR; checks
the account is paper; qualifies the contract the *feed* named and says if
IBKR resolved a different month; reads positions and margin; and prints
the message budget against IBKR's ceiling. Run it during the session --
outside it the book is thin and the tape is empty, which reads as failure.

## 4. Dry run, then paper

```bash
sudo systemctl enable --now harvester            # ships in --dry-run
journalctl -u harvester -f
```

Leave it a full session. Then:

```bash
sudo -u harvester /opt/harvester/build/harvester report -c /opt/harvester/configs/es_paper.yaml
```

Read the markouts by toxicity level. If fills in `toxic` are not worse than
fills in `calm`, the gate is not doing anything on this product at these
settings, and routing real orders will only confirm that expensively. When
the dry run says something, edit `/etc/harvester.env` to remove
`--dry-run` and `systemctl restart harvester`.

## Stopping it

- `touch /opt/harvester/runs/HALT` -- every quote is pulled within a cycle
  and nothing is placed while the file exists. Remove it to resume. This
  does not flatten.
- `systemctl stop harvester` -- quotes pulled, position left as it is.
- A **halt** (loss limit, margin, a position the book cannot see) pulls
  quotes, flattens if configured, and stays stopped. `Restart=on-failure`
  brings the process back after a crash, and a fresh process has no
  memory of the halt: read the journal's `events-*.jsonl` before you let
  it run again.

## What the runner survives, and what it does not

Survives: the gateway's daily restart, a dropped IBKR socket, a Databento
reconnect (its own policy), a contract roll (the feed names the new
month; the runner reconnects and re-qualifies).

Does not survive, by design: a foreign position in the account (a
reconciliation error, the process exits), a broker position that
disagrees with the book (halt), the kill file, the loss limit.

## Updating

```bash
sudo /opt/harvester/deploy/bootstrap.sh     # pulls, rebuilds, re-runs the tests
sudo systemctl restart harvester
```
