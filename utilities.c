#include "globals.h"
#include "utilities.h"

#define CRC32_POLY 0x04C11DB7  // Standard CRC-32 polynomial

uint32_t crc32_table[256];

//------------------------------------------------------------------------------
// Functions to aid debugging
//------------------------------------------------------------------------------


const char* get_state_name(const ChannelStateEnum state)
{
	switch (state) {
		case EMPTY: 		return "EMPTY";
		case READY: 		return "READY";
		case STARTING: 		return "STARTING";
		case ERASING: 		return "ERASING";
		case FORMATING: 	return "FORMATING";
		case PARTITIONING: 	return "PARTITIONING";
		case MOUNTING: 		return "MOUNTING";
		case COPYING: 		return "COPYING";
		case UNMOUNTING: 	return "UNMOUNTING";
		case VERIFYING: 	return "VERIFYING";
		case SUCCESS: 		return "SUCCESS";		
		case FAILED: 		return "FAILED";
		case CRC_FAILED: 	return "CRC_FAILED";
		case LED_TEST: 		return "LED_TEST";
		case INDICATING:	return "INDICATING";
	}
	
	return "UNKNOWN" ;
}



/**
 * Display the contents of the client_info struct for debugging purposes
 */
void print_client_info(const ChannelInfoStruct* client_info_p) {
	printf("  DEVICE_ID     %u\n", client_info_p->device_id);
	printf("  CHANNEL NUM   %u\n", client_info_p->hub_number);
	printf("  PORT NUM      %u\n", client_info_p->port_number);
	printf("  HALT          %s\n", client_info_p->halt ? "true" : "false");
	printf("  STATE         %s\n", get_state_name(client_info_p->state));
	printf("  PID           %d\n", client_info_p->pid);
	printf("  START_TIME    %lu\n", client_info_p->start_time);
	printf("  DEVICE_NAME   %s\n", client_info_p->device_name);
	printf("  DEVICE_PATH   %s\n", client_info_p->device_path);
	printf("  BYTES COPIED  %lu\n\n", client_info_p->bytes_copied);
}	



/**
 * Displays the contents of the entire shared memory area
 */
void print_shared_data(const SharedDataStruct* shared_data_p) {
	printf("\n\n\nSHARED DATA\n==========================\n");
	printf("ShareDataStruct size = %lu\n", sizeof(SharedDataStruct));
	printf(" settings.autostart = %d\n", shared_data_p->settings.autostart); 
	printf(" settings.reformat = %d\n\n", shared_data_p->settings.reformat); 
	printf("DEVICE INFO :\n");

	for (int i=0; i<MAX_USB_CHANNELS; i++)
	{
		print_client_info(&shared_data_p->channel_info[i]);
	}
	printf("=============================\n\n");
}



//------------------------------
// Shared Helper functions
//------------------------------


int get_device_id_from_hub_and_port_number(const SharedDataStruct* shared_data_p, int hub_number, int port_number) {
	
	for (int device_id=0; device_id<MAX_USB_CHANNELS; device_id++)
	{
		const ChannelInfoStruct* channel_info_p = &shared_data_p->channel_info[device_id];
		if ((channel_info_p->port_number == port_number) && (channel_info_p->hub_number == hub_number)) {
			return device_id;
		}
	}
	
    fprintf(stderr, "ERROR: get_device_id_from_hub_and_port_number failed. hub_number=%d, port_number=%d\n", 
		hub_number, port_number);
    exit(1);
}


void trim(char *str) {
    int start = 0, end = strlen(str) - 1;
    
    // Remove leading whitespace
    while (start <= end && isspace(str[start])) {
        start++;
    }
    
    // Remove trailing whitespace
    while (end >= start && isspace(str[end])) {
        end--;
    }
    
    // Shift string to remove leading whitespace
    if (start > 0) {
        memmove(str, str + start, end - start + 2);
    } else {
        str[end + 1] = '\0'; // Null-terminate after last non-whitespace char
    }
}


/**
 * Extract just the usb route (3-1.3:1.0) part from the usb path, e.g.
 *  /sys/devices/platform/axi/1000120000.pcie/1f00300000.usb/xhci-hcd.1/
 *     usb3/3-1/3-1.3/3-1.3:1.0/host1/target1:0:0/1:0:0:0/block/sdb
 */
void extract_usb_path(const char *input, char *output) {
    // Initialize output as empty
    output[0] = '\0';

    // Find "/host"
    const char *host = strstr(input, "/host");

    // Move backward to find the start of the component (previous ':')
    const char *end = host;
    while (end > input && *end != ':') {
        end--; // Move back until we hit a ':'
    }


    // Find the start of the component
    const char *start = end;
    while (start > input && *(start - 1) != '/') {
        start--; // Move back until we hit a '/'
    }

    // Copy the component (e.g., "3-1.3:1.0")
    size_t length = end - start;
    strncpy(output, start, length);
    output[length] = '\0';
}




/**
 * Function to execute shell commands and check for errors
 */

