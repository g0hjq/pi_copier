#include "globals.h"
#include "utilities.h"
#include "lcd.h"
#include "gpio.h"
#include "usb.h"

char buffer[STRING_LEN*2];
SharedDataStruct* shared_data_p = NULL;
sem_t ffmpeg_sem;
pthread_mutex_t crc_file_mutex = PTHREAD_MUTEX_INITIALIZER;



//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------
// Helper Routines
//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------
// Sets the state of a single USB Channel
void set_state(int device_id, ChannelStateEnum state) {
	shared_data_p->channel_info[device_id].state = state;
}


// Sets the state of all USB Channels
void set_all_states(ChannelStateEnum state) {
	for (int device_id=0; device_id<MAX_USB_CHANNELS; device_id++) {
		shared_data_p->channel_info[device_id].state = state;
	}

}



//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------
// run_ffmpeg
// 
// Equalizes loudness, trims silence of beginning and reduces noise in quiet sections of an MP3 file
//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------

//ffmpeg -i MAY29006.mp3 -af 
// "agate=mode=downward:ratio=1.2, silenceremove=start_periods=1:start_threshold=-45dB:start_silence=0.7, loudnorm=I=-18:TP=-2:LRA=11" 
// -ar 44.1k -ab 128k -ac 1 output06.mp3
int ffmpeg_complete_count = 0;
int ffmpeg_file_count = 0;
static pthread_mutex_t ffmpeg_count_mutex = PTHREAD_MUTEX_INITIALIZER;

void* ffmpeg_thread_function(void* arg)
{
	// These are built from RAMDIR_PATH + a d_name of up to 255 chars, so they need to be
	// the same size as the buffer the caller built the path in.
	char mp3_file[STRING_LEN*2];
	char temp_file[STRING_LEN*2];
	char buffer2[STRING_LEN*4];
	
	int ret = 0;
	
	snprintf(mp3_file, sizeof(mp3_file), "%s", (const char*)arg);
	
	const char* last_dot = strrchr(mp3_file, '.');
	size_t base_len = last_dot ? (size_t)(last_dot - mp3_file) : strlen(mp3_file);
	
	snprintf(temp_file, sizeof(temp_file), "%.*s.tmp", (int)base_len, mp3_file);


	// Use a semaphore to only allow 4 instances of ffmpeg to run at one time (one per cpu core)
	if (sem_wait(&ffmpeg_sem) == -1)
	{
		fprintf(stderr, "ERROR: sem_wait failed\n");
		return(NULL);
	}
	
	printf("*******run_ffmpeg(%s) ....STARTING\n", mp3_file);

	// run ffmpeg. output in 128K mono
	snprintf(buffer2, sizeof(buffer2), 
		"ffmpeg -i \"%s\" -y -loglevel error -af \"%s\" -f mp3 -ar 44.1K -ab 128k -ac 1 \"%s\"", 
		mp3_file, FFMPEG_FILTERS, temp_file);
	ret = execute_command(-1, buffer2, false);
	
	// Don't return early on a sem_post failure - that would permanently lose one of the
	// four slots for the rest of the run.
	if (sem_post(&ffmpeg_sem) == -1) {
    	fprintf(stderr, "ERROR: sem_post failed\n");
	}
		
	printf("*******run_ffmpeg(%s) ....FINISHED\n", mp3_file);

	// Up to MAX_FILES threads share this counter
	pthread_mutex_lock(&ffmpeg_count_mutex);
	ffmpeg_complete_count++;
	int percent_complete = (ffmpeg_complete_count*100) / ffmpeg_file_count;
	pthread_mutex_unlock(&ffmpeg_count_mutex);

	lcd_display_bargraph(percent_complete, 3);
	
	if (ret != 0) {
		fprintf(stderr, "ERROR running ffmpeg on %s - keeping the original\n", mp3_file);
		// Remove the half-written .tmp. It isn't an .mp3 so it never gets a CRC, but
		// copy_directory() would happily write it out to every stick.
		snprintf(buffer2, sizeof(buffer2), "rm -f \"%s\"", temp_file);
		execute_command(-1, buffer2, true);
		return(NULL);
	}

	snprintf(buffer2, sizeof(buffer2), "rm \"%s\"", mp3_file);
	if (execute_command(-1, buffer2, false) != 0) {
		fprintf(stderr, "ERROR deleting %s\n", mp3_file);
		return(NULL);
	}
		

	snprintf(buffer2, sizeof(buffer2), "mv \"%s\" \"%s\"", temp_file, mp3_file);		
	if (execute_command(-1, buffer2, false) != 0) {
		fprintf(stderr, "ERROR renaming %s to %s\n", temp_file, mp3_file);
	}
	
	
	return NULL;
}


// Function to check if a file is an MP3
int is_mp3(const char *filename) {
    const char *ext = strrchr(filename, '.');
    // strcasecmp, to match generate_crcs() - a ".MP3" file used to get a CRC but was
    // never loudness-normalised.
    return ext && strcasecmp(ext, ".mp3") == 0;
}


