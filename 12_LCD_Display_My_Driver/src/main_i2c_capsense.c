/**************************************************************************//**
 * @file
 * @brief Empty Project
 * @author Energy Micro AS
 * @version 3.20.2
 ******************************************************************************
 * @section License
 * <b>(C) Copyright 2014 Silicon Labs, http://www.silabs.com</b>
 *******************************************************************************
 *
 * This file is licensed under the Silicon Labs Software License Agreement. See 
 * "http://developer.silabs.com/legal/version/v11/Silicon_Labs_Software_License_Agreement.txt"  
 * for details. Before using this software for any purpose, you must agree to the 
 * terms of that agreement.
 *
 ******************************************************************************/
#include "em_device.h"
#include "em_chip.h"
#include "em_usart.h"
#include "em_cmu.h"
#include "em_i2c.h"
#include "em_int.h"

#include "ili9341.h"
#include "utilities.h"
#include "fonts.h"

#include <stdarg.h>
#include <stdio.h>

#define FT6206_ADDR           	0x38 << 1
#define FT6206_FIRMID_OFFSET	0xA6
#define FT6206_G_MODE_OFFSET	0xA4

// Used for our timer
extern uint32_t msTicks;

uint8_t data_array[16];

// Data structures to hole current capacitance sensor data
typedef struct cap_data_type
{
	bool touched;
	uint8_t touch_id;
	uint16_t x_position;
	uint16_t y_position;
} cap_data_struct;

cap_data_struct touch_data[2];
uint8_t num_touch_points = 0;
bool interrupt_triggered = false;
uint16_t pen_color;

// Need to declare the function prototype to call it before we implement it
int gpio_int_handler_process_touch(uint8_t pin);

void peripheral_setup()
{
	// Set up the necessary peripheral clocks
	CMU_ClockEnable(cmuClock_GPIO, true);
	CMU_ClockEnable(cmuClock_USART1, true);
	CMU_ClockEnable(cmuClock_I2C0, true);

	// Initialize and enable the USART
	USART_InitSync_TypeDef init = USART_INITSYNC_DEFAULT;
	init.baudrate = 24000000;		// This is the fastest it can go
	init.msbf = true;

	// This will get the HFPER clock running at 48MHz
	CMU_ClockSelectSet(cmuClock_HF, cmuSelect_HFXO);
//	uint32_t foo = CMU_ClockFreqGet(cmuClock_HFPER);
//	foo = CMU_ClockFreqGet(cmuClock_HF);
//	foo = CMU_ClockFreqGet(cmuClock_CORE);

	USART_InitSync(USART1, &init);

	uint32_t baud = USART_BaudrateGet(USART1);

	USART1->CTRL |= USART_CTRL_AUTOCS;

	// Connect the USART signals to the GPIO peripheral
	USART1->ROUTE = USART_ROUTE_RXPEN | USART_ROUTE_TXPEN |
			USART_ROUTE_CLKPEN | USART_ROUTE_CSPEN | USART_ROUTE_LOCATION_LOC1;

	// Set up i2c
	I2C_Init_TypeDef i2c_init = I2C_INIT_DEFAULT;
	I2C_Init(I2C0, &i2c_init);

	// Use I2C0 location #1
	I2C0->ROUTE = (I2C0->ROUTE & ~_I2C_ROUTE_LOCATION_MASK) | I2C_ROUTE_LOCATION_LOC1;

	// Route the pins through to the GPIO block, so that I2C block can control them
	I2C0->ROUTE |= I2C_ROUTE_SCLPEN | I2C_ROUTE_SDAPEN;

	// Enable the GPIO pins for the USART, starting with CS lines
	// This is to avoid clocking the flash chip when we set CLK high
	GPIO_PinModeSet(gpioPortD, 3, gpioModePushPull, 1);		// CS
	GPIO_PinModeSet(gpioPortD, 8, gpioModePushPull, 1);		// MicroSD CS
	GPIO_PinModeSet(gpioPortD, 0, gpioModePushPull, 0);		// MOSI
	GPIO_PinModeSet(gpioPortD, 1, gpioModeInput, 0);		// MISO
	GPIO_PinModeSet(gpioPortD, 2, gpioModePushPull, 1);		// CLK

	// Enable the GPIO pins for the misc signals, leave pulled high
	GPIO_PinModeSet(gpioPortD, 4, gpioModePushPull, 1);		// DC
	GPIO_PinModeSet(gpioPortD, 5, gpioModePushPull, 1);		// RST

	// Enable the GPIO pins for the i2c signals, open drain, pulled up, with filter
	GPIO_PinModeSet(gpioPortD, 6, gpioModeWiredAndPullUpFilter, 1);		// SDA
	GPIO_PinModeSet(gpioPortD, 7, gpioModeWiredAndPullUpFilter, 1);		// SCL
	GPIO_PinModeSet(gpioPortD, 13, gpioModeInput, 1);					// IRQ
}

