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
static NamePathStruct usb_devices_loaded[MAX_USB_CHANNELS];


//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------
// Helper Functions
//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------


bool device_is_loaded(char* device_name) {
	
    for (int i = 0; i < MAX_USB_CHANNELS; i++) {		
        if (strcmp(usb_devices_loaded[i].device_name, device_name) == 0) {
            return true;
        }
	}
	
	return false;
}



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
	
	//fprintf(stderr, "get_device_id_from_path: path not found. Path=%s\n", path);
	return -1;
}




//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------
// List of usb devices currently loaded
// (Used to detect insertions or removals)
//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------


// Adds a new usb name and path to the list of loaded devices. 
// or updates the name of the existing path (Should probably never happen)
// if it's a new device, it adds it to the first vacant slot.
void add_to_devices_loaded_list(char* name, char* path) {

	printf("Entering add_to_devices_loaded_list(%s, %s)\n", name, path);
	
    for (int i = 0; i < MAX_USB_CHANNELS; i++) {
		
		if (strcmp(usb_devices_loaded[i].device_path, path) == 0) {
			strncpy(usb_devices_loaded[i].device_name, name, STRING_LEN - 1);
			usb_devices_loaded[i].device_name[STRING_LEN - 1] = '\0';
			printf("Updated device name in slot %d\n", i);
			return;
		}	
	}

    // Path not found. Save in first empty slot
    for (int i = 0; i < MAX_USB_CHANNELS; i++) {
		if (usb_devices_loaded[i].device_path[0] == 0) {
			strncpy(usb_devices_loaded[i].device_name, name, STRING_LEN);
			strncpy(usb_devices_loaded[i].device_path, path, STRING_LEN);			
			return;
		}			
	}
	
	fprintf(stderr, "ERROR: No empty slots in usb_devices_loaded[]\n");
    exit(1);
}


int find_in_devices_loaded_list(char* path) {
	
    for (int i = 0; i < MAX_USB_CHANNELS; i++) {		
        if (strcmp(usb_devices_loaded[i].device_path, path) == 0) {
            return i;
        }
	}
	
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
    char link_path[PATH_LEN];
	char path[PATH_LEN];
	char buff[STRING_LEN];
	NamePathStruct usb_devices[MAX_USB_CHANNELS];  // list of usb drives actually present
	int usb_devices_count;  // number of entries in usb_devices array above
	struct dirent *ent;

    while (true) {

		// Get the names of all loaded devices from /sys/block
		DIR *dir = opendir("/sys/block");
		if (dir == NULL) {
			perror("Failed to open /sys/block\n");
			exit(1);
		}

		memset(usb_devices, 0, sizeof(usb_devices));
		usb_devices_count = 0;
		
		
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
            snprintf(path, sizeof(path), "/sys/block/%s", ent->d_name);
            ssize_t len = readlink(path, link_path, sizeof(link_path) - 1);
            if (len == -1) {
                perror("Failed to read symbolic link");
                continue;
            }
            link_path[len] = '\0'; // Null-terminate the string

			extract_usb_path(link_path, path);
			
			// truncate oversize strings
			sprintf(usb_devices[usb_devices_count].device_name, "/dev/%s", ent->d_name);			
			
			path[STRING_LEN-1] = 0;
			strcpy(usb_devices[usb_devices_count].device_path, path);
			usb_devices_count++;

		}
		
		closedir(dir);
		
		// Check for any recently inserted devices (i.e. those not already in the devices_loaded list)
		for (int i=0; i<usb_devices_count; i++) {			
			if (find_in_devices_loaded_list(usb_devices[i].device_path) < 0)
			{
				add_to_devices_loaded_list(usb_devices[i].device_name, usb_devices[i].device_path);
				strcpy(usb_name, usb_devices[i].device_name);
				strcpy(usb_path, usb_devices[i].device_path);
				usb_inserted = true;
				
				if (shared_data_p)
				{
					int device_id = get_device_id_from_path(shared_data_p, usb_path);
					printf("add usb device. path=%s, device_id=%d\n", usb_path, device_id);
				
					if (device_id >= 0)
					{
						ChannelInfoStruct *client_info_p = &shared_data_p->channel_info[device_id];
						client_info_p->state = READY;
						strcpy(client_info_p->device_name, usb_name);
					}
				}					
			}	
		}
		
		// check for any recently removed devices (i.e. those in the devices_loaded list but not actually present)
		for (int i = 0; i < MAX_USB_CHANNELS; i++) {
		
			if (usb_devices_loaded[i].device_path[0] != 0) {
				
				bool found = false;
				for (int j=0; j<usb_devices_count; j++) {										
					if (strcmp(usb_devices[j].device_path, usb_devices_loaded[i].device_path) == 0) {
						found = true;
						//printf("...Found in slot %d = %b\n", j, found);
						break;
					}
				}
					
				if (!found) {
					int device_id = get_device_id_from_path(shared_data_p, usb_devices_loaded[i].device_path);
					printf("Removed usb device. id=%d, path=%s\n", device_id, usb_devices_loaded[i].device_path);

					// Device is no longer present. Remove the device from the usb_devices_loaded list
					usb_devices_loaded[i].device_name[0] = 0;
					usb_devices_loaded[i].device_path[0] = 0;
					usb_removed = true;
					
					if (device_id >= 0) {
						ChannelInfoStruct *client_info_p = &shared_data_p->channel_info[device_id];
						if ((client_info_p->state != FAILED) && (client_info_p->state != CRC_FAILED)) {
							 client_info_p->state = EMPTY;
						}
						client_info_p->device_name[0] = '\0';
					}
				}				
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

	memset(usb_devices_loaded, 0, sizeof(usb_devices_loaded));

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


