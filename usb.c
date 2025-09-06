#include "globals.h"
#include "utilities.h"
#include "usb.h"
#include "lcd.h"

static pthread_t usb_monitor_thread; 
static SharedDataStruct* shared_data_p;


//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------
// Helper Functions
//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------



// Returns -1 if the name is invalid
int get_device_id_from_name(SharedDataStruct* shared_data_p, char* path)
{
	// Scan all usb ports looking for a matching device path.
	for (int device_id=0; device_id<shared_data_p->channels_active; device_id++)
	{
		const ChannelInfoStruct* channel_info_p = &shared_data_p->channel_info[device_id];
		if (strcmp(channel_info_p->device_path, path) == 0)
		{
			// Found it
			return device_id;
		}		
	}
	
	//fprintf(stderr, "get_device_id_from_name: name not found. Path=%s\n", path);
	return -1;
}




// Returns -1 if the path is invalid
int32_t get_device_id_from_path(SharedDataStruct* shared_data_p, char* path)
{
	// Scan all usb ports looking for a matching device path.
	for (int device_id = 0; device_id<shared_data_p->channels_active; device_id++)
	{
		const ChannelInfoStruct* channel_info_p = &shared_data_p->channel_info[device_id];
		if (strcmp(channel_info_p->device_path, path) == 0)
		{
			// Found it
			return device_id;
		}		
	}
	
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
 * @brief Thread function to monitor USB drive insertions and removals.
 * @param arg Pointer to SharedDataStruct.
 * @return NULL.
 */
void *monitor_usb_drives_thread_function(void* arg) {

    printf("Monitoring for USB drive events in thread...\n");
    char path[PATH_LEN];
    char link_path[PATH_LEN];
	char buff[STRING_LEN];
	struct dirent *ent;

	char device_path[PATH_LEN];
	char device_name[STRING_LEN];
 	bool usb_present[MAX_USB_CHANNELS];
	

    while (true) {

		// Get the names of all loaded devices from /sys/block and
		// Populate the array usb_devices[] with each device's name and path.
		DIR *dir = opendir("/sys/block");
		if (dir == NULL) {
			perror("Failed to open /sys/block\n");
			exit(1);
		}

		memset(usb_present, 0, sizeof(usb_present));
		
		while ((ent = readdir(dir)) != NULL) {

			// check if the name starts with "SD"
			if (strncmp(ent->d_name, "sd", 2) != 0) continue;
			
			// Check if removable
			snprintf(path, sizeof(path), "/sys/block/%s/removable", ent->d_name);
			FILE *f = fopen(path, "r");
			if (!f) continue;
			
			if (fgets(buff, sizeof(buff), f) == NULL || atoi(buff) != 1) {
				fclose(f);
				continue;
			}

			fclose(f);

			// Resolve the symbolic link to get the USB port path
            snprintf(path, sizeof(path), "/sys/block/%.60s", ent->d_name);
            ssize_t len = readlink(path, link_path, sizeof(link_path) - 1);
            if (len == -1) {
                perror("Failed to read symbolic link");
                continue;
            }
            link_path[len] = '\0'; // Null-terminate the string

			// Get the USB Devices name (i.e. sda) and path (i.e. 3/1.2.3)			
			snprintf(device_name, sizeof(device_name), "/dev/%.60s", ent->d_name);			
			extract_usb_path(link_path, device_path);
			device_path[sizeof(device_path)-1] = 0;
			
			int32_t device_id = get_device_id_from_path(shared_data_p, device_path);
			//printf("detected usb device id %u: name=%s, path=%s\n", 
			//	device_id, device_name, device_path);
			
			if (device_id < 0) {				
				// We've not seen this port before. Add it as the highest numbered port.
				device_id = shared_data_p->channels_active;
				printf("add new usb device. id=%u, path=%s, name=%s\n", 
					device_id, device_path, device_name);

				ChannelInfoStruct *client_info_p = &shared_data_p->channel_info[device_id];
				strcpy(client_info_p->device_name, device_name);
				strcpy(client_info_p->device_path, device_path);
				client_info_p->state = READY;
				usb_present[device_id] = true;
				shared_data_p->channels_active++;
			}
			else {
				ChannelInfoStruct *client_info_p = &shared_data_p->channel_info[device_id];
				if (strcmp(client_info_p->device_name, device_name) != 0)
				{
					// port is known but contains a different USB stick or none. Update it.
					printf("changing usb device name. id=%u, path=%s, name=%s\n", 
						device_id, device_path, device_name);
					
					strcpy(client_info_p->device_name, device_name);
					client_info_p->state = READY;					
				}	
				usb_present[device_id] = true;
			}			
		}

		closedir(dir);
		
		

		// And finally, if a usb device has been removed, 
		// delete its name from the shared memory channel_info array
		
		for (int device_id = 0; device_id<MAX_USB_CHANNELS; device_id++) {
		
			ChannelInfoStruct *client_info_p = &shared_data_p->channel_info[device_id];
			if ((!usb_present[device_id]) && (client_info_p->device_name[0] != 0)) {				
				printf("Remove usb device %u. Path=%s name=%s\n", 
					device_id, client_info_p->device_path, client_info_p->device_name);

				if ((client_info_p->state != FAILED) && (client_info_p->state != CRC_FAILED)) {
					 client_info_p->state = EMPTY;
				}
				client_info_p->device_name[0] = '\0';
			}
		}
		
		usleep(200000);
	}
	
}



//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------
// Shared functions
//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------


void usb_init(SharedDataStruct* shared_data)
{
	shared_data_p = shared_data;
	if (!shared_data_p) {
        fprintf(stderr, "ERROR: Invalid shared_data passed to usb_init\n");
		exit(1);
	}

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


