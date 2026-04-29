#include "globals.h"
#include "utilities.h"
#include "gpio.h"
#include "lcd.h"

// -----------------------------------------------------------------------------
// Module state
// -----------------------------------------------------------------------------

static int              i2c_fd     = -1;  // -1 until lcd_init() succeeds
static pthread_mutex_t  lcd_mutex  = PTHREAD_MUTEX_INITIALIZER;

// DDRAM start offset for each row on a 20x4 HD44780.
// (20x4 panels are driven as two folded logical lines.)
static const unsigned char line_offsets[LCD_ROWS] = {0x00, 0x40, 0x14, 0x54};

// Custom character data for bargraph.
// We install these in CGRAM slots 1..5 (NOT 0), so that writing the codes
// never produces a '\0' byte. Slot 0 is left unused.
//
//   slot 1: 1/5 filled
//   slot 2: 2/5 filled
//   slot 3: 3/5 filled
//   slot 4: 4/5 filled
//   slot 5: 5/5 filled (full block)
#define BARGRAPH_SLOT_BASE  1
#define BARGRAPH_FULL_CODE  (BARGRAPH_SLOT_BASE + 4)   // = 5

static const unsigned char bargraph_chars[5][8] = {
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10}, // 1/5
    {0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18}, // 2/5
    {0x1C, 0x1C, 0x1C, 0x1C, 0x1C, 0x1C, 0x1C, 0x1C}, // 3/5
    {0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E}, // 4/5
    {0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F}  // 5/5
};


// -----------------------------------------------------------------------------
// Low-level I/O
// -----------------------------------------------------------------------------

// Write one byte on the I2C bus. Reports errors at most once per burst so a
// disconnected display doesn't flood the log.
static void i2c_write_byte(unsigned char byte)
{
    static bool error_reported = false;

    if (i2c_fd < 0) return;

    if (write(i2c_fd, &byte, 1) != 1) {
        if (!error_reported) {
            fprintf(stderr, "ERROR: I2C write failed: %s\n", strerror(errno));
            error_reported = true;
        }
    } else {
        error_reported = false;
    }
}

static void lcd_send_nibble(unsigned char nibble, unsigned char rs)
{
    unsigned char data = 0;

    if (nibble & 0x01) data |= D4;
    if (nibble & 0x02) data |= D5;
    if (nibble & 0x04) data |= D6;
    if (nibble & 0x08) data |= D7;

    data |= (rs ? RS : 0);
    data |= BL; // backlight always on

    i2c_write_byte(data | EN);   // Enable high
    usleep(1);                   // Enable pulse width >450ns
    i2c_write_byte(data & ~EN);  // Enable low
    usleep(100);                 // Execution time for most commands
}

static void lcd_send_byte(unsigned char byte, unsigned char rs)
{
    lcd_send_nibble(byte >> 4,   rs); // high nibble
    lcd_send_nibble(byte & 0x0F, rs); // low nibble
}


// -----------------------------------------------------------------------------
// String / line writers (caller locks as needed)
// -----------------------------------------------------------------------------

void lcd_write_char(char c)
{
    lcd_send_byte((unsigned char)c, 1);
}

// Write `str` to `line`, padding to LCD_COLS with spaces. Caller holds the lock.
static void lcd_write_string_no_lock(const char* str, int line)
{
    if (line >= 0 && line < LCD_ROWS) {
        lcd_send_byte(LCD_SET_DDRAM | line_offsets[line], 0);
    }

    int col = 0;
    while (str && *str && col < LCD_COLS) {
        lcd_write_char(*str++);
        col++;
    }
    while (col < LCD_COLS) {
        lcd_write_char(' ');
        col++;
    }
}

void lcd_write_string(const char* str, int line)
{
    pthread_mutex_lock(&lcd_mutex);
    lcd_write_string_no_lock(str, line);
    pthread_mutex_unlock(&lcd_mutex);
}


// -----------------------------------------------------------------------------
// Screen-level operations
// -----------------------------------------------------------------------------

void lcd_clear(void)
{
    pthread_mutex_lock(&lcd_mutex);
    lcd_send_byte(LCD_CLEAR, 0);
    usleep(1520);                    // datasheet: clear takes up to 1.52ms
    pthread_mutex_unlock(&lcd_mutex);
}

void lcd_home(void)
{
    pthread_mutex_lock(&lcd_mutex);
    lcd_send_byte(LCD_HOME, 0);
    usleep(1520);
    pthread_mutex_unlock(&lcd_mutex);
}

