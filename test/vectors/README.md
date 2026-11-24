# Test vectors

Reference frames for the KNX IP Secure tests, made outside KNstaX so the tests
compare the stack against an independent implementation rather than against
itself. The tests read them by path relative to the repository root.

| Directory | Contents | Made by |
|---|---|---|
| `knxnetip_secure_session/` | Session handshake: SESSION_REQUEST, SESSION_RESPONSE, SESSION_AUTHENTICATE (plain and wrapped) | `node tools/gen_knxultimate_secure_session_vectors.js` (Node's built-in crypto, constants from KNXUltimate) |
| `knxnetip_secure_wrapper/` | SECURE_WRAPPER for tunnelling and routing | `node tools/gen_knxultimate_secure_wrapper_vectors.js` (KNXUltimate's security primitives) |
| `knxnetip_secure_wrapper/timernotify_0955_capture.vec` | A TIMER_NOTIFY taken from a Wireshark capture | By hand; the file header says from where |

The files are checked in, so the tests need neither Node nor KNXUltimate.
Regenerate them only when a vector itself has to change. The wrapper generator
loads [KNXUltimate](https://github.com/Supergiovane/KNXUltimate) from
`external/KNXUltimate/`, which is not part of this repository: clone it there
first.
