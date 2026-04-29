#ifndef LCD_H
#define LCD_H

#include "globals.h"   // for SharedDataStruct used by lcd_init()

// -----------------------------------------------------------------------------
// Display geometry (20x4 HD44780-compatible)
// -----------------------------------------------------------------------------
#define LCD_COLS 20
#define LCD_ROWS 4

// -----------------------------------------------------------------------------
// HD44780 command codes
// -----------------------------------------------------------------------------
#define LCD_CLEAR        0x01
#define LCD_HOME         0x02
#define LCD_ENTRY_MODE   0x06   // Increment cursor, no display shift
#define LCD_DISPLAY_ON   0x0C   // Display on, cursor off, blink off
#define LCD_FUNCTION_SET 0x28   // 4-bit bus, 2-line mode, 5x8 font
                                // (20x4 panels are driven as 2 logical lines)
#define LCD_SET_CGRAM    0x40   // Set CGRAM address (custom characters)
#define LCD_SET_DDRAM    0x80   // Set DDRAM address (screen position)

// -----------------------------------------------------------------------------
// PCF8574 I2C backpack pin mapping
// -----------------------------------------------------------------------------
#define RS 0x01  // P0: Register Select (0 = command, 1 = data)
#define RW 0x02  // P1: Read/Write      (tied low - write-only)
#define EN 0x04  // P2: Enable
#define BL 0x08  // P3: Backlight
#define D4 0x10  // P4: Data 4
#define D5 0x20  // P5: Data 5
#define D6 0x40  // P6: Data 6
#define D7 0x80  // P7: Data 7

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

// NOTE: shared_data_p is currently unused inside lcd_init(), but is retained
// so callers don't need to change when/if the LCD module gains the ability
// to read state directly from shared memory.
void lcd_init(SharedDataStruct* shared_data_p);
void lcd_cleanup(void);

void lcd_clear(void);
void lcd_home(void);

void lcd_write_char(char c);
void lcd_write_string(const char* str, int line);

void lcd_display_message(const char* line0,
                         const char* line1,
                         const char* line2,
                         const char* line3);

void lcd_display_message_no_flash(const char* line0,
                         const char* line1,
                         const char* line2,
                         const char* line3);

void lcd_display_error_message(const char* message1, const char* message2);

// Draw a horizontal bargraph on `row` for a value in the range [0, 100].
void lcd_display_bargraph(int value, int row);

#endif // LCD_H

