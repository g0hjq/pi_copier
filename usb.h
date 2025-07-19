#ifndef USB_H
#define USB_H

void get_usb_port_paths();
void usb_init(SharedDataStruct* shared_data_p);
void usb_cleanup();
bool usb_device_inserted(char* name, char* path);
bool usb_device_removed(char* name, char* path);
bool device_is_loaded(char* device_name);
int get_device_id_from_path(const SharedDataStruct* shared_data_p, char* path);

#endif // USB_H
