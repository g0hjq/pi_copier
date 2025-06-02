#ifndef GPIO_H
#define GPIO_H


#define CONSUMER "usb_copier"

#define GPIO_DELAY 1



void gpio_init(SharedDataStruct* shared_data_p); 
void gpio_cleanup(void);
void* gpio_thread_function(void* arg);

ButtonStateEnum get_button_state0(void);
ButtonStateEnum get_button_state1(void);

void tone(int frequency, int milliseconds);
void beep();
void long_beep();
void double_beep();
void done_beep();
void error_beep();

//void gpio_test_leds();

#endif // GPIO_H