// Multi-threaded Function to process all MP3 files in a directory, 4 at a time 
int process_all_mp3_files(const char *dir_path) {
	
	int ret = 0;
	pthread_t threads[MAX_FILES];
	char* filenames[MAX_FILES];
	
	ffmpeg_file_count = 0;
	
    // Initialize the unnamed semaphore with value 4
    if (sem_init(&ffmpeg_sem, 0, NUMBER_OF_FFMPEG_THREADS) == -1) {
        perror("sem_init failed");
        exit(1);
    }

    DIR *dir = opendir(dir_path);
    if (!dir) {
		fprintf(stderr, "ERROR opening directory %s\n", dir_path);
		return -1;
    }

    struct dirent *entry;
    char mp3_file[STRING_LEN*2];


	// Get the path of each file in the directory
    while ((entry = readdir(dir))) {			
        if (entry->d_type != DT_DIR && is_mp3(entry->d_name)) {				
            if (ffmpeg_file_count >= MAX_FILES) {
				fprintf(stderr, "ERROR: Too many mp3 files in '%s' (max %d)\n", dir_path, MAX_FILES);
				break;
			}
            snprintf(mp3_file, sizeof(mp3_file), "%s/%s", dir_path, entry->d_name);
			filenames[ffmpeg_file_count] = strdup(mp3_file);
			ffmpeg_file_count++;
		}
	}
		
	printf("Found %d mp3 file(s) to optimise in %s\n", ffmpeg_file_count, dir_path);
	
	if (ffmpeg_file_count == 0) {
		closedir(dir);
		sem_destroy(&ffmpeg_sem);
		return 0;
	}
		
		
    // Create a new thread for each file
    for (int i = 0; i < ffmpeg_file_count; i++) {
		if (pthread_create(&threads[i], NULL, ffmpeg_thread_function, (void*)filenames[i]) != 0) {
			perror("pthread_create failed");
			sem_destroy(&ffmpeg_sem);
			exit(-1);				
		}
		//pthread_join(threads[i], NULL);  // waits for one thread to finish before starting the next
    }


    // Wait for all threads to complete
    for (int i = 0; i < ffmpeg_file_count; i++) {
        if (pthread_join(threads[i], NULL) != 0) {
            perror("pthread_join failed");
        }
    }


	// Free allocated strings
	for (int i = 0; i < ffmpeg_file_count; i++) {
		free(filenames[i]); 
    }
	
    // Destroy the semaphore
    sem_destroy(&ffmpeg_sem);

    closedir(dir);

	return ret;
}



//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------
// Generate CRCs
//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------

void generate_crcs(char* path, FILE *crc_file) {

	DIR *dir = opendir(path);
    if (!dir) {
        perror("opendir");
        return;
    }

    struct dirent *entry;
    struct stat statbuf;
    char *subpath = NULL;

    while ((entry = readdir(dir))) {
		
        // Skip "." and ".." entries
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        // Construct full path
        size_t path_len = strlen(path) + strlen(entry->d_name) + 2; // +2 for '/' and '\0'
        subpath = realloc(subpath, path_len);
        snprintf(subpath, path_len, "%s/%s", path, entry->d_name);

        // Get file stats
        if (stat(subpath, &statbuf) == -1) {
            perror("stat");
            continue;
        }

        if (S_ISDIR(statbuf.st_mode)) {
            // Recurse into subdirectory
            generate_crcs(subpath, crc_file);
			
        } else if (S_ISREG(statbuf.st_mode)) {
            // Check if file ends with ".mp3"
            size_t len = strlen(entry->d_name);
            if (len >= 4 && strcasecmp(&entry->d_name[len - 4], ".mp3") == 0) {
				
				// Write '<filename>[tab]<crc>' to the CRC file
				uint32_t actual_crc = compute_crc32(subpath);				
				
				pthread_mutex_lock(&crc_file_mutex);
				const char* ptr = subpath + strlen(RAMDIR_PATH) + 1;  
				fprintf(crc_file, "%s\t%08x\n", ptr, actual_crc);	
				fflush(crc_file);
				pthread_mutex_unlock(&crc_file_mutex);
            }
        }
    }

    free(subpath);
    closedir(dir);
	
}



//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------
// Processing Functions
//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------


// Tracks the pid of the client process running for each device_id (0 = none running).
// Used to reap finished clients by their exact pid, so we never interfere with the
// waitpid() that system() (used by execute_command()) does for its own children.
static pid_t client_pid[MAX_USB_CHANNELS] = {0};


