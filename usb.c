#include "globals.h"
#include "utilities.h"
#include "usb.h"
#include "lcd.h"

static pthread_t usb_monitor_thread; 
static bool usb_inserted;
static bool usb_removed;
static char usb_name[STRING_LEN];
static char usb_path[STRING_LEN];
static SharedDataStruct* shared_data_p;



//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------
// Helper Functions
//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------

bool device_is_loaded(char* device_name) {
	
	struct udev *udev;
    struct udev_enumerate *enumerate;
    struct udev_list_entry *devices, *dev_list_entry;
    struct udev_device *dev;
    bool found = false;

    // Initialize udev
    udev = udev_new();
    if (!udev) {
        fprintf(stderr, "ERROR: Cannot create udev context\n");
        exit(1);
    }

    // Create an enumeration context
    enumerate = udev_enumerate_new(udev);
    udev_enumerate_add_match_subsystem(enumerate, "block");
    udev_enumerate_add_match_property(enumerate, "DEVTYPE", "disk");
    udev_enumerate_scan_devices(enumerate);

    // Get the list of devices
    devices = udev_enumerate_get_list_entry(enumerate);
    udev_list_entry_foreach(dev_list_entry, devices) {
		
        const char *path = udev_list_entry_get_name(dev_list_entry);
        dev = udev_device_new_from_syspath(udev, path);

        // Check if device is a USB disk
        if (dev) {
            const char *id_bus = udev_device_get_property_value(dev, "ID_BUS");
            const char *devname = udev_device_get_devnode(dev);
			
            if (id_bus && strcmp(id_bus, "usb") == 0 && devname) {
				
				if (strcmp(devname, device_name)==0) {
					found = true;
				}
            }
            udev_device_unref(dev);
        }
    }
	

    // Cleanup
    udev_enumerate_unref(enumerate);
    udev_unref(udev);

    return found;
}
	
/*
void assign_device_paths_for_hub(int hub_number, char* clue)
{
	char line[STRING_LEN];

	printf("assign_device_paths_for_hub(%d, %s)\n", hub_number, clue);
	
	if (!clue || (strlen(clue) < 3) || (strlen(clue) > 20))
	{
        fprintf(stderr, "ERROR: Invalid clue in assign_device_paths_for_hub : %s\n", clue);
        exit(1);
	}
		
	
	FILE *f = fopen(USB_CONFIG_FILE, "r");
	if (f == NULL) {
        fprintf(stderr, "ERROR: Failed to open usb configuration file %s\n", USB_CONFIG_FILE);
        exit(1);
	}
	
	// Scan through the config file looking for the correctly numbered section
	char search_string[20];
	search_string[0] = '[';
	search_string[1] = clue[0];
	search_string[2] = ']';
	search_string[3] = '\0';

	while (fgets(line, sizeof(line), f) != NULL) {

		// Search for the correct section, i.e. [3]
		if (strncmp(line, search_string,3) == 0) {
		
			// Section found. Read all the usb paths in that section and assign them to the channel_infos
			int port_number = 0;
			while(fgets(line, sizeof(line), f) != NULL) {

				trim(line);
				if (line[0] != clue[0]) {
					// No more matches. Finish
					printf("Finish\n");
					fclose(f);
					return;
				}
				
				// Search for the corresponding usb port in channel_infos;
				int device_id = get_device_id_from_hub_and_port_number(shared_data_p, hub_number, port_number);
				printf("Setting channel_info[%d].usb_path = %s\n", device_id, line);
				ChannelInfoStruct *client_info_p = &shared_data_p->channel_info[device_id];
				strcpy(client_info_p->device_path, line);
				
				port_number++;
			}
			
		}	
	}
	
	fclose(f);
}
*/


// Returns -1 if the path is invalid
int get_device_id_from_path(const SharedDataStruct* shared_data_p, char* path)
{
	// Scan all usb ports looking for a matching device path.
	for (int device_id=0; device_id<MAX_USB_CHANNELS; device_id++)
	{
		const ChannelInfoStruct* channel_info_p = &shared_data_p->channel_info[device_id];
		if (strcmp(channel_info_p->device_path, path) == 0)
		{
			// Found it
			return device_id;
		}		
	}
	
	// None found. Find the first usb hub which has not had it's usb paths setup yet
	/* for (int hub=0; hub<NUMBER_OF_HUBS; hub++)
	{
		int device_id = get_device_id_from_hub_and_port_number(shared_data_p, hub, 0);
		const ChannelInfoStruct* channel_info_p = &shared_data_p->channel_info[device_id];
		if (strlen(channel_info_p->device_path) == 0)
		{
			assign_device_paths_for_hub(hub, path);
			break;
		}
	}
	
	// And scan again
	for (int device_id=0; device_id<MAX_USB_CHANNELS; device_id++)
	{
		const ChannelInfoStruct* channel_info_p = &shared_data_p->channel_info[device_id];
		if (strcmp(channel_info_p->device_path, path) == 0)
		{
			// Found it
			return device_id;
		}		
	}
	*/
	
	//fprintf(stderr, "get_device_id_from_path: path not found. Path=%s\n", path);
	return -1;
}



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

    printf("Initialising USB drive events thread\n");

    udev = udev_new();
    if (!udev) {
        fprintf(stderr, "ERROR: Failed to create udev context\n");
        exit(1);
    }

    mon = udev_monitor_new_from_netlink(udev, "udev");
    if (!mon) {
        fprintf(stderr, "ERROR: Failed to create udev monitor\n");
        exit(1);
    }

    if (udev_monitor_filter_add_match_subsystem_devtype(mon, "block", NULL) < 0) {
        fprintf(stderr, "ERROR: Failed to set monitor filter\n");
        exit(1);
    }

    if (udev_monitor_enable_receiving(mon) < 0) {
        fprintf(stderr, "ERROR: Failed to enable monitor\n");
        exit(1);
    }

    fd = udev_monitor_get_fd(mon);
    if (fd < 0) {
        fprintf(stderr, "ERROR: Failed to get monitor file descriptor\n");
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
						
						printf("Action:%s id_bus=%s devtype:%s path:%s devnode:%s subsystem:%s syspath:%s deviceid:%d\n", 
								action, id_bus, devtype, buff, devnode, subsystem, syspath, device_id);
						
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


//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------
// Shared functions
//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------

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


void usb_init(SharedDataStruct* shared_data)
{
	shared_data_p = shared_data;
	if (!shared_data_p) {
        fprintf(stderr, "ERROR: Invalid shared_data passed to usb_init\n");
		exit(1);
	}

	usb_inserted = false;
	usb_removed = false;
	usb_name[0] = '\0';
	usb_path[0] = '\0';
	
	// Start the USB monitor thread to look for USB devices being inserted or removed
    if (pthread_create(&usb_monitor_thread, NULL, monitor_usb_drives_thread_function, NULL) != 0) {
        perror("Failed to create USB monitor thread");
        exit(1);
    }
    pthread_detach(usb_monitor_thread);
}


void usb_cleanup()
{
}


