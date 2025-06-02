
#include "globals.h"
#include "utilities.h"
#include "lcd.h"
#include "gpio.h"
#include "usb.h"



char buffer[STRING_LEN*2];
SharedDataStruct* shared_data_p = NULL;




//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------
// Helper Routines
//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------

// Sets the state of a single USB Channel
void set_state(int device_id, ChannelStateEnum state)
{
	shared_data_p->channel_info[device_id].state = state;
}


// Sets the state of all USB Channels
void set_all_states(ChannelStateEnum state)
{
	for (int device_id=0; device_id<MAX_USB_CHANNELS; device_id++) {
		shared_data_p->channel_info[device_id].state = state;
	}

}



//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------
// Processing Functions
//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------



// Prompts the user to insert a usb drive in each port in turn to obtain
// the path of each USB port so we can associate the hub's port paths with 
// the usb port number to enable us to light the correct LEDs
void map_usb_ports(void) {

	char name[STRING_LEN];
	char path[STRING_LEN];
	char line4[STRING_LEN];
	
	// Clear any old states
	get_button_state0();
	get_button_state1();
	usb_device_inserted(NULL, NULL);
	strcpy(line4, "Press Button to skip");
	
	for (int device_id=0; device_id< MAX_USB_CHANNELS; device_id++)
	{		
		ChannelInfoStruct* channel_info_p = &shared_data_p->channel_info[device_id];
		channel_info_p->state = INDICATING;
			
		sprintf(buffer, "Put disk in slot %u", device_id+1);
		lcd_display_message("Testing USB Ports", buffer, NULL, line4);
		
		while(true) {
				
			if (usb_device_inserted(name, path)) {
				printf("found usb device %d : name=%s path=%s\n", device_id, name, path);
								
				// error if the channel has already been mapped
				if (get_device_id_from_path(shared_data_p, path) >= 0) { 
					error_beep();
					strcpy(line4, "ERROR: SOCKET IN USE");
				}
				else {
					beep();
					strcpy(channel_info_p->device_name, name);
					strcpy(channel_info_p->device_path, path);
					channel_info_p->state = SUCCESS;
					strcpy(line4, name);
					strcat(line4, " ");
					strcat(line4, path);
				}
				break;
			}			
			
			if ((get_button_state0() == BUTTON_SHORT_PRESS) || (get_button_state1() == BUTTON_SHORT_PRESS)) {
				printf("Button Pressed - skip port\n");
				channel_info_p->state = NOT_IN_USE;
				break;
			}

			usleep(200000);
		}
	}	

	lcd_clear();
}



// Starts a new client program in a seperate Linux process
// Returns the pid of the new process or -1 if error
int start_process(int device_id)
{
		
	printf("Start client process for device number %d \n", device_id);
	
	if ((device_id < 0) || (device_id > MAX_USB_CHANNELS)) {
		fprintf(stderr, "Start_processs: device_id %d invalid\n", device_id);
		exit(1);
	}
	
	ChannelInfoStruct* channel_info_p = &shared_data_p->channel_info[device_id];
	
	if (channel_info_p->state == UNUSED) {
		fprintf(stderr, "Start_processs: channel %d unused\n", device_id);
		return -1;
	}
		
	
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
        fprintf(stderr, "Failed to execute sudo ./client %s: %s\n", buffer, strerror(errno));
        exit(EXIT_FAILURE);
	}
	
	return pid;
}





// quick power on check and visual indication we are ready
void test_leds() {

	printf("LED Test - All Red\n");
	set_all_states(FAILED);
	usleep(200000);

	printf("LED Test - All Yellow\n");
	set_all_states(READY);
	usleep(200000);

	printf("LED Test - All Green\n");
	set_all_states(SUCCESS);
	usleep(200000);

	printf("LED Test - Left to Right\n");
	for (int device_id=0; device_id<MAX_USB_CHANNELS; device_id++) {		
		set_all_states(EMPTY);		
		set_state(device_id, LED_TEST);
		usleep(100000);
	}

	printf("LED Test - All Off\n");
	set_all_states(EMPTY);
	done_beep();
}