// Starts a new client program in a seperate Linux process
// Returns the pid of the new process or -1 if error
int start_process(int device_id) {
		
	printf("Start client process for device number %d\n", device_id);
	
	if ((device_id < 0) || (device_id >= MAX_USB_CHANNELS)) {
		fprintf(stderr, "ERROR: Start_processs: device_id %d invalid\n", device_id);
		exit(1);
	}
	
	ChannelInfoStruct* channel_info_p = &shared_data_p->channel_info[device_id];

	// Set the state here, in the parent, *before* forking. It used to be set by the
	// child after fork(), which left a window where hub_main() could see copying == 0
	// and declare the run already finished.
	channel_info_p->state = STARTING;
	channel_info_p->start_time = time(NULL);
	channel_info_p->bytes_copied = 0;

	// Build the argument up front - the child must not touch the shared global buffer,
	// and snprintf() isn't safe to call between fork() and exec() either.
	char id_str[16];
	snprintf(id_str, sizeof(id_str), "%d", device_id);

	// Fork a new instance of the client process	
	pid_t pid = fork();
	if (pid < 0) {
		perror("Fork Failed");
		channel_info_p->state = FAILED;
		return -1;
	} 
	else if (pid != 0)
	{
		// Parent process. Remember the pid so we can reap it once it exits.
		client_pid[device_id] = pid;
	} 
	else {
		// Client process.
		// Only async-signal-safe calls from here until execvp(). This is a multithreaded
		// parent (gpio, usb monitor, ffmpeg), so a printf() here can deadlock the child
		// on stdout's lock if another thread happened to hold it at fork() time.

        // Redirect stdin to /dev/null to prevent interactive prompts and screen corruption
        int dev_null = open("/dev/null", O_RDONLY);
        if (dev_null >= 0) {
            dup2(dev_null, STDIN_FILENO);
            close(dev_null);
        }

        char *args[] = {"sudo", "./client", id_str, NULL};
        execvp(args[0], args);

        // execvp() only returns on failure. write() is async-signal-safe; fprintf isn't.
        static const char msg[] = "ERROR: failed to exec sudo ./client\n";
        ssize_t ignored = write(STDERR_FILENO, msg, sizeof(msg) - 1);
        (void)ignored;
        _exit(EXIT_FAILURE);
	}
	
	return pid;
}


// Reaps any client processes that have finished, by their exact pid, so they don't
// accumulate as zombies. Only ever waits on a specific known pid (never -1/WNOHANG on
// "any child"), so this can never race with or steal the exit status that system()
// (used internally by execute_command()) is waiting on for its own child.
void reap_finished_clients(void) {
	for (int device_id = 0; device_id < MAX_USB_CHANNELS; device_id++) {
		if (client_pid[device_id] == 0) {
			continue;
		}
		int status;
		pid_t result = waitpid(client_pid[device_id], &status, WNOHANG);
		if (result == client_pid[device_id]) {
			// Client has exited - reaped.
			client_pid[device_id] = 0;
		}
		else if (result == -1) {
			// Gone already or otherwise no longer waitable - stop tracking it.
			client_pid[device_id] = 0;
		}
		// result == 0 means still running - leave it tracked.
	}
}


// True if any client process is still running. Used to make sure a master reload never
// wipes the ramdrive out from under a duplication run.
static bool any_client_running(void) {
	for (int device_id = 0; device_id < MAX_USB_CHANNELS; device_id++) {
		if (client_pid[device_id] != 0) {
			return true;
		}
	}
	return false;
}





// quick power on check and visual indication we are ready
void test_leds() {

	printf("LED Test - All Red\n");
	set_all_states(FAILED);
	usleep(400000);

	printf("LED Test - All Yellow\n");
	set_all_states(READY);
	usleep(400000);

	printf("LED Test - All Green\n");
	set_all_states(SUCCESS);
	usleep(400000);

	printf("LED Test - Left to Right\n");
	for (int device_id=0; device_id<MAX_USB_CHANNELS; device_id++) {		
		set_all_states(EMPTY);		
		set_state(device_id, LED_TEST);
		usleep(150000);
	}

	printf("LED Test - All Off\n");
	set_all_states(EMPTY);
}



// Shows the file currently being read off the master on the bottom line of the LCD, so
// a multi-minute master copy doesn't sit on a completely static screen. Called by
// copy_directory() once per file; lcd_write_string() truncates to the 20 column width.
static void master_copy_progress(const char *filename) {
	lcd_write_string(filename, 3);
}


