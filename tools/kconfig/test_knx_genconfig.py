#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen
"""The configuration generator: what a profile resolves to, and what it refuses.

A fragment that names a symbol that does not exist, or asks for a value Kconfig
will not honour, must fail the build at configure time. The alternative is a
device built with something other than what the profile says, found out on the
bench.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
GEN = HERE / 'knx_genconfig.py'
TOP = HERE / 'Kconfig.standalone.top'
PROFILES = ROOT / 'configs' / 'profiles'


def _run(fragments, kconfig=TOP, out=None):
    """Run the generator; returns (returncode, stderr, out dir)."""
    out = out or Path(tempfile.mkdtemp(prefix='knx_genconfig_'))
    cmd = [sys.executable, '-I', str(GEN), '--kconfig', str(kconfig), '--out-dir', str(out)]
    for fragment in fragments:
        cmd += ['--fragment', str(fragment)]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    return proc.returncode, proc.stderr, out


def _fragment(text):
    handle = tempfile.NamedTemporaryFile('w', suffix='.defconfig', delete=False)
    handle.write(text)
    handle.close()
    return Path(handle.name)


def _header(out):
    return (out / 'knx' / 'autoconf.h').read_text().splitlines()


def test_host_test_profile_is_everything_on():
    rc, err, out = _run([PROFILES / 'host-test.defconfig'])
    assert rc == 0, err
    lines = _header(out)
    for macro in ('CONFIG_KNX_BUILD_TP1 1', 'CONFIG_KNX_BUILD_NETIP 1', 'CONFIG_KNX_ENABLE_COUPLER 1',
                  'CONFIG_KNX_ENABLE_TUNNELING 1', 'CONFIG_KNX_ENABLE_SECURITY 1',
                  'CONFIG_KNX_BUILD_VIRTUAL_BUS 1', 'CONFIG_KNX_BUILD_TEST_HOOKS 1',
                  'CONFIG_KNX_LOG_LEVEL 5'):
        assert f'#define {macro}' in lines, macro


def test_tp1_small_is_tp1_only_baremetal_without_logs():
    rc, err, out = _run([PROFILES / 'tp1-small.defconfig'])
    assert rc == 0, err
    text = '\n'.join(_header(out))
    assert '#define CONFIG_KNX_STANDALONE_PLATFORM_BAREMETAL 1' in text
    assert '#define CONFIG_KNX_BUILD_TP1 1' in text
    assert '#define CONFIG_KNX_LOG_LEVEL 0' in text
    for absent in ('CONFIG_KNX_BUILD_NETIP', 'CONFIG_KNX_ENABLE_TUNNELING', 'CONFIG_KNX_ENABLE_COUPLER',
                   'CONFIG_KNX_ENABLE_SECURITY', 'CONFIG_KNX_BUILD_VIRTUAL_BUS',
                   'CONFIG_KNX_BUILD_ETS_TOOLING', 'CONFIG_KNX_BUILD_TEST_HOOKS', 'CONFIG_KNX_BUILD_ZLIB',
                   'CONFIG_KNX_ENABLE_PROPERTY_EXT', 'CONFIG_KNX_ENABLE_MEMORY_EXT',
                   'CONFIG_KNX_ENABLE_NETWORK_PARAMETER', 'CONFIG_KNX_ENABLE_GO_DIAGNOSTICS', 'CONFIG_KNX_DPT_FLOAT',
                   'CONFIG_KNX_DPT_TIME', 'CONFIG_KNX_DPT_STRING', 'CONFIG_KNX_DPT_COLOUR', 'CONFIG_KNX_DPT_ACCESS',
                   'CONFIG_KNX_ENABLE_DPT_CATALOGUE'):
        assert absent not in text, f'{absent} must be absent when n'
    assert '#define CONFIG_KNX_ENABLE_FUNCTION_PROPERTIES 1' in text


CAPACITY_SYMBOLS = ('MAX_APDU_LENGTH', 'TP1_RX_QUEUE_FRAMES', 'TRANSPORT_RX_QUEUE', 'ADDRESS_TABLE_ENTRIES',
                    'ASSOCIATION_TABLE_ENTRIES', 'ADDITIONAL_INDIVIDUAL_ADDRESSES',
                    'MAX_GROUP_OBJECT_PAYLOAD_BYTES', 'SEND_PIPELINE_DEPTH')


def _capacities(profile):
    rc, err, out = _run([PROFILES / f'{profile}.defconfig'])
    assert rc == 0, err
    values = {}
    for line in _header(out):
        for symbol in CAPACITY_SYMBOLS:
            prefix = f'#define CONFIG_KNX_{symbol} '
            if line.startswith(prefix):
                values[symbol] = int(line[len(prefix):])
    return values


def _without_capacities(profile):
    _, _, out = _run([PROFILES / f'{profile}.defconfig'])
    return {line for line in _header(out)
            if not any(line.startswith(f'#define CONFIG_KNX_{s} ') for s in CAPACITY_SYMBOLS)}


OPTIONAL_FEATURES = ('KNX_ENABLE_FUNCTION_PROPERTIES', 'KNX_ENABLE_PROPERTY_EXT', 'KNX_ENABLE_MEMORY_EXT',
                     'KNX_ENABLE_NETWORK_PARAMETER', 'KNX_ENABLE_GO_DIAGNOSTICS', 'KNX_DPT_FLOAT', 'KNX_DPT_TIME',
                     'KNX_DPT_STRING', 'KNX_DPT_COLOUR', 'KNX_DPT_ACCESS', 'KNX_ENABLE_DPT_CATALOGUE')


def _features(profile):
    """The optional-feature macros (and the coupler and Secure) a profile turns on."""
    _, _, out = _run([PROFILES / f'{profile}.defconfig'])
    wanted = OPTIONAL_FEATURES + ('KNX_ENABLE_COUPLER', 'KNX_ENABLE_SECURITY')
    return {s for s in wanted if f'#define CONFIG_{s} 1' in _header(out)}


def test_tp1_standard_differs_from_small_only_in_log_level_features_and_capacities():
    diff = _without_capacities('tp1-small') ^ _without_capacities('tp1-standard')
    # Secure joins tp1-standard with the portable AES backend (K6).
    features = {f'#define CONFIG_{s} 1' for s in OPTIONAL_FEATURES
                if s not in ('KNX_ENABLE_FUNCTION_PROPERTIES', 'KNX_ENABLE_DPT_CATALOGUE')}
    assert diff == {'#define CONFIG_KNX_LOG_LEVEL 0', '#define CONFIG_KNX_LOG_LEVEL 1'} | features, diff


def test_optional_features_per_profile():
    everything = set(OPTIONAL_FEATURES) | {'KNX_ENABLE_COUPLER', 'KNX_ENABLE_SECURITY'}
    for profile in ('full', 'host-test', 'host-small'):
        assert _features(profile) == everything, profile
    # Only the function property services stay on in a small end device.
    assert _features('tp1-small') == {'KNX_ENABLE_FUNCTION_PROPERTIES'}
    assert _features('host-minimal') == {'KNX_ENABLE_FUNCTION_PROPERTIES'}
    # The standard end device: the services, no coupler, no run-time catalogue
    # (and no Secure until K6 gives bare metal an AES backend).
    assert _features('tp1-standard') == set(OPTIONAL_FEATURES) - {'KNX_ENABLE_DPT_CATALOGUE'}


def test_optional_features_default_to_on_when_unset():
    # A product that sets none of them (the ESP-IDF default case) keeps today's behaviour.
    rc, err, out = _run([_fragment('CONFIG_KNX_BUILD_TP1=y\n')])
    assert rc == 0, err
    lines = _header(out)
    for symbol in OPTIONAL_FEATURES:
        assert f'#define CONFIG_{symbol} 1' in lines, symbol
    assert '#define CONFIG_KNX_ENABLE_COUPLER 1' not in lines


def test_secure_needs_extended_property_services():
    # Kconfig `select` raises KNX_ENABLE_PROPERTY_EXT with Secure; turning it off as well is refused.
    rc, err, out = _run([PROFILES / 'tp1-small.defconfig', _fragment('CONFIG_KNX_ENABLE_SECURITY=y\n')])
    assert rc != 0 or '#define CONFIG_KNX_ENABLE_PROPERTY_EXT 1' in _header(out), err
    if rc != 0:
        assert 'KNX_ENABLE_PROPERTY_EXT' in err or 'KNX_ENABLE_SECURITY' in err, err


def test_group_object_diagnostics_need_function_properties():
    bad = _fragment('CONFIG_KNX_ENABLE_SECURITY=n\nCONFIG_KNX_ENABLE_FUNCTION_PROPERTIES=n\nCONFIG_KNX_ENABLE_GO_DIAGNOSTICS=y\n')
    rc, err, _ = _run([PROFILES / 'host-test.defconfig', bad])
    assert rc != 0 and 'KNX_ENABLE_GO_DIAGNOSTICS' in err, err


def test_coupler_needs_function_properties():
    bad = _fragment('CONFIG_KNX_ENABLE_SECURITY=n\nCONFIG_KNX_ENABLE_FUNCTION_PROPERTIES=n\nCONFIG_KNX_ENABLE_COUPLER=y\n')
    rc, err, _ = _run([PROFILES / 'host-test.defconfig', bad])
    assert rc != 0 and 'KNX_ENABLE_COUPLER' in err, err


def test_capacities_per_profile():
    full = dict(MAX_APDU_LENGTH=254, TP1_RX_QUEUE_FRAMES=32, TRANSPORT_RX_QUEUE=8, ADDRESS_TABLE_ENTRIES=256,
                ASSOCIATION_TABLE_ENTRIES=512, ADDITIONAL_INDIVIDUAL_ADDRESSES=16,
                MAX_GROUP_OBJECT_PAYLOAD_BYTES=16, SEND_PIPELINE_DEPTH=8)
    small = dict(MAX_APDU_LENGTH=15, TP1_RX_QUEUE_FRAMES=4, TRANSPORT_RX_QUEUE=2, ADDRESS_TABLE_ENTRIES=128,
                 ASSOCIATION_TABLE_ENTRIES=160, ADDITIONAL_INDIVIDUAL_ADDRESSES=0,
                 MAX_GROUP_OBJECT_PAYLOAD_BYTES=14, SEND_PIPELINE_DEPTH=2)
    standard = dict(MAX_APDU_LENGTH=55, TP1_RX_QUEUE_FRAMES=8, TRANSPORT_RX_QUEUE=4, ADDRESS_TABLE_ENTRIES=255,
                    ASSOCIATION_TABLE_ENTRIES=400, ADDITIONAL_INDIVIDUAL_ADDRESSES=4,
                    MAX_GROUP_OBJECT_PAYLOAD_BYTES=14, SEND_PIPELINE_DEPTH=4)
    assert _capacities('full') == full
    assert _capacities('host-test') == full
    assert _capacities('tp1-small') == small
    assert _capacities('host-small') == small
    assert _capacities('tp1-standard') == standard


def test_capacity_defaults_equal_full_when_unset():
    # A product that sets no capacity (the ESP-IDF default case) gets today's values.
    rc, err, out = _run([_fragment('CONFIG_KNX_BUILD_TP1=y\n')])
    assert rc == 0, err
    text = '\n'.join(_header(out))
    for macro in ('CONFIG_KNX_MAX_APDU_LENGTH 254', 'CONFIG_KNX_TP1_RX_QUEUE_FRAMES 32',
                  'CONFIG_KNX_TRANSPORT_RX_QUEUE 8', 'CONFIG_KNX_ADDRESS_TABLE_ENTRIES 256',
                  'CONFIG_KNX_ASSOCIATION_TABLE_ENTRIES 512', 'CONFIG_KNX_ADDITIONAL_INDIVIDUAL_ADDRESSES 16',
                  'CONFIG_KNX_SEND_PIPELINE_DEPTH 8'):
        assert f'#define {macro}' in text, macro


def test_capacity_out_of_range_is_refused():
    rc, err, _ = _run([_fragment('CONFIG_KNX_MAX_APDU_LENGTH=14\n')])
    assert rc != 0 and 'MAX_APDU_LENGTH' in err, err


def test_every_profile_resolves():
    for profile in sorted(PROFILES.glob('*.defconfig')):
        rc, err, _ = _run([profile])
        assert rc == 0, f'{profile.name}: {err}'


def test_override_applies_after_the_profile():
    override = _fragment('CONFIG_KNX_LOG_LEVEL=2\nCONFIG_KNX_RESTART_PROCESS_TIME_S=30\n')
    rc, err, out = _run([PROFILES / 'tp1-small.defconfig', override])
    assert rc == 0, err
    lines = _header(out)
    assert '#define CONFIG_KNX_LOG_LEVEL 2' in lines
    assert '#define CONFIG_KNX_RESTART_PROCESS_TIME_S 30' in lines
    assert '#define CONFIG_KNX_LOG_LEVEL 0' not in lines
    cmake = (out / 'knx_config.cmake').read_text()
    assert 'set(CONFIG_KNX_LOG_LEVEL 2)' in cmake
    assert 'set(CONFIG_KNX_BUILD_NETIP n)' in cmake
    assert 'set(CONFIG_KNX_BUILD_TP1 y)' in cmake
    assert 'CONFIG_KNX_LOG_LEVEL=2' in (out / 'knx.config').read_text()


def test_legacy_style_override_fragment():
    # What knx_configure() writes for -DKNX_FEATURE_NETIP=OFF -DKNX_SECURE_ENABLED=OFF.
    legacy = _fragment('CONFIG_KNX_BUILD_NETIP=n\nCONFIG_KNX_ENABLE_SECURITY=n\n')
    rc, err, out = _run([PROFILES / 'host-test.defconfig', legacy])
    assert rc == 0, err
    text = '\n'.join(_header(out))
    assert 'CONFIG_KNX_BUILD_NETIP' not in text and 'CONFIG_KNX_ENABLE_SECURITY' not in text
    # Tunnelling follows the medium: it cannot be on without KNXnet/IP.
    assert 'CONFIG_KNX_ENABLE_TUNNELING' not in text
    assert 'CONFIG_KNX_BUILD_TP1 1' in text


def test_unknown_symbol_fails_and_is_named():
    bad = _fragment('CONFIG_KNX_NO_SUCH_THING=y\n')
    rc, err, _ = _run([PROFILES / 'host-test.defconfig', bad])
    assert rc != 0
    assert 'CONFIG_KNX_NO_SUCH_THING' in err and 'unknown symbol' in err, err


def test_out_of_range_value_fails_and_is_named():
    bad = _fragment('CONFIG_KNX_LOG_LEVEL=9\n')
    rc, err, _ = _run([PROFILES / 'host-test.defconfig', bad])
    assert rc != 0
    assert 'CONFIG_KNX_LOG_LEVEL' in err and 'does not stick' in err, err


def test_value_that_a_dependency_hides_fails():
    # The IP menu is only visible under the IP medium, which a standalone build never selects.
    bad = _fragment('CONFIG_KNX_IP_PORT=3672\n')
    rc, err, _ = _run([PROFILES / 'host-test.defconfig', bad])
    assert rc != 0
    assert 'CONFIG_KNX_IP_PORT' in err and 'not visible' in err, err


def test_tunnelling_without_knxnet_ip_fails():
    bad = _fragment('CONFIG_KNX_BUILD_NETIP=n\nCONFIG_KNX_ENABLE_TUNNELING=y\n')
    rc, err, _ = _run([PROFILES / 'host-test.defconfig', bad])
    assert rc != 0
    assert 'CONFIG_KNX_ENABLE_TUNNELING' in err, err


def test_malformed_line_fails():
    bad = _fragment('KNX_LOG_LEVEL=1\n')
    rc, err, _ = _run([bad])
    assert rc != 0 and 'not a CONFIG_X=value line' in err, err


def test_ip_medium_is_refused_in_a_standalone_configuration():
    bad = _fragment('CONFIG_KNX_MEDIUM_IP=y\n')
    rc, err, _ = _run([PROFILES / 'host-test.defconfig', bad])
    assert rc != 0 and 'KNX_MEDIUM_IP' in err, err


def test_esp_idf_view_of_kconfig_parses_without_warnings():
    rc, err, out = _run([], kconfig=ROOT / 'Kconfig')
    assert rc == 0, err
    assert 'warning' not in err, err
    lines = _header(out)
    assert '#define CONFIG_KNX_MEDIUM_TP1 1' in lines
    assert not any('CONFIG_KNX_BUILD_' in line for line in lines), 'standalone symbols leaked into the ESP-IDF schema'


def test_unchanged_output_is_not_rewritten():
    rc, err, out = _run([PROFILES / 'tp1-small.defconfig'])
    assert rc == 0, err
    header = out / 'knx' / 'autoconf.h'
    before = header.stat().st_mtime_ns
    rc, err, _ = _run([PROFILES / 'tp1-small.defconfig'], out=out)
    assert rc == 0, err
    assert header.stat().st_mtime_ns == before, 'an identical re-run must leave the header alone'


def main():
    failures = 0
    for name, test in sorted(globals().items()):
        if not name.startswith('test_') or not callable(test):
            continue
        try:
            test()
            print(f'  ok  {name}')
        except AssertionError as error:
            failures += 1
            print(f'FAIL  {name}: {error}')
    if failures:
        print(f'{failures} genconfig test(s) failed')
        return 1
    print('genconfig tests passed')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
