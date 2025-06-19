#ifndef GLOBALS_H
#define GLOBALS_H

#define _GNU_SOURCE

#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h> 
#include <linux/fs.h> 
#include <unistd.h>
#include <stdbool.h>
#include <mntent.h>
#include <libudev.h>
#include <time.h>
#include <errno.h>
#include <dirent.h>
#include <gpiod.h>
#include <execinfo.h>
#include <dirent.h>


#define VERSION_STRING "v0.0.4  2025-06-03"
#define VERIFY true
#define CRC_SIZE 1*1024*1024   // CRCs will only be generated and checked for the first 1MB in each file

#define SHM_NAME "/usb_copier_shm"
#define RAMDIR_PATH "/var/ramdrive/master"
#define MOUNT_POINT "/mnt/usb"
#define USB_CONFIG_FILE "./usb_ports.config"
#define CRC_FILE "/var/ramdrive/crc.txt"
#define FFMPEG_FILTERS "agate=mode=downward:ratio=1.2, silenceremove=start_periods=1:start_threshold=-40dB:start_silence=0.7, loudnorm=I=-14:TP=-2:LRA=15"
#define NUMBER_OF_FFMPEG_THREADS 4

	
#define MAX_FILES 1024      // Maximum number of files/directories per directory
#define COPY_BUFFER_SIZE 65536  // Buffer size for file copying
#define STRING_LEN 256        // general name string length
#define PATH_LEN 512          // Maximum path length

#define NUMBER_OF_HUBS 2
#define PORTS_PER_CHANNEL 7
#define MAX_USB_CHANNELS (NUMBER_OF_HUBS * PORTS_PER_CHANNEL)  


#define GPIO_CHIP "gpiochip0"
#define GPIO_BUTTON0 20   // top button
#define GPIO_BUTTON1 21   // bottom button
#define GPIO_DATA 5       // 74HC595 data line
#define GPIO_CLOCK 6      // 74HC595 clock line
#define GPIO_LATCH 13     // 74HC595 latch line
#define GPIO_SPEAKER 18   // speaker


#define I2C_DEVICE "/dev/i2c-1" // I2C bus 1 on Raspberry Pi
#define I2C_ADDR 0x27          // Default I2C address (adjust to 0x3F if needed)


//---------------------
// shared data structs
//---------------------
typedef enum {
		EMPTY = 1,
		READY = 2,
		STARTING = 3,
		ERASING = 4,
		FORMATING = 5,
		PARTITIONING = 6,
		MOUNTING = 7,
		COPYING = 8,
		UNMOUNTING = 9,
		VERIFYING = 10,
		SUCCESS = 11,
		FAILED = 12,
		CRC_FAILED = 13,
		LED_TEST = 14,
		INDICATING = 15  // sequence red/yellow/green to show which slot to use to read master & map usb
} ChannelStateEnum ;


typedef enum {
    BUTTON_NOT_PRESSED = 0,
    BUTTON_SHORT_PRESS = 1,
    BUTTON_LONG_PRESS = 2,
} ButtonStateEnum;


typedef struct {
	int device_id;
	int hub_number;
	int port_number;
	bool halt;
	ChannelStateEnum state;
	pid_t pid;
	time_t start_time;
	char device_name[STRING_LEN];
	char device_path[STRING_LEN];
	off_t bytes_copied;
} ChannelInfoStruct;


typedef struct {
	bool autostart;
	bool reformat;
} SettingsStruct;


typedef struct {
	char device_name[STRING_LEN];
	char device_path[STRING_LEN];
} NamePathStruct;


typedef struct {
	off_t total_size;
	SettingsStruct settings;
	ChannelInfoStruct channel_info[MAX_USB_CHANNELS];
} SharedDataStruct;


#endif // GLOBALS_H