// Mounts the given device's first partition, wipes the ramdrive, and copies every file
// from the device onto the ramdrive. Always attempts to unmount before returning.
// Returns 0 on success, non-zero on failure.
static int copy_master_to_ramdrive(const char* device_name, off_t* total_size_out) {

    // Choose the name of the mount point.
	// If device name is /dev/sda, the mount point will be /mnt/usb/sda1
	char mount_point[STRING_LEN];
	const char* last_slash = strrchr(device_name, '/');
	if (!last_slash)
	{
		fprintf(stderr, "ERROR: device_name '%s' is not in expected format\n", device_name);
		return 1;
	}		
    snprintf(mount_point, sizeof(mount_point), "%s/%s1", MOUNT_POINT, last_slash+1);	
	
	// Append '1' to the device name to get the partition name, i.e. /dev/sdb1
	char partition_name[STRING_LEN];
	strncpy(partition_name, device_name, STRING_LEN-1);
	partition_name[STRING_LEN-1] = '\0';
	strncat(partition_name, "1", STRING_LEN - strlen(partition_name) - 1);
	printf("Mount Point=%s Partition=%s\n", mount_point, partition_name);


    // Create mount point if it doesn't exist
	snprintf(buffer, sizeof(buffer), "sudo mkdir -p %s", mount_point);
	if (execute_command(-1, buffer, false) != 0) {
		fprintf(stderr, "ERROR: Creating mount point\n");
		return 1;
	}


    // Mount the USB drive
	snprintf(buffer, sizeof(buffer), "sudo mount %s %s", partition_name, mount_point);
	if (execute_command(-1, buffer, false) != 0) {
		fprintf(stderr, "ERROR: Mounting the USB drive\n");
		return 1;
	}

 
	// Empty the ramdrive
    snprintf(buffer, sizeof(buffer), "sudo rm -rf %s/*", RAMDIR_PATH);
	if (execute_command(-1, buffer, false) != 0) {
		fprintf(stderr, "ERROR: empty_directory failed\n");
		snprintf(buffer, sizeof(buffer), "sudo umount %s", mount_point);
		execute_command(-1, buffer, true);
        return 1;
	}

	
	*total_size_out = 0;
	bool halt = false;
	printf("Copying master files from %s to %s (this can take a while for large libraries)...\n",
		mount_point, RAMDIR_PATH);

	set_copy_progress_callback(master_copy_progress);
	int copy_result = copy_directory(mount_point, RAMDIR_PATH, &halt, total_size_out);
	set_copy_progress_callback(NULL);

	if (copy_result != 0) {
		fprintf(stderr, "ERROR: copy_directory failed\n");
		snprintf(buffer, sizeof(buffer), "sudo umount %s", mount_point);
		execute_command(-1, buffer, true);
        return 1;
	}

	printf("Total Size=%lu\n", *total_size_out);

    // Unmount the USB drive
	snprintf(buffer, sizeof(buffer), "sync %s", mount_point);
	if (execute_command(-1, buffer, false) != 0) {
		fprintf(stderr, "ERROR: Cannot sync device\n");
		return 1;
	}
	
	snprintf(buffer, sizeof(buffer), "sudo umount %s", mount_point);
	if (execute_command(-1, buffer, false) != 0) {
		fprintf(stderr, "ERROR: Unmounting the USB drive %s\n", mount_point);
		return 1;
	}

	return 0;
}


// Re-encodes every mp3 on the ramdrive and (re)writes crc.txt to match. Returns 0 on success.
// Caller is responsible for showing an appropriate "please wait" message before calling this.
static int optimize_and_generate_crcs(void) {

	process_all_mp3_files(RAMDIR_PATH);

	lcd_display_message("Calculating", "Checksums", NULL, NULL);

	initialise_crc_table();
	FILE *crc_file = fopen(CRC_FILE, "w");
	if (!crc_file) {
		fprintf(stderr, "ERROR: Cannot create CRC file %s\n", CRC_FILE);
		return 1;
	}

	generate_crcs(RAMDIR_PATH, crc_file);
	printf("Generate CRCs finished\n");

	if (fclose(crc_file) == -1) {
		perror("close crc_file");
		return 1;
	}

	// ffmpeg has just re-encoded every mp3 to 128k mono and trimmed the leading silence,
	// so the byte count captured while reading the master no longer describes what is on
	// the ramdrive. Recompute it here, at the point the contents are final. This is the
	// figure the client's partition size check and hub_main()'s progress bar are both
	// measured against - left stale, the bar would top out well short of 100%.
	off_t size_before = shared_data_p->total_size;
	shared_data_p->total_size = (off_t)get_directory_size(RAMDIR_PATH);

	printf("Ramdrive size after optimising: %lluMB (was %lluMB before ffmpeg)\n",
		(unsigned long long)(shared_data_p->total_size / 1024 / 1024),
		(unsigned long long)(size_before / 1024 / 1024));

	if (shared_data_p->total_size == 0) {
		fprintf(stderr, "ERROR: Ramdrive appears to be empty after optimising\n");
		return 1;
	}

	return 0;
}


