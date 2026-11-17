// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file application_program_object.hpp
 * @brief KNX Application Program Object (Interface Object Type 3)
 * 
 * Manages application program metadata and parameters.
 * Per KNX spec 3/7/2, provides program identification and version information.
 */

#pragma once

#include "knx/objects/interface_object.hpp"
#include "knx/types.hpp"
#include "knx/util/result.hpp"
#include <cstdint>
#include <functional>
#include <vector>
#include <string>
#include <span>

namespace knx {
namespace objects {

/**
 * @brief Standard KNX application program properties (PID_*)
 */
enum class AppProgramProperty : uint8_t {
    ObjectType = 1,              // PID_OBJECT_TYPE
    LoadState = 5,               // PID_LOAD_STATE_CONTROL
    TableReference = 7,          // PID_TABLE_REFERENCE (code segment address)
    ProgramVersion = 13,         // PID_PROGRAM_VERSION
    ApplicationId = 14,          // PID_APPLICATION_ID (manufacturer-specific)
    ApplicationVersion = 15,     // PID_APPLICATION_VERSION
    ApplicationNumber = 16,      // PID_APPLICATION_NUMBER
    ApplicationArea = 17,        // PID_APPLICATION_AREA
    ParameterStart = 18,         // PID_PARAMETER_START
    ParameterEnd = 19,           // PID_PARAMETER_END
    ProgramControl = 20,         // PID_PROGRAM_CONTROL
    ProgramState = 21,           // PID_PROGRAM_STATE
    ProgramData = 22,            // PID_PROGRAM_DATA
    ProgramName = 23,            // PID_PROGRAM_NAME
    ProgramDescription = 24,     // PID_PROGRAM_DESCRIPTION
    ApplicationManufacturer = 25 // PID_APPLICATION_MANUFACTURER
};

/**
 * @brief Program state per KNX spec
 */
enum class ProgramState : uint8_t {
    Inactive = 0x00,     // Program not active
    Active = 0x01,       // Program running
    Loading = 0x02,      // Program being loaded
    Unloading = 0x03,    // Program being unloaded
    Error = 0xFF         // Program in error state
};

/**
 * @brief Program control commands
 */
enum class ProgramControl : uint8_t {
    NoControl = 0x00,    // No action
    Start = 0x01,        // Start program
    Stop = 0x02,         // Stop program
    Reset = 0x03,        // Reset program
    Reload = 0x04        // Reload program
};

/**
 * @brief The product's parameter image (docs/design/parameter-model.md §3.4)
 *
 * The one buffer the ETS parameters live in. The product runtime owns it and
 * binds it before BusAccessUnit::init(). ETS memory writes land in it, the
 * product reads it through a view, and it is stored with its layout
 * fingerprint, so an image written for another layout is not taken into use.
 */
struct ParameterImageBinding {
    /// The image, in RAM. Outlives the BusAccessUnit.
    std::span<uint8_t> image{};
    /// The image with every parameter at its default: image.size() octets.
    const uint8_t* defaults{nullptr};
    /// Identifies the layout the image is read with.
    uint32_t fingerprint{0};
    /// True when every parameter of the image is inside its bounds. Null: the
    /// image is not checked.
    bool (*withinBounds)(std::span<const uint8_t> image){nullptr};
};

/**
 * @brief Application Program Object - Interface Object Type 3
 * 
 * Stores application program identification, version, and configuration.
 * Provides interface for ETS to identify and manage application programs.
 */
class ApplicationProgramObject : public InterfaceObject {
public:
    ApplicationProgramObject();
    ~ApplicationProgramObject() override = default;

    // === InterfaceObject interface ===
    InterfaceObjectType objectType() const override { return InterfaceObjectType::applicationProgram(); }
    KernelBinding kernelBinding() const override;

    // Disable copy, enable move
    ApplicationProgramObject(const ApplicationProgramObject&) = delete;
    ApplicationProgramObject& operator=(const ApplicationProgramObject&) = delete;
    ApplicationProgramObject(ApplicationProgramObject&&) = default;
    ApplicationProgramObject& operator=(ApplicationProgramObject&&) = default;

    // === Program Identification ===

    // PID_PROGRAM_VERSION (13) — PDT_GENERIC_05 per KNX 03.05.01: manufacturer
    // id (2 bytes), application number (2 bytes), version (1 byte). ETS writes
    // the whole block during the application download.
    static constexpr size_t kProgramVersionSize = 5;
    void setProgramVersionBlock(std::span<const uint8_t> block) {
        const size_t n = block.size() < kProgramVersionSize ? block.size() : kProgramVersionSize;
        std::copy_n(block.begin(), n, _programVersion.begin());
    }
    std::span<const uint8_t> getProgramVersionBlock() const { return _programVersion; }

    // PID_APPLICATION_ID (14) - Manufacturer-specific application ID
    void setApplicationId(uint16_t id) { _applicationId = id; }
    uint16_t getApplicationId() const { return _applicationId; }

    // PID_APPLICATION_VERSION (15)
    void setApplicationVersion(uint8_t version) { _applicationVersion = version; }
    uint8_t getApplicationVersion() const { return _applicationVersion; }

    // PID_APPLICATION_NUMBER (16)
    void setApplicationNumber(uint16_t number) { _applicationNumber = number; }
    uint16_t getApplicationNumber() const { return _applicationNumber; }

    // PID_APPLICATION_AREA (17)
    void setApplicationArea(uint8_t area) { _applicationArea = area; }
    uint8_t getApplicationArea() const { return _applicationArea; }

    // PID_APPLICATION_MANUFACTURER (25)
    void setApplicationManufacturer(uint16_t manufacturer) { _applicationManufacturer = manufacturer; }
    uint16_t getApplicationManufacturer() const { return _applicationManufacturer; }

