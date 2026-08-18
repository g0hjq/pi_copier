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
        return ret;
    }

    // A command killed by a signal (e.g. the OOM killer during four parallel ffmpegs)
    // never sets WIFEXITED, so without this it was silently treated as success.
    if (!ignore_errors && WIFSIGNALED(ret)) {
        fprintf(stderr, "ERROR: Command '%s' killed by signal %d\n", cmd, WTERMSIG(ret));
        return ret;
    }

    return 0;
}


//-----------------------------------------------------------------------------------------
// Recursive copy of all files and sub directories from one directory or device to another
//-----------------------------------------------------------------------------------------


// Optional per-file progress hook - see set_copy_progress_callback() in utilities.h
static void (*copy_progress_callback)(const char *filename) = NULL;

void set_copy_progress_callback(void (*callback)(const char *filename)) {
    copy_progress_callback = callback;
}


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
 * @return 0 on success or halted, -1 on failure
 */
int copy_file(const char *src_path, const char *dest_path, volatile bool *halt_p, off_t *bytes_copied_p) {
    struct stat stat_buf;

    // Get source file size
    if (stat(src_path, &stat_buf) < 0) {		
   		fprintf(stderr, "ERROR: Failed to stat source file '%s'\n", src_path);
		return -1;
    }
	
    int src_fd = open(src_path, O_RDONLY);
    if (src_fd < 0) {
   		fprintf(stderr, "ERROR: Failed to open source file '%s'\n", src_path);
		return -1;
    }

    int dest_fd = open(dest_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dest_fd < 0) {
   		fprintf(stderr, "ERROR: Failed to open destination file '%s'\n", dest_path);
        close(src_fd);
		return -1;
    }

    char buffer[COPY_BUFFER_SIZE];
    ssize_t bytes_read;

    while ((bytes_read = read(src_fd, buffer, COPY_BUFFER_SIZE)) > 0) {
		
		if (*halt_p) return 0;
		
        if (write(dest_fd, buffer, bytes_read) != bytes_read) {
			fprintf(stderr, "ERROR: Failed to write to '%s'\n", dest_path);
            close(src_fd);
            close(dest_fd);
			return -1;
        }
		
		*bytes_copied_p += bytes_read; 	
    }

    if (bytes_read < 0) {
   		fprintf(stderr, "ERROR: Failed to read from '%s'\n", src_path);
        close(src_fd);
        close(dest_fd);
		return -1;
    }

    if (fsync(dest_fd) == -1) {
   		fprintf(stderr, "ERROR: Failed to fsync '%s'\n", dest_path);
        close(src_fd);
        close(dest_fd);
		return -1;
    }

    close(src_fd);
    close(dest_fd);
    return 0;
}


// Remove odd characters such as "?" from the filename as these cause errors if written to a FAT32 usb drive
void sanitize_filename(char *filename) {
    for (int i = 0; filename[i]; i++) {
        if (filename[i] == '?' || filename[i] == '*' || filename[i] == '/' || filename[i] == '\\' || filename[i] < 32) {
            filename[i] = '_'; // Replace invalid characters with underscore
        }
    }
}

// Truncates filename in place to at most max_len characters, keeping the extension.
// Idempotent - which matters, because it is applied twice (master -> ramdrive, then
// ramdrive -> USB) and the CRC file records the ramdrive names.
void shorten_filename(char *filename, size_t max_len) {

    size_t len = strlen(filename);
    if (len <= max_len) {
        return;
    }

    const char *ext = strrchr(filename, '.');
    size_t ext_len = ext ? strlen(ext) : 0;

    // No usable extension (or one so long it would leave nothing of the stem) -
    // just cut the name short. The old code blanked the name entirely here, which
    // then produced a dest_path of "dir/" and failed the whole copy.
    if (ext_len == 0 || ext_len >= max_len) {
        filename[max_len] = '\0';
        return;
    }

    size_t keep_len = max_len - ext_len;
    memmove(filename + keep_len, ext, ext_len + 1);  // overlapping, includes the NUL
}



/**
 * Copies name into out (size out_size), truncating safely if it somehow doesn't fit.
 * Used instead of snprintf("%s") so GCC can see a hard upper bound on the length -
 * it treats names[i] as potentially unterminated within the whole 2D array otherwise,
 * which trips -Wformat-truncation under -Werror.
 */
static void copy_name(char *out, size_t out_size, const char *name) {
    size_t len = strnlen(name, out_size - 1);
    memcpy(out, name, len);
    out[len] = '\0';
}


