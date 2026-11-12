// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file interface_object.hpp
 * @brief Base class for KNX Interface Objects
 *
 * Every interface object knows its type and exposes its properties through a
 * Property Kernel binding. The handler table behind that binding is the one
 * declaration of the object's properties: which exist, their datatype, access
 * and size, and (by PID order) their property index.
 */

#pragma once

#include "knx/types.hpp"
#include "knx/objects/property_kernel.hpp"

namespace knx {
namespace objects {

class InterfaceObject {
public:
    virtual ~InterfaceObject() = default;

    /**
     * @brief The standardised interface object type (03/07/03 Table 1)
     */
    virtual InterfaceObjectType objectType() const = 0;

    /**
     * @brief Property Kernel binding: the object's property handlers
     */
    virtual KernelBinding kernelBinding() const = 0;
};

} // namespace objects
} // namespace knx
