#ifndef LCD_H
#define LCD_H

// LCD commands
#define LCD_CLEAR 0x01
#define LCD_HOME 0x02
#define LCD_ENTRY_MODE 0x06
#define LCD_DISPLAY_ON 0x0C
#define LCD_FUNCTION_SET 0x28 // 4-bit, 2 lines, 5x8 font
#define LCD_SET_DDRAM 0x80    // Set DDRAM address
#define LCD_SET_CGRAM 0x40    // Set DDRAM address

// PCF8574 backpack pin mapping
#define RS 0x01  // P0: Register Select
#define RW 0x02  // P1: Read/Write (not used, grounded)
#define EN 0x04  // P2: Enable
#define BL 0x08  // P3: Backlight
#define D4 0x10  // P4: Data 4
#define D5 0x20  // P5: Data 5
#define D6 0x40  // P6: Data 6
#define D7 0x80  // P7: Data 7


// Function prototypes
void lcd_write_char(char c);
void lcd_write_string(const char *str, int line );
void lcd_set_backlight(int state);
void lcd_clear(void);
void lcd_home(void);
void lcd_cleanup(void);
void lcd_display_message(const char *line0, const char *line1, const char *line2, const char *line3);
void lcd_display_error_message(const char* message1, const char* message2);
void lcd_display_bargraph(int value, int line);
void lcd_init(SharedDataStruct* shared_data_p);

#endif // LCD_H