/**
 * Joins dir + "/" + name into out. Returns false (having logged the problem) if the
 * result wouldn't fit, rather than letting snprintf silently truncate - a truncated
 * path here would mean reading or writing the wrong file, which is far worse than
 * failing the copy.
 */
static bool build_path(char *out, size_t out_size, const char *dir, const char *name) {

    size_t dir_len  = strnlen(dir, out_size);
    size_t name_len = strnlen(name, out_size);

    if (dir_len + 1 + name_len + 1 > out_size) {
        fprintf(stderr, "ERROR: Path too long: '%.*s/%.*s'\n",
            (int)dir_len, dir, (int)name_len, name);
        return false;
    }

    memcpy(out, dir, dir_len);
    out[dir_len] = '/';
    memcpy(out + dir_len + 1, name, name_len);
    out[dir_len + 1 + name_len] = '\0';
    return true;
}


/**
 * Returns the total size in bytes of every regular file under path, recursively.
 * Returns 0 if the directory can't be read at all.
 *
 * This was declared in utilities.h but never actually implemented. It mirrors
 * copy_directory()'s rules - skips the Windows "System Volume Information" folder and
 * counts regular files only - so the figure it returns matches what a copy of the same
 * tree would actually write.
 */
uint64_t get_directory_size(const char *path) {

    DIR *dir = opendir(path);
    if (!dir) {
        return 0;
    }

    uint64_t total = 0;
    struct dirent *entry;
    char subpath[PATH_LEN];
    struct stat statbuf;

    while ((entry = readdir(dir)) != NULL) {

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        if (!build_path(subpath, sizeof(subpath), path, entry->d_name)) {
            continue;
        }

        // stat, not lstat - copy_directory() follows symlinks and copies the target,
        // so this must do the same or the two totals won't agree.
        if (stat(subpath, &statbuf) < 0) {
            continue;
        }

        if (S_ISDIR(statbuf.st_mode)) {
            // copy_directory() skips this one, so don't count it either
            if (strstr(entry->d_name, "System Volume Information")) {
                continue;
            }
            total += get_directory_size(subpath);
        }
        else if (S_ISREG(statbuf.st_mode)) {
            total += (uint64_t)statbuf.st_size;
        }
    }

    closedir(dir);
    return total;
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
int copy_directory(const char *src_dir, const char *dest_dir, volatile bool* halt_p, off_t *bytes_copied_p) {

	//printf("Copying files from %s to %s\n", src_dir, dest_dir);
	

	//Skip the windows hidden directory "System Volume Information".
	if (strstr(src_dir, "System Volume Information"))
	{
		printf("Ignoring \"System Volume Information\" hidden directory\n");
		return 0;
	}

    DIR *dir = opendir(src_dir);
    if (!dir) {
   		fprintf(stderr, "ERROR: Failed to open source directory '%s'\n", src_dir);
		return -1;
    }

    if (mkdir(dest_dir, 0755) < 0 && errno != EEXIST) {
   		fprintf(stderr, "ERROR: Failed to create destination directory '%s'\n", dest_dir);
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
			fprintf(stderr, "ERROR: Too many files in directory '%s' (max: %d)\n", src_dir, MAX_FILES);
            closedir(dir);
			return -1;
        }

        if (strlen(entry->d_name) >= PATH_LEN) {
			fprintf(stderr, "ERROR: File name too long: '%s' (max: %d characters)\n",
				entry->d_name, PATH_LEN - 1);
            closedir(dir);
			return -1;
        }

        copy_name(names[count], PATH_LEN, entry->d_name);
        count++;
    }
    closedir(dir);

    // Sort names alphabetically
    if (count > 0) {
        qsort(names, count, PATH_LEN, compare_names);
    }

	if (*halt_p) return 0;
	

    // Second pass: process sorted entries
    char src_path[PATH_LEN], dest_path[PATH_LEN], dest_name[PATH_LEN];
    struct stat stat_buf;



	
    for (int i = 0; i < count; i++) {

		// NOTE: the old length pre-checks here read dest_path before it had ever been
		// written (undefined behaviour on the first iteration) and only printed anyway.
		// build_path() does the check properly and actually fails on overflow.

		if (!build_path(src_path, sizeof(src_path), src_dir, names[i])) {
			return -1;
		}

		// Remove any invalid characters such as "?" and "*" as these cause errors 
		// if written to a FAT32 flash drive
		copy_name(dest_name, sizeof(dest_name), names[i]);
		sanitize_filename(dest_name);

		// Truncate the name to 64 characters to avoid string overflows
		shorten_filename(dest_name, 64);

		if (!build_path(dest_path, sizeof(dest_path), dest_dir, dest_name)) {
			return -1;
		}

        if (stat(src_path, &stat_buf) < 0) {
			fprintf(stderr, "ERROR: Failed to stat '%s'\n", src_path);
			return -1;
        }

		if (*halt_p) return 0;

        if (S_ISREG(stat_buf.st_mode)) {

			if (copy_progress_callback) {
				copy_progress_callback(dest_name);
			}

            if (copy_file(src_path, dest_path, halt_p, bytes_copied_p) < 0) {
				fprintf(stderr, "ERROR: Failed to copy file: '%s' -> '%s'\n", src_path, dest_path);
				return -1;
            }
        }
	}
	

	

    // Then copy directories
    for (int i = 0; i < count; i++) {

        // Directory names need the same treatment as file names - an unsanitised "?"
        // or "*" makes mkdir() fail on FAT32 and aborts the entire copy. Applying the
        // identical (idempotent) transform on both hops keeps the ramdrive and the USB
        // in step, which is what lets verify() find the files by their CRC-file names.
        copy_name(dest_name, sizeof(dest_name), names[i]);
        sanitize_filename(dest_name);
        shorten_filename(dest_name, 64);

        if (!build_path(src_path, sizeof(src_path), src_dir, names[i])) {
            return -1;
        }
        if (!build_path(dest_path, sizeof(dest_path), dest_dir, dest_name)) {
            return -1;
        }

        if (stat(src_path, &stat_buf) < 0) {
			fprintf(stderr, "ERROR: Failed to stat '%s'\n", src_path);
			return -1;
        }

		if (*halt_p) return 0;
	
        if (S_ISDIR(stat_buf.st_mode)) {
            if (copy_directory(src_path, dest_path, halt_p, bytes_copied_p) < 0) {
				fprintf(stderr, "ERROR: Failed to copy subdirectory '%s'\n", src_path);
				return -1;
            }
        }
    }

    return 0;
}