int execute_command(const int device_id, const char *cmd, const bool ignore_errors) {
	
    if (!cmd || *cmd == '\0') {
        fprintf(stderr, "ERROR: Invalid or empty command\n");
        return -1;
    }

	if (device_id < 0) {
		printf("Executing: %s\n", cmd);
	}
	else {
		printf("[%d] Executing: %s\n", device_id, cmd);
	}		
	
    int ret = system(cmd);
    if (ret == -1) {
        fprintf(stderr, "ERROR: Failed to execute '%s': %s\n", cmd, strerror(errno));
        return -1;
    }

    if (!ignore_errors && WIFEXITED(ret) && WEXITSTATUS(ret) != 0) {
        fprintf(stderr, "ERROR: Command '%s' failed with exit code %d\n", cmd, WEXITSTATUS(ret));
        return -1;
    }
    return 0;
}


//-----------------------------------------------------------------------------------------
// Recursive copy of all files and sub directories from one directory or device to another
//-----------------------------------------------------------------------------------------


/**
 * Comparison function for qsort
 */
int compare_names(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}


/**
 * Function to copy a single file (ignoring permissions) and return its size
 * @param src_path Source file path
 * @param dest_path Destination file path
 * @param halt_p Pointer to the halt flag. Aborts copy if true
 * @param bytes_copied_p Pointer to store the bytes copied (output)
 * @param crc_file handle for storing the name and CRC for the file being copied. Set to NULL if not required
 * @return 0 on success or halted, -1 on failure
 */
int copy_file(const char *src_path, const char *dest_path, bool *halt_p, off_t *bytes_copied_p, FILE* crc_file) {
    char error_msg[STRING_LEN];
    struct stat stat_buf;
    uint32_t crc = 0xFFFFFFFF;  // Initial CRC value

    // Get source file size
    if (stat(src_path, &stat_buf) < 0) {		
        snprintf(error_msg, sizeof(error_msg), "Failed to stat source file '%s'", src_path);
   		fprintf(stderr, "ERROR: %s\n", error_msg);
		return -1;
    }
	
    int src_fd = open(src_path, O_RDONLY);
    if (src_fd < 0) {
        snprintf(error_msg, sizeof(error_msg), "Failed to open source file '%s'", src_path);
   		fprintf(stderr, "ERROR: %s\n", error_msg);
		return -1;
    }

    int dest_fd = open(dest_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dest_fd < 0) {
        snprintf(error_msg, sizeof(error_msg), "Failed to open destination file '%s'", dest_path);
   		fprintf(stderr, "ERROR: %s\n", error_msg);
        close(src_fd);
		return -1;
    }

    char buffer[COPY_BUFFER_SIZE];
    ssize_t bytes_read;
	ssize_t crc_bytes = 0;

    while ((bytes_read = read(src_fd, buffer, COPY_BUFFER_SIZE)) > 0) {
		
		if (*halt_p) return 0;
		
        if (write(dest_fd, buffer, bytes_read) != bytes_read) {
            snprintf(error_msg, sizeof(error_msg), "Failed to write to '%s'", dest_path);
			fprintf(stderr, "ERROR: %s\n", error_msg);
            close(src_fd);
            close(dest_fd);
			return -1;
        }
		
		for (int i=0; i < bytes_read; i++) {			
			if (crc_bytes < CRC_SIZE) {
				crc = (crc << 8) ^ crc32_table[((crc >> 24) ^ buffer[i]) & 0xFF];
				crc_bytes++;
			}
		}
		
		*bytes_copied_p += bytes_read; 	
    }

    crc ^= 0xFFFFFFFF;  // Final XOR

    if (bytes_read < 0) {
        snprintf(error_msg, sizeof(error_msg), "Failed to read from '%s'", src_path);
   		fprintf(stderr, "ERROR: %s\n", error_msg);
        close(src_fd);
        close(dest_fd);
		return -1;
    }

    if (fsync(dest_fd) == -1) {
        snprintf(error_msg, sizeof(error_msg), "Failed to fsync '%s'", dest_path);
   		fprintf(stderr, "ERROR: %s\n", error_msg);
        close(src_fd);
        close(dest_fd);
		return -1;
    }

	if (crc_file) {
		// Write '<filename><tab><crc>' to the CRC file
		// skip the front (/var/ramdrive/master) part of the filename
		const char* ptr = dest_path + strlen(RAMDIR_PATH) + 1;  
		fprintf(crc_file, "%s\t%08x\n", ptr, crc);
	}
		
    close(src_fd);
    close(dest_fd);
    return 0;
}



/**
 * Function to recursively copy a directory and return total file size
 * @param src_dir Source directory path
 * @param dest_dir Destination directory path
 * @param halt_p Pointer to the halt flag. Aborts copy if true
 * @param bytes_copied_p Pointer to store total file size copied (output)
 * @param crc_file handle for storing the name and CRC for the file being copied. Set to NULL if not required
 * @return 0 on success or halted, -1 on failure
 */
