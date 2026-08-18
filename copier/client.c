#include "globals.h"
#include "utilities.h"



ChannelInfoStruct* client_info_p = NULL;
SharedDataStruct* shared_data_p = NULL;
int device_id = -1;
int shm_fd = -1;

char buffer[STRING_LEN*2];
extern uint32_t crc32_table[256];




// Reports the error message back to the server and shuts down the client program
void failed(char* errormessage) {
	
    char temp_str[STRING_LEN*2];
    char error_buf[STRING_LEN];

	// globals.h defines _GNU_SOURCE, so this is the GNU strerror_r: it returns a char*
	// and may not touch error_buf at all. The old code compared the returned pointer
	// against 0, which never matched, so every error lost its errno text.
	const char *reason = strerror_r(errno, error_buf, sizeof(error_buf));
	snprintf(temp_str, sizeof(temp_str), "ERROR: [%d] %s - %s\n", device_id, errormessage, reason);
	
    fprintf(stderr, "%s", temp_str);
    
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


// -------------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------------
// Verify
// -------------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------------


// Splits one line of the CRC file (in format <filename>[TAB]<crc>) into filename and CRC.
// Returns false on a malformed line. (It used to call exit(0) - i.e. exit *successfully* -
// which left the channel stuck showing VERIFYING with no error anywhere.)
static bool parse_crc_file(const char *crc_line, char *filename, size_t filename_size, uint32_t *crc) {

    // Find the tab separator
    const char *tab = strchr(crc_line, '\t');
    if (!tab) {
        fprintf(stderr, "VERIFY ERROR: No tab separator found in CRC file\n");
		return false;
    }

    // Extract filename (before tab)
    size_t filename_len = (size_t)(tab - crc_line);
    if (filename_len >= filename_size) {
        fprintf(stderr, "VERIFY ERROR: Filename too long in CRC file\n");
        return false;
    }
    memcpy(filename, crc_line, filename_len);
    filename[filename_len] = '\0';

    // Extract CRC (after tab)
    *crc = (uint32_t)strtoul(tab + 1, NULL, 16);
    return true;
}


// Returns true if all files can be read and their CRCs are correct
bool verify(char* partition_name, char *mount_point) {
	
	static struct timeval start_time;
	static struct timeval end_time;

	uint32_t expected_crc, actual_crc;
	char filename[PATH_LEN];
	// Big enough for mount_point + '/' + filename + NUL, so the join below can't truncate.
	// A truncated path here would silently verify the wrong file.
	char tmpstr[STRING_LEN + PATH_LEN + 2];

	gettimeofday(&start_time, NULL);

	printf("[%d] Starting Verify\n", device_id);

	initialise_crc_table();
	
	// Open the CRC file in the Ramdrive
	FILE *crc_file = fopen(CRC_FILE, "r");
    if (!crc_file) {
	    fprintf(stderr, "VERIFY ERROR: Unable to open CRC file\n");
		return false;
    }
	
	// Mount the USB drive
	if (!client_info_p->halt)
	{		
		snprintf(buffer, sizeof(buffer), "mount %s %s >/dev/null", partition_name, mount_point);
		if (execute_command(device_id, buffer, false) != 0) {
			fclose(crc_file);
			fprintf(stderr, "VERIFY ERROR: Unable to mount the USB drive\n");
			return false;
		}
	}

	printf("[%d] Verify checking CRCs\n", device_id);

    // Compare the CRC of each file with those stored in the crc file
	while (!client_info_p->halt) {
		char* ptr = fgets(buffer, sizeof(buffer), crc_file);

		if (!ptr) {
			break;
		}

		if (!parse_crc_file(buffer, filename, sizeof(filename), &expected_crc)) {
			fclose(crc_file);
			snprintf(buffer, sizeof(buffer), "umount %s", mount_point);
			execute_command(device_id, buffer, true);
			return false;
		}

		// Was strcpy + two strcat into a PATH_LEN buffer with no bound on either part
		snprintf(tmpstr, sizeof(tmpstr), "%s/%s", mount_point, filename);
	
		actual_crc = compute_crc32(tmpstr);
					
		if (expected_crc != actual_crc) {
			fprintf(stderr, "VERIFY ERROR: CRC Invalid. File='%s'\n", filename);
			fclose(crc_file);
			snprintf(buffer, sizeof(buffer), "umount %s", mount_point);
			execute_command(device_id, buffer, true);
			return false;
		}				
    }
	
	// Done with the CRC file - closing here means the error paths below can't leak it
	fclose(crc_file);

	// Halted (either before we ever mounted, or part way through the comparisons)
	if (client_info_p->halt) {
		snprintf(buffer, sizeof(buffer), "umount %s", mount_point);
		execute_command(device_id, buffer, true);
		return false;
	}

	gettimeofday(&end_time, NULL);
	int seconds = end_time.tv_sec - start_time.tv_sec;
 
	printf("[%d] Verify complete in %d seconds\n", device_id, seconds);

    // Sync the USB drive
	snprintf(buffer, sizeof(buffer), "sync %s", mount_point);
	if (execute_command(device_id, buffer, false) != 0) {
		fprintf(stderr, "VERIFY ERROR: Cannot sync device\n");
		return false;
	}

    // Unmount the USB drive
	snprintf(buffer, sizeof(buffer), "umount %s", mount_point);
	if (execute_command(device_id, buffer, false) != 0) {
		fprintf(stderr, "VERIFY ERROR: Cannot unmount device\n");
		return false;
	}

	printf("[%d] Finished\n", device_id);

	return true;
}



//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
// Program Main
//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
//---------------------------------------------------------------------------

int main(int argc, char *argv[]) {

    // Force line-buffered stdout so log messages show up immediately - see server.c for details
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (argc != 2) {
        printf("Usage: %s <device_id>\n", argv[0]);
        return 1;  // Exit with state code 1 if arguments are incorrect
    }
	
    // Check if running as root
    if (getuid() != 0) {
        failed("This program must be run as root (e.g., with sudo)");
    }
	
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
	strncpy(partition_name, client_info_p->device_name, STRING_LEN-1);
	partition_name[STRING_LEN-1] = '\0';
	strncat(partition_name, "1", STRING_LEN - strlen(partition_name) - 1);

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

	// Step 2: Get the size of the partition we are going to write to.
	// The partition, not the whole device - we reuse whatever partitioning the drive
	// already has, so the partition is what actually has to be big enough.
    uint64_t partition_size = 0;
    int fd = open(partition_name, O_RDONLY);
    if (fd >= 0) {
		if (ioctl(fd, BLKGETSIZE64, &partition_size) < 0) {
			fprintf(stderr, "WARNING: [%d] Could not get partition size for %s\n", device_id, partition_name);
		}
		close(fd);
	}
	else {
		fprintf(stderr, "ERROR: [%d] Cannot open %s: %s\n", device_id, partition_name, strerror(errno));
	}
	printf("Partition Size=%" PRIu64 "\n", partition_size);

	if (partition_size == 0) {
		failed("No usable partition 1 - the drive needs an existing partition table");
	}
	if (partition_size < (uint64_t)shared_data_p->total_size) {
		fprintf(stderr, "ERROR: [%d] Partition is %" PRIu64 " bytes but the master data is %" PRIu64 " bytes\n",
			device_id, partition_size, (uint64_t)shared_data_p->total_size);
		failed("Partition too small for the master data");
	}
	
	// Steps 3 and 4 (erase with wipefs, then repartition) have both been removed.
	// The drive's existing partition table is left completely alone, and mkfs.vfat
	// below simply writes a fresh FAT32 filesystem over whatever was in partition 1.
	// The wear-levelling trick of randomising the partition start offset and capping
	// the size at 90% went with them, along with the srand() that seeded it.

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
		printf("[%d] Copying files\n", device_id);
		client_info_p->state = COPYING;
		if (copy_directory(RAMDIR_PATH, mount_point, &client_info_p->halt, &client_info_p->bytes_copied) != 0) {
			failed("Copying files");
		}
	}

    // Step 9: Unmount the USB drive		
	client_info_p->state = UNMOUNTING;
	
	snprintf(buffer, sizeof(buffer), "sync %s", mount_point);
	if (execute_command(device_id, buffer, false) != 0) {
		failed("Cannot sync device before unmount");
	}

	snprintf(buffer, sizeof(buffer), "umount %s", mount_point);
	if (execute_command(device_id, buffer, false) != 0) {
		failed("Unmounting drive");
	}

// Step 10: Verify all files have been written (Optional)
#if VERIFY
	if (!client_info_p->halt) {
		client_info_p->state = VERIFYING;
		bool crc_ok = verify(partition_name, mount_point);
		if (crc_ok) {
			client_info_p->state = client_info_p->halt ? FAILED : SUCCESS;	
		}
		else {
			client_info_p->state = CRC_FAILED;
		}		
	}
	else {
		client_info_p->state  = FAILED;
	}
#else
	client_info_p->state = client_info_p->halt ? FAILED : SUCCESS;
#endif

	//------------------------------------------------------------
	// End of main program
	//------------------------------------------------------------
	
    // Cleanup
	if (munmap(shared_data_p, sizeof(SharedDataStruct)) == -1) {
		failed("Failed to unmap shared memory");
	}
 	
    return 0;
}

