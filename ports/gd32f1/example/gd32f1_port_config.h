/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Port configuration of the example (see ../src/gd32f1_config.h for every
 * option). The pins are those of the H8i8o module, where the port was
 * validated; change them for another board. */
#pragma once

/* The TP-UART2's SAVE output on PB15, low while the bus voltage fails. */
#define GD32F1_SAVE_PORT 1
#define GD32F1_SAVE_PIN 15
#define GD32F1_SAVE_ACTIVE_LOW 1

/* The example's own pins (not port options). */
#define EXAMPLE_OUTPUT_PORT 0 /* PA0: the switched output, active high */
#define EXAMPLE_OUTPUT_PIN 0
#define EXAMPLE_BUTTON_PORT 0 /* PA11: programming button, active low */
#define EXAMPLE_BUTTON_PIN 11
#define EXAMPLE_LED_PORT 0 /* PA12: programming LED, active high */
#define EXAMPLE_LED_PIN 12
