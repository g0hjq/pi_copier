#include "globals.h"
#include "utilities.h"
#include "gpio.h"
#include "lcd.h"
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>

extern SharedDataStruct* shared_data_p;

struct gpiod_chip *chip;
struct gpiod_line *button_line0;
struct gpiod_line *button_line1;
struct gpiod_line *clock_line;
struct gpiod_line *data_line;
struct gpiod_line *latch_line0;
struct gpiod_line *latch_line1;
struct gpiod_line *speaker_line;

static ButtonStateEnum button_state0;
static ButtonStateEnum button_state1;

static pthread_t gpio_thread; 
unsigned char bits[24*NUMBER_OF_HUBS];

uint64_t milliseconds;

void tone(int frequency, int milliseconds)
{
	
    // Calculate period in microseconds (1/frequency)
    unsigned int period_us = 1000000 / frequency;
    unsigned int half_period_us = period_us / 2;
    unsigned int cycles = frequency * milliseconds / 1000;

    // Generate square wave
    for (unsigned int i = 0; i < cycles; i++) {
        gpiod_line_set_value(speaker_line, 1); // High
        usleep(half_period_us);
        gpiod_line_set_value(speaker_line, 0); // Low
        usleep(half_period_us);
    }
}


void beep() {
	tone(1000, 200);
}


void long_beep() {
	tone(1000, 1000);
}


void double_beep() {
	tone(1000, 80);
	usleep(80000);
	tone(1000, 80);
}


void error_beep() {
	for (int i=0; i<25; i++)
	{
		tone(300, 20);
		tone(320, 20);
	}
}


static void set_leds(int port_num, ChannelStateEnum state, unsigned char *red_p, unsigned char *yellow_p, unsigned char *green_p)
{
	*red_p = 0;
	*yellow_p = 0;
	*green_p = 0;
	
	switch (state)
	{
		case EMPTY:
			break;
			
		case READY:
			*yellow_p = 1;
			break;
			
		case STARTING:
		case ERASING:
		case PARTITIONING:
		case MOUNTING:
		case FORMATING:
		case COPYING:
		case UNMOUNTING:		
			*yellow_p = ((milliseconds - (port_num*70)) % 600) < 300 ? 0 : 1;
			break;
			
		case VERIFYING:		
			*yellow_p = ((milliseconds - (port_num*20)) % 200) < 100 ? 0 : 1;
			break;
		
		case SUCCESS:
			*green_p = 1;
			break;

		case FAILED:
			*red_p = 1;
			break;
			
		case CRC_FAILED:
			*red_p = 1;
			*yellow_p = 1;
			break;
			
		case LED_TEST:
			*red_p = 1;
			*yellow_p = 1;
			*green_p = 1;
			break;
		
		case INDICATING:
			*red_p = 0;
			*yellow_p = 0;
			*green_p = 0;
			switch ((milliseconds/80)%4) {
			case 0:
				*red_p = 1;
				break;
			case 1:
				*yellow_p = 1;
				break;
			case 2:			
				*green_p = 1;
				break;
			default:
			}
			break;
	}
}


// Short delay, because usleep isn't any good for delays less than 100uS or so.
static void delay_us(uint32_t us) {
    volatile uint64_t i;
    // Calibrate this loop based on your Raspberry Pi 5's clock speed
    for (i = 0; i < us * 220; i++) {
        asm volatile("nop"); // No operation, burns 1 cycle
    }
}


static void send_led_data(unsigned char* bits, int bit_count)
{		

	// Send the LED data to the bottom set of shift registers
	for (int i=bit_count-1; i>=bit_count/2; i--)
	{
		gpiod_line_set_value(data_line, bits[i] ? 1 : 0);		
		delay_us(GPIO_DELAY);
		gpiod_line_set_value(clock_line, 1);
		delay_us(GPIO_DELAY);
		gpiod_line_set_value(clock_line, 0);
	}		
	gpiod_line_set_value(latch_line1, 1);
	delay_us(GPIO_DELAY);
	gpiod_line_set_value(latch_line1, 0);
	delay_us(GPIO_DELAY);
	
	// Send the LED data to the top set of shift registers
	for (int i=bit_count/2-1; i>=0; i--)
	{
		gpiod_line_set_value(data_line, bits[i] ? 1 : 0);		
		delay_us(GPIO_DELAY);
		gpiod_line_set_value(clock_line, 1);
		delay_us(GPIO_DELAY);
		gpiod_line_set_value(clock_line, 0);
	}		
	gpiod_line_set_value(latch_line0, 1);
	delay_us(GPIO_DELAY);
	gpiod_line_set_value(latch_line0, 0);
	delay_us(GPIO_DELAY);

}
		