void writecommand(uint8_t command)
{
	delay(1);
	GPIO_PinOutClear(gpioPortD, 4);
	USART_Tx(USART1, command);
	delay(1);
}

void writedata(uint8_t data)
{
	GPIO_PinOutSet(gpioPortD, 4);
	USART_Tx(USART1, data);

}

void set_drawing_window(uint16_t x_start, uint16_t y_start, uint16_t x_end, uint16_t y_end)
{
	if (x_start > ILI9341_TFTWIDTH || x_end > ILI9341_TFTWIDTH || y_start > ILI9341_TFTHEIGHT || y_end > ILI9341_TFTHEIGHT)
	{
		DEBUG_BREAK
	}

	// Swap things if they are backwards
	if (x_start > x_end)
	{
		uint16_t tmp;
		tmp = x_end;
		x_end = x_start;
		x_start = tmp;
	}

	// Swap things if they are backwards
	if (y_start > y_end)
	{
		uint16_t tmp;
		tmp = y_end;
		y_end = y_start;
		y_start = tmp;
	}

	writecommand(ILI9341_CASET); // Column addr set
	writedata(x_start >> 8);
	writedata(x_start);
	writedata(x_end >> 8);
	writedata(x_end );

	writecommand(ILI9341_PASET); // Page (row) addr set
	writedata(y_start >> 8);
	writedata(y_start);
	writedata(y_end >> 8);
	writedata(y_end);
}

void draw_filled_rectangle(uint16_t x_start, uint16_t y_start, uint16_t x_end, uint16_t y_end, uint16_t color)
{
	set_drawing_window(x_start, y_start, x_end, y_end);
	writecommand(ILI9341_RAMWR);
	for (int i = 0; i < x_end; i++)
	{
		for (int j = 0; j < y_end; j++)
		{
			writedata( color >> 8);
			writedata( color );
		}
	}
}

void draw_vertical_line(uint16_t x_start, uint16_t y_start, uint16_t length, int16_t thickness, uint16_t color)
{
	uint16_t x_end = x_start + thickness;
	uint16_t y_end = y_start + length;
	draw_filled_rectangle(x_start, y_start, x_end, y_end, color);
}

void draw_horizontal_line(uint16_t x_start, uint16_t y_start, uint16_t length, int16_t thickness, uint16_t color)
{
	uint16_t x_end = x_start + length;
	uint16_t y_end = y_start + thickness;
	draw_filled_rectangle(x_start, y_start, x_end, y_end, color);
}

void draw_unfilled_rectangle(uint16_t x_start, uint16_t y_start, uint16_t width, uint16_t height, uint16_t line_thickness, uint16_t color)
{
	// Draw left side
	draw_vertical_line(x_start, y_start, height, line_thickness, color);

	// Draw bottom side
	draw_horizontal_line(x_start, y_start+height, width, -line_thickness, color);

	// Draw top
	draw_horizontal_line(x_start, y_start, width, line_thickness, color);

	// Draw right side
	draw_vertical_line(x_start+width, y_start, height, -line_thickness, color);
}