int copy_directory(const char *src_dir, const char *dest_dir, bool* halt_p, off_t *bytes_copied_p, FILE* crc_file) {
	
    char error_msg[600];

	//printf("Copying files from %s to %s\n", src_dir, dest_dir);
	

	//Skip the windows hidden directory "System Volume Information".
	if (strstr(src_dir, "System Volume Information"))
	{
		printf("Ignoring \"System Volume Information\" hidden directory\n");
		return 0;
	}

    DIR *dir = opendir(src_dir);
    if (!dir) {
        snprintf(error_msg, sizeof(error_msg), "Failed to open source directory '%s'", src_dir);
   		fprintf(stderr, "ERROR: %s\n", error_msg);
		return -1;
    }

    if (mkdir(dest_dir, 0755) < 0 && errno != EEXIST) {
        snprintf(error_msg, sizeof(error_msg), "Failed to create destination directory '%s'", dest_dir);
   		fprintf(stderr, "ERROR: %s\n", error_msg);
        closedir(dir);
		return -1;
    }

    if (!halt_p) {
   		fprintf(stderr, "ERROR: copy_directory: halt is NULL\n");
		return -1;
    }
	
	
    // Fixed-size 2D array for names
    char names[MAX_FILES][PATH_LEN];
    int count = 0;

    // First pass: collect names
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        if (count >= MAX_FILES) {
            snprintf(error_msg, sizeof(error_msg), "Too many files in directory '%s' (max: %d)", src_dir, MAX_FILES);
			fprintf(stderr, "ERROR: %s\n", error_msg);
            closedir(dir);
			return -1;
        }

        if (strlen(entry->d_name) >= PATH_LEN) {
            snprintf(error_msg, sizeof(error_msg), "File name too long: '%s' (max: %d characters)", entry->d_name, PATH_LEN - 1);
			fprintf(stderr, "ERROR: %s\n", error_msg);
            closedir(dir);
			return -1;
        }

        strncpy(names[count], entry->d_name, PATH_LEN);
        names[count][PATH_LEN - 1] = '\0';
        count++;
    }
    closedir(dir);

    // Sort names alphabetically
    if (count > 0) {
        qsort(names, count, PATH_LEN, compare_names);
    }

	if (*halt_p) return 0;
	

    // Second pass: process sorted entries
    char src_path[PATH_LEN], dest_path[PATH_LEN];
    struct stat stat_buf;


    // Copy files first
    for (int i = 0; i < count; i++) {
		
		if (strlen(src_dir) + strlen(names[i]) + 2 >= PATH_LEN) {
			fprintf(stderr, "ERROR: src_dir and names[i] is too long\n");			
		}
			
		if (strlen(dest_path) + strlen(names[i]) + 2 >= PATH_LEN) {
			fprintf(stderr, "ERROR: dest_path and names[i] is too long\n");			
		}
			
		snprintf(src_path, PATH_LEN, "%s/%s", src_dir, names[i]);
        snprintf(dest_path, PATH_LEN, "%s/%s", dest_dir, names[i]);
	
        if (stat(src_path, &stat_buf) < 0) {
            snprintf(error_msg, sizeof(error_msg), "Failed to stat '%s'", src_path);
			fprintf(stderr, "ERROR: %s\n", error_msg);
			return -1;
        }

		if (*halt_p) return 0;

        if (S_ISREG(stat_buf.st_mode)) {
            if (copy_file(src_path, dest_path, halt_p, bytes_copied_p, crc_file) < 0) {
                snprintf(error_msg, sizeof(error_msg), "Failed to copy file: '%s' -> '%s'", src_path, dest_path);
				fprintf(stderr, "ERROR: %s\n", error_msg);
				return -1;
            }
			
			
        }
		
	}
	

    // Then copy directories
    for (int i = 0; i < count; i++) {
		
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"        
        snprintf(src_path, PATH_LEN, "%s/%s", src_dir, names[i]);
        snprintf(dest_path, PATH_LEN, "%s/%s", dest_dir, names[i]);
#pragma GCC diagnostic pop


        if (stat(src_path, &stat_buf) < 0) {
            snprintf(error_msg, sizeof(error_msg), "Failed to stat '%s'", src_path);
			fprintf(stderr, "ERROR: %s\n", error_msg);
			return -1;
        }

		if (*halt_p) return 0;
	
        if (S_ISDIR(stat_buf.st_mode)) {
            if (copy_directory(src_path, dest_path, halt_p, bytes_copied_p, crc_file) < 0) {
                snprintf(error_msg, sizeof(error_msg), "Failed to copy subdirectory '%s'", src_path);
				fprintf(stderr, "ERROR: %s\n", error_msg);
				return -1;
            }
        }
    }

    return 0;
}




// Initialize CRC-32 table
void initialise_crc_table() {
	
	#define CRC32_POLY 0x04C11DB7	
	
    for (int i = 0; i < 256; i++) {
        uint32_t crc = i << 24;
        for (int j = 0; j < 8; j++) {
            crc = (crc << 1) ^ ((crc & 0x80000000) ? CRC32_POLY : 0);
        }
        crc32_table[i] = crc;
    }
}