void wait_for_button_release() {
	do {
		usleep(100000);
	} while ((gpiod_line_get_value(button_line0)==0) || (gpiod_line_get_value(button_line0)==0));

	button_state0 = BUTTON_NOT_PRESSED;
	button_state1 = BUTTON_NOT_PRESSED;
}



void get_ip_address(char* ip_addr, int addr_len) {
	
	struct ifaddrs *ifaddr, *ifa;
    char ip[INET_ADDRSTRLEN];
	ip_addr[0] = '\0';

    // Get list of network interfaces
    if (getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs");
        return;
    }

    // Iterate through interfaces
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL)
            continue;

        // Check for IPv4 addresses
        if (ifa->ifa_addr->sa_family == AF_INET) {
            // Convert binary address to string
            struct sockaddr_in *addr = (struct sockaddr_in *)ifa->ifa_addr;
            inet_ntop(AF_INET, &addr->sin_addr, ip, INET_ADDRSTRLEN);

            // Skip loopback interface
            if (strcmp(ifa->ifa_name, "wlan0") == 0) {
				strncpy(ip_addr, ip, addr_len);
				ip_addr[addr_len-1] = '\0';
				break;
            }
        }
    }

    // Free the interface list
    freeifaddrs(ifaddr);
	
    return;
}	
	
	
		
// Both buttons held. Display System menu
const char *system_commands[] = {"[AP Mode]", "[WiFi Mode]", "[Restart]", "[Shutdown]", "[Cancel]"};
const int command_count = sizeof(system_commands) / sizeof(char*);

void display_system_menu(void) {
	
	int command_num = 0;
	char ip_addr[STRING_LEN];
	
	while (true) {		
	
		lcd_display_message(system_commands[command_num], NULL, "TOP for next option", "BOTTOM to execute");

		bool done = false;
		while (!done) {
			
			int value0 = gpiod_line_get_value(button_line0);
			int value1 = gpiod_line_get_value(button_line1);
		
			if (value0==0) {
				// Next option
				command_num++;
				if (command_num >= command_count) {
					command_num = 0;
				}
				wait_for_button_release();
				done = true;
			} 
			else {
				
				if (value1==0) {
					
					// Execute					
					switch (command_num) {
						case 0: // AP_Mode
							lcd_display_message(NULL,"Setting up AP Mode","Please Wait",NULL);
							execute_command(-1, "sudo ./setup_ap.sh", true);
							get_ip_address(ip_addr, sizeof(ip_addr));
							lcd_display_message("AP Mode", "SSID: Copier2_AP", ip_addr, "Password: LetMeIn123");
							done = true;
							break;
						case 1: // WiFi Mode
							lcd_display_message(NULL,"Setting up WiFi Mode","Please Wait",NULL);
							execute_command(-1, "sudo ./disable_ap.sh", true);
							get_ip_address(ip_addr, sizeof(ip_addr));
							lcd_display_message("WiFi Mode", ip_addr, "Username : pi", "Password: raspberry");
							done = true;
							break;
						case 2: // Restart
							lcd_display_message(NULL, "Restarting", "Back soon!", NULL);
							exit(0);
							break;
						case 3: // Shutdown
							lcd_display_message(NULL, "Shutting down", "Please power down", NULL);									
							execute_command(-1, "sudo shutdown now", true);					
							break;
						case 4: // Cancel
							lcd_display_message(NULL, "Ready", NULL, NULL);
							done = true;
							break;
						}
					
					return;
				}	
				
				usleep(100000);
			}
			usleep(100000);			
		}	
	}
}