void draw_vertical_line_old(uint16_t x_start, uint16_t y_start, uint16_t y_end, uint16_t color)
{
	set_drawing_window(x_start, y_start, x_start, y_end);
	uint16_t length = y_end - y_start + 1;
	writecommand(ILI9341_RAMWR);
	for (int j = 0; j < length; j++)
	{
		writedata( color >> 8);
		writedata( color );
	}
}

#define FONT_WIDTH 	8
#define FONT_HEIGHT 14

void draw_char(uint16_t x_start, uint16_t y_start, char single_char, uint16_t color)
{
	uint16_t pixel_buffer[FONT_HEIGHT * FONT_WIDTH];
	uint16_t index = (uint16_t) single_char;

	uint16_t row_offset = 0;
	for (int i=0; i<FONT_HEIGHT; i++)
	{
		uint8_t row_data = font_8x14[index][i];
		for (int j=0; j<FONT_WIDTH; j++)
		{
			if (row_data & 0x1)
			{
				pixel_buffer[row_offset+j] = color;
			}
			else
			{
				// Set the pixel to black
				// This assumes a black background!
				pixel_buffer[row_offset+j] = ILI9341_BLACK;
			}

			// Shift the row of the text char over by one for the next pixel
			row_data = (row_data >> 1);
		}
		row_offset += FONT_WIDTH;
	}

	// Now, send the pixel_buffer to the screen
	set_drawing_window(x_start, y_start, x_start+FONT_WIDTH-1, y_start+FONT_HEIGHT-1);
	writecommand(ILI9341_RAMWR);
	for (int i = 0; i < (FONT_HEIGHT * FONT_WIDTH); i++)
	{
		writedata( pixel_buffer[i] >> 8);
		writedata( pixel_buffer[i] );
	}
}

void tft_print(uint16_t x_start, uint16_t y_start, uint16_t color, const char* format, ...)
{
	// This stuff here deals with the elipsis (...) input
    char       msg[ILI9341_TFTWIDTH / FONT_WIDTH];
    va_list    args;

    va_start(args, format);
    vsnprintf(msg, sizeof(msg), format, args);
    va_end(args);

    // Get a pointer to the message
    char * string = msg;
    while (*string != 0)
    {
    	draw_char(x_start, y_start, *string++, color);
    	x_start += FONT_WIDTH;
    }
}

// Used by the read_register and write_register functions
// data_array is read or write data, depending on the flag
void i2c_transfer(uint16_t device_addr, uint8_t data_array[], uint16_t data_len, uint8_t flag)
{
	// Transfer structure
	I2C_TransferSeq_TypeDef i2cTransfer;

	// Initialize I2C transfer
	I2C_TransferReturn_TypeDef result;
	i2cTransfer.addr          = device_addr;
	i2cTransfer.flags         = flag;
	i2cTransfer.buf[0].data   = data_array;
	i2cTransfer.buf[0].len    = data_len;

	INT_Disable();

	// Set up the transfer
	result = I2C_TransferInit(I2C0, &i2cTransfer);

	// Do it until the transfer is done
	while (result != i2cTransferDone)
	{
		if (result != i2cTransferInProgress)
		{
			DEBUG_BREAK;
		}
		result = I2C_Transfer(I2C0);
	}

	INT_Enable();
}

// Tailored for the FT6x06 device only
void i2c_read_device_id()
{
	// First, set the address to read
	data_array[0] = FT6206_FIRMID_OFFSET;
	i2c_transfer(FT6206_ADDR, data_array, 1, I2C_FLAG_WRITE);

	// Then, do the actual read of the register contents, from the offset +9
	i2c_transfer(FT6206_ADDR, data_array, 9, I2C_FLAG_READ);

	// Check FT6206_FIRMID_OFFSET + 2 for FocalTech�s Panel ID = 0x11
	if (data_array[2] != 0x11)
	{
		DEBUG_BREAK
	}

	// Set interrupts to polling mode
	data_array[0] = FT6206_G_MODE_OFFSET;
	data_array[1] = 0;
	i2c_transfer(FT6206_ADDR, data_array, 2, I2C_FLAG_WRITE);

	return;
}

