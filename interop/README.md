# KNstaX interop harness

Wire-level interoperability tests for the KNXnet/IP side of the stack. The
harness drives KNstaX with real KNXnet/IP frames, using
[`xknx`](https://github.com/XKNX/xknx) as an independent, specification-following
peer, and runs in CI on every push.

## What it is for

The unit tests check the stack against the specification texts. This harness
checks it against *another implementation*: frames go out on a real UDP or TCP
socket, a third-party stack has to understand them, and what it sends back has
to be understood in turn. It covers:

- KNXnet/IP tunnelling over UDP: connect, heartbeat and disconnect, tunnelling
  request and acknowledge, cEMI encoding and the L_Data.con confirmations;
- KNXnet/IP routing over multicast;
- KNX IP Secure tunnelling over TCP: session set-up and the secure wrapper;
- KNX Data Secure group communication carried over a tunnel, including replay
  and unknown-sender rejection;
- a KNstaX KNXnet/IP interface (tunnelling server bridged to TP1).

Passing these tests is strong evidence that KNstaX interoperates with other
specification-compliant implementations **for the features the tests cover**.
It is not certification: `xknx` is not the KNX Association's conformance test
suite, and a local simulator cannot reproduce every real gateway's timing. The
KNX IP medium has also been run against ETS6 on hardware (Habinari); see the
[conformance status](../docs/reference/knx_conformance_status.md) for what has
and has not been validated that way.

Out of scope, on purpose:

- certification coverage of device management, property services and the other
  management procedures (the C++ host tests cover those);
- discovery and autodetection: every test is given its peer binaries, ports and
  credentials explicitly, so a CI run is reproducible;
- hardware in the loop.

## How it works

```
 xknx (or a raw tunnelling client)
        │  KNXnet/IP over UDP / TCP / multicast, on localhost
        ▼
 gateway simulator  (interop/python/knip_gateway/)
        │
        ▼
 KNstaX peer        (interop/knstax_peer/, a C++ executable running the stack
                     and performing scripted actions)
```

A local gateway simulator makes the frames real and the sequencing
deterministic without external hardware. The tests assert at the protocol
boundary — frames, sequence counters, service types — which is what a real peer
would see.

| Path | Contents |
|---|---|
| `knstax_peer/` | The KNstaX peers: tunnelling client, routing device, and KNXnet/IP interface (tunnelling server + scripted TP1 line) |
| `python/knip_gateway/` | Minimal KNXnet/IP tunnelling, routing and IP Secure gateway simulator, with the codecs it needs |
| `python/tests/` | The `pytest` suite |
| `scripts/` | Stand-alone tools for debugging by hand (below) |

## Running

```bash
./build.sh test-all
```

This creates `interop/.venv`, installs `interop/python/requirements.txt`, builds
the peers and runs the suite through CTest (`ENABLE_INTEROP_TESTS=ON`). CI does
the same in its `interop` job.

By hand:

```bash
python -m venv interop/.venv
. interop/.venv/bin/activate
pip install -r interop/python/requirements.txt

cmake -S . -B build -DBUILD_TESTS=ON -DENABLE_INTEROP_TESTS=ON
cmake --build build -j --target interop_peers

export KNSTAX_TUNNEL_PEER_BIN=build/interop/bin/knstax_tunnel_peer
export KNSTAX_ROUTING_PEER_BIN=build/interop/bin/knstax_routing_peer
export KNSTAX_INTERFACE_PEER_BIN=build/interop/bin/knstax_interface_peer  # optional: defaults to the routing peer's directory
pytest -q interop/python/tests
```

The Data Secure tests generate a temporary `.knxkeys` keyring for each run. To
use one of your own, set `XKNX_KNXKEYS_FILE` and `XKNX_KNXKEYS_PASSWORD`.

`requirements.txt` pins the `xknx` version the suite is verified against, and
CI installs the same. The Data Secure tests build signed keyrings through
`xknx` internals, so move the pin deliberately and with a full interop run.

### Debugging by hand

`scripts/run_secure_gw.py` starts the IP Secure gateway simulator on a free port
and prints it; `scripts/run_xknx_client.py <host> <port> [group]` connects
`xknx` to it over secure TCP tunnelling and does one write and one read. Run
them with `PYTHONPATH=interop/python`. User id and password come from
`XKNX_IPSEC_USER_ID` and `XKNX_IPSEC_PASSWORD` (defaults `1` and `password`).

When a test fails, look at all three logs: the gateway simulator's, the KNstaX
peer's and `xknx`'s.

## Test coverage

| File | Covers |
|---|---|
| `test_knstax_tunneling.py`, `test_knstax_tunneling_compliance.py` | UDP tunnelling: `GroupValueWrite` both ways, `GroupValueRead`/`Response`, DPT 1, 5 and 9 round trips, device descriptor read, connect/heartbeat/disconnect |
| `test_knstax_routing.py` | Routing multicast: `GroupValueWrite` both ways, `GroupValueRead`/`Response` for DPT 1, 5 and 9 |
| `test_knstax_ip_secure_xknx.py` | IP Secure tunnelling over TCP: writes both ways, read/response |
| `test_knstax_ip_secure_mac_rejection.py` | A secure wrapper with a tampered MAC is rejected |
| `test_knstax_data_secure_xknx.py` | Data Secure over UDP tunnelling: writes both ways, read/response |
| `test_knstax_data_secure_negatives.py` | Data Secure: a replayed sequence number is dropped, a sender not in the security table is rejected |
| `test_knstax_interop_suite.py` | The write and read/response round trip across plain, Data Secure and IP Secure combinations |
| `test_knstax_interface_xknx.py` | `xknx` tunnelling through a KNstaX KNXnet/IP interface onto TP1: positive and negative L_Data.con, Confirm flag, tunnel address on the line |
| `test_smoke_gateway.py` | The gateway simulator starts and binds |

Tests that need `xknx` skip when it is not installed.

## Rules for changing the harness

- **Keep the tests wire-level.** Assert on what a real peer would see, not on
  KNstaX internals.
- **Keep configuration explicit.** No discovery: peer paths, ports and
  credentials are passed in.
- **Fix KNstaX to the specification, not to `xknx`.** A change made to satisfy
  this peer has to be correct under the KNX specification too, not a workaround
  for an `xknx` quirk.
- **Keep the suite fast enough for CI.** It is the baseline. Confidence beyond
  `xknx` comes from running the same scenarios against a real gateway, another
  stack or ETS from time to time, not from making this suite slower.

Two things the harness depends on that are easy to break:

- The KNstaX peer installs its Data Secure keys *after*
  `BusAccessUnit::init()`. `init()` restores the interface objects and rebuilds
  the key maps from the persisted key tables, so a key set before it is
  silently dropped and every secured telegram fails with "Unknown Key".
- The test gateway's CONNECT_RESPONSE carries a real tunnelling CRD with the
  assigned address (channel *n* is 1.1.*n*). `xknx` adopts that address as its
  source; a CRD that only echoes the CRI makes it 0.2.0.
