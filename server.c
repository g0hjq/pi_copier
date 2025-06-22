
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
void* ffmpeg_thread_function(void* arg)
{
	char mp3_file[STRING_LEN];
	char temp_file[STRING_LEN];
	char buffer2[STRING_LEN*2];
	
	int ret = 0;
	
	strcpy(mp3_file, (char*)arg);
	
	const char* last_dot = strrchr(mp3_file, '.');
	size_t base_len = last_dot - mp3_file;
	
	strncpy(temp_file, mp3_file, base_len);
	temp_file[base_len] = 0;
	strcat(temp_file, ".tmp");


	// Use a semaphore to only allow 4 instances of ffmpeg to run at one time (one per cpu core)
	if (sem_wait(&ffmpeg_sem) == -1)
	{
		fprintf(stderr, "ERROR: sem_wait failed\n");
		return(NULL);
	}
	

	// run ffmpeg. output in 128K mono
	snprintf(buffer2, sizeof(buffer2), "ffmpeg -i %s -y -loglevel error -af \"%s\" -f mp3 -ar 44.1K -ab 128k -ac 1 %s", mp3_file, FFMPEG_FILTERS, temp_file);
	ret = execute_command(-1, buffer2, false);
	
	if (sem_post(&ffmpeg_sem) == -1) {
    	fprintf(stderr, "ERROR: sem_post failed\n");
		return(NULL);
	}
		
	printf("*******run_ffmpeg(%s) ....FINISHED\n", mp3_file);
	
	if (ret != 0) {
		fprintf(stderr, "ERROR running ffmpeg\n");
		return(NULL);
	}

	snprintf(buffer2, sizeof(buffer2), "rm %s", mp3_file);
	if (execute_command(-1, buffer2, false) != 0) {
		fprintf(stderr, "ERROR deleting %s\n", mp3_file);
		return(NULL);
	}
		

	snprintf(buffer2, sizeof(buffer), "mv %s %s", temp_file, mp3_file);		
	if (execute_command(-1, buffer2, false) != 0) {
		fprintf(stderr, "ERROR renaming %s to %s\n", temp_file, mp3_file);
	}
	
	
	return NULL;
}


// Function to check if a file is an MP3
int is_mp3(const char *filename) {
    const char *ext = strrchr(filename, '.');
    return ext && strcmp(ext, ".mp3") == 0;
}