// Reads the button and updates the LEDs according to the contents of shared_data every 100 milliseconds
void* gpio_thread_function(void* arg) {
		
	static uint64_t press_start_time0 = 0;
	static uint64_t press_start_time1 = 0;
	static bool is_pressed0 = false;
	static bool is_pressed1 = false;
	static bool wait_for_release0 = false;
	static bool wait_for_release1 = false;
	static int prev_value0 = true;
	static int prev_value1 = true;
	
	SharedDataStruct* shared_data_p = (SharedDataStruct*)arg;
	if (!shared_data_p) {
        fprintf(stderr, "ERROR: gpio_thread_function failed to initialise\n");
		exit(1);
	}

	printf("Started GPIO button and LED thead\n");

	while (true) {
		
	    struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		milliseconds = (ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;

		// -------------------------
		// Button 0 
		// -------------------------
        int value0 = gpiod_line_get_value(button_line0);
        if (value0 < 0) {
            fprintf(stderr, "ERROR: Failed to read button0 value\n");
            exit(1);
        }

        long press_duration0 = milliseconds - press_start_time0;

        if (value0 == 0 && prev_value0 == 1) { // Button pressed (falling edge)
			wait_for_release0 = false;
            press_start_time0 = milliseconds;
            is_pressed0 = true;
		}
		else if (press_duration0 >= LONG_PRESS_TIME && value0 == 0 && !wait_for_release0) { // button held down
            button_state0 = BUTTON_LONG_PRESS;
			wait_for_release0 = true;
		}
		else if (value0 == 1 && prev_value0 == 0 && is_pressed0) { // Button released (rising edge)
			if (!wait_for_release0 && press_duration0 >= SHORT_PRESS_TIME){
				button_state0 = BUTTON_SHORT_PRESS;
			}
            is_pressed0 = false;
        }

        prev_value0 = value0;		
		

		// -------------------------
		// Button 1
		// -------------------------
        int value1 = gpiod_line_get_value(button_line1);
        if (value1 < 0) {
            fprintf(stderr, "ERROR: Failed to read button2 value\n");
            exit(1);
        }

        long press_duration1 = milliseconds - press_start_time1;

        if (value1 == 0 && prev_value1 == 1) { // Button pressed (falling edge)
			wait_for_release1 = false;
            press_start_time1 = milliseconds;
            is_pressed1 = true;			
        }
		else if (press_duration1 >= LONG_PRESS_TIME && value1 == 0 && !wait_for_release1) { // button held down
            button_state1 = BUTTON_LONG_PRESS;
			wait_for_release1 = true;
		}
        else if (value1 == 1 && prev_value1 == 0 && is_pressed1) { // Button released (rising edge)
			if (!wait_for_release1 && press_duration1 >= SHORT_PRESS_TIME){
				button_state1 = BUTTON_SHORT_PRESS;
			}
            is_pressed1 = false;
		}
		
        prev_value1 = value1;		
		
				
		// Shutdown (or restart if systemd is configured for that) when both buttons are held down
		if (((button_state0 == BUTTON_LONG_PRESS) && (button_state1 != BUTTON_NOT_PRESSED)) ||
		    ((button_state1 == BUTTON_LONG_PRESS) && (button_state0 != BUTTON_NOT_PRESSED))) {
				
			// Both buttons held. Display System menu
			for (int device_id=0; device_id < MAX_USB_CHANNELS; device_id++) {
				shared_data_p->channel_info[device_id].halt = true;
			}
			memset(bits, 0, sizeof(bits));
			send_led_data(bits, sizeof(bits));
			long_beep();
			display_system_menu();
			wait_for_button_release();
		}

		
		// Update the LEDs
		memset(bits, 0, sizeof(bits));
		
	// Map the Red, Yellow and Green LEDs for each channel to shift register bits
	//   (Shift register bits 0, 8, 16, 24, 32 and 40 are unused)
	//
	//        CHAN 13 |  CHAN 12 |  CHAN 11 |  CHAN 10 |  CHAN 9  |  CHAN 8  |  CHAN 7
	//		  R  G  Y |  R  G  Y |  R  G  Y |  R  G  Y |  R  G  Y |  R  G  Y | R  G  Y
	//       ---------+----------+----------+----------+----------+----------+----------
	//		 25 26 27 | 28 29 30 | 31 33 34 | 35 36 37 | 38 39 41 | 42 43 44 | 45 46 47
	//		
	//         CHAN 6 |  CHAN 5  |  CHAN 4  |  CHAN 3  |  CHAN 2  |  CHAN 1  |  CHAN 0
	//		  R  G  Y |  R  G  Y |  R  G  Y |  R  G  Y |  R  G  Y |  R  G  Y | R  G  Y
	//       ---------+----------+----------+----------+----------+----------+----------
	//		  1  2  3 |  4  5  6 |  7  9 10 | 11 12 13 | 14 15 17 | 18 19 20 | 21 22 23		

			
		for (int i=0; i<NUMBER_OF_HUBS; i++) {
			int base = i * PORTS_PER_CHANNEL;
			int offset = i * 24;
			
			set_leds(0, shared_data_p->channel_info[base].state, &bits[offset+1], &bits[offset+2], &bits[offset+3]);
			set_leds(1, shared_data_p->channel_info[base+1].state, &bits[offset+4], &bits[offset+5], &bits[offset+6]);
			set_leds(2, shared_data_p->channel_info[base+2].state, &bits[offset+7], &bits[offset+9], &bits[offset+10]);
			set_leds(3,shared_data_p->channel_info[base+3].state, &bits[offset+11], &bits[offset+12], &bits[offset+13]);
			set_leds(4,shared_data_p->channel_info[base+4].state, &bits[offset+14], &bits[offset+15], &bits[offset+17]);
			set_leds(5,shared_data_p->channel_info[base+5].state, &bits[offset+18], &bits[offset+19], &bits[offset+20]);
			set_leds(6,shared_data_p->channel_info[base+6].state, &bits[offset+21], &bits[offset+22], &bits[offset+23]);
		}

		send_led_data(bits, sizeof(bits));
			
		// Wait 50 milliseconds to reduce CPU usage
		usleep(50000);
	}
}




void gpio_cleanup(void) {
	gpiod_line_release(button_line0);
	gpiod_line_release(button_line1);
    gpiod_line_release(clock_line);
    gpiod_line_release(data_line);
    gpiod_line_release(latch_line0);
    gpiod_line_release(latch_line1);
    gpiod_line_release(speaker_line);
    gpiod_chip_close(chip);
}


// Function to check if button is pressed (short or long)
ButtonStateEnum get_button_state0(void) {
	
    ButtonStateEnum state0 = button_state0;
	
    // Reset state after reading (one-shot detection)
    if (state0 != BUTTON_NOT_PRESSED) {
        button_state0 = BUTTON_NOT_PRESSED;
    }
	
    return state0;
}




// Function to check if button is pressed (short or long)
ButtonStateEnum get_button_state1(void) {
	
    ButtonStateEnum state1 = button_state1;
	
    // Reset state after reading (one-shot detection)
    if (state1 != BUTTON_NOT_PRESSED) {
        button_state1 = BUTTON_NOT_PRESSED;
    }
	
    return state1;
}






void gpio_init(SharedDataStruct* shared_data_p) {

    // Open the GPIO chip
    chip = gpiod_chip_open_by_name(GPIO_CHIP);
    if (!chip) {
        fprintf(stderr, "ERROR: Failed to open GPIO chip");
        exit(1);
    }

    // Get the GPIO lines
    button_line0 = gpiod_chip_get_line(chip, GPIO_BUTTON0);
    if (!button_line0) {
        fprintf(stderr, "ERROR: Failed to get button 0 GPIO line");
		exit(1);
    }

    button_line1 = gpiod_chip_get_line(chip, GPIO_BUTTON1);
    if (!button_line1) {
        fprintf(stderr, "ERROR: Failed to get button 1 GPIO line");
		exit(1);
    }

    clock_line = gpiod_chip_get_line(chip, GPIO_CLOCK);
    if (!clock_line) {
        fprintf(stderr, "ERROR: Failed to get clock GPIO line");
		exit(1);
    }

    data_line = gpiod_chip_get_line(chip, GPIO_DATA);
    if (!data_line) {
        fprintf(stderr, "ERROR: Failed to get data GPIO line");
		exit(1);
    }

    latch_line0 = gpiod_chip_get_line(chip, GPIO_LATCH0);
    if (!latch_line0) {
        fprintf(stderr, "ERROR: Failed to get latch 0 GPIO line");
		exit(1);
    }

    latch_line1 = gpiod_chip_get_line(chip, GPIO_LATCH1);
    if (!latch_line1) {
        fprintf(stderr, "ERROR: Failed to get latch 1 GPIO line");
		exit(1);
    }

    speaker_line = gpiod_chip_get_line(chip, GPIO_SPEAKER);
    if (!speaker_line) {
        fprintf(stderr, "ERROR: Failed to get speaker GPIO line");
		exit(1);
    }

    // Configure both buttons as inputs. They have hardware pullups
    if (gpiod_line_request_input(button_line0, CONSUMER) < 0) {
        fprintf(stderr, "ERROR: Failed to set button line 1 as input\n");
        exit(1);
    }

    if (gpiod_line_request_input(button_line1, CONSUMER) < 0) {
        fprintf(stderr, "ERROR: Failed to set button line 2 as input\n");
        exit(1);
    }

    // Configure the outputs to the 74HC595 LED drivers
    if (gpiod_line_request_output(clock_line, CONSUMER, 0) < 0) {
        fprintf(stderr, "ERROR: Failed to set clock line as output");
        exit(1);
    }

    if (gpiod_line_request_output(data_line, CONSUMER, 0) < 0) {
        fprintf(stderr, "ERROR: Failed to set data line as output");
        exit(1);
    }

    if (gpiod_line_request_output(latch_line0, CONSUMER, 0) < 0) {
        fprintf(stderr, "ERROR: Failed to set latch line 0 as output");
        exit(1);
    }

    if (gpiod_line_request_output(latch_line1, CONSUMER, 0) < 0) {
        fprintf(stderr, "ERROR: Failed to set latch line 1 as output");
        exit(1);
    }

    if (gpiod_line_request_output(speaker_line, CONSUMER, 0) < 0) {
        fprintf(stderr, "ERROR: Failed to set speaker line as output");
        exit(1);
    }

	// Start the GPIO thread to read the button and update the LEDs
	memset(bits, 0, sizeof(bits));

	if (pthread_create(&gpio_thread, NULL, gpio_thread_function, shared_data_p) != 0) {
        perror("Failed to create GPIO thread");
        exit(1);
    }
	pthread_detach(gpio_thread);

	printf("GPIO Thread initialised\n");
}

