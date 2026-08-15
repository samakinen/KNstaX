# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen

cmake_minimum_required(VERSION 3.16)

# Build this app in-place against the local KNstaX component in the repo root,
# or fall back to a separate ESP-IDF app workspace with KNstaX under components/KNstaX.
get_filename_component(KNSTAX_COMPONENT_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if(NOT EXISTS "${KNSTAX_COMPONENT_DIR}/idf_component.yml")
    set(KNSTAX_COMPONENT_DIR "${CMAKE_CURRENT_LIST_DIR}/components/KNstaX")
endif()
set(EXTRA_COMPONENT_DIRS "${KNSTAX_COMPONENT_DIR}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(knstax_esp_idf_temperature_sensor)
