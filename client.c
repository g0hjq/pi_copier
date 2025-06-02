#include "globals.h"
#include "utilities.h"


ChannelInfoStruct* client_info_p = NULL;
SharedDataStruct* shared_data_p = NULL;
int device_id = -1;
int shm_fd = -1;

char buffer[STRING_LEN*2];




// Reports the error message back to the server and shuts down the client program
void failed(char* errormessage) {
    char temp_str[STRING_LEN];
    char error_buf[STRING_LEN];
	
    int offset = snprintf(temp_str, sizeof(temp_str), "[%d] ", device_id);
    
    if (errno != 0) {
        if (strerror_r(errno, error_buf, sizeof(error_buf)) == 0) {
            offset += snprintf(temp_str + offset, sizeof(temp_str) - offset, "%s ", error_buf);
        } else {
            offset += snprintf(temp_str + offset, sizeof(temp_str) - offset, "Error ");
        }
    }
	
    snprintf(temp_str + offset, sizeof(temp_str) - offset, "%s", errormessage);
    
    fprintf(stderr, "%s\n", temp_str);
    
    if (client_info_p) {
        client_info_p->state = FAILED;
		client_info_p->halt = true;
    }
    
    if (shared_data_p && (shared_data_p != MAP_FAILED)) {
        munmap(shared_data_p, sizeof(SharedDataStruct));
    }
    
    if (shm_fd >= 0) {
        close(shm_fd);
    }
    
    _exit(1);
}