// Multi-threaded Function to process all MP3 files in a directory, 4 at a time 
int process_all_mp3_files(const char *dir_path) {
	
	int ret = 0;
	pthread_t threads[MAX_FILES];
	char* filenames[MAX_FILES];
	
	int filename_count = 0;
	
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
            snprintf(mp3_file, sizeof(mp3_file), "%s/%s", dir_path, entry->d_name);
			filenames[filename_count] = strdup(mp3_file);
			filename_count++;
		}
	}
		
		
    // Create a new thread for each file
    for (int i = 0; i < filename_count; i++) {
		if (pthread_create(&threads[i], NULL, ffmpeg_thread_function, (void*)filenames[i]) != 0) {
			perror("pthread_create failed");
			sem_destroy(&ffmpeg_sem);
			exit(-1);				
		}
		//pthread_join(threads[i], NULL);  // waits for one thread to finish before starting the next
    }


    // Wait for all threads to complete. Update the LCD bargraph as it goes
    for (int i = 0; i < filename_count; i++) {
		int percent_complete = (i*100) / filename_count;
		lcd_display_bargraph(percent_complete, 3);
        if (pthread_join(threads[i], NULL) != 0) {
            perror("pthread_join failed");
        }
    }


	// Free allocated strings
	for (int i = 0; i < filename_count; i++) {
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


// Starts a new client program in a seperate Linux process
// Returns the pid of the new process or -1 if error
int start_process(int device_id) {
		
	printf("Start client process for device number %d \n", device_id);
	
	if ((device_id < 0) || (device_id > MAX_USB_CHANNELS)) {
		fprintf(stderr, "ERROR: Start_processs: device_id %d invalid\n", device_id);
		exit(1);
	}
	
	ChannelInfoStruct* channel_info_p = &shared_data_p->channel_info[device_id];
		
	// Fork a new instance of the client process	
	pid_t pid = fork();
	if (pid < 0) {
		perror("Fork Failed");
		return -1;
	} 
	else if (pid != 0)
	{
		// Parent process. Do nothing and just continue
	} 
	else {
		// Client process
        // Redirect stdin to /dev/null to prevent interactive prompts and screen corruption
        int dev_null = open("/dev/null", O_RDONLY);
        if (dev_null == -1) {
            perror("Failed to open /dev/null");
            exit(EXIT_FAILURE);
        }
        dup2(dev_null, STDIN_FILENO); // Redirect stdin
        close(dev_null);

        // Prepare arguments
        sprintf(buffer, "%d", device_id);

        char *args[] = {"sudo", "./client", buffer, NULL};

        // Execute command
		channel_info_p->pid = pid;
		channel_info_p->state = STARTING;
		channel_info_p->start_time = time(NULL);
		channel_info_p->bytes_copied = 0;
		printf("running %s %s %s\n", args[0], args[1], args[2]);
        execvp(args[0], args);

        // If execvp fails
        fprintf(stderr, "ERROR: Failed to execute sudo ./client %s: %s\n", buffer, strerror(errno));
        exit(EXIT_FAILURE);
	}
	
	return pid;
}





// quick power on check and visual indication we are ready
void test_leds() {

	printf("LED Test - All Red\n");
	set_all_states(FAILED);
	usleep(100000);

	printf("LED Test - All Yellow\n");
	set_all_states(READY);
	usleep(100000);

	printf("LED Test - All Green\n");
	set_all_states(SUCCESS);
	usleep(100000);

	printf("LED Test - Left to Right\n");
	for (int device_id=0; device_id<MAX_USB_CHANNELS; device_id++) {		
		set_all_states(EMPTY);		
		set_state(device_id, LED_TEST);
		usleep(100000);
	}

	printf("LED Test - All Off\n");
	set_all_states(EMPTY);
	double_beep();
}



// Prompts the user to insert the master USB in slot one. 
// Recursively copies all files to the ramdrive
int load_master() {
	char name[STRING_LEN];
	char path[STRING_LEN];
	
	lcd_display_message(NULL, "Insert Master", "in slot 1", NULL);
	set_all_states(EMPTY);
	set_state(0, INDICATING);

	// Wait for USB inserted
	printf("Waiting for master USB to be inserted\n");
	while(!usb_device_inserted(name, path)) {
		usleep(100000);
	}
	
	printf("found master : name=%s path=%s\n", name, path);
	
	set_state(0, COPYING);
	lcd_display_message("Reading Master", NULL, name, path);
	
    // Choose the name of the mount point. 
	// If device name is /dev/sda, the mount point will be /mnt/usb/sda1
	char mount_point[STRING_LEN];
	const char* last_slash = strrchr(name, '/');
	if (!last_slash)
	{
		fprintf(stderr, "ERROR: device_name '%s' is not in expected format\n", name);
		return 1;
	}		
    snprintf(mount_point, sizeof(mount_point), "%s/%s1", MOUNT_POINT, last_slash+1);	
	
	// Append '1' to the device name to get the partition name, i.e. /dev/sdb1
	char partition_name[STRING_LEN];
	strncpy(partition_name, name, STRING_LEN);
	strncat(partition_name, "1", STRING_LEN-1);
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
        return 1;
	}

	
	shared_data_p->total_size = 0;
	bool halt = false;
	if (copy_directory(mount_point, RAMDIR_PATH, &halt, &shared_data_p->total_size) != 0) {
		fprintf(stderr, "ERROR: copy_directory failed\n");
        return 1;
	}

	printf("Total Size=%lu\n", shared_data_p->total_size);

    // Unmount the USB drive
	snprintf(buffer, sizeof(buffer), "sync %s", mount_point);
	if (execute_command(-1, buffer, false) != 0) {
		fprintf(stderr, "VERIFY ERROR: Cannot sync device\n");
		return false;
	}
	
	snprintf(buffer, sizeof(buffer), "sudo umount %s", mount_point);
	if (execute_command(-1, buffer, false) != 0) {
		fprintf(stderr, "ERROR: Unmounting the USB drive %s\n", mount_point);
		return 1;
	}

	lcd_clear();
	beep();

	return 0;
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
		if (!device_is_loaded(channel_info_p->device_name)) {
			channel_info_p->state = EMPTY;
		}
		
		if (channel_info_p->hub_number == hub_number) {
			
			printf("%d=%s %s (%s)\n", device_id,
				get_state_name(channel_info_p->state), channel_info_p->device_name, channel_info_p->device_path);
						
			if ((channel_info_p->state == READY) || 
			    (channel_info_p->state == SUCCESS) || 
				(channel_info_p->state == FAILED) ) {
					
				int pid = start_process(device_id);					
				if (pid < 0) {
					fprintf(stderr, "ERROR: start_process failed\n");
					result = 1;
				}
				usleep(500000);	
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
	int lcd_line = hub_number*2;

	for (int i=0; i<MAX_USB_CHANNELS; i++)
	{		
		ChannelInfoStruct* channel_info_p = &shared_data_p->channel_info[i];

		if (channel_info_p->hub_number == hub_number)
		{
			total_bytes_copied += channel_info_p->bytes_copied;
			
			ChannelStateEnum state = channel_info_p->state;
			if ((state == STARTING) || (state == ERASING) || 
				(state == FORMATING) || (state == PARTITIONING) || (state == MOUNTING) ||
				(state == COPYING) || (state == UNMOUNTING)) { 
				copying++; 
			}
			else if (state == VERIFYING) { 
				verifying++; 
			}
			else if (state == SUCCESS) { 
				pass++; 
			}
			else if (state == FAILED) { 
				fail++; 
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
				int count = copying + pass + fail;
				if ((count == 0) || (shared_data_p->total_size==0))
					percent = 0;
				else
					percent = 100*total_bytes_copied / shared_data_p->total_size / count;
				
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

				beep();
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
	
	shared_data_p->settings.reformat = true;
	shared_data_p->settings.autostart = false;
	for (int device_id=0; device_id<MAX_USB_CHANNELS; device_id++) {	
		ChannelInfoStruct* channel_info_p = &shared_data_p->channel_info[device_id];	
		channel_info_p->device_id = device_id;
		channel_info_p->hub_number = device_id / PORTS_PER_CHANNEL;
		channel_info_p->port_number = device_id % PORTS_PER_CHANNEL;
		channel_info_p->state = EMPTY;
	}
	
	// Initialise the LCD etc
	gpio_init(shared_data_p);
	lcd_init(shared_data_p);
	lcd_display_message("RPi USB Duplicator", NULL, VERSION_STRING, "(Gary Bleads G0HJQ)");
	usb_init(shared_data_p);
	
	test_leds();
	
	load_master();
	
	lcd_display_message("Optimising MP3 files", "Please Wait", NULL, NULL);
	process_all_mp3_files(RAMDIR_PATH);

	lcd_display_message("Calculating", "Checksums", NULL, NULL);
	
	// Copy the master files to the ramdrive and save the CRCs for each to crc.txt on the ramdrive
	initialise_crc_table();
	FILE *crc_file = fopen(CRC_FILE, "w");
	if (!crc_file) {
		fprintf(stderr, "Error: Cannot create CRC file %s\n", CRC_FILE);
	   return 0;
	}

	generate_crcs(RAMDIR_PATH, crc_file);
	printf("Generate CRCs finished\n");
		
    if (fclose(crc_file) == -1) {
        perror("close crc_file");
        exit(1);
    }

	// prompt the user to remove the master USB drive
	snprintf(buffer, sizeof(buffer), "%luMB Loaded OK.", shared_data_p->total_size / 1024 / 1024);
	lcd_display_message(buffer, "Remove master.","Insert blanks then","press button to copy");
	set_state(0, READY);
	beep();

	bool starting = true;
	
	while(true) {
		
		ButtonStateEnum button_state0 = get_button_state0();
		ButtonStateEnum button_state1 = get_button_state1();

		if (starting && ((button_state0 != BUTTON_NOT_PRESSED) || (button_state0 != BUTTON_NOT_PRESSED)))
		{
			lcd_clear();
			starting = false;
		}

		hub_main(0, button_state0);
		hub_main(1, button_state1);
		
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

