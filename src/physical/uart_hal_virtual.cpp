// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/physical/uart_hal_virtual.hpp"

#include <algorithm>

namespace knx {
namespace physical {

namespace {

bool isSupportedParity(knx_uart_hal_parity_t parity)
{
    return parity == KNX_UART_HAL_PARITY_NONE || parity == KNX_UART_HAL_PARITY_EVEN ||
           parity == KNX_UART_HAL_PARITY_ODD;
}

} // namespace

UartHalVirtual::UartHalVirtual(size_t txCapacity)
    : _txCapacity(txCapacity)
{
}

void UartHalVirtual::bind(knx_uart_hal_t& outHal, Pins pins)
{
    outHal.context = this;
    outHal.ops.open = &openShim;
    outHal.ops.close = &closeShim;
    outHal.ops.write = &writeShim;
    outHal.ops.tx_free = &txFreeShim;
    outHal.ops.set_reset = pins.reset ? &setResetShim : nullptr;
    outHal.ops.read_save = pins.save ? &readSaveShim : nullptr;
}

bool UartHalVirtual::deliverRx(uint8_t octet, uint8_t flags)
{
    if (!_open || _rxCb == nullptr) {
        return false;
    }
    _inRxCallback = true;
    _rxCb(_rxContext, octet, flags, static_cast<uint32_t>(_nowUs));
    _inRxCallback = false;
    return true;
}

std::vector<uint8_t> UartHalVirtual::transmit(size_t maxOctets)
{
    const size_t count = std::min(maxOctets, _txQueue.size());
    std::vector<uint8_t> out(_txQueue.begin(), _txQueue.begin() + static_cast<std::ptrdiff_t>(count));
    _txQueue.erase(_txQueue.begin(), _txQueue.begin() + static_cast<std::ptrdiff_t>(count));
    _transmitted.insert(_transmitted.end(), out.begin(), out.end());
    return out;
}

std::vector<uint8_t> UartHalVirtual::transmitAll()
{
    return transmit(_txQueue.size());
}

bool UartHalVirtual::openShim(void* context,
                              const knx_uart_hal_config_t* config,
                              knx_uart_hal_rx_cb_t rx_cb,
                              void* rx_context)
{
    auto* self = static_cast<UartHalVirtual*>(context);
    if (self == nullptr || self->_open || config == nullptr || rx_cb == nullptr || config->baud == 0 ||
        !isSupportedParity(config->parity)) {
        return false;
    }
    self->_config = *config;
    self->_rxCb = rx_cb;
    self->_rxContext = rx_context;
    self->_open = true;
    ++self->_openCount;
    return true;
}

void UartHalVirtual::closeShim(void* context)
{
    auto* self = static_cast<UartHalVirtual*>(context);
    if (self == nullptr) {
        return;
    }
    self->_open = false;
    self->_rxCb = nullptr;
    self->_rxContext = nullptr;
    self->_txQueue.clear();
}

bool UartHalVirtual::writeShim(void* context, const uint8_t* data, size_t len)
{
    auto* self = static_cast<UartHalVirtual*>(context);
    if (self == nullptr) {
        return false;
    }
    if (!self->_open || data == nullptr || len == 0 || self->_txCapacity - self->_txQueue.size() < len) {
        ++self->_rejectedWrites;
        return false;
    }
    self->_txQueue.insert(self->_txQueue.end(), data, data + len);
    ++self->_acceptedWrites;
    if (self->_inRxCallback) {
        ++self->_writesFromRxCallback;
    }
    if (self->_txListener != nullptr) {
        self->_txListener(self->_txListenerContext);
    }
    return true;
}

size_t UartHalVirtual::txFreeShim(void* context)
{
    const auto* self = static_cast<const UartHalVirtual*>(context);
    if (self == nullptr || !self->_open) {
        return 0;
    }
    return self->_txCapacity - self->_txQueue.size();
}

bool UartHalVirtual::setResetShim(void* context, bool asserted)
{
    auto* self = static_cast<UartHalVirtual*>(context);
    if (self == nullptr) {
        return false;
    }
    self->_resetAsserted = asserted;
    return true;
}

bool UartHalVirtual::readSaveShim(void* context)
{
    const auto* self = static_cast<const UartHalVirtual*>(context);
    return self != nullptr && self->_saveAsserted;
}

} // namespace physical
} // namespace knx
