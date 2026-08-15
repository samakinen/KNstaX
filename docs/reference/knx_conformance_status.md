# KNX conformance status

What this stack implements, what it does not, and why. The device profile
targeted is **mask version 07B0 (System B, TP1)**.

Written originally against KNX Specification v2.1, and revised against **The
KNX Standard v3.0.0**, which supersedes it for anything Secure-related — the
v2.1 set predates the extended memory and extended property services entirely.
Check claims about those against the v3.0.0 volumes, not v2.1. Neither document
set is distributed with this repository; obtain your own from my.knx.org.

This document is the inventory a certification effort would start from. It is
deliberately blunt about gaps: a feature listed as implemented is one that has
runtime behaviour and a test, not one that has an enum value.

Read "implemented" as exactly that, and no further. Two areas have been
validated on real hardware with real ETS: TP1 over the bit-bang backend, and
KNX Data Secure over it (secure download and secure group communication).
Everything else in this document rests on host tests written against the
specification texts — it has never met real ETS, a real gateway, or real
transceiver hardware. The stack is also not finished; absence from the gap list
at the end is not proof that an area is complete.

## Application layer services (03/03/07)

### Implemented

| Service | APCI | Notes |
|---|---|---|
| A_GroupValue_Read / _Response / _Write | 0x000 / 0x040 / 0x080 | Gated by the object's R/W/U/C flags |
| A_IndividualAddress_Read / _Write | 0x100 / 0x0C0 | Programming mode required |
| A_IndividualAddressSerialNumber_Read / _Response | 0x3DC / 0x3DD | Broadcast; answers only on serial-number match |
| A_IndividualAddressSerialNumber_Write | 0x3DE | Commissioning without the programming button |
| A_Memory_Read / _Response / _Write | 0x200 / 0x240 / 0x280 | ETS download path; write answers with a verify read-back |
| A_MemoryExtended_Write / _Write_Response | 0x1FB / 0x1FC | Required for Data Secure: ETS loads the Security Interface Object key tables through this path |
| A_MemoryExtended_Read / _Read_Response | 0x1FD / 0x1FE | 24-bit address narrowed onto the 16-bit region map; addresses above 0xFFFF answer `AddressVoid` |
| A_DeviceDescriptor_Read / _Response | 0x300 / 0x340 | Descriptor type 0, mask 07B0 |
| A_PropertyValue_Read / _Response / _Write | 0x3D5 / 0x3D6 / 0x3D7 | |
| A_PropertyDescription_Read / _Response | 0x3D8 / 0x3D9 | |
| A_FunctionPropertyCommand | 0x2C7 | PDT_FUNCTION properties; used for PID_SECURITY_MODE and PID_ROUTETABLE_CONTROL |
| A_FunctionPropertyState_Read / _Response | 0x2C8 / 0x2C9 | State_Read is side-effect free |
| A_PropertyExtValue_Read / _Response | 0x1CC / 0x1CD | 12-bit object instance and property ID; start_index 0 answers the element count |
| A_PropertyExtValue_WriteCon / _WriteConRes | 0x1CE / 0x1CF | Confirmed write; answers with a return code even on refusal |
| A_PropertyExtValue_WriteUnCon | 0x1D0 | Unconfirmed; never answers, not even on failure |
| A_PropertyExtDescription_Read / _Response | 0x1D2 / 0x1D3 | Description type zero; DPT main/sub reported as 0 (not tracked per property) |
| A_FunctionPropertyExtCommand | 0x1D4 | Answers with A_FunctionPropertyExtState_Response |
| A_FunctionPropertyExtState_Read / _Response | 0x1D5 / 0x1D6 | Return code precedes the data, unlike the value services. Reaches the same object functions as the classic services, but addressed by object type + instance and answering in the unified return-code space of §3.4.8.3 — the two sets are different code spaces, not the same numbers at different widths |
| A_NetworkParameter_Read / _Response / _Write | 0x3DA / 0x3DB / 0x3E4 | Silent when the device does not expose the parameter |
| A_SystemNetworkParameter_Read / _Response | 0x1C8 / 0x1C9 | `NM_Read_SerialNumber_By_ProgrammingMode` (03/05/02 §2.17.1.4) — how ETS scans for devices in programming mode. Answers only in programming mode, only for Device Object / PID_SERIAL_NUMBER / operand 01h; silent otherwise. Sends immediately instead of the specified random 0–1 s wait |
| A_Authorize_Request / _Response | 0x3D1 / 0x3D2 | |
| A_Restart | 0x380 | Basic and Master Reset |
| A_ADC_Read / _Response | 0x180 / 0x1C0 | channel_nr in the APCI data field, read_count and the 2-octet sum in the payload. The converter itself is supplied by the product through `ApplicationLayer::setAdcReadProvider`; with none installed every channel is answered with `read_count = 0`, which §3.5.2 defines as "wrong channel number". The answer is the point: the request arrives on a transport connection, and silence costs the client its full timeout |

