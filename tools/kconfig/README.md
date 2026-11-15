# tools/kconfig

Build-time configuration for standalone CMake builds. `Kconfig` at the repository root is the
schema; ESP-IDF reads it directly, and everything else goes through this directory.

| File | What it is |
|---|---|
| `kconfiglib.py`, `menuconfig.py` | [Kconfiglib](https://github.com/ulfalizer/Kconfiglib) 14.1.0 by Ulf Magnusson, unmodified. Licence: ISC, text in `LICENSE.kconfiglib.txt`. |
| `knx_genconfig.py` | Resolves a top Kconfig and defconfig fragments into `knx/autoconf.h`, `knx_config.cmake` and `knx.config`. Fails on an unknown symbol or a value Kconfig does not honour. |
| `Kconfig.standalone.top` | The top file the generator reads for a standalone build: `Kconfig` plus `Kconfig.standalone`. |
| `test_knx_genconfig.py` | ctest `test_knx_genconfig` (label `script`). |

Why vendored: the build needs Kconfiglib at configure time, on hosts and in the toolchain image,
without `pip`. The ISC licence is compatible with the GPL. Python 3 is already a build requirement
(the `.knxprod` exporter). To update, replace the two files from the release tarball and re-run the
tests.

Run the generator in isolated mode; it adds its own directory to the import path:

```sh
python3 -I tools/kconfig/knx_genconfig.py --kconfig tools/kconfig/Kconfig.standalone.top \
    --fragment configs/profiles/tp1-small.defconfig --fragment my.defconfig --out-dir /tmp/knx-config
```

Fragments apply in order; a later one wins. `cmake/knx_config.cmake` (`knx_configure()`) does this
for CMake builds. `menuconfig.py` needs a terminal with curses (on Windows, the `windows-curses`
package); it is run through the `knx_menuconfig` target.
