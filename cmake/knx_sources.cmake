# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen

# Single source of truth for the KNstaX translation units.
#
# Both build modes (ESP-IDF component and standalone CMake) consume these lists.
# Keeping one list is what makes media gating meaningful: if a group is not in
# the build, its objects never reach the linker in the first place.
#
# Paths are relative to the repository root.

# --- Always built --------------------------------------------------------
set(KNX_SOURCES_CORE
    # Data Link Layer
    "src/datalink/frame_codec.cpp"
    "src/datalink/tp1_data_link_layer.cpp"

    # Network Layer
    "src/network/network_layer.cpp"
    "src/network/routing_table.cpp"
    "src/network/filter_table.cpp"
    "src/network/routing_table_control.cpp"
    "src/network/coupler_routing.cpp"
    "src/network/two_port_coupler.cpp"

    # Transport Layer
    "src/transport/transport_layer.cpp"
    "src/transport/transport_layer_send.cpp"
    "src/transport/transport_layer_receive.cpp"
    "src/transport/connection_state.cpp"
    "src/transport/connection_table.cpp"

    # Application Layer
    "src/application/dpt.cpp"
    "src/application/group_object.cpp"
    "src/application/device_descriptor_service.cpp"
    "src/application/property_store.cpp"
    "src/application/property_services.cpp"
    "src/application/property_ext_services.cpp"
    "src/application/function_property_services.cpp"
    "src/application/address_space.cpp"
    "src/application/memory_service.cpp"
    "src/application/authorization_service.cpp"
    "src/application/restart_service.cpp"
    "src/application/network_parameter_service.cpp"
    "src/application/security_access_policy.cpp"
    "src/application/application_layer.cpp"

    # Objects
    "src/objects/interface_object.cpp"
    "src/objects/device_object.cpp"
    "src/objects/address_table_object.cpp"
    "src/objects/group_object_table_object.cpp"
    "src/objects/application_program_object.cpp"
    "src/objects/association_table_object.cpp"
    "src/objects/security_interface_object.cpp"
    "src/objects/generic_interface_object.cpp"
    "src/objects/object_property_manifest.cpp"
    "src/objects/reference_object_registry.cpp"
    "src/objects/object_property_compliance.cpp"
    "src/objects/object_persistence.cpp"
    "src/objects/interface_object_manager.cpp"

    # BAU
    "src/bau/bau.cpp"
    "src/bau/bau_stack_port.cpp"
    "src/bau/bau_group_runtime.cpp"
    "src/bau/bau_management.cpp"
    "src/bau/bau_go_diagnostics.cpp"

    # Utilities
    "src/util/log.cpp"
)

# --- TP1 medium ----------------------------------------------------------
set(KNX_SOURCES_TP1
    "src/physical/tp1_mac_physical.cpp"
    "src/physical/tp1_mac_controller.cpp"
    "src/physical/tp1_frame_codec.cpp"
    "src/physical/manchester_codec.cpp"
    "src/physical/bitbang_driver_timer_isr.cpp"
    "src/physical/bitbang_driver_timer_isr_espidf.cpp"
    "src/physical/bitbang_medium_backend_adapter.cpp"
    "src/physical/tpuart_medium_backend_adapter.cpp"
    "src/physical/timer_gpio_hal_espidf.cpp"
    "src/physical/timer_gpio_hal_virtual.cpp"
    "src/physical/physical_factory.cpp"
)

# --- KNXnet/IP medium ----------------------------------------------------
# Gated by KNX_FEATURE_NETIP.  A TP1-only device links none of this.
set(KNX_SOURCES_NETIP
    "src/physical/ip_physical_layer.cpp"
    "src/physical/ip_tunneling_physical.cpp"
    "src/physical/ip_routing_physical.cpp"
    "src/netip/cemi.cpp"
    "src/netip/header_codec.cpp"
    "src/netip/gateway_discovery_client.cpp"
    "src/netip/tunneling_session_client.cpp"
    "src/netip/tunneling_server_endpoint.cpp"
    "src/netip/tcp_frame_channel.cpp"
    "src/netip/udp_datagram_channel.cpp"
    "src/netip/device_management.cpp"
    "src/netip/device_management_codec.cpp"
    "src/netip/routing.cpp"
    "src/netip/routing_endpoint.cpp"
)

# Coupler/bridge code needs both media present.
set(KNX_SOURCES_TP1_NETIP_BRIDGE
    "src/network/tp1_ip_interface_bridge.cpp"
)

# --- KNX Secure ----------------------------------------------------------
# Data Secure (the on-bus S-AL security) needs only AES: CBC-MAC for
# authentication and CTR for confidentiality.
set(KNX_SOURCES_SECURE_DATA
    "src/security/data_secure.cpp"
    "src/security/secure_application_layer.cpp"
    "src/security/aes128_cbc_mac.cpp"
    "src/security/aes128_ctr.cpp"
)

# SHA-256, X25519 and PBKDF2 exist for IP Secure session establishment and
# .knxkeys import, not for Data Secure. They are kept separate so a TP1-only
# build does not pull in crypto it never calls.
set(KNX_SOURCES_SECURE_IP_CRYPTO
    "src/security/sha256.cpp"
    "src/security/x25519.cpp"
    "src/security/key_derivation.cpp"
)

set(KNX_SOURCES_SECURE
    ${KNX_SOURCES_SECURE_DATA}
    ${KNX_SOURCES_SECURE_IP_CRYPTO}
)

# IP Secure additionally requires the KNXnet/IP group.
set(KNX_SOURCES_SECURE_NETIP
    "src/physical/ip_secure_tunneling_physical.cpp"
    "src/netip/secure_wrapper.cpp"
    "src/netip/secure_session.cpp"
    "src/netip/secure_session_bootstrap.cpp"
    "src/netip/secure_routing_security.cpp"
    "src/netip/secure_tunneling_client.cpp"
)

# --- ETS import/migration tooling (host builds only) ---------------------
set(KNX_SOURCES_ETS
    "src/ets/ets_config_loader.cpp"
    "src/ets/ets_format_validator.cpp"
    "src/ets/ets_migration_tool.cpp"
    "src/ets/knxprog_importer.cpp"
)

# --- Virtual TP1 bus simulator (host tests; also usable on target) -------
set(KNX_SOURCES_VIRTUAL
    "src/physical/virtual_time_engine.cpp"
    "src/physical/virtual_tp1_bus_peer.cpp"
    "src/physical/virtual_tp1_trace.cpp"
    "src/physical/virtual_tp1_test_runtime.cpp"
    "src/platform/virtual_test_clock.cpp"
    "src/platform/virtual_platform.cpp"
)

# --- Platform backends ---------------------------------------------------
set(KNX_SOURCES_PLATFORM_ESPIDF
    "src/platform/esp32_platform.cpp"
    "src/platform/esp32_memory.cpp"
    "src/platform/esp32_network.cpp"
    "src/platform/esp32_uart.cpp"
    "src/platform/esp32_spi.cpp"
)

set(KNX_SOURCES_PLATFORM_LINUX
    "src/platform/linux_platform.cpp"
)

# TPUART front-end is only meaningful with the TP1 medium.
set(KNX_SOURCES_TPUART
    "src/physical/tpuart_physical.cpp"
)