### Not implemented

| Service | Why it is absent |
|---|---|
| A_SystemNetworkParameter_Write | 0x1CA. The read/response pair above is implemented; the write is not. Nothing in the download or normal-operation path needs it. |
| A_DomainAddress_* | Open-media (PL/RF) addressing. No RF or Powerline medium is shipped. |
| A_UserMemory_* / A_UserManufacturerInfo_* | BCU1/BCU2 mask-specific (0012h/0020h). Not part of System B. |
| A_Link_Read / _Write / _Response | Coupler link management. Meaningful only once a full coupler profile exists. |
| A_GroupPropValue_* | LTE (Logical Tag Extended) addressing, which this stack does not implement. |

### APCI decoding note: the 0x1C0 group is shared

`A_ADC_Response` is `0x1C0` with six APCI data bits, so on paper it spans
`0x1C0..0x1FF`. Both extended service blocks live inside that span and use all
ten APCI bits.
`APCIField::service()` therefore matches `0x1CC..0x1D6` (extended property) and
`0x1FB..0x1FE` (extended memory) exactly, *before* falling back to the six-bit
service group — otherwise the device would answer ETS's property and key-table
traffic as ADC replies. `test_MemoryExtended_ApciDecodeDoesNotCollideWithAdcResponse`
and `test_PropertyExt_ApciDecodeDoesNotCollideWithAdcResponse` pin both blocks
and the surviving ADC range.

### Extended property services

Implemented in `application::PropertyExtServices`, and the generated `.knxprod`
sets `Static/Options/@SupportsExtendedPropertyServices`.

They are here because
The KNX Standard v3.0.0, Volume 6 Profiles (06 Profiles v02.01.01)
§9.1.2.3.1 lists both Extended Property services and Extended Memory services
as `M` (mandatory) for the **KNX Data Security** profile. That requirement is
about the Secure profile, not the base mask profile — mask 07B0 on its own does
use the non-extended services, which is why this was mis-scoped at first.

§9.1.2.3.2 names the seven mandatory services. Six are handled as a server;
`A_PropertyExtValue_InfoReport` is a device-initiated notification with no
request to answer, and no call site currently emits one.

What differs from the classic property services, and why they could not simply
be widened:

- Objects are addressed by **(object type, object instance)** rather than by
  object index. `PropertyExtServices::resolveObject()` maps them, treating
  instance as 1-based over the objects of that type in index order.
- **Object instance and property ID are 12 bits each**, straddling octet 11 of
  the PDU. `PropertyExtHeader` owns that packing.
- **start_index is 16 bits**, where the classic service packs a 4-bit count and
  12-bit index into two octets.
- Every request is answered with a return code from the shared Error Code Set,
  including refusals — the exception being `_WriteUnCon`, which must stay
  silent even on failure.

Known limitation: `A_PropertyExtDescription_Response` reports DPT main and sub
as zero, because the property store does not track a DPT per property. The
access flags, PDT and max-element count are real.

## Interface objects (03/05/01)

### Live objects with real behaviour

| Type | # | Notes |
|---|---|---|
| Device | 0 | Address, serial, order info, load state machine, programming mode |
| Address Table | 1 | Memory-mapped download via PID_TABLE_REFERENCE |
| Association Table | 2 | Memory-mapped download |
| Application Program | 3 | Program version, load state, parameter memory segment |
| Group Object Table | 9 | PID_TABLE serves and accepts spec Group Object Descriptors (§4.12.5.2.4, Table 52). PID_GO_DIAGNOSTICS (§4.8.1) is implemented in the BAU and declared here as a PDT_FUNCTION property, which is how a Management Client discovers Group Object Diagnostics exists |
| Security | 17 | Data Secure keys, sequence numbers, security mode via Function Property |

