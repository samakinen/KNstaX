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

Read "implemented" as exactly that, and no further. These areas have been
validated on real hardware with ETS6: TP1 over the bit-bang backend (ESP32),
TP1 over a TP-UART2 (GD32F103, bare metal), KNX Data Secure over both (secure
commissioning, secured download, secured group communication), and the KNX IP
medium over KNXnet/IP routing (ESP32-C6 on Wi-Fi). KNXnet/IP tunnelling, KNX IP
Secure tunnelling and Data Secure over IP are exercised on the wire against
`xknx` in CI ([interop harness](../../interop/README.md)). Everything else in
this document rests on host tests written against the specification texts — it
has never met real ETS, a real gateway, or real transceiver hardware. The stack is also not finished; absence from the gap list
at the end is not proof that an area is complete.

## Application layer services (03/03/07)

### Implemented

**Configuration.** The claims below are for a build with every feature on (`host-test`, `full`). A build
can leave out, by Kconfig symbol: the extended memory services (`KNX_ENABLE_MEMORY_EXT`), the extended property
and extended function property services (`KNX_ENABLE_PROPERTY_EXT`), the function property services
(`KNX_ENABLE_FUNCTION_PROPERTIES`), the network parameter services (`KNX_ENABLE_NETWORK_PARAMETER`), group
object diagnostics (`KNX_ENABLE_GO_DIAGNOSTICS`), the Router Object and routing (`KNX_ENABLE_COUPLER`) and the
Security Interface Object and Data Secure (`KNX_ENABLE_SECURITY`). A service that is off is ignored like an
unknown APCI; an object that is off is not present, not described and not counted. Data Secure needs the
extended property, extended memory and function property services and group object diagnostics, so
`KNX_ENABLE_SECURITY` selects them. Where the result of a service does not fit the APDU, the service answers
with the error KNX defines instead of dropping the request: the extended memory, extended property value and
function property services use E_EXCEED_MAX_APDU_LENGTH, the classic function property E_ERROR.

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
| A_PropertyExtValue_Read / _Response | 0x1CC / 0x1CD | 12-bit object instance and property ID; start_index 0 answers the element count. The Device Object's PID_IO_LIST (71), mandatory with these services (03/05/01 §4.3.22), lists the interface object types in object-index order |
| A_PropertyExtValue_WriteCon / _WriteConRes | 0x1CE / 0x1CF | Confirmed write; answers with a return code even on refusal |
| A_PropertyExtValue_WriteUnCon | 0x1D0 | Unconfirmed; never answers, not even on failure |
| A_PropertyExtDescription_Read / _Response | 0x1D2 / 0x1D3 | Description type zero; DPT main/sub reported as 0 (not tracked per property) |
| A_FunctionPropertyExtCommand | 0x1D4 | Answers with A_FunctionPropertyExtState_Response |
| A_FunctionPropertyExtState_Read / _Response | 0x1D5 / 0x1D6 | Return code precedes the data, unlike the value services. Reaches the same object functions as the classic services, but addressed by object type + instance and answering in the unified return-code space of §3.4.8.3 — the two sets are different code spaces, not the same numbers at different widths |
| A_NetworkParameter_Read / _Response / _Write | 0x3DA / 0x3DB / 0x3E4 | Silent when the device does not expose the parameter |
| A_SystemNetworkParameter_Read / _Response | 0x1C8 / 0x1C9 | `NM_Read_SerialNumber_By_ProgrammingMode` (03/05/02 §2.17.1.4) — how ETS scans for devices in programming mode. Answers only in programming mode, only for Device Object / PID_SERIAL_NUMBER / operand 01h; silent otherwise. Sends immediately instead of the specified random 0–1 s wait |
| A_Authorize_Request / _Response | 0x3D1 / 0x3D2 | Connection-oriented only. Levels 0 (maximum) to 15 (minimum); the key of a level grants it, FFFFFFFFh and no authorisation both give the most privileged level without a key, any other key the minimum level (03/03/07 §3.5.7). A device with no key on any level grants level 0 to everyone |
| A_Key_Write / A_Key_Response | 0x3D3 / 0x3D4 | Connection-oriented only. One key per level for levels 0–14; FFFFFFFFh removes it. Answered with the level set, or FFh when the requester's current level is numerically above the one written (03/03/07 §3.5.8). The table is persisted as a device Resource of its own and reset by Master Reset 02h/07h from the requester's level up (03/05/02 §3.7.1.2.3.2.3) |
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
use the non-extended services.