// Prompts the user to insert the master USB in slot one. 
// Recursively copies all files to the ramdrive
int load_master() {
	
	lcd_display_message(NULL, "Insert Master", "in slot 1", NULL);
	set_all_states(EMPTY);
	set_state(0, INDICATING);

	ChannelInfoStruct* channel_info_p = &shared_data_p->channel_info[0];

	// Wait for USB inserted
	printf("Waiting for master USB to be inserted\n");
	while(channel_info_p->device_name[0] == '\0') {
		usleep(100000);
	}
	
	printf("found master : name=%s path=%s\n", channel_info_p->device_name, channel_info_p->device_path);
	
	set_state(0, COPYING);
	// Line 3 is left clear - copy_master_to_ramdrive() shows the current file there
	lcd_display_message("Reading Master", NULL, channel_info_p->device_name, NULL);
	
	int result = copy_master_to_ramdrive(channel_info_p->device_name, &shared_data_p->total_size);

	// If this boot master happens to also be labelled "MASTER", the monitor thread will have
	// raised master_reload_requested for it too - it's already been handled above, so clear
	// the flag to stop the main loop redundantly reloading the exact same data again.
	shared_data_p->master_reload_requested = false;

	return result;
}


// Re-reads the master data from a newly-inserted "MASTER"-labelled USB drive in slot 0, at
// any point after boot, without requiring the port-mapping dance in map_usb_port_numbers()
// to be repeated. Called from the main loop whenever master_reload_requested is set.
void reload_master(void) {

	int master_device_id = shared_data_p->master_device_id;
	char master_device_name[STRING_LEN];
	snprintf(master_device_name, sizeof(master_device_name), "%s", shared_data_p->master_device_name);

	printf("Reloading master from %s (device id=%d)\n", master_device_name, master_device_id);

	lcd_display_message("Reading New Master", NULL, master_device_name, NULL);

	if (copy_master_to_ramdrive(master_device_name, &shared_data_p->total_size) != 0) {
		lcd_display_error_message("Failed to read", "new master");
		shared_data_p->channel_info[master_device_id].state = FAILED;
		shared_data_p->master_reload_requested = false;
		return;
	}

	snprintf(buffer, sizeof(buffer), "Read %luMB", shared_data_p->total_size / 1024 / 1024);
	lcd_display_message(buffer, NULL, "Optimising MP3 files", "Please Wait");

	if (optimize_and_generate_crcs() != 0) {
		lcd_display_error_message("Failed to process", "new master");
		shared_data_p->channel_info[master_device_id].state = FAILED;
		shared_data_p->master_reload_requested = false;
		return;
	}

	printf("Master reload complete\n");
	beep();
	lcd_display_message("New Master Loaded", NULL, "Insert blank USBs", "then push button");

	// Slot stays INDICATING (special/reserved) while the master remains plugged in;
	// usb.c reverts it to EMPTY automatically once the drive is physically removed.
	shared_data_p->master_reload_requested = false;
}


// Bump this if the identifier format written to PORT_MAP_FILE ever changes again -
// it lets an old-format file be recognised as stale and trigger a fresh remap
// instead of loading identifiers that will never match anything.
#define PORT_MAP_FORMAT "by-path-v1"


// Loads a previously-saved USB port map (physical socket -> stable device identifier)
// from disk into channel_info[].device_path. The identifier is a /dev/disk/by-path
// name, which is safe to trust as-is across reboots (see get_disk_by_path_id() in
// usb.c for why). A missing file, or one written in an older/different format,
// returns 0 so the caller falls back to remapping interactively.
// Returns the number of slots successfully populated (0..MAX_USB_CHANNELS).
static int load_port_map(void) {

	FILE *f = fopen(PORT_MAP_FILE, "r");
	if (!f) {
		printf("No saved USB port map found at %s\n", PORT_MAP_FILE);
		return 0;
	}

	char line[STRING_LEN];

	if (!fgets(line, sizeof(line), f) || strncmp(line, PORT_MAP_FORMAT, strlen(PORT_MAP_FORMAT)) != 0) {
		printf("Saved port map at %s is missing or in an old format - ignoring it\n", PORT_MAP_FILE);
		fclose(f);
		return 0;
	}

	while (fgets(line, sizeof(line), f)) {
		int device_id;
		char path[STRING_LEN];
		if (sscanf(line, "%d=%255s", &device_id, path) == 2) {
			if (device_id >= 0 && device_id < MAX_USB_CHANNELS) {
				snprintf(shared_data_p->channel_info[device_id].device_path,
					sizeof(shared_data_p->channel_info[device_id].device_path), "%s", path);
			}
			else {
				fprintf(stderr, "WARNING: Ignoring invalid device_id %d in %s\n", device_id, PORT_MAP_FILE);
			}
		}
	}

	fclose(f);

	int loaded = 0;
	for (int device_id = 0; device_id < MAX_USB_CHANNELS; device_id++) {
		if (shared_data_p->channel_info[device_id].device_path[0] != '\0') {
			loaded++;
		}
	}

	printf("Loaded %d/%d USB port mappings from %s\n", loaded, MAX_USB_CHANNELS, PORT_MAP_FILE);
	return loaded;
}


