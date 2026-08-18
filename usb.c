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



// Returns -1 if the path is invalid
int32_t get_device_id_from_path(SharedDataStruct* shared_data_p, char* path)
{
	// Scan all usb ports looking for a matching device path.
	for (int device_id = 0; device_id < (int)shared_data_p->channels_active; device_id++)
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




// Reading a volume label means shelling out to blkid, and the partition node can take
// a moment to appear after the disk itself does. The old version sat in a 5-second
// retry loop, which stalled the whole monitor thread - no other slot was scanned and no
// removal was noticed for the duration. Instead we remember which device we're waiting
// on and make one attempt per scan until the deadline passes.
#define MASTER_LABEL_TIMEOUT_MS 5000

static char     pending_master_device[STRING_LEN] = "";
static uint64_t pending_master_deadline_ms = 0;


static uint64_t monotonic_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}


// If the newly-seen device is in slot 0 (socket 1), arm a label check for it. Slot 0
// only - other slots are ignored even if labelled MASTER.
static void check_for_master_label(SharedDataStruct* shared_data_p, int device_id,
                                    const char* device_name) {

	if (device_id != 0) {
		return;
	}

	if (shared_data_p->master_reload_requested) {
		// A reload is already pending or in progress - don't stomp on it.
		return;
	}

	snprintf(pending_master_device, sizeof(pending_master_device), "%s", device_name);
	pending_master_deadline_ms = monotonic_ms() + MASTER_LABEL_TIMEOUT_MS;
}


// Called once per scan. Makes at most one blkid attempt, so the scan loop keeps running.
static void poll_master_label_check(SharedDataStruct* shared_data_p) {

	if (pending_master_device[0] == '\0') {
		return;
	}

	ChannelInfoStruct* client_info_p = &shared_data_p->channel_info[0];

	// The device we were waiting on has been pulled or replaced - abandon the check.
	if (strcmp(client_info_p->device_name, pending_master_device) != 0) {
		pending_master_device[0] = '\0';
		return;
	}

	// +2 so appending the "1" can never truncate (pending_master_device is STRING_LEN)
	char partition_name[STRING_LEN + 2];
	snprintf(partition_name, sizeof(partition_name), "%s1", pending_master_device);

	char label[STRING_LEN];
	if (get_volume_label(partition_name, label, sizeof(label))) {

		if (strcasestr(label, MASTER_LABEL_KEYWORD) != NULL) {
			printf("Master label detected on %s (slot 0)\n", pending_master_device);

			// Reserve the slot - keeps run() from ever using it as a duplication target
			client_info_p->state = INDICATING;

			snprintf(shared_data_p->master_device_name, sizeof(shared_data_p->master_device_name),
				"%s", pending_master_device);
			shared_data_p->master_device_id = 0;
			shared_data_p->master_reload_requested = true;
		}

		pending_master_device[0] = '\0';   // got an answer, either way
		return;
	}

	if (monotonic_ms() >= pending_master_deadline_ms) {
		pending_master_device[0] = '\0';   // no label showed up in time - give up
	}
}




// Finds the /dev/disk/by-path/ entry name that resolves to the given whole-disk kernel
// device (e.g. "sda"). This name is built by udev's own topology-based path_id logic,
// which deliberately excludes the kernel-assigned bus number (the "1-" in "1-1.1.4") -
// that number isn't fixed, since modern kernels probe USB controllers asynchronously
// at boot and can assign it differently run to run. The physical port-chain portion
// is stable, but relying on the bus number prefix alongside it is what was causing the
// mapping to occasionally shift between reboots. by-path avoids that entirely.
// Returns true and fills id_path_out with the entry name on success.
static bool get_disk_by_path_id(const char *kernel_name, char *id_path_out, size_t out_size) {

	id_path_out[0] = '\0';

	DIR *dir = opendir("/dev/disk/by-path");
	if (!dir) {
		return false;
	}

	char expected_target[PATH_LEN];
	snprintf(expected_target, sizeof(expected_target), "../../%s", kernel_name);

	struct dirent *ent;
	bool found = false;

	while ((ent = readdir(dir)) != NULL) {
		if (ent->d_name[0] == '.') continue;

		char link_path[PATH_LEN];
		snprintf(link_path, sizeof(link_path), "/dev/disk/by-path/%s", ent->d_name);

		char target[STRING_LEN];
		ssize_t len = readlink(link_path, target, sizeof(target)-1);
		if (len == -1) continue;
		target[len] = '\0';

		// Only match the whole-disk entry (e.g. "../../sda"), not a partition ("../../sda1")
		if (strcmp(target, expected_target) == 0) {
			snprintf(id_path_out, out_size, "%s", ent->d_name);
			found = true;
			break;
		}
	}

	closedir(dir);
	return found;
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

			// Get the USB device's name (i.e. /dev/sda) and a stable topology identifier
			// for it via /dev/disk/by-path (not the raw kernel busnum-based path, since
			// that number itself isn't guaranteed stable across reboots)
			snprintf(device_name, sizeof(device_name), "/dev/%.60s", ent->d_name);

			if (!get_disk_by_path_id(ent->d_name, device_path, sizeof(device_path))) {
				// udev may not have finished creating the by-path symlink for a
				// freshly-inserted device yet - just try again on the next scan.
				continue;
			}
				
			int32_t device_id = get_device_id_from_path(shared_data_p, device_path);
			//printf("detected usb device id %u: name=%s, path=%s\n", 
			//	device_id, device_name, device_path);
			
			if (device_id < 0) {				
				// We've not seen this port before. Add it as the highest numbered port.
				if (shared_data_p->channels_active >= MAX_USB_CHANNELS) {
					fprintf(stderr, "WARNING: Ignoring USB device beyond MAX_USB_CHANNELS. path=%s, name=%s\n",
						device_path, device_name);
					continue;
				}

				device_id = shared_data_p->channels_active;
				printf("add new usb device. id=%u, path=%s, name=%s\n", 
					device_id, device_path, device_name);

				ChannelInfoStruct *client_info_p = &shared_data_p->channel_info[device_id];
				// device_path is a PATH_LEN buffer being copied into a STRING_LEN field -
				// snprintf, not strcpy.
				snprintf(client_info_p->device_name, sizeof(client_info_p->device_name), "%s", device_name);
				snprintf(client_info_p->device_path, sizeof(client_info_p->device_path), "%s", device_path);
				client_info_p->state = READY;
				usb_present[device_id] = true;
				shared_data_p->channels_active++;

				check_for_master_label(shared_data_p, device_id, device_name);
			}
			else {
				ChannelInfoStruct *client_info_p = &shared_data_p->channel_info[device_id];
				if (strcmp(client_info_p->device_name, device_name) != 0)
				{
					// port is known but contains a different USB stick or none. Update it.
					printf("changing usb device name. id=%u, path=%s, name=%s\n", 
						device_id, device_path, device_name);
					
					snprintf(client_info_p->device_name, sizeof(client_info_p->device_name), "%s", device_name);
					client_info_p->state = READY;					

					check_for_master_label(shared_data_p, device_id, device_name);
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

		// One cheap blkid attempt per scan, rather than blocking the loop for 5 seconds
		poll_master_label_check(shared_data_p);

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