    // === Program Metadata ===

    // PID_PROGRAM_NAME (23)
    void setProgramName(const std::string& name) { _programName = name; }
    const std::string& getProgramName() const { return _programName; }

    // PID_PROGRAM_DESCRIPTION (24)
    void setProgramDescription(const std::string& desc) { _programDescription = desc; }
    const std::string& getProgramDescription() const { return _programDescription; }

    // === Program State & Control ===

    // PID_PROGRAM_STATE (21)
    void setProgramState(ProgramState state) { _programState = state; }
    ProgramState getProgramState() const { return _programState; }

    // PID_PROGRAM_CONTROL (20)
    util::Result<void> executeProgramControl(ProgramControl control);

    // === Parameter Management ===

    // PID_PARAMETER_START (18) - Start address of parameter memory
    void setParameterStart(MemoryAddress address) { _parameterStart = address; }
    MemoryAddress getParameterStart() const { return _parameterStart; }

    // PID_PARAMETER_END (19) - End address of parameter memory
    void setParameterEnd(MemoryAddress address) { _parameterEnd = address; }
    MemoryAddress getParameterEnd() const { return _parameterEnd; }

    // Parameter data access
    void setParameterData(std::span<const uint8_t> data) { _parameterData.assign(data.begin(), data.end()); }
    std::span<const uint8_t> getParameterData() const { return _parameterData; }

    // Parameter size calculation
    uint16_t getParameterSize() const {
        if (_parameterStart.isZero() && _parameterEnd.isZero()) {
            return 0; // No parameters configured
        }
        return _parameterStart.distanceInclusive(_parameterEnd);
    }

    // === Program Data ===

    // PID_PROGRAM_DATA (22) - Application-specific program data. With a
    // parameter image bound this is the image: a write is cut to its size and
    // an empty write returns it to the defaults.
    void setProgramData(std::span<const uint8_t> data);
    std::span<const uint8_t> getProgramData() const
    {
        return hasParameterImage() ? std::span<const uint8_t>(_image.image) : std::span<const uint8_t>(_programData);
    }

    // === Parameter image ===

    /// Bind the product's parameter image and fill it with the defaults.
    /// Before BusAccessUnit::init(); the restore then replaces the defaults
    /// with a stored image of the same layout.
    void bindParameterImage(const ParameterImageBinding& binding);
    bool hasParameterImage() const { return _image.defaults != nullptr; }
    std::span<uint8_t> parameterImage() { return _image.image; }
    std::span<const uint8_t> parameterImage() const { return _image.image; }
    void resetParameterImage();
    /// False when the image holds a value outside its bounds.
    bool parameterImageWithinBounds() const
    {
        return _image.withinBounds == nullptr || _image.withinBounds(_image.image);
    }
    /// Take a just-written image into use: checked against the bounds; one
    /// that fails is replaced by the defaults and the load state becomes
    /// Error. Returns whether the image was accepted.
    bool acceptParameterImage();
    /// What is stored for the image: fingerprint and image while the
    /// application program is loaded, one zero octet otherwise.
    void appendParameterImageRecord(std::vector<uint8_t>& out) const;
    static constexpr size_t kParameterImageRecordOverhead = 4;
    /// Take a stored record into use. Anything but a record of this layout
    /// with every value inside its bounds leaves the defaults in place and
    /// the application program unloaded.
    void restoreParameterImageRecord(std::span<const uint8_t> record);

    /// Register a callback that is invoked whenever the KNX management model
    /// (e.g. ETS over TP1 or KNXnet/IP) writes new program data to this object.
    /// The span is valid only for the duration of the call.
    using ProgramDataChangedCallback = std::function<void(std::span<const uint8_t>)>;
    void setOnProgramDataChanged(ProgramDataChangedCallback callback)
    {
        _onProgramDataChanged = std::move(callback);
    }

    void notifyProgramDataChanged() const
    {
        if (_onProgramDataChanged) {
            _onProgramDataChanged(getProgramData());
        }
    }

    // === Load state (PID_LOAD_STATE_CONTROL = 5) ===
    // Driven by the ETS download procedure via LoadControlProperty. Loading
    // does not discard program metadata: ETS rewrites the fields it manages.
    uint8_t loadState() const { return _loadState; }
    void setLoadState(uint8_t state) { _loadState = state; }
    void loadControlReset() {}

    // === Validation ===

    bool isValid() const;
    bool hasParameters() const {
        return !(_parameterStart.isZero() && _parameterEnd.isZero()) && _parameterStart.isBeforeOrEqual(_parameterEnd);
    }
    bool isActive() const { return _programState == ProgramState::Active; }

private:
    // Program identification
    uint8_t _loadState{1};  // loadstate::kLoaded — firmware boots with a usable program
    std::array<uint8_t, kProgramVersionSize> _programVersion{};
    uint16_t _applicationId{0};
    uint8_t _applicationVersion{0};
    uint16_t _applicationNumber{0};
    uint8_t _applicationArea{0};
    uint16_t _applicationManufacturer{0};

    // Program metadata
    std::string _programName;
    std::string _programDescription;

    // Program state
    ProgramState _programState{ProgramState::Inactive};

    // Parameter memory
    MemoryAddress _parameterStart{MemoryAddress(0)};
    MemoryAddress _parameterEnd{MemoryAddress(0)};
    std::vector<uint8_t> _parameterData;

    // Program data
    std::vector<uint8_t> _programData;
    ParameterImageBinding _image{};
    ProgramDataChangedCallback _onProgramDataChanged;

    ValidationPolicy _validationPolicy{ValidationPolicy::OnWrite};
};

} // namespace objects
} // namespace knx