// Prompts the user to insert the master USB in slot one. 
// Recursively copies all files to the ramdrive
int load_master()
{
	char name[STRING_LEN];
	char path[STRING_LEN];
	
	lcd_display_message(NULL, "Insert Master", "in slot 1", NULL);
	set_all_states(UNUSED);
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
		fprintf(stderr, "device_name '%s' is not in expected format\n", name);
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
		fprintf(stderr, "Creating mount point\n");
		return 1;
	}


    // Mount the USB drive
	snprintf(buffer, sizeof(buffer), "sudo mount %s %s", partition_name, mount_point);
	if (execute_command(-1, buffer, false) != 0) {
		fprintf(stderr, "Mounting the USB drive\n");
		return 1;
	}

 
	// Empty the ramdrive
    snprintf(buffer, sizeof(buffer), "sudo rm -rf %s/*", RAMDIR_PATH);
	if (execute_command(-1, buffer, false) != 0) {
		fprintf(stderr, "empty_directory failed\n");
        return 1;
	}

	
	// Copy the master files to the ramdrive
	shared_data_p->total_size = 0;
	bool halt = false;
	if (copy_directory(mount_point, RAMDIR_PATH, &halt, &shared_data_p->total_size) != 0) {
		fprintf(stderr, "copy_directory failed\n");
        return 1;
	}

	printf("Total Size=%lu\n", shared_data_p->total_size);

    // Unmount the USB drive
	snprintf(buffer, sizeof(buffer), "sudo umount %s", mount_point);
	if (execute_command(-1, buffer, false) != 0) {
		fprintf(stderr, "Ejecting the USB drive %s\n", mount_point);
		return 1;
	}

	snprintf(buffer, sizeof(buffer), "Size = %luMB", shared_data_p->total_size / 1024 / 1024);
	lcd_display_message("Master loaded OK.", buffer, NULL, "Remove Master USB");
	set_state(0, READY);
	beep();
	
	// Wait for USB removed
	printf("Waiting for master USB to be removed\n");
	while(!usb_device_removed(name, path)) {
		usleep(100000);
	}
	set_state(0, EMPTY);
	
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



int run(int hub_number)
{
	int result = 0;

	for (int device_id=0; device_id<MAX_USB_CHANNELS; device_id++) {
		
		ChannelInfoStruct* channel_info_p = &shared_data_p->channel_info[device_id];	
		
		if (channel_info_p->hub_number == hub_number) {
			
			if ((channel_info_p->state == READY) || 
			    (channel_info_p->state == SUCCESS) || 
				(channel_info_p->state == FAILED) ) {
					
				int pid = start_process(device_id);					
				if (pid < 0) {
					fprintf(stderr, "start_process failed\n");
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
	int busy=0; 
	int fail=0; 
	int pass=0; 
	off_t total_bytes_copied = 0;

	for (int i=0; i<MAX_USB_CHANNELS; i++)
	{
		ChannelInfoStruct* channel_info_p = &shared_data_p->channel_info[i];
		if (channel_info_p->hub_number == hub_number)
		{
			ChannelStateEnum state = channel_info_p->state;
			if ((state == STARTING) || (state == ERASING) || 
				(state == FORMATING) || (state == PARTITIONING) || (state == MOUNTING) ||
				(state == COPYING) || (state == UNMOUNTING)) { 
				busy++; 
				total_bytes_copied += channel_info_p->bytes_copied;
			}
			else if (state == SUCCESS) { 
				pass++; 
				total_bytes_copied += channel_info_p->bytes_copied;
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
			lcd_write_string("CANCELLED", hub_number*2);
			error_beep();
			channel_busy[hub_number] = false;
		} 
		else {		
			if (busy > 0) {
			    // copying. Display progress so far
				float percent;
				int count = busy + pass;
				if ((count == 0) || (shared_data_p->total_size==0))
					percent = 0;
				else
					percent = 100*total_bytes_copied / shared_data_p->total_size / count;
				
				sprintf(buffer, "Busy=%-2u OK=%-2u Bad=%-2u", busy, pass, fail);		
				lcd_write_string(buffer, hub_number*2);				
				lcd_display_bargraph(percent, hub_number*2+1);				
			}
			else 
			{
				// Copy has just finished. Display a summary
				printf("Channel %d finished\n", hub_number);
				gettimeofday(&end_time[hub_number], NULL);
				int seconds = end_time[hub_number].tv_sec - start_time[hub_number].tv_sec;
				
				sprintf(buffer, "Done. OK=%-2u Bad=%-2u", pass, fail);		
				lcd_write_string(buffer, hub_number*2);				

				snprintf(buffer, sizeof(buffer), "Wrote %luMB in %1u:%02u", total_bytes_copied/1024/1024, seconds/60, seconds%60);
				lcd_write_string(buffer, hub_number*2 + 1);

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
			lcd_write_string("", hub_number*2);
			lcd_write_string("", hub_number*2+1);
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
        perror("close");
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
		channel_info_p->state = UNUSED;
	}
	
	// Initialise the LCD etc
	gpio_init(shared_data_p);
	lcd_init(shared_data_p);
	lcd_display_message("RPi USB Duplicator", NULL, VERSION_STRING, "(Gary Bleads G0HJQ)");
	usb_init(shared_data_p);
	
	test_leds();
	
	load_master();
	
	map_usb_ports();
	
	
	// Main program loop
	lcd_display_message("Ready to copy", NULL, "Press Red button", "to begin");
	double_beep();
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