// Saves the current channel_info[].device_path values to disk so map_usb_port_numbers()
// doesn't need to be repeated on every boot. Written to a temp file in /tmp (always
// writable) then copied into place with sudo, since /boot/firmware's exact mount
// permissions can vary.
static void save_port_map(void) {

	char tmp_path[] = "/tmp/port_map.XXXXXX";
	int fd = mkstemp(tmp_path);
	if (fd < 0) {
		fprintf(stderr, "ERROR: Could not create temp file for port map: %s\n", strerror(errno));
		return;
	}

	FILE *f = fdopen(fd, "w");
	if (!f) {
		fprintf(stderr, "ERROR: Could not open temp file for port map: %s\n", strerror(errno));
		close(fd);
		remove(tmp_path);
		return;
	}

	fprintf(f, "%s\n", PORT_MAP_FORMAT);

	int saved = 0;
	for (int device_id = 0; device_id < MAX_USB_CHANNELS; device_id++) {
		const char* path = shared_data_p->channel_info[device_id].device_path;
		if (path[0] != '\0') {
			fprintf(f, "%d=%s\n", device_id, path);
			saved++;
		}
	}

	fclose(f);

	char cmd[PATH_LEN*2];
	snprintf(cmd, sizeof(cmd), "sudo cp %s %s", tmp_path, PORT_MAP_FILE);
	if (execute_command(-1, cmd, false) != 0) {
		fprintf(stderr, "ERROR: Could not copy port map into %s\n", PORT_MAP_FILE);
	}
	else {
		printf("Saved USB port map (%d/%d slots) to %s\n", saved, MAX_USB_CHANNELS, PORT_MAP_FILE);
	}

	remove(tmp_path);
}


// Map USB port numbers
//
// Prompts the user to insert a usb drive in each port in turn to obtain
// the path of each USB port so we can associate the hub's port paths with 
// the usb port number to enable us to light the correct LEDs
void map_usb_port_numbers(void)
{	
	char message[STRING_LEN+20];
	uint32_t ports_mapped = 0;
	
	strcpy(message, "Push Button to skip");
	
	while (ports_mapped < MAX_USB_CHANNELS)
	{
		ChannelInfoStruct* channel_info_p = &shared_data_p->channel_info[ports_mapped];
		channel_info_p->state = INDICATING;
		
		lcd_clear();
		beep();
		
		sprintf(buffer, "in socket %d", ports_mapped+1);
		lcd_display_message( (ports_mapped == 0)?"Insert first device":"Insert next device",
					buffer,
					NULL,
					message);
		
		
		while (true) {
						
			if ((get_button_state0() == BUTTON_SHORT_PRESS) || (get_button_state1() == BUTTON_SHORT_PRESS)) {
				channel_info_p->state = EMPTY;
				channel_info_p->device_path[0] = '\0';
				ports_mapped++;
				// Keep channels_active in step with ports_mapped. Without this, the
				// next device inserted was given device_id = channels_active, which
				// pointed at an already-mapped slot and silently overwrote it - and
				// the "channels_active > ports_mapped" test below then never fired.
				shared_data_p->channels_active = ports_mapped;
				usleep(500000);
				break;
			}

			if (shared_data_p->channels_active > ports_mapped)
			{
				snprintf(message, sizeof(message), "[Port %u=%s]", 
					shared_data_p->channels_active, channel_info_p->device_path);
				channel_info_p->state = READY;
				ports_mapped++;
				
				usleep(200000);
				break;
			}			
			
			usleep(100000);
		}		
	}	

	lcd_clear();	
}


//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------
// Main Loop
//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------



int run(int hub_number) {
	
	print_shared_data(shared_data_p);
	
	int result = 0;

	for (int device_id=0; device_id<MAX_USB_CHANNELS; device_id++) {

		ChannelInfoStruct* channel_info_p = &shared_data_p->channel_info[device_id];	
		
		// Clears error lights from previous fails when the drive has been removed
		if (channel_info_p->device_name[0] == '\0') {
			channel_info_p->state = EMPTY;
		}
		
		if (channel_info_p->hub_number == hub_number) {
			
			printf("%d=%s %s (%s)\n", device_id,
				get_state_name(channel_info_p->state), channel_info_p->device_name, channel_info_p->device_path);
						
			if ((channel_info_p->state == READY) || 
			    (channel_info_p->state == SUCCESS) || 
				(channel_info_p->state == FAILED) || 
				(channel_info_p->state == CRC_FAILED)) {
					
				int pid = start_process(device_id);					
				if (pid < 0) {
					fprintf(stderr, "ERROR: start_process failed\n");
					result = 1;
				}
				usleep(250000);	
			}
		}
	}
	
	return result;
}



// Tidily stop all running client processes on the specified hub
void terminate(int hub_number) {
	for (int device_id=0; device_id<MAX_USB_CHANNELS; device_id++) {		
		ChannelInfoStruct* channel_info_p = &shared_data_p->channel_info[device_id];
		if (channel_info_p->hub_number == hub_number)
		{
			printf("....Halting Channel %d\n", channel_info_p->port_number);
			channel_info_p->halt = true;
		}
	}
}



