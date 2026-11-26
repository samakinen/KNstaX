# ETS product pipeline

| | |
|---|---|
| Status | Done, 26 November 2026. Current behaviour: [ETS export guide](../reference/ets_export_guide.md#building) |
| Goal | One way for every KNstaX firmware to produce, check and pack its ETS product file, with the packer replaceable |
| Builds on | `knx_commissioned_product()`, `tools/knxprod_exporter`, [parameter-model.md](parameter-model.md) |

## 1. Problem

Two firmwares built on KNstaX turned a `constexpr` product definition into a
`.knxprod` the same way at the core (`knx_commissioned_product()` → JSON →
`exporter.py` → `.knxprod.xml` → OpenKNXproducer), and each wrapped it in its own
scripts:

| | ESP-IDF firmware | Bare-metal firmware |
|---|---|---|
| XML produced | in every firmware build, by a nested host build of its own | by a separate host project, run by hand |
| Configuration the export saw | KNstaX defaults and a few `-D` options, not the firmware's `sdkconfig` | the firmware's fragments, composed a second time |
| MasterData removed by | a PowerShell script | a Python script, run by hand |
| OpenKNXproducer call | `-N`, tolerates the crash after the archive is written | typed by hand |

What that cost:

- **Duplication.** The MasterData workaround and the OpenKNXproducer quirks
  existed twice, in two languages, and a third product would have written them
  a third time.
- **Configuration drift.** A product file declares the table sizes of the build
  that exported it. In the ESP-IDF firmware they agreed with the device (256
  addresses, 512 associations) only because the KNstaX defaults happened to
  match its `sdkconfig`.
- **Forgotten version bumps.** ETS refuses a changed program under an
  application version it has imported. Both firmwares relied on someone
  remembering, and both track KNstaX `main`, so an exporter change could move
  both product files without anyone touching them.
- **A hard-wired packer.** Replacing OpenKNXproducer meant editing every
  firmware.

## 2. Decision

KNstaX owns the pipeline from the product definition to the signed archive. A
firmware declares its product with one call, `knx_ets_product()`
(`cmake/knx_ets_product.cmake`), and commits a product lock.

1. **Built with the firmware.** In a host build the product is built in place.
   In a cross build the function runs a nested host build
   (`cmake/ets_product_host`), so `cmake --build` (or `idf.py build`) produces
   the firmware and its product together.
2. **The firmware's configuration.** The nested build imports the firmware's
   resolved configuration: a standalone build's `knx.config`, or the
   `CONFIG_KNX_*` lines of an ESP-IDF `sdkconfig`. `knx_configure()` gained
   `IMPORT` for it (§3.1). `configs/ets-export.defconfig` (and
   `ets-export-secure.defconfig`) then make it a host build without media,
   drivers or mbedTLS, changing nothing the product declares.
3. **Fixed outputs.** `<name>.json`, `<name>.knxprod.xml` (self-contained,
   schema-validated when `xmlschema` is there), `<name>.ets.xml` (the exporter's
   new `ets` format: no MasterData, so nothing strips it afterwards), and
   `<name>.knxprod` where a packer is available.
4. **Product lock.** `tools/ets_product/ets_product.py lock` compares the
   product (SHA-256 of the canonical `.ets.xml`) and its application version
   with the firmware's `product.lock`; a change without a new version fails the
   build.
5. **Packer backends.** `ets_product.py pack` runs a backend (OpenKNXproducer
   today) on a copy of the input, then checks the archive it wrote. A build
   without a backend says how to pack and goes on.
6. **Dist.** `<name>_dist` collects the product and the firmware image with a
   manifest naming the commits and hashes.

## 3. Notes from the work

### 3.1 An ESP-IDF sdkconfig is not a standalone fragment

The first version passed the firmware's configuration as an ordinary fragment.
The bare-metal firmware's resolved `knx.config` resolved cleanly; the ESP-IDF
`sdkconfig` did not, for three reasons:

- `knx_genconfig.py` holds every fragment to its assignments ("does not
  stick"), and a resolved ESP-IDF configuration assigns choices a standalone
  build cannot make: `KNX_AES_BACKEND_ESP_HW`, and the `KNX_MEDIUM` choice,
  which a standalone build refuses outright (it selects media with
  `KNX_BUILD_*`).
- The IP menu (`KNX_IP_PORT`, the KNXnet/IP buffer sizes) depends on
  `KNX_MEDIUM_IP`, so in a standalone configuration it is invisible.
- A fragment of overrides cannot set the AES backend unconditionally: with Data
  Secure off the symbol is invisible and the assignment fails. Hence the
  separate `ets-export-secure.defconfig`.

A hand-kept list of ESP-IDF-only symbols to filter out would break silently on
the next one. `IMPORT` instead applies an imported configuration as far as the
standalone schema takes it and lists the rest in the configure output, for
example for the ESP-IDF KNXnet/IP image:

```
knx_genconfig: 9 imported setting(s) do not apply to this configuration:
CONFIG_KNX_MEDIUM_TP1, CONFIG_KNX_MEDIUM_IP, CONFIG_KNX_IP_PORT, …,
CONFIG_KNX_AES_BACKEND_SOFTWARE, CONFIG_KNX_AES_BACKEND_ESP_HW
```

None of those reaches the product file; a capacity that failed to apply would
show up in the same list.

### 3.2 Measurements

Every product file of both firmwares was built the old way and the new way and
compared:

| Product | JSON | `.knxprod.xml` |
|---|---|---|
| bare-metal, standard variant | identical | identical |
| bare-metal, extended variant | identical | identical |
| ESP-IDF, TP1 | identical | — |
| ESP-IDF, KNXnet/IP | identical | — |

OpenKNXproducer 4.3.12 packed the new `.ets.xml` of the standard variant into
an archive with the same signed application id (`M-00FA_A-0001-05-5A79`) as the
old process, and the archive check passed. Firmware image sizes did not change.

## 4. Alternatives

**Keep the scripts in each firmware.** Rejected for the duplication and drift
above.

**A separate repository for the product tools.** Not now. It pays off if the
packer grows a non-Python part (a .NET tool, a Rust signer) with its own
release cycle; the backend interface lets it move out without changing the
firmwares' call.

**An own signer.** A `.knxprod` needs an MD5 hash and an id fingerprint over a
.NET-specific serialisation of the application program, and an RSA-SHA1
signature of the manufacturer folder with ETS's private key. OpenKNXproducer
does neither itself; it calls `Knx.Ets.XmlSigning.dll` from the ETS
installation. The hash can be reimplemented (knx-rs-prod, a GPL-3.0 Rust
packer, claims byte-exact agreement with ETS); the key is not public and cannot
be shipped. Candidate backends, in order: knx-rs-prod as a tool once a product
of ours packed by it imports into ETS6; a thin tool of our own over the ETS
libraries if OpenKNXproducer keeps breaking. Signing without ETS's key is not
pursued.

**Packing in CI.** Not possible without ETS. CI builds, validates and checks
the lock.