int main(int argc, char *argv[]) {

    if (argc != 2) {
        printf("Usage: %s <device_id>\n", argv[0]);
        return 1;  // Exit with state code 1 if arguments are incorrect
    }
	
    // Check if running as root
    if (getuid() != 0) {
        failed("This program must be run as root (e.g., with sudo)");
    }
	
	srand(time(NULL));
	
	char *startptr = argv[1];
	char *endptr;
	device_id = strtol(startptr, &endptr, 10); // Base 10 conversion
	
	if ((endptr == startptr) || (*endptr != '\0') || (device_id<0) || (device_id>=MAX_USB_CHANNELS))
	{		
        snprintf(buffer, sizeof(buffer), "device_id %s is invalid\n", startptr);
		failed(buffer);
	}

		
    // Open existing shared memory object
    shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
    if (shm_fd < 0) {
		failed("shm_open failed. Ensure server is running first");
    }

    // Map shared memory
	shared_data_p = mmap(0, sizeof(SharedDataStruct), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shared_data_p == MAP_FAILED) {
		failed("shm_open failed. Ensure server is running first");
    }

    // Close the file descriptor as it's no longer needed after mmap
    if (close(shm_fd) == -1) {
		failed("Failed to close shared memory file descriptor");
    }

	client_info_p = &shared_data_p->channel_info[device_id];


    // Error handling test : Trigger a segmentation fault
    //int *ptr = NULL;
    //*ptr = 42; // This will cause SIGSEGV


	//------------------------------------------------------------
	// Start of main program
	//------------------------------------------------------------	

	if (strlen(client_info_p->device_name) < 4)
	{
		snprintf(buffer, sizeof(buffer), "device_name '%s' is invalid", client_info_p->device_name);
		failed(buffer);
	}
		
	if (strlen(client_info_p->device_path) < 2)
	{
        snprintf(buffer, sizeof(buffer), "device_path '%s' is invalid", client_info_p->device_path);
		failed(buffer);
	}
	
	printf("[%d] Client starting. Pid=%d, device=%s path=%s\n", device_id, getpid(), client_info_p->device_name, client_info_p->device_path);

	
    // Step 0: Get the name of the mount point
	// i.e. if device name is /dev/sda then the mount point will be /mnt/usb/sda1
	char mount_point[STRING_LEN];
	const char* last_slash = strrchr(client_info_p->device_name, '/');
	if (!last_slash)
	{
        snprintf(buffer, sizeof(buffer), "device_name '%s' is not in expected format", client_info_p->device_name);
		failed(buffer);
	}
		
    snprintf(mount_point, sizeof(mount_point), "%s/%s1", MOUNT_POINT, last_slash+1);	
	
	// Append 1 to the device name to get the partition name, i.e. /dev/sdb1
	char partition_name[STRING_LEN];
	strncpy(partition_name, client_info_p->device_name, STRING_LEN);
	strncat(partition_name, "1", STRING_LEN-1);

	printf("[%d] Mount Point=%s Partition=%s\n", device_id, mount_point, partition_name);


	// Step 1: Unmount the device if it is already mounted (it shouldn't be)
	client_info_p->state = STARTING;
	client_info_p->halt = false;
	client_info_p->bytes_copied = 0;
	
	if (!client_info_p->halt)
	{		
		snprintf(buffer, sizeof(buffer), "umount %s 2>/dev/null", mount_point);
		execute_command(device_id, buffer, true); // Ignore errors if not mounted
	}

	// Step 2: Get the size of the device
    uint64_t device_size;
    int fd = open(client_info_p->device_name, O_RDONLY);
    if (fd >= 0) {
		ioctl(fd, BLKGETSIZE64, &device_size);
	}
	printf("Device Size=%lu\n", device_size);
    
	
	// Step 3: Erase the device
	if (!client_info_p->halt)
	{
		client_info_p->state = ERASING;
		snprintf(buffer, sizeof(buffer), "wipefs -a %s", client_info_p->device_name);
		if (execute_command(device_id, buffer, false) != 0) {
			failed("Erasing device");
		}
	}

	
    // Step 4: Create a primary partition. 
	if (!client_info_p->halt)
	{
		client_info_p->state = PARTITIONING;

		if ((device_size - shared_data_p->total_size ) > (200*1024*1024))
		{
			// For larger disks with at least 200MB spare capacity ...
			// Change the start-offset at random to move the FAT table and 
			// only use 90% of the remaining space to improve the drive's wear leveling
			int start_offset = (1 + (rand() % 16)) * 4;	
			snprintf(buffer, sizeof(buffer), "parted -s %s mklabel msdos mkpart primary fat32 %uMiB 90%% >/dev/null", 
				client_info_p->device_name, start_offset);		
		}
		else
		{
			// Small disks .... use all the space
			snprintf(buffer, sizeof(buffer), "parted -s %s mklabel msdos mkpart primary fat32 1MiB 100%% >/dev/null", 
				client_info_p->device_name);		
		}
		if (execute_command(device_id, buffer, false) != 0) {
			failed("Creating primary partition");
		}
	}

    // Step 5: Format the partition as FAT32
	if (!client_info_p->halt)
	{
		client_info_p->state = FORMATING;
		snprintf(buffer, sizeof(buffer), "mkfs.vfat -n TALKINGNEWS -F 32 %s >/dev/null", partition_name);
		if (execute_command(device_id, buffer, false) != 0) {
			failed("Formatting partition");
		}
	}

    // Step 6: Create mount point if it doesn't exist/
	if (!client_info_p->halt)
	{
		client_info_p->state = MOUNTING;
		snprintf(buffer, sizeof(buffer), "mkdir -p %s", mount_point);
		if (execute_command(device_id, buffer, false) != 0) {
			failed("Creating mount point");
		}
	}

    // Step 7: Mount the USB drive
	if (!client_info_p->halt)
	{		
		snprintf(buffer, sizeof(buffer), "mount %s %s >/dev/null", partition_name, mount_point);
		if (execute_command(device_id, buffer, false) != 0) {
			failed("Mounting the USB drive");
		}
	}

    // Step 8: Copy all files from Ramdrive to the USB drive, alphabetically sorted
	if (!client_info_p->halt)
	{	
		client_info_p->state = COPYING;
		if (copy_directory(RAMDIR_PATH, mount_point, &client_info_p->halt, &client_info_p->bytes_copied) != 0) {
			failed("Copying files");
		}
	}

    // Step 9: Unmount the USB drive
	if (!client_info_p->halt)
	{		
		client_info_p->state = UNMOUNTING;
		//snprintf(buffer, sizeof(buffer), "eject %s 2>/dev/null", mount_point);
		snprintf(buffer, sizeof(buffer), "umount %s", mount_point);
		if (execute_command(device_id, buffer, false) != 0) {
			failed("Unmounting drive");
		}
	}

	client_info_p->state = client_info_p->halt ? FAILED : SUCCESS;

	
	//------------------------------------------------------------
	// End of main program
	//------------------------------------------------------------
	
    // Cleanup
	if (munmap(shared_data_p, sizeof(SharedDataStruct)) == -1) {
		failed("Failed to unmap shared memory");
	}
 	
    return 0;
}

