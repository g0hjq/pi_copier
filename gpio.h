#ifndef GPIO_H
#define GPIO_H


#define CONSUMER "usb_copier"

#define GPIO_DELAY 1
#define SHORT_PRESS_TIME 150
#define LONG_PRESS_TIME 1500


void gpio_init(SharedDataStruct* shared_data_p); 
void gpio_cleanup(void);
void* gpio_thread_function(void* arg);

ButtonStateEnum get_button_state0(void);
ButtonStateEnum get_button_state1(void);

void tone(int frequency, int milliseconds);
void beep();
void long_beep();
void double_beep();
void error_beep();

#endif // GPIO_H