§9.1.2.3.2 names the seven mandatory services. Six are handled as a server;
`A_PropertyExtValue_InfoReport` is a device-initiated notification with no
request to answer, and no call site currently emits one.

What differs from the classic property services, and why they could not simply
be widened:

- Objects are addressed by **(object type, object instance)** rather than by
  object index. The application layer maps them (its object resolver), treating
  instance as 1-based over the objects of that type in index order.
- **Object instance and property ID are 12 bits each**, straddling octet 11 of
  the PDU. `PropertyExtHeader` owns that packing.
- **start_index is 16 bits**, where the classic service packs a 4-bit count and
  12-bit index into two octets.
- Every request is answered with a return code from the shared Error Code Set,
  including refusals — the exception being `_WriteUnCon`, which must stay
  silent even on failure.

Both families answer from the same place: the interface objects' property
handlers, through the providers the BAU installs. Only the PDU encoding
differs, so the classic and extended services report the same property at the
same index with the same datatype, access and size (the classic
`max_nr_of_elem` has 12 bits and saturates at 4095; the extended one has 16).

Known limitation: `A_PropertyExtDescription_Response` reports DPT main and sub
as zero ("If no DPT is available, both DPT fields shall be set to zero"),
because the property handlers do not carry a DPT. The access flags, PDT and
max-element count are real.

### Property index

