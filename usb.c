#include "globals.h"
#include "utilities.h"
#include "usb.h"
#include "lcd.h"

static pthread_t usb_monitor_thread; 
static bool usb_inserted;
static bool usb_removed;
static char usb_name[STRING_LEN];
static char usb_path[STRING_LEN];

//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------
// Monitor USB Drives thread
//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------

/**
 * @brief Thread function to monitor USB drive events.
 * @param arg Pointer to SharedDataStruct.
 * @return NULL.
 */
void *monitor_usb_drives_thread_function(void* arg) {

    struct udev *udev;
    struct udev_monitor *mon;
    struct udev_device *dev;
    int fd;
	char buff[STRING_LEN];

	SharedDataStruct* shared_data_p = (SharedDataStruct*)arg;
	if (!shared_data_p) {
        fprintf(stderr, "monitor_usb_drives_thread_function failed to initialisr\n");
		exit(1);
	}

    printf("Initialising USB drive events thread\n");

    udev = udev_new();
    if (!udev) {
        fprintf(stderr, "Failed to create udev context\n");
        exit(1);
    }

    mon = udev_monitor_new_from_netlink(udev, "udev");
    if (!mon) {
        fprintf(stderr, "Failed to create udev monitor\n");
        exit(1);
    }

    if (udev_monitor_filter_add_match_subsystem_devtype(mon, "block", NULL) < 0) {
        fprintf(stderr, "Failed to set monitor filter\n");
        exit(1);
    }

    if (udev_monitor_enable_receiving(mon) < 0) {
        fprintf(stderr, "Failed to enable monitor\n");
        exit(1);
    }

    fd = udev_monitor_get_fd(mon);
    if (fd < 0) {
        fprintf(stderr, "Failed to get monitor file descriptor\n");
        exit(1);
    }

    printf("Monitoring for USB drive events in thread...\n");

    while (true) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);

        if (select(fd + 1, &fds, NULL, NULL, NULL) > 0 && FD_ISSET(fd, &fds)) {
            dev = udev_monitor_receive_device(mon);
            if (dev) {
                const char *id_bus = udev_device_get_property_value(dev, "ID_BUS");
                const char *subsystem = udev_device_get_subsystem(dev);
				const char *devtype = udev_device_get_devtype(dev);
                const char *action = udev_device_get_action(dev);
                const char *devnode = udev_device_get_devnode(dev);
                const char *syspath = udev_device_get_syspath(dev);


				if (id_bus  && devtype && devnode && syspath && action)
				{
					if ( (strcmp(id_bus,"usb")==0) && 
					     (strcmp(subsystem,"block")==0) && 
						 (strcmp(devtype,"partition")!=0) )
					{				
						extract_usb_path(syspath, buff);
						
						int device_id = -1;
						if (shared_data_p)
						{
							device_id = get_device_id_from_path(shared_data_p, buff);	
							//printf("buff=%s, device_id=%d\n", buff, device_id);
						}
						
						//printf("Action:%s id_bus=%s devtype:%s path:%s devnode:%s subsystem:%s syspath:%s\n", 
						//		action, id_bus, devtype, buff, devnode, subsystem, syspath);
						
						if (strcmp(action, "add") == 0) {
							
							// User has just plugged in a USB drive
							strcpy(usb_path, buff);
							strcpy(usb_name, devnode);
							usb_removed = false;
							usb_inserted = true;
							if (device_id >= 0) {
								ChannelInfoStruct *client_info_p = &shared_data_p->channel_info[device_id];
								client_info_p->state = READY;
								strcpy(client_info_p->device_name, usb_name);
								printf("Inserted %s (%s) into slot %d\n", usb_name, usb_path, device_id);
							}
						} else if (strcmp(action, "remove") == 0) {

							// User has just removed a USB drive
							strcpy(usb_path, buff);
							strcpy(usb_name, devnode);							
							usb_inserted = false;
							
							usb_removed = true;

							if (device_id >= 0) {
								ChannelInfoStruct *client_info_p = &shared_data_p->channel_info[device_id];
								if (client_info_p->state != FAILED) {
									client_info_p->state = EMPTY;
								}
								client_info_p->device_name[0] = '\0';
							}
						}
                    }
                }
				
                udev_device_unref(dev);			
            }
        }
    }

    udev_monitor_unref(mon);
    udev_unref(udev);
    return NULL;
}



void usb_init(SharedDataStruct* shared_data_p)
{
	usb_inserted = false;
	usb_removed = false;
	usb_name[0] = '\0';
	usb_path[0] = '\0';
	
	// Start the USB monitor thread to look for USB devices being inserted or removed
    if (pthread_create(&usb_monitor_thread, NULL, monitor_usb_drives_thread_function, shared_data_p) != 0) {
        perror("Failed to create USB monitor thread");
        exit(1);
    }
    pthread_detach(usb_monitor_thread);
}


void usb_cleanup()
{
}


// If a USB device has been inserted it copies the name and path 
// to the supplied string arrays and returns true
// 
// Returns false if no usb device has recently been inserted
bool usb_device_inserted(char* name, char* path)
{
	if (usb_inserted)
	{
		if (name) strcpy(name, usb_name);
		if (path) strcpy(path, usb_path);
		
		usb_inserted = false;
		return true;
	}
	else {
		if (name) *name = '\0';
		if (path) *path = '\0';
		return false;
	}
}


// If a USB device has been removed it copies the name and path 
// to the supplied string arrays and returns true
// 
// Returns false if no usb device has recently been removed
bool usb_device_removed(char* name, char* path)
{
	if (usb_removed)
	{
		if (name) strcpy(name, usb_name);
		if (path) strcpy(path, usb_path);
		
		usb_removed = false;
		return true;
	}
	else {
		if (name) *name = '\0';
		if (path) *path = '\0';
		return false;
	}
}
