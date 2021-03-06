/*
 * Copyright  2008-2009 INRIA/SensTools
 *
 * <dev-team@sentools.info>
 *
 * This software is a set of libraries designed to develop applications
 * for the WSN430 embedded hardware platform.
 *
 * This software is governed by the CeCILL license under French law and
 * abiding by the rules of distribution of free software.  You can  use,
 * modify and/ or redistribute the software under the terms of the CeCILL
 * license as circulated by CEA, CNRS and INRIA at the following URL
 * "http://www.cecill.info".
 *
 * As a counterpart to the access to the source code and  rights to copy,
 * modify and redistribute granted by the license, users are provided only
 * with a limited warranty  and the software's author,  the holder of the
 * economic rights,  and the successive licensors  have only  limited
 * liability.
 *
 * In this respect, the user's attention is drawn to the risks associated
 * with loading,  using,  modifying and/or developing or reproducing the
 * software by the user in light of its specific status of free software,
 * that may mean  that it is complicated to manipulate,  and  that  also
 * therefore means  that it is reserved for developers  and  experienced
 * professionals having in-depth computer knowledge. Users are therefore
 * encouraged to load and test the software's suitability as regards their
 * requirements in conditions enabling the security of their systems and/or
 * data to be ensured and,  more generally, to use and operate it in the
 * same conditions as regards security.
 *
 * The fact that you are presently reading this means that you have had
 * knowledge of the CeCILL license and that you accept its terms.
 */

/* Global includes */
#include <io.h>
#include <stdlib.h>
#include <stdio.h>

/* Scheduler includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

/* Project Includes */
#include "leds.h"
#include "phy.h"

/* Drivers Include */
#include "cc1101.h"
#include "spi1.h"
#include "timerB.h"

#define DEBUG 0
#if DEBUG==1
#define PRINTF printf
#else
#define PRINTF(...)
#endif

/* Type def */
enum phy_state {
	IDLE = 1, RX = 2, TX = 3
};

#define PHY_MAX_LENGTH 125
#define PHY_FOOTER_LENGTH 2

#define TX_MAX_DURATION 190

/* Function Prototypes */
static void cc1101_task(void* param);
static void cc1101_driver_init(void);
static void handle_received_frame(void);
static void restore_state(void);

/* CC1101 callback functions */
static uint16_t sync_irq(void);
/* static uint16_t rx_irq(void); */     /* unused */

/* Static variables */
static xSemaphoreHandle rx_sem, spi_mutex;
static phy_rx_callback_t rx_cb;
static uint8_t radio_channel, radio_power;
static volatile enum phy_state state, old_state;
static volatile uint16_t sync_word_time;
static uint8_t rx_data[PHY_MAX_LENGTH + PHY_FOOTER_LENGTH];
static uint16_t rx_data_length;

void phy_init(xSemaphoreHandle spi_m, phy_rx_callback_t callback,
		uint8_t channel, enum phy_tx_power power) {
	// Store the SPI mutex
	spi_mutex = spi_m;

	// Store the callback
	rx_cb = callback;

	// Store the channel
	radio_channel = channel;

	// Store the TX power
	switch (power) {
	case PHY_TX_0dBm:
		radio_power = CC1101_868MHz_TX_0dBm;
		break;
	case PHY_TX_m5dBm:
		radio_power = CC1101_868MHz_TX_m6dBm;   // No exact match so using the nearest
		break;
	case PHY_TX_m10dBm:
		radio_power = CC1101_868MHz_TX_m10dBm;
		break;
	case PHY_TX_m20dBm:
		radio_power = CC1101_868MHz_TX_m20dBm;
		break;
	default:
		radio_power = CC1101_868MHz_TX_0dBm;
		break;
	}

	// Initialize the semaphore
	vSemaphoreCreateBinary(rx_sem);

	// Set initial state
	state = IDLE;

	// Create the task
	xTaskCreate(cc1101_task, (const signed char*) "phy_cc1101", configMINIMAL_STACK_SIZE, NULL, configMAX_PRIORITIES-1, NULL);
}

void phy_rx(void) {
	// Take the SPI mutex
	xSemaphoreTake(spi_mutex, portMAX_DELAY);

	// set IDLE state, flush and calibrate
	cc1101_cmd_idle();
	cc1101_cmd_flush_rx();
	cc1101_cmd_flush_tx();

	// Set gdo2 RXFIFO
	cc1101_cfg_gdo2(CC1101_GDOx_RX_FIFO);

	// Start RX
	cc1101_cmd_rx();

	// Release Semaphore
	xSemaphoreGive(spi_mutex);

	// Update state
	state = RX;
	old_state = RX;
}