// Initialize CRC-32 table
void initialise_crc_table() {

    for (int i = 0; i < 256; i++) {
        uint32_t crc = i << 24;
        for (int j = 0; j < 8; j++) {
            crc = (crc << 1) ^ ((crc & 0x80000000) ? CRC32_POLY : 0);
        }
        crc32_table[i] = crc;
    }
}




// Compute CRC-32 checksum of a file. Only the first CRC_SIZE bytes are used.
uint32_t compute_crc32(char *filename) {

    FILE *file = fopen(filename, "rb");
    if (!file) {
        fprintf(stderr, "VERIFY ERROR : Compute CRC cannot open file %s\n", filename);
		return 0;
    }

    uint32_t crc = 0xFFFFFFFF;  // Initial CRC value
	uint32_t crc_bytes = 0;
    int byte;

    while (((byte = fgetc(file)) != EOF) && (crc_bytes < CRC_SIZE)) {
		crc = (crc << 8) ^ crc32_table[((crc >> 24) ^ byte) & 0xFF];
		crc_bytes++;
	}

    crc ^= 0xFFFFFFFF;  // Final XOR
    fclose(file);
	
    return crc;
}



// Reads the filesystem LABEL of a partition device (e.g. "/dev/sdc1") using blkid.
// Returns true and fills label_out with a null-terminated label if one was found,
// false (with label_out left as an empty string) otherwise.
bool get_volume_label(const char *partition_name, char *label_out, size_t label_out_size) {

	label_out[0] = '\0';

	char cmd[PATH_LEN];
	snprintf(cmd, sizeof(cmd), "sudo blkid -s LABEL -o value %s 2>/dev/null", partition_name);

	FILE *fp = popen(cmd, "r");
	if (!fp) {
		fprintf(stderr, "ERROR: get_volume_label: popen failed for %s\n", partition_name);
		return false;
	}

	bool got_line = (fgets(label_out, label_out_size, fp) != NULL);
	pclose(fp);

	if (!got_line) {
		label_out[0] = '\0';
		return false;
	}

	// Strip the trailing newline fgets leaves in place
	size_t len = strlen(label_out);
	if (len > 0 && label_out[len-1] == '\n') {
		label_out[len-1] = '\0';
	}

	return (label_out[0] != '\0');
}