// Monitors the button, starts and stops coping, 
// and displays the progress of all ports in one USB hub
void hub_main(int hub_number, ButtonStateEnum button_state) 
{
	static struct timeval start_time[NUMBER_OF_HUBS] = {0};
	static struct timeval end_time[NUMBER_OF_HUBS] = {0};
	static bool channel_busy[NUMBER_OF_HUBS] = {false};

	// count the number of running, failed and finished processes for this hub
	int copying = 0; 
	int verifying = 0;
	int fail = 0; 
	int pass = 0; 
	off_t total_bytes_copied = 0;
	int lcd_line = (hub_number==0) ? 2 : 0;

	for (int i=0; i<MAX_USB_CHANNELS; i++)
	{		
		ChannelInfoStruct* channel_info_p = &shared_data_p->channel_info[i];

		if (channel_info_p->hub_number == hub_number)
		{
			// Only accumulate bytes for channels that are also included in the count
			// below. bytes_copied is never cleared when a drive is removed, so an idle
			// slot can still be holding the figure from an earlier run - summing those
			// in while count only tallies the active channels made the percentage far
			// too high. Running a single stick after a full run pegged the bar at 100%
			// before the copy had even started.
			ChannelStateEnum state = channel_info_p->state;
			if ((state == STARTING) || (state == ERASING) || 
				(state == FORMATING) || (state == PARTITIONING) || (state == MOUNTING) ||
				(state == COPYING) || (state == UNMOUNTING)) { 
				copying++; 
				total_bytes_copied += channel_info_p->bytes_copied;
			}
			else if (state == VERIFYING) { 
				verifying++; 
				total_bytes_copied += channel_info_p->bytes_copied;
			}
			else if (state == SUCCESS) { 
				pass++; 
				total_bytes_copied += channel_info_p->bytes_copied;
			}
			else if ((state == FAILED) || (state == CRC_FAILED)) { 
				fail++; 
				total_bytes_copied += channel_info_p->bytes_copied;
			}
		}
	}				

	if (channel_busy[hub_number]) {
		// Usb hub is busy.
		if (button_state == BUTTON_LONG_PRESS)
		{
			printf("Terminate %d\n", hub_number);
			terminate(hub_number);
			lcd_write_string("CANCELLED", lcd_line);
			error_beep();
			channel_busy[hub_number] = false;
		} 
		else {		
			if (copying > 0) {
			    // copying. Display progress so far
				float percent;
				int count = copying + verifying + pass + fail;
				if ((count == 0) || (shared_data_p->total_size==0))
					percent = 0;
				else
					percent = 100.0 * total_bytes_copied / shared_data_p->total_size / count;
				
				sprintf(buffer, "Busy=%-2u OK=%-2u Bad=%-2u", copying + verifying, pass, fail);		
				lcd_write_string(buffer, lcd_line);				
				lcd_display_bargraph(percent, lcd_line+1);				
			}
			else if (verifying > 0) {
				lcd_write_string("Verifying", lcd_line+1);
			}
			else 
			{
				// Copy has just finished. Display a summary
				printf("Channel %d finished\n", hub_number);
				gettimeofday(&end_time[hub_number], NULL);
				int seconds = end_time[hub_number].tv_sec - start_time[hub_number].tv_sec;
				
				sprintf(buffer, "Done. OK=%-2u Bad=%-2u", pass, fail);		
				lcd_write_string(buffer, lcd_line);				

				snprintf(buffer, sizeof(buffer), "Wrote %luMB in %1u:%02u", total_bytes_copied/1024/1024, seconds/60, seconds%60);
				lcd_write_string(buffer, lcd_line + 1);

				if (fail > 0) {
					error_beep();
				}
				else {
					beep();
				}
				
				channel_busy[hub_number] = false;
			}
		}
	}
	else  {

		// USB hub is not busy. Wait for a button press
		if (button_state == BUTTON_SHORT_PRESS) {
			printf("Channel %d start\n", hub_number);
			gettimeofday(&start_time[hub_number], NULL);
			channel_busy[hub_number] = true;
			lcd_write_string("", lcd_line);
			lcd_write_string("", lcd_line+1);		
			beep();
			run(hub_number);
		}
	}
}


//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------
// Program Main
//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------