void lcd_init(SharedDataStruct* shared_data_p)
{
    (void)shared_data_p;   // reserved for future use

    i2c_fd = open(I2C_DEVICE, O_RDWR);
    if (i2c_fd < 0) {
        perror("Failed to open I2C device");
        exit(1);
    }

    if (ioctl(i2c_fd, I2C_SLAVE, I2C_ADDR) < 0) {
        perror("Failed to set I2C address");
        close(i2c_fd);
        i2c_fd = -1;
        exit(1);
    }

    usleep(15000); // >15ms power-on delay

    // HD44780 wake-up / 4-bit mode initialisation sequence
    lcd_send_nibble(0x03, 0); usleep(4100);
    lcd_send_nibble(0x03, 0); usleep(100);
    lcd_send_nibble(0x03, 0); usleep(100);
    lcd_send_nibble(0x02, 0); usleep(100);   // switch to 4-bit bus

    // Function set: 4-bit, 2 logical lines, 5x8 font
    lcd_send_byte(LCD_FUNCTION_SET, 0); usleep(100);

    // Install bargraph custom characters into CGRAM slots 1..5.
    // (Slot 0 is deliberately left unused so we never emit a NUL byte.)
    lcd_send_byte(LCD_SET_CGRAM | (BARGRAPH_SLOT_BASE * 8), 0);
    for (int i = 0; i < 5; i++) {
        for (int j = 0; j < 8; j++) {
            lcd_send_byte(bargraph_chars[i][j], 1);
        }
    }
    lcd_send_byte(LCD_SET_DDRAM | 0x00, 0);  // return to DDRAM

    lcd_send_byte(LCD_DISPLAY_ON, 0); usleep(100);
    lcd_send_byte(LCD_CLEAR, 0);      usleep(2000);
    lcd_send_byte(LCD_ENTRY_MODE, 0); usleep(100);
}



// Internal: write up to 4 lines, each centred on a 20-column row.
// NULL lines are skipped (existing content preserved).
// Caller must hold lcd_mutex.
static void lcd_write_centred_lines(const char* lines[LCD_ROWS])
{
    char buf[LCD_COLS + 1];
 
    for (int i = 0; i < LCD_ROWS; i++) {
        if (!lines[i]) continue;
 
        size_t len = strlen(lines[i]);
        if (len > LCD_COLS) len = LCD_COLS;
        int padding = (int)((LCD_COLS - len) / 2);
 
        // Space-fill, then drop the (possibly truncated) text into the
        // centre with memcpy - no embedded NULs, no re-padding needed.
        memset(buf, ' ', LCD_COLS);
        memcpy(buf + padding, lines[i], len);
        buf[LCD_COLS] = '\0';
 
        lcd_write_string_no_lock(buf, i);
    }
}
 
 
// Display up to 4 lines, each centred on a 20-column row.
// NULL lines are skipped (left untouched).
//
// The lock is held for the entire operation, including the 250ms flash delay
// after LCD_CLEAR. This intentionally blocks bargraph updates during the
// flash - otherwise a bargraph write landing between the clear and the
// writes would corrupt the display. 250ms of blocked bargraph updates is
// acceptable for status/error messages.
void lcd_display_message(const char* line0,
                         const char* line1,
                         const char* line2,
                         const char* line3)
{
    const char* lines[LCD_ROWS] = {line0, line1, line2, line3};
 
    pthread_mutex_lock(&lcd_mutex);
 
    lcd_send_byte(LCD_CLEAR, 0);
    usleep(200000);  // deliberate flash - lock held to protect the sequence 
    lcd_write_centred_lines(lines);
    pthread_mutex_unlock(&lcd_mutex);
}
 
 
 
// Same as lcd_display_message but without the clear-and-flash. Lines are
// rewritten in place. Use for frequent progress updates where flashing
// the screen on every call would be visually noisy and slow.
//
// NULL lines leave the existing row content untouched.
void lcd_display_message_no_flash(const char* line0,
                                  const char* line1,
                                  const char* line2,
                                  const char* line3)
{
    const char* lines[LCD_ROWS] = {line0, line1, line2, line3};
 
    pthread_mutex_lock(&lcd_mutex);
    lcd_write_centred_lines(lines);
    pthread_mutex_unlock(&lcd_mutex);
}
 
 
 

// Display a bargraph for values 0..100 on the given row.
void lcd_display_bargraph(int value, int row)
{
    if (row < 0 || row >= LCD_ROWS) return;

    if (value < 0)   value = 0;
    if (value > 100) value = 100;

    pthread_mutex_lock(&lcd_mutex);

    lcd_send_byte(LCD_SET_DDRAM | line_offsets[row], 0);

    // Each character cell represents 5 percentage points (20 cells x 5 = 100).
    int full_blocks = value / 5;
    int partial     = value % 5;
    int written     = 0;

    for (int i = 0; i < full_blocks && written < LCD_COLS; i++, written++) {
        lcd_write_char(BARGRAPH_FULL_CODE);
    }

    if (partial > 0 && written < LCD_COLS) {
        // partial is 1..4 -> slot 1..4 (1/5..4/5 filled)
        lcd_write_char(BARGRAPH_SLOT_BASE + (partial - 1));
        written++;
    }

    while (written < LCD_COLS) {
        lcd_write_char(' ');
        written++;
    }

    pthread_mutex_unlock(&lcd_mutex);
}


void lcd_display_error_message(const char* message1, const char* message2)
{
    lcd_display_message("****ERROR****", NULL, message1, message2);
    long_beep();
    sleep(3);
}


void lcd_cleanup(void)
{
    if (i2c_fd < 0) return;

    lcd_clear();
    close(i2c_fd);
    i2c_fd = -1;
}