void phy_idle(void) {
	// Take the Semaphore
	xSemaphoreTake(spi_mutex, portMAX_DELAY);

	// Disable interrupts
	cc1101_gdo2_int_disable();

	// Set Idle
	cc1101_cmd_idle();
	cc1101_cmd_calibrate();

	// Release semaphore
	xSemaphoreGive(spi_mutex);

	// Update state
	state = IDLE;
	old_state = IDLE;
}

uint16_t phy_send(uint8_t* data, uint16_t length, uint16_t *timestamp) {
	uint8_t tx_length, *tx_data;

	if (length > PHY_MAX_LENGTH) {
		return 0;
	}
	state = TX;

	tx_data = data;
	tx_length = length;

	// Take semaphore
	xSemaphoreTake(spi_mutex, portMAX_DELAY);

	// Stop radio and flush TX FIFO
	cc1101_cmd_idle();
	cc1101_cmd_flush_tx();

	// Configure gdo2 to TX FIFO
	cc1101_cfg_gdo2(CC1101_GDOx_TX_FIFO);

	// Send length byte and first set
	length = tx_length > 63 ? 63 : tx_length;

	// Start TX
	cc1101_cmd_tx();

	cc1101_fifo_put(&tx_length, 1);
	cc1101_fifo_put(tx_data, length);
	tx_data += length;
	tx_length -= length;

	// Loop for sending everything
	while (tx_length != 0) {
		if (cc1101_gdo2_read() == 0) {
			length = tx_length > 31 ? 31 : tx_length;
			cc1101_fifo_put(tx_data, length);
			tx_data += length;
			tx_length -= length;
		}
	}

	// Wait while there are bytes in TX FIFO and EOP has not happened
	while (cc1101_status_txbytes() || cc1101_gdo0_read()) {
		nop();
	}

	// Release semaphore
	xSemaphoreGive(spi_mutex);

	// Set timestamp if required
	if (timestamp != 0x0) {
		*timestamp = sync_word_time;
	}

	restore_state();
	return 1;
}

uint16_t phy_send_cca(uint8_t* data, uint16_t length, uint16_t *timestamp) {
	// TODO real cca
	return phy_send(data, length, timestamp);
}

uint16_t phy_get_max_tx_duration(void) {
	return TX_MAX_DURATION;
}
uint16_t phy_get_estimate_tx_duration(uint8_t length) {
#define OVERHEAD 9 // Add 4 preamble, 2 sync word, 1 length, 2 CRC
	uint16_t duration;
	length += OVERHEAD;
	uint16_t length16 = length;

	// TX MAX DURATION corresponds to a frame a 125 payload bytes,
	// hence 134 real bytes

	duration = (TX_MAX_DURATION * length16) / (PHY_MAX_LENGTH +OVERHEAD);

	if (duration < (TX_MAX_DURATION / 8)) {
		duration = TX_MAX_DURATION / 8;
	} else if (duration > TX_MAX_DURATION) {
		duration = TX_MAX_DURATION;
	}
	return duration;
}

static void cc1101_task(void* param) {
	// Initialize the radio
	cc1101_driver_init();

	// Make sure the semaphore is taken
	xSemaphoreTake(rx_sem, 0);

	// Infinite loop for handling received frames
	while (1) {
		if (xSemaphoreTake(rx_sem, portMAX_DELAY) == pdTRUE) {
			handle_received_frame();
		}
	}
}

static void cc1101_driver_init(void) {
	// Take the SPI mutex before any radio access
	xSemaphoreTake(spi_mutex, portMAX_DELAY);

	// Initialize the radio driver
	cc1101_init();
	cc1101_cmd_idle();

	// configure the radio behavior
	cc1101_cfg_append_status(CC1101_APPEND_STATUS_ENABLE);
	cc1101_cfg_crc_autoflush(CC1101_CRC_AUTOFLUSH_DISABLE);
	cc1101_cfg_white_data(CC1101_DATA_WHITENING_ENABLE);
	cc1101_cfg_crc_en(CC1101_CRC_CALCULATION_ENABLE);
	cc1101_cfg_freq_if(0x06);
	cc1101_cfg_fs_autocal(CC1101_AUTOCAL_NEVER);
	cc1101_cfg_mod_format(CC1101_MODULATION_MSK);
	cc1101_cfg_sync_mode(CC1101_SYNCMODE_30_32);
	cc1101_cfg_manchester_en(CC1101_MANCHESTER_DISABLE);

	// set channel bandwidth (500 kHz)
	cc1101_cfg_chanbw_e(0);
	cc1101_cfg_chanbw_m(2);

	// set channel spacing 200kHz
	cc1101_cfg_chanspc_e(0x2);
	cc1101_cfg_chanspc_m(0xE5);

	// set data rate (0xD/0x2F is 250kbps)
	cc1101_cfg_drate_e(0x0D);
	cc1101_cfg_drate_m(0x2F);

	// go to idle after RX and TX
	cc1101_cfg_rxoff_mode(CC1101_RXOFF_MODE_IDLE);
	cc1101_cfg_txoff_mode(CC1101_TXOFF_MODE_IDLE);

	// Set channel
	cc1101_cfg_chan(radio_channel);

	// Set FIFO threshold to middle
	cc1101_cfg_fifo_thr(7);

	// Set gdo0 SYNC word detection (both RX and TX)
	cc1101_gdo0_int_disable();
	cc1101_cfg_gdo0(CC1101_GDOx_SYNC_WORD);
	cc1101_gdo0_int_set_rising_edge();
	cc1101_gdo0_register_callback(sync_irq);
	cc1101_gdo0_int_enable();

	// Calibrate a first time
	cc1101_cmd_calibrate();

	// Configure TX power
	cc1101_cfg_patable(&radio_power, 1);
	cc1101_cfg_pa_power(0);

	// Release mutex
	xSemaphoreGive(spi_mutex);
}

