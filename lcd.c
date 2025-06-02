#include "globals.h"
#include "utilities.h"
#include "gpio.h"
#include "lcd.h"

static int i2c_fd;

// Custom character data for bargraph (5 levels: 0/5 to 5/5 pixels filled)
static const unsigned char bargraph_chars[5][8] = {
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10}, // 1/5 filled (leftmost column)
    {0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18}, // 2/5 filled (left two columns)
    {0x1C, 0x1C, 0x1C, 0x1C, 0x1C, 0x1C, 0x1C, 0x1C}, // 3/5 filled (left three columns)
    {0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E}, // 4/5 filled (left four columns)
	{0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F}};  // 5 MuHahaha!


static void i2c_write_byte(unsigned char byte) {
    if (write(i2c_fd, &byte, 1) != 1) {
        fprintf(stderr, "I2C write failed: %s\n", strerror(errno));
    }
}


static void lcd_send_nibble(unsigned char nibble, unsigned char rs) {
    unsigned char data = 0;

    // Map nibble to D4-D7
    if (nibble & 0x01) data |= D4;
    if (nibble & 0x02) data |= D5;
    if (nibble & 0x04) data |= D6;
    if (nibble & 0x08) data |= D7;

    // Set RS	
    data |= (rs ? RS : 0);
	
	// turn on backight;
	data |= BL;

    // Send with Enable high
    i2c_write_byte(data | EN);
    usleep(1); // Enable pulse width >450ns
    // Send with Enable low
    i2c_write_byte(data & ~EN);
    usleep(100); // Execution time
}


static void lcd_send_byte(unsigned char byte, unsigned char rs) {
    // Send high nibble
    lcd_send_nibble(byte >> 4, rs);
    // Send low nibble
    lcd_send_nibble(byte & 0x0F, rs);
}


void lcd_write_char(char c) {
    lcd_send_byte(c, 1);
}


void lcd_write_string(const char *str, int line) {
	
    // Address offsets for 20x4 LCD
    static const unsigned char line_offsets[] = {0x00, 0x40, 0x14, 0x54};

    // Set cursor position
    if (line >= 0 && line < 4) {
        lcd_send_byte(LCD_SET_DDRAM | line_offsets[line], 0);
    }

    // Write string
	int col = 0;
    while (*str && col < 20) {
        lcd_write_char(*str++);
        col++;
    }
	
	// pad with spaces to end of line
	while (col < 20) {
		lcd_write_char(' ');
		col++;
	}	
}


void lcd_clear(void) {
    lcd_send_byte(LCD_CLEAR, 0);
    usleep(1520);
}


void lcd_home(void) {
    lcd_send_byte(LCD_HOME, 0);
	usleep(1520);
}


void lcd_init(SharedDataStruct* shared_data_p) {
	
    // Open I2C device
    i2c_fd = open(I2C_DEVICE, O_RDWR);
    if (i2c_fd < 0) {
        perror("Failed to open I2C device!");
        exit(1);
    }

    // Set I2C slave address
    if (ioctl(i2c_fd, I2C_SLAVE, I2C_ADDR) < 0) {
        perror("Failed to set I2C address");
        close(i2c_fd);
        exit(1);
    }

    // Initial delay
    usleep(15000); // >15ms

    // Send initialization sequence for 4-bit mode
    lcd_send_nibble(0x03, 0);
    usleep(4100); // >4.1ms
    lcd_send_nibble(0x03, 0);
    usleep(100); // >100us
    lcd_send_nibble(0x03, 0);
    usleep(100);
    lcd_send_nibble(0x02, 0); // Set 4-bit mode
    usleep(100);

    // Function set: 4-bit, 2 lines, 5x8 font
    lcd_send_byte(LCD_FUNCTION_SET, 0);
    usleep(100);

    // Load custom characters for bargraph
    lcd_send_byte(LCD_SET_CGRAM | 0x00, 0); // Set CGRAM address to 0
    for (int i = 0; i < 5; i++) {
        for (int j = 0; j < 8; j++) {
            lcd_send_byte(bargraph_chars[i][j], 1);
        }
    }
    lcd_send_byte(LCD_SET_DDRAM | 0x00, 0); // Return to DDRAM

    // Display on, cursor off
    lcd_send_byte(LCD_DISPLAY_ON, 0);
    usleep(100);

    // Clear display
    lcd_send_byte(LCD_CLEAR, 0);	
    usleep(2000); // >1.53ms

    // Entry mode: increment cursor, no shift
    lcd_send_byte(LCD_ENTRY_MODE, 0);
    usleep(100);
	
}


void lcd_display_message(const char *line0, const char *line1, const char *line2, const char *line3)
{
    char buf[21];
	
	// Clear the LCD. Delay to deliberately make it flash.
    lcd_send_byte(LCD_CLEAR, 0);
    usleep(250000);
	

    const char *lines[] = {line0, line1, line2, line3};
	
    for (int i = 0; i < 4; i++) {
        
		if (lines[i]) { // Check for NULL and empty
		
            size_t len = strlen(lines[i]);
            int padding = (20 - (len > 20 ? 20 : len)) / 2;
			memset(buf, ' ', sizeof(buf));			
			strncpy(buf+padding, lines[i], 20-padding);
            buf[20] = '\0';
            lcd_write_string(buf, i);
        }
    }	
}



// Function to display a bargraph for values 0-100
void lcd_display_bargraph(int value, int row) {

    // Ensure value is within 0-100
    if (value < 1) value = 1;
    if (value > 100) value = 100;

    // Address offsets for 20x4 LCD
    static const unsigned char row_offsets[] = {0x00, 0x40, 0x14, 0x54};

    // Set cursor to start of specified row
    if (row >= 0 && row < 4) {
        lcd_send_byte(LCD_SET_DDRAM | row_offsets[row], 0);
    } else {
        return; // Invalid row
    }

    // Calculate number of full blocks and partial block
    int full_blocks = value / 5; // Each block represents 5 units
    int partial = value % 5;     // Remainder for partial block

    // Write full blocks (character 4: fully filled)
    for (int i = 0; i < full_blocks && i < 20; i++) {
        lcd_write_char(4); // Custom character 4 (full block)
    }

    // Write partial block if needed
    if (full_blocks < 20 && partial > 0) {
        lcd_write_char(partial - 1); // Custom characters 0-3 for partial fill
        full_blocks++;
    }

    // Fill remaining space with empty characters
    for (int i = full_blocks; i < 20; i++) {
        lcd_write_char(' ');
    }
}


void lcd_display_error_message(const char* message1, const char* message2) {
	lcd_display_message("****ERROR****", NULL, message1, message2);
	long_beep();
	sleep(3);
}


void lcd_cleanup(void) {
	lcd_clear();
	close(i2c_fd);	
}	