// Helper function to convert touchscreen coordinates to touch screen coordinates
uint16_t map(uint16_t x, uint16_t in_min, uint16_t in_max, uint16_t out_min, uint16_t out_max)
{
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

void i2c_read_capacitance_values()
{
	// First, set the address to read
	data_array[0] = 0;
	i2c_transfer(FT6206_ADDR, data_array, 1, I2C_FLAG_WRITE);

	// Then, do the actual read of the register contents, from the offset +15
	i2c_transfer(FT6206_ADDR, data_array, 15, I2C_FLAG_READ);

	num_touch_points = data_array[0x2] & 0x0F;
	for (int i=0; i < num_touch_points; i++)
	{
		uint8_t base_offset = 0x3 + i * 0x6;

		// Check the status, and shift over by 6 to compare bits 7:6 to 0b11
		uint8_t status = data_array[base_offset];
		if (((status >> 6) & 0b11) == 0b11)
		{
			touch_data[i].touched = false;
		}
		else
		{
			touch_data[i].touched = true;
		}

		touch_data[i].touch_id = data_array[base_offset + 0x2] & 0xF0;
		uint16_t raw_x = data_array[base_offset + 0x1] + ((data_array[base_offset] & 0x0F) << 8);
		uint16_t raw_y = data_array[base_offset + 0x3] + ((data_array[base_offset + 0x2] & 0x0F) << 8);
		touch_data[i].x_position = map(raw_x, 0, ILI9341_TFTWIDTH, ILI9341_TFTWIDTH, 0);
		touch_data[i].y_position = map(raw_y, 0, ILI9341_TFTHEIGHT, ILI9341_TFTHEIGHT, 0);
	}
}

// Callback for capacitive sense IRQ
int gpio_int_handler_process_touch(uint8_t pin)
{
	// Read the capacitance values into touch_data[]
	i2c_read_capacitance_values();

	interrupt_triggered = true;

	return 0;
}

#define BOXSIZE 40
void select_pen_color(uint16_t color)
{
	uint16_t box_x_start;
	// First, unhighlight the old pen color
	switch (pen_color)
	{
		case ILI9341_RED: { box_x_start = 0;  break;}
		case ILI9341_YELLOW: { box_x_start = BOXSIZE;  break;}
		case ILI9341_GREEN: { box_x_start = BOXSIZE*2;  break;}
		case ILI9341_CYAN: { box_x_start = BOXSIZE*3;  break;}
		case ILI9341_BLUE: { box_x_start = BOXSIZE*4;  break;}
		case ILI9341_MAGENTA: { box_x_start = BOXSIZE*5;  break;}
	}
	draw_unfilled_rectangle(box_x_start, 0, BOXSIZE, BOXSIZE, 6, pen_color);

	// Now highlight the new color
	pen_color = color;
	switch (pen_color)
	{
		case ILI9341_RED: { box_x_start = 0; break;}
		case ILI9341_YELLOW: { box_x_start = BOXSIZE;  break;}
		case ILI9341_GREEN: { box_x_start = BOXSIZE*2;  break;}
		case ILI9341_CYAN: { box_x_start = BOXSIZE*3;  break;}
		case ILI9341_BLUE: { box_x_start = BOXSIZE*4;  break;}
		case ILI9341_MAGENTA: { box_x_start = BOXSIZE*5;  break;}
	}
	draw_unfilled_rectangle(box_x_start, 0, BOXSIZE, BOXSIZE, 6, ILI9341_WHITE);
}


void draw_color_pallet()
{
	draw_filled_rectangle(0, 0, BOXSIZE, BOXSIZE, ILI9341_RED);
	draw_filled_rectangle(BOXSIZE, 0, BOXSIZE*2, BOXSIZE, ILI9341_YELLOW);
	draw_filled_rectangle(BOXSIZE*2, 0, BOXSIZE*3, BOXSIZE, ILI9341_GREEN);
	draw_filled_rectangle(BOXSIZE*3, 0, BOXSIZE*4, BOXSIZE, ILI9341_CYAN);
	draw_filled_rectangle(BOXSIZE*4, 0, BOXSIZE*5, BOXSIZE, ILI9341_BLUE);
	draw_filled_rectangle(BOXSIZE*5, 0, BOXSIZE*6, BOXSIZE, ILI9341_MAGENTA);
//	pen_color = ILI9341_RED;
//	select_pen_color(pen_color);
}

/**************************************************************************//**
 * @brief  Main function
 *****************************************************************************/
int main(void)
{
	/* Chip errata */
	CHIP_Init();

	if (SysTick_Config(CMU_ClockFreqGet(cmuClock_CORE) / 1000))
	{
		DEBUG_BREAK;
	}

	peripheral_setup();

	// Reset the display driver chip
	GPIO_PinOutSet(gpioPortD, 5);
	GPIO_PinOutClear(gpioPortD, 5);
	delay(1);
	GPIO_PinOutSet(gpioPortD, 5);

	delay(10);
	writecommand(ILI9341_SLPOUT);    //Exit Sleep
	delay(120);
	writecommand(ILI9341_DISPON);    //Display on

	writecommand(ILI9341_PIXFMT);
	writedata(0x55);

	writecommand(ILI9341_MADCTL);    // Memory Access Control
	writedata(0x48);

	delay(200);

	draw_filled_rectangle(0,0, ILI9341_TFTWIDTH, ILI9341_TFTHEIGHT, ILI9341_BLACK);
	tft_print(60,100, ILI9341_RED, "FINGER PAINTING");
	tft_print(40,160, ILI9341_WHITE, "Touch screen to start");

	// Check to see that we can talk to the i2c capacitive sensor
	i2c_read_device_id();

	// Set up capsense interrupt
	set_gpio_interrupt(gpioPortD, 13, false, true, (GPIOINT_IrqCallbackPtr_t) gpio_int_handler_process_touch);

	draw_color_pallet();

	bool activated = false;
	/* Infinite loop */
	while (1)
	{
		// Keep checking after the first interrupt until the event is over
		if (interrupt_triggered)
		{
			i2c_read_capacitance_values();
			if (!activated)
			{
				// Clear the screen and display the color palette
				draw_filled_rectangle(0,0, ILI9341_TFTWIDTH, ILI9341_TFTHEIGHT, ILI9341_BLACK);
				draw_color_pallet();
				pen_color = ILI9341_RED;
				select_pen_color(pen_color);
				activated = true;
				delay(100);
				continue;
			}
		}

		// Process any touches
		for (int i=0; i<num_touch_points; i++)
		{
			if (touch_data[i].touched)
			{
				uint16_t x = touch_data[i].x_position;
				uint16_t y = touch_data[i].y_position;

				if (y < BOXSIZE)
				{
					if (x < BOXSIZE) select_pen_color(ILI9341_RED);
					else if (x < BOXSIZE*2) select_pen_color(ILI9341_YELLOW);
					else if (x < BOXSIZE*3) select_pen_color(ILI9341_GREEN);
					else if (x < BOXSIZE*4) select_pen_color(ILI9341_CYAN);
					else if (x < BOXSIZE*5) select_pen_color(ILI9341_BLUE);
					else select_pen_color(ILI9341_MAGENTA);
					delay(200);
					continue;
				}
				draw_filled_rectangle(x, y, x+6, y+6, pen_color);
			}
			else
			{
				interrupt_triggered = false;
			}
		}
		num_touch_points = 0;
		//delay(10);

	}
}