static void restore_state(void) {
	// Reset old state
	switch (old_state) {
	case RX:
		phy_rx();
		break;
	case IDLE:
	default:
		phy_idle();
		break;
	}
}

static void handle_received_frame(void) {
	uint8_t rx_length, length, *rx_ptr;

	// Take semaphore
	xSemaphoreTake(spi_mutex, portMAX_DELAY);

	// Check if there is at least one byte in fifo
	if (cc1101_status_rxbytes() == 0) {
		xSemaphoreGive(spi_mutex);
		restore_state();
		PRINTF("[PHY] no byte\n");
		return;
	}

	// Get length byte
	cc1101_fifo_get(&rx_length, 1);

	// Check length
	if (rx_length > PHY_MAX_LENGTH) {
		xSemaphoreGive(spi_mutex);
		restore_state();
		PRINTF("[PHY] length too big\n");
		return;
	}

	rx_data_length = rx_length;

	// Add 2 to the length for the status bytes
	rx_length += PHY_FOOTER_LENGTH;
	rx_ptr = rx_data;

	// Loop until end of packet
	while (cc1101_gdo0_read()) {
		// get the bytes in FIFO
		length = cc1101_status_rxbytes();

		// Check for overflow
		if (length & 0x80) {
			// Release semaphore
			xSemaphoreGive(spi_mutex);

			restore_state();
			PRINTF("[PHY] overflow\n");
			return;
		}

		// Check for local overflow
		if (length > rx_length) {
			// Release semaphore
			xSemaphoreGive(spi_mutex);
			restore_state();
			PRINTF("[PHY] local overflow\n");
			return;
		}

		// Read every byte but one, to prevent CC1101 bug.
		length -= 1;
		cc1101_fifo_get(rx_ptr, length);
		rx_ptr += length;
		rx_length -= length;

		// Wait until FIFO is filled above threshold, or EOP
		while (!cc1101_gdo2_read() && cc1101_gdo0_read()) {
			;
		}
	}

	// Packet complete, get the end
	length = cc1101_status_rxbytes();

	// Check for overflow
	if (length & 0x80) {
		// Release semaphore
		xSemaphoreGive(spi_mutex);
		restore_state();
		PRINTF("[PHY] overflow\n");
		return;
	}

	// Check for local overflow
	if (length > rx_length) {
		// Release semaphore
		xSemaphoreGive(spi_mutex);
		restore_state();
		PRINTF("[PHY] local overflow\n");
		return;
	}

	// Get the bytes
	cc1101_fifo_get(rx_ptr, length);
	rx_ptr += length;

	// Release semaphore
	xSemaphoreGive(spi_mutex);

	// Check CRC
	if ((rx_data[rx_data_length + 1] & 0x80) == 0) {
		// Bad CRC
		restore_state();
		PRINTF("[PHY] bad crc\n");
		return;
	}

	// Get RSSI
	int16_t rssi;
	rssi = rx_data[rx_data_length];
	if (rssi > 128) {
		rssi -= 256;
	}
	rssi -= 148;
	rssi /= 2;

	// Call callback function if any
	if (rx_cb) {
		rx_cb(rx_data, rx_data_length, (int8_t) rssi, sync_word_time);
	}

	// Restore state
	restore_state();
}

static uint16_t sync_irq(void) {
	sync_word_time = TBR;

	if (state == RX) {
		portBASE_TYPE yield;
		if (xSemaphoreGiveFromISR(rx_sem, &yield) == pdTRUE) {
#if configUSE_PREEMPTION
			if (yield) {
				portYIELD();
			}
#endif
		}
		return 1;
	}
	return 0;
}