int main() {

    // Force line-buffered stdout so log messages show up immediately even when this
    // isn't attached to a terminal (e.g. running under systemd) - otherwise printf()
    // output sits in a several-KB buffer and can appear to "stop" for long stretches
    // even though the program is still working.
    setvbuf(stdout, NULL, _IOLBF, 0);

    // Create shared memory object
    int shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("shm_open");
        exit(1);
    }
	
    // Set size of shared memory 
    if (ftruncate(shm_fd, sizeof(SharedDataStruct)) == -1) {
        perror("ftruncate");
        exit(1);
    }

    // Map shared memory
	shared_data_p = mmap(0, sizeof(SharedDataStruct), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shared_data_p == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
	
	// Close in parent, child will inherit if needed
    if (close(shm_fd) == -1) {
        perror("close shm_fd");
        exit(1);
    }

	// initialise values in shared memory
	memset(shared_data_p, 0, sizeof(SharedDataStruct));
	shared_data_p->channels_active = 0;
	
	for (int device_id=0; device_id<MAX_USB_CHANNELS; device_id++) {	
		ChannelInfoStruct* channel_info_p = &shared_data_p->channel_info[device_id];	
		channel_info_p->device_id = device_id;
		channel_info_p->hub_number = device_id / PORTS_PER_CHANNEL;
		channel_info_p->port_number = device_id % PORTS_PER_CHANNEL;
		channel_info_p->state = EMPTY;
	}
	
	// Try to reuse a previously-saved physical port map so the user doesn't have to
	// redo map_usb_port_numbers() on every single boot. The topology path for a given
	// socket only changes if the wiring itself changes, so a complete saved map is
	// trusted as-is. An incomplete or missing file falls back to the interactive dance
	// further down, which saves a fresh map once it's done. This must happen before
	// usb_init() starts the monitor thread, so every device it ever sees is matched
	// against the correct slot from the very first scan.
	bool port_map_loaded = (load_port_map() == MAX_USB_CHANNELS);
	if (port_map_loaded) {
		shared_data_p->channels_active = MAX_USB_CHANNELS;
	}
	
	// Initialise the LCD *before* the GPIO thread starts - that thread can open the
	// system menu (and so drive the LCD) the moment both buttons are held.
	lcd_init(shared_data_p);
	gpio_init(shared_data_p);
	lcd_display_message("RPi USB Duplicator", "---", VERSION_STRING, "(Gary Bleads G0HJQ)");
	usb_init(shared_data_p);
	
	test_leds();
	test_leds();
	double_beep();
	
	load_master();

	snprintf(buffer, sizeof(buffer), "Read %luMB", shared_data_p->total_size / 1024 / 1024);
	lcd_display_message(buffer, NULL, "Optimising MP3 files", "Please Wait");

	if (optimize_and_generate_crcs() != 0) {
		fprintf(stderr, "ERROR: Failed to optimise/checksum master data\n");
		exit(1);
	}

	lcd_display_message(NULL, "Please", "Remove Master USB", NULL);
	set_state(0, READY);
	beep();
	
	// Wait for USB removed
	printf("Waiting for master USB to be removed\n");
	while(shared_data_p->channel_info[0].device_name[0] != '\0') {
		usleep(200000);
	}
	
	if (port_map_loaded) {
		printf("Using saved USB port map - skipping interactive port mapping\n");
		lcd_display_message("Using saved", "USB port map", NULL, NULL);
		usleep(1500000);
	}
	else {
		lcd_clear();
		beep();
		usleep(500000);

		shared_data_p->channel_info[0].device_path[0] = '\0';
		shared_data_p->channels_active = 0;

		// Ask the user to load a blank usb stick into each slot in turn
		// so we can work out the channel number (and hence LEDs) to associate with each USB slot
		get_button_state0();
		get_button_state1();
		map_usb_port_numbers();
		get_button_state0();
		get_button_state1();

		save_port_map();
	}
	
	usleep(100000);
	beep();	
	lcd_display_message("Insert Blank Disks", NULL, "Push button to start", NULL);
	
	// Discard anything raised during the setup dance above - the boot master was already
	// handled by load_master(), and we don't want to react mid-mapping.
	shared_data_p->master_reload_requested = false;
	
	bool starting = true;
	bool reload_deferred = false;
	while(true) {
		
		reap_finished_clients();
		
		if (shared_data_p->master_reload_requested) {
			if (any_client_running()) {
				// reload_master() does "rm -rf RAMDIR_PATH/*" - never do that while
				// clients are still copying out of it. The request stays pending.
				if (!reload_deferred) {
					printf("Master reload requested, but a run is in progress - deferring\n");
					reload_deferred = true;
				}
			}
			else {
				reload_deferred = false;
				reload_master();
				starting = true;
			}
		}
		
		ButtonStateEnum button_state0 = get_button_state0();
		ButtonStateEnum button_state1 = get_button_state1();

		if (starting && ((button_state0 != BUTTON_NOT_PRESSED) || (button_state1 != BUTTON_NOT_PRESSED)))
		{
			lcd_clear();
			starting = false;
		}

		hub_main(0, button_state1);
		hub_main(1, button_state0);
		
		usleep(100000);
	}
	
	
    // Cleanup
    if (munmap(shared_data_p, sizeof(SharedDataStruct)) == -1) {
        perror("munmap");
        exit(1);
    }

    if (shm_unlink(SHM_NAME) == -1) {
        perror("shm_unlink");
        exit(1);
    }

	usb_cleanup();
	lcd_cleanup();
	gpio_cleanup();
	
    return 0;
}
