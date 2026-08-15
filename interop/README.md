# KNstaX Interop Harness

A fully-automated interop harness that validates KNXnet/IP interoperability
using on-the-wire frames, with `xknx` as a spec-following peer.

See `interop/GOALS.md` for the long-lived scope and non-goals.

The harness spins up a minimal KNXnet/IP **tunnelling or routing simulator**
locally and uses `xknx` or a raw tunnelling client as a black-box KNX peer
alongside the KNstaX peer executable.

## Why this design

- A local gateway simulator lets us validate real on-the-wire frames and
  protocol sequencing deterministically without external hardware.
- Keeping tests wire-level makes it easy to catch regressions at the protocol
  boundary rather than at the application layer.

## Layout

- `interop/python/knip_gateway/`: minimal KNXnet/IP tunnelling and IP-Secure gateway simulator
- `interop/python/tests/`: `pytest` tests covering tunneling, routing, IP Secure, and Data Secure
- `interop/knstax_peer/`: C++ executable that runs KNstaX and performs scripted actions (tunneling and routing variants)

## Running

```bash
./build.sh test-all
```

The `test-all` path automatically:
- creates an `interop/.venv` virtualenv,
- installs `interop/python/requirements.txt`,
- builds the KNstaX peer executables,
- and runs the full interop suite via CTest (`ENABLE_INTEROP_TESTS=ON`).

For a manual run:

```bash
python -m venv interop/.venv
. interop/.venv/bin/activate
pip install -r interop/python/requirements.txt

cmake -S . -B build -DBUILD_TESTS=ON -DKNX_BUILD_EXAMPLES=ON -DENABLE_INTEROP_TESTS=ON
cmake --build build -j

export KNSTAX_TUNNEL_PEER_BIN=build/interop/bin/knstax_tunnel_peer
export KNSTAX_ROUTING_PEER_BIN=build/interop/bin/knstax_routing_peer
pytest -q interop/python/tests
```

## Test coverage

### Classic UDP tunneling (`test_knstax_tunneling.py`, `test_knstax_tunneling_compliance.py`)
- Bidirectional `GroupValueWrite` (client → KNstaX, KNstaX → client)
- `GroupValueRead` / `GroupValueResponse` round-trip
- DPT-1 (bool), DPT-5 (uint8), DPT-9 (2-byte float) typed round-trips
- Device descriptor read/response
- KNXnet/IP connect / heartbeat / disconnect lifecycle

### IP Routing multicast (`test_knstax_routing.py`)
- Bidirectional `GroupValueWrite`
- `GroupValueRead` / `GroupValueResponse` for DPT-1, DPT-5, DPT-9
- Full read/write round-trips over multicast

### KNX/IP Secure tunneling — xknx peer (`test_knstax_ip_secure_xknx.py`)
- xknx → KNstaX `GroupValueWrite` over TCP secure tunneling
- KNstaX → xknx `GroupValueWrite` over TCP secure tunneling
- `GroupValueRead` / `GroupValueResponse` over TCP secure tunneling

### KNX Data Secure — xknx peer (`test_knstax_data_secure_xknx.py`)
- xknx → KNstaX Data Secure `GroupValueWrite` over UDP tunneling
- xknx Data Secure `GroupValueRead` / `GroupValueResponse` from KNstaX
- KNstaX → xknx Data Secure `GroupValueWrite`

### KNX Data Secure — negative cases (`test_knstax_data_secure_negatives.py`)
- Replay attack: second identical sequence number is dropped
- Unauthorized sender: telegram from non-whitelisted IA is rejected

### KNX/IP Secure MAC rejection (`test_knstax_ip_secure_mac_rejection.py`)
- Tampered MAC in a secure wrapper is rejected by KNstaX

Note: `xknx`-dependent tests skip automatically when `xknx` (or the optional
`xknx[secure]` extras) are not installed. Install
`interop/python/requirements-secure.txt` for the full secure test set.