03/03/07 §3.4.3.1 and §3.4.3.2 only require a sequential number ("property_index
= 0 shall mean the first Property"). KNstaX numbers an object's properties in
ascending PID order (`propertyIndexOf()` in `property_kernel.hpp`), so
PID_OBJECT_TYPE is always index 0 and the order does not depend on how a
handler table is written. A description read by PID reports the property's own
index, as §3.4.3.1 requires of new implementations.

## Interface objects (03/05/01)

### Live objects with real behaviour

| Type | # | Notes |
|---|---|---|
| Device | 0 | Address, serial, order info, load state machine, programming mode |
| Address Table | 1 | Memory-mapped download via PID_TABLE_REFERENCE |
| Association Table | 2 | Memory-mapped download |
| Application Program | 3 | Program version, load state, parameter memory segment |
| Group Object Table | 9 | PID_TABLE (23, PDT_GENERIC_02[], writeable, 256 elements; 03/05/01 §4.18.6.2.4, Tables 85–87) serves and accepts the Group Object Descriptors, one per element, element 0 being the length. PID_GO_DIAGNOSTICS (§4.8.1) is implemented in the BAU and declared here as a PDT_FUNCTION property, which is how a Management Client discovers Group Object Diagnostics exists |
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

### Registered on demand, built from the reference property declarations

| Type | # | Status |
|---|---|---|
| KNXnet/IP Parameter | 11 | Auto-registered on IP builds; individual address, installation id and friendly name are seeded at start-up. The address-bearing properties — current IP address, subnet mask, gateway, TTL, MAC, assignment method and device capabilities — are supplied by the firmware through `applyKnxNetIpConfiguration()`, since none of them is knowable before the interface has a lease and most change again when one is renewed. `PID_BACKBONE_KEY` is not inert storage: writing it applies a Secure Routing group key to the live socket. `PID_ROUTING_MULTICAST_ADDRESS` belongs to ETS: it is persisted, a value that is not in 224.0.0.0/4 is refused, and a written value becomes the routing group at the next restart (03_08_03 §2.5.17); the product's configured group only fills it on the first start and after Factory Reset (Erase Code 02h, 03/05/02 Table 6). A_DomainAddressSerialNumber_Write with the 4-octet KNX IP plain format writes the same property. A new value is E11 for Secure Routing and restarts the multicast timer synchronisation; the same value written again is E12 and changes nothing. |
| Router | 6 | Published by `BusAccessUnit::configureRouterRole()`. Opt-in on purpose: a plain end device advertising a Router Object would claim routing it does not perform. Once declared, `PID_ROUTETABLE_CONTROL` (a Function Property) applies ETS filter-table downloads to the coupler's live `FilterTable`, and `PID_*_LCCONFIG` / `PID_*_LCGRPCONFIG` reach the live `CouplerRoutingPolicy` via `syncRouterRoutingConfig()`. |
| cEMI Server | 8 | Declared, no live wiring. |
| Interface Program, LTE routing, Polling Master, File Server, RF Medium, E-mode objects | 4, 7, 10, 13, 19, … | Declarations only, via `setReferenceInterfaceObjectTypes()`. |

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
| Data Secure — sequence-number persistence | Implemented ([design](../design/data-secure-sequence-persistence.md)). **Sending:** a ceiling `CONFIG_KNX_SECURE_SEQ_RESERVE` numbers ahead of `PID_SEQUENCE_NUMBER_SENDING` is stored before any number below it is used, and renewed once half of that window is used, so no number is sent twice after any reset, power loss included, at one write per half window. A number that cannot be reserved is not sent. **Receiving:** each partner's *last valid* sequence number is kept in its entry of `PID_SECURITY_INDIVIDUAL_ADDRESS_TABLE` as 03/05/01 §6.3.8.4 requires, and a download never moves it back (an entry ETS replaces keeps its number in a slot until the partner is written again); a sender with no entry is tracked in one of `CONFIG_KNX_SECURITY_UNLISTED_SENDERS` slots, and refused when they are all taken; the Sequence Number for Tool Access (§6.2), which is deliberately not a Property, has its own persistence record. Each is checkpointed only when it changed, at most once per `CONFIG_KNX_SECURE_REPLAY_CHECKPOINT_S` (15 minutes by default), always on the restart path, and at once when the product reports a power failure (`requestPowerFailSave()`). The residual exposure is a crash, or a power loss the board cannot signal: partner numbers accepted since the last checkpoint are replayable once, until the next S-A_Sync. |
| Access Policies (03/4/1 §6.2) | Enforced at two levels by `application/security_access_policy.*`. **Per Resource:** the Security Interface Object admits only the Role "Tool" with A+C — no unsecured write to any of its properties, and no read at all of the key tables, the Tool Key, the Security Individual Address Table, `PID_SEQUENCE_NUMBER_SENDING` or `PID_GO_SECURITY_FLAGS`. Object identity, load state and the security mode stay readable in plain, because ETS reads them before it has a secure link. This does not depend on the current mode: §6.3.5 requires that "even if Security Mode is disabled, it shall only be possible to enable it by using secure communication". **Per service:** once Security Mode is on, every management write (property, memory, function property, individual address, restart, authorise/key write, network parameter) requires tool-secured communication and is otherwise dropped without an answer. Roles R0–R15 are not configurable — a sender is "Tool" or "Unlisted" — so the Roles column of the Point-to-point Key Table is stored but not evaluated. |
| KNXnet/IP Secure Tunnelling | Implemented, with ECDH session establishment and `.knxkeys` import. |
| KNX Secure Routing | Implemented, and reachable the way ETS reaches it: a `PID_BACKBONE_KEY` download applies the group key to the live routing socket without a restart, and clearing the key turns it off again. Discovery and ROUTING_SYSTEM_BROADCAST stay in the clear on a secured group as 03/02/06 §4.1.3 requires, and plain routing indications are dropped once a group is secured. Verified against the CCM vectors and by host tests; **never run against a real peer.** |
| IP Secure tunnelling sessions | Implemented, and separable: `KNX_SECURE_IP_TUNNELING` gates the ECDH/PBKDF2 session machinery apart from the rest of KNX Secure. A routing-only device compiles it out — it is not a tunnelling client, and the primitives call legacy mbedTLS entry points that mbedTLS 4 (recent ESP-IDF) no longer provides. |
| FDSK / device certificate | **The product's.** A Factory Default Setup Key is per device, so it is not in the firmware image: the product supplies it as `SecureCommissioningOptions::factoryToolKey`. A product that makes it at the first start (with `security::SeededRandom` on a part without a hardware RNG) keeps it in the product key range of persistence, which no Master Reset and no schema change erases, and prints its device certificate with `security::formatDeviceCertificate()` (serial number, FDSK and CRC-4 in Base32, the form ETS asks for). The H8i8o does this. A product that does not can still provision a tool key itself and give it to ETS by other means. |

## Media

| Medium | Status |
|---|---|
| TP1 — timer-ISR bit-bang | Validated on real hardware (ESP32), including Data Secure. |
| TP1 — TP-UART2 | **Validated on real hardware, including Data Secure**: a GD32F103 (Cortex-M3, bare metal) with a Siemens TP-UART2 completed ETS6 product import, individual-address assignment, application and parameter download and group communication on a live bus, and with Data Secure on, secure commissioning with the device certificate, a secured download and secured group communication in both directions. Data Secure there uses the portable software AES (`KNX_AES_BACKEND_SOFTWARE`, no crypto library) and fixed-capacity Security Interface Object tables (`KNX_SECURITY_*`), so a secure download does not allocate after start-up; the size gate measures it (`tp1-small` + Data Secure on Cortex-M3). `TpUartMediumBackend` runs over a board's `knx_uart_hal_t` (standalone and bare-metal builds; there is no ESP-IDF UART HAL). The host tests run it against a scripted chip and against `VirtualTpUartChip`, a simulator of the chip on a virtual TP1 line (`-L tpuart`). What was observed on the chip, and what is still open, is in `docs/design/cortex-m-tpuart2-support.md` §3.8. |
| TP1 — NCN5120 | **Not implemented.** The TP-UART backend is structured for it (`TpUartFamily`), but the differences are not coded until a board can test them. |
| KNXnet/IP tunnelling (unicast) | Implemented; compiled out entirely unless `CONFIG_KNX_MEDIUM_IP` / `KNX_FEATURE_NETIP` is set. Validated on the wire against `xknx` in CI, as a client and as an interface's tunnelling server; **not yet against ETS or a commercial gateway.** |
| KNXnet/IP routing (multicast) | Implemented as a KNX IP medium (03/02/06), not only as a transport: Core discovery and self-description are answered, the system-broadcast domain is carried on ROUTING_SYSTEM_BROADCAST, the egress budget and ROUTING_BUSY back-off are enforced, and the Device Descriptor reports the KNX IP mask version. Wire formats are covered octet by octet by host tests against the specification's own layouts, and group communication is tested on the wire against `xknx`. **Run against ETS6 on hardware** (Habinari, ESP32-C6 on Wi-Fi) on an unsecured backbone; a secured backbone needs Device Management (below). |
| KNXnet/IP Core (server side) | SEARCH_REQUEST, SEARCH_REQUEST_EXTENDED and DESCRIPTION_REQUEST are answered, with the full Search Request Parameter set including the unknown-SRP rule. Mandatory for Device Class C (03/08/01 Table 2). Shares the routing socket, so it is present exactly when routing is. |
| KNXnet/IP Device Management (server side) | **Not implemented.** Mandatory for Device Class C. On an unsecured backbone a KNX IP end device does not need it: ETS commissions it over the KNX network. On a secured backbone every point-to-point frame is wrapped in a `SECURE_WRAPPER`, so a device that has no backbone key cannot be reached over the KNX network at all — and `PID_BACKBONE_KEY`, the write that would give it one, is itself such a frame. A unicast management connection is the only way in, which makes this the gap that stops the device being added to a secure installation without de-securing it first. Observed with ETS6 on hardware. |
| KNXnet/IP Tunnelling (server side) | **Not implemented**, and optional for Device Class C. The device is an end device on the medium, not a gateway; ETS cannot use it as an interface. |
| RF | Not implemented, and not planned — no RF hardware is available to develop or validate against. |
| Powerline | Not implemented, and not planned, for the same reason. |

## Known gaps for certification

Ranked by how much stands between the current state and a KNX-certifiable
device:

1. **No KNX Conformance Test Tool run.** The repository has one conformance
   evidence test. Certification requires the official test suite against real
   hardware.
2. **The IP-side secure paths are only partly exercised.** KNXnet/IP Secure
   Tunnelling is tested on the wire against `xknx`, but not against ETS or a
   commercial gateway. Secure Routing is verified against the specification's
   CCM vectors and host tests only, and a secured backbone cannot be
   commissioned until KNXnet/IP Device Management exists (see the IP table
   above).

   Data Secure itself is *not* in this list: secure commissioning, a full ETS6
   secure download and secured group communication have completed on hardware
   over both TP1 backends, which
   exercised the `.knxprod` declarations (`IsSecureEnabled`, the
   `MaxSecurity*Entries` table sizes, `SupportsExtendedMemoryServices` /
   `SupportsExtendedPropertyServices`) and the extended memory and property
   services that carry the key tables.
3. **`A_PropertyExtValue_InfoReport` is not emitted.** Listed `M` in Profiles
   §9.1.2.3.2. It is device-initiated, so nothing rejects a device that never
   sends one, but a strict profile review would flag it.
4. **Property access levels are not populated from the profile tables.**
   `InterfaceObjectManager::describeProperty` reports 0 / 0 for every
   property. Profiles §9.1.2.6.2
   specifies real values — PID_PROGMODE is 3/2, for example. The property write
   services compare the partner's current level with the described write level
   in wire numbering (0 = maximum access; refused when the partner's level is
   numerically higher), so populating the levels means carrying wire-order
   numbers through the property handlers and `describeProperty`; the
   authorisation service already works in wire numbering.
5. **Coupler profile is implemented, untested, and not complete.** None of the
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
   - The layer-2 acknowledge of group frames (`GROUP_IACK_ROUT` and the
     Filter Table) is enforced: `TwoPortCoupler` installs a
     `physical::GroupAckFilter` per port, which the bit-bang ISR and the
     TP-UART backend evaluate. `PHYS_IACK` is only computed and reported
     through `TwoPortCoupler::setAckPolicyCallback()`: the backends decide the
     acknowledge of individually addressed frames in the receive interrupt and
     cannot consult a per-frame decision made in the coupler.
   - System broadcast is only distinguishable on media that carry the
     distinction; on TP1 it is indistinguishable from ordinary broadcast, so
     `ROUTE_LAST` is reachable only through the explicit `FrameClass` overload.
   - `PID_MAIN_LCCONFIG` and friends are applied to the live policy by
     `syncRouterRoutingConfig()`, which the application must call after a
     download; there is no automatic property-write hook. They are persisted
     (with `PID_FILTER_TABLE_USE`) and applied at start-up, and Master Reset
     02h/07h returns them to the configuration declared in
     `configureRouterRole()`. The filter table does not need this — it applies
     live through `PID_ROUTETABLE_CONTROL`, and its memory window accepts
     `A_Memory_Write` only while its load state is Loading (§5.3.1.5).
6. **HIL coverage is thin.** Hardware validation is manual, on two products; there is no automated hardware-in-the-loop test. Virtual TP1
   simulation does not substitute for electrical/timing compliance testing on
   real transceivers.