### Array properties: the current number of elements

A read of array element 0 answers "the current number of elements of the
Property Value array" (03/03/07 §3.4.4.1) — the table's *usage*, distinct from
the capacity the descriptor reports (03/05/01: "the maximum number of elements
in the Property shall show the maximum size of the table and current length
shall show the current usage"). Both the classic and the extended value
services answer it with `nr_of_elem = 1`, whatever the client asked for.

A write to element 0 sets that length, which is how ETS clears a table before
re-downloading it. Properties supply the two through the `count` / `resize`
hooks of `PropertyHandler`: the Security Interface Object's key tables support
both; the address, association and group object tables report their length but
refuse to have it set, because there it follows the downloaded entries rather
than a client's declaration.

### Registered on demand, backed by the reference property manifest

| Type | # | Status |
|---|---|---|
| KNXnet/IP Parameter | 11 | Auto-registered on IP builds; individual address, installation id, MAC, friendly name and device capabilities are seeded from the device. Current IP address is filled by the transport once bound. |
| Router | 6 | Published by `BusAccessUnit::configureRouterRole()`. Opt-in on purpose: a plain end device advertising a Router Object would claim routing it does not perform. Once declared, `PID_ROUTETABLE_CONTROL` (a Function Property) applies ETS filter-table downloads to the coupler's live `FilterTable`, and `PID_*_LCCONFIG` / `PID_*_LCGRPCONFIG` reach the live `CouplerRoutingPolicy` via `syncRouterRoutingConfig()`. |
| cEMI Server | 8 | Manifest present, no live wiring. |
| Interface Program, LTE routing, Polling Master, File Server, RF Medium, E-mode objects | 4, 7, 10, 13, 19, … | Manifest only, via `setReferenceInterfaceObjectTypes()`. |

## Group object communication flags

All six KNX flags plus transmission priority are modelled and **enforced**, not
just declared. The Group Object Descriptor served from PID_TABLE matches
03/05/01 Table 52 bit for bit.

| Flag | Behaviour |
|---|---|
| Communication (C) | Master gate. When clear, the object is disconnected in both directions and no firmware callback fires. |
| Read (R) | The device answers A_GroupValue_Read for this object. Objects without it stay silent, which is what lets several devices share a group address with one designated responder. |
| Write (W) | An inbound A_GroupValue_Write updates the value. |
| Transmit (T) | The device may originate telegrams for this object. |
| Response-Update (U) | An inbound A_GroupValue_Response updates the value. Distinct from W. |
| Read on Init (I) | After reset the device issues A_GroupValue_Read. Spread across `loop()` calls and pushed through the rate limiter — 03/05/01 §4.12.5.2.4.1.3 warns this multiplies bus load after a whole-installation restart. |
| Priority | 2-bit transmission priority, per object. |

## Programming mode

The standard defines two mutually exclusive realisations and assigns them by
device profile (Profiles v02.01.01 §4.4.1.1). This stack presents mask 07B0
(System B), so it implements **Realisation Type 1 only**.

| Realisation | Profiles | Resource | Client procedure | Status |
|---|---|---|---|---|
| Type 1 — property based | **System B**, mask 57B0h | `PID_PROGMODE` (54), Resources v01.10.01 §4.3.5 | `DMP_InterfaceObjectWrite_R` / `_Verify_R` / `_Read_R` | **Implemented.** Mandatory per Profiles §9.1.2.6.2 |
| Type 2 — memory mapped | System 1, System 2, BCU 1, BCU 2, BIM M112 | `curr_prog_mode` @ **0x0060**, Resources §4.26.3 | `DMP_ProgModeSwitch_RCo` | **Not implemented, by design** — belongs to profiles this stack does not present |

Address 0x0060 is deliberately absent. Resources §4.3.5 notes PID_PROGMODE "is
equal to bit 0 of address 60h of a BCU": same state, two encodings, one per
profile. Implementing both would create a second source of truth for programming
mode without buying conformance for System B. Note that the *only* remote
prog-mode switch procedure in 03/05/02 (§3.13.2) is written against Type 2, which
makes it easy to conclude 0x0060 is universally required — the profile mapping in
Volume 6 is what settles it.

Behaviour required of Type 1 and covered by tests in `test_device_object.cpp`:

- bit 0 carries the state; bits 1–7 are reserved and always read back 0 (§4.3.5)
- the value is 0 after reset — PID_PROGMODE is **not** persisted
- writes go through the *notifying* setter, so the application layer's
  programming-mode gate and the LED follow a client-driven change. Binding this
  to the silent setter made ETS report "the address of the device was
  successfully programmed but the final check of this procedure failed … you may
  need to switch off programming mode (LED) manually": the stored bit cleared
  while the device kept answering `A_IndividualAddress_Read`
- a programming-button press is visible to a client reading the property
  (§4.26.3.3 — the reported state follows the internal one whatever changed it)

## KNX Secure

| Area | Status |
|---|---|
| Data Secure | Implemented as a Secure Application Layer (`security/secure_application_layer.*`): SCF decoding, S-A_Data on group addresses (group key), on point-to-point frames (Tool Key or P2P key) and on broadcast, plus the S-A_Sync request/response service. Keys and sequence numbers live in the Security Interface Object; `PID_SECURITY_MODE` is switched by A_FunctionPropertyCommand, which is how ETS drives it. The CCM framing is tested against the worked examples of 03/03/07 Annex C (C.1.1–C.1.4). |
| Data Secure — sequence-number persistence | Implemented. `PID_SEQUENCE_NUMBER_SENDING` is persisted as an ordinary property. Each partner's *last valid* sequence number is written through into its entry of `PID_SECURITY_INDIVIDUAL_ADDRESS_TABLE` as 03/05/01 §6.3.8.4 requires, and adopted back from it after a download or a restore; the Sequence Number for Tool Access (§6.2), which is deliberately not a Property, has its own persistence record. Both are checkpointed at most once a minute and always on the restart path, so sustained secure traffic does not turn into flash wear. The residual exposure is an *unexpected* power loss: sequence numbers accepted since the last checkpoint are replayable once, until the next S-A_Sync. |
| Access Policies (03/4/1 §6.2) | Enforced at two levels by `application/security_access_policy.*`. **Per Resource:** the Security Interface Object admits only the Role "Tool" with A+C — no unsecured write to any of its properties, and no read at all of the key tables, the Tool Key, the Security Individual Address Table, `PID_SEQUENCE_NUMBER_SENDING` or `PID_GO_SECURITY_FLAGS`. Object identity, load state and the security mode stay readable in plain, because ETS reads them before it has a secure link. This does not depend on the current mode: §6.3.5 requires that "even if Security Mode is disabled, it shall only be possible to enable it by using secure communication". **Per service:** once Security Mode is on, every management write (property, memory, function property, individual address, restart, authorise/key write, network parameter) requires tool-secured communication and is otherwise dropped without an answer. Roles R0–R15 are not configurable — a sender is "Tool" or "Unlisted" — so the Roles column of the Point-to-point Key Table is stored but not evaluated. |
| KNXnet/IP Secure Tunnelling | Implemented, with ECDH session establishment and `.knxkeys` import. |
| KNX Secure Routing | Implemented, opt-in via `IpRoutingPhysical::secureRoutingEnabled_`. Not the default. |
| FDSK / device certificate | **Not implemented, by design.** A Factory Default Setup Key is per-device, and this stack is used with a single firmware image flashed to many devices. The tool key is instead provisioned into NVS at first boot. This is a deliberate deployment trade-off, not an oversight: it means secure commissioning is a lab/self-service flow rather than the certificate-based flow an integrator expects from a catalogue product. |

## Media

| Medium | Status |
|---|---|
| TP1 — timer-ISR bitbang | Primary, and the only path validated on real hardware. |
| TP1 — TPUART | Implemented behind the same MAC engine; **never run against an NCN5120-class transceiver.** |
| KNXnet/IP tunnelling (unicast) | Implemented; compiled out entirely unless `CONFIG_KNX_MEDIUM_IP` / `KNX_FEATURE_NETIP` is set. **Validated only by host interop against `xknx`** — never against real ETS or a real gateway. |
| KNXnet/IP routing (multicast) | Implemented, same gating, same validation gap. |
| RF | Not implemented, and not planned — no RF hardware is available to develop or validate against. |
| Powerline | Not implemented, and not planned, for the same reason. |

## Known gaps for certification

Ranked by how much stands between the current state and a KNX-certifiable
device:

1. **No KNX Conformance Test Tool run.** The repository has one conformance
   evidence test. Certification requires the official test suite against real
   hardware.
2. **FDSK absent** (see above) — blocks certification of a *secure* product,
   though not of a plain one.
3. **The IP-side secure paths are unexercised.** KNXnet/IP Secure Tunnelling
   and Secure Routing are implemented and verified against the specification's
   CCM vectors, but neither has run against a real peer, so the session and
   wrapper mechanics are unconfirmed on the wire.

   Data Secure itself is *not* in this list any more: a full ETS secure download
   and secure group communication have both completed on hardware, which
   exercised the `.knxprod` declarations (`IsSecureEnabled`, the
   `MaxSecurity*Entries` table sizes, `SupportsExtendedMemoryServices` /
   `SupportsExtendedPropertyServices`) and the extended memory and property
   services that carry the key tables.
4. **`A_PropertyExtValue_InfoReport` is not emitted.** Listed `M` in Profiles
   §9.1.2.3.2. It is device-initiated, so nothing rejects a device that never
   sends one, but a strict profile review would flag it.
5. **Property access levels are not populated from the profile tables.** The
   manifest carries `readLevel` / `writeLevel` per property, they are reported in
   `A_PropertyDescription_Response` and enforced by `PropertyStore`, but nearly
   every entry leaves them at 0. Profiles §9.1.2.6.2 specifies real values —
   PID_PROGMODE is 3/2, for example. Nothing is blocked today because an
   unsecured device grants Maximum, so this is latent rather than broken.
   Resolve the ordering convention before populating them: KNX wire levels are
   inverted (0 = maximum access), the internal `AuthorizationLevel` enum is
   privilege-ordered, and `PropertyStore` compares with `accessLevel >=
   descriptor.writeLevel` — filling in wire-order numbers without checking which
   convention the descriptors use would invert access control rather than
   implement it.
6. **Coupler profile is implemented, untested, and not complete.** None of the
   routing actions below has been exercised against real hardware or a real
   two-line installation; what follows describes implemented code and host
   tests. The routing algorithm of
   03/03/03 §2.4.2.4 is implemented in full by `network::CouplerRoutingPolicy`
   — all six routing actions, line and backbone coupler roles derived from the
   coupler's own address, point-to-point, multicast and broadcast rules, and
   the `PID_*_LCCONFIG` / `PID_*_LCGRPCONFIG` fields of 03/05/01 §4.4.4–4.4.5.
   `TwoPortCoupler` applies it, and `createTp1CouplerStackPort()` /
   `product::CouplerOptions` make a coupler buildable through the ordinary
   commissioned-product path (see `examples/tp1_line_coupler`). What is still
   missing:
   - `A_Link_Read/_Write/_Response` and the LTE services.
   - The layer-2 acknowledge fields (`PHYS_IACK`, `GROUP_IACK_ROUT`) are
     computed and exposed through `TwoPortCoupler::setAckPolicyCallback()` but
     not enforced: the TP1 backends decide `L_ACK` inside the receive ISR from
     a published address table and cannot consult a per-frame decision made in
     the coupler. A backend that defers the acknowledge can wire the callback.
   - System broadcast is only distinguishable on media that carry the
     distinction; on TP1 it is indistinguishable from ordinary broadcast, so
     `ROUTE_LAST` is reachable only through the explicit `FrameClass` overload.
   - `PID_MAIN_LCCONFIG` and friends are applied to the live policy by
     `syncRouterRoutingConfig()`, which the application must call after a
     download; there is no automatic property-write hook. The filter table does
     not need this — it applies live through `PID_ROUTETABLE_CONTROL`.
7. **HIL coverage is thin** — one hardware validation test. Virtual TP1
   simulation does not substitute for electrical/timing compliance testing on
   real transceivers.
