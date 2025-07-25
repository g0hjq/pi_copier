# pi_copier
A Raspberry Pi based USB flash drive copier for talking newspapers and the like.

![alt text](images/PXL_20250601_150253859.jpg "Pi Copier")

## Introduction

## Hardware
You will need the following:
* A Raspberry Pi 5 with at least 4GB of Ram. A Raspberry Pi 4 should also work, but I don't recommend using a Pi 3 or older as they only have USB 2 ports which may not be fast enough.
* 5 volt power supply capable of supplying 27 Watts or more
* Raspberry Pi cooler. Actually this is optional as the processor isn't very heavily utilised and doesn't get very hot.
* Micro SD card, 8GB or more.
* Two USB hubs. I used a pair of TP-Link UH700 7-Port USB 3 hubs. These come with an external 12v power supply but seem to work OK without for us, drawing their power from the Raspberry Pi's USB ports. If you are copying to high-speed flash drives, you may need to supply this extra power.
* 20-character x 4-line (2004) LCD module with I2C adaptor
* 2 x LED board (See later)
* 1 x Raspberry Pi copier backpack. This is optional but neater than wiring directly to the Raspberry Pi's GPIO pins (See later)

### LED Boards
There are two of these. They fit above each USB hub and contain 21 LEDs in 7 sets of Red, Yellow and Green LEDs to show the status of each port. If you use different USB hubs you may need to redesign these. 74HC595 shift register allow the Raspberry Pi to control all the LEDs using just three GPIO pins - Data, Clock and Latch.

![alt text](images/ledboard.png)


### Raspberry Pi backpack
This simplifies connections to the Raspberry PI's GPIO ports. It is optional, in that you may wire the LED boards directly to the Raspberry Pi and also the LCD module (via an I2C Level shifter). 

![alt text](images/backpack.png)

## Software

The software is written in C for Raspberry Pi OS Bookworm (64-bit lite version). 

It comprises of two executables : server and client

* Server - This is the main program, which should be started automatically at bootup. It is responsible for the "user interface". It monitors the USB ports and buttons and drives the LCD, LEDs and speaker. When a start button is pressed, it starts up one or more instances of the client executable - one for each target USB drive.
  
* Client - This is the "worker". It formats, writes and verifies the data on a single USB flash drive, then terminates when complete. The Server program creates an instance of the client program for each USB drive.

#### Data Sharing

An area of Linux shared memory is used to send information to the client processes and for the clients to signal progress and success/fail back to the main server application.

### Ram Drive
A tmpfs partition at least 2GB must be manually created in the memory of the Raspberry Pi in /var/ramdrive as part of the installation process. This is used to store a fast copy of all the files on the master Flash drive. It is also used to hold a file crc.txt containing the CRC of each master file.

#### Server Workflow
On startup, the user is prompted to insert the master USB. When the server detects a drive has been inserted into USB port 1, its entire contents are copied to to the ramdrive and the CRC computed.


#### USB Port Mapping

#### File Ordering

#### Wear Reduction

#### Files
All the below files are in /home/pi/copier

| Filename         | Purpose 
|------------------|---------------------------------------------------|
| server.*         | The server executable                             |
| client.*         | The client executable                             |
| gpio.*           | controls the LEDs and polls the push buttons      |
| lcd.*            | Displays text and the progress bar on the LCD     |
| usb.*            | Polls the USB ports looking for flash drives to be inserted and removed |
| utilities.*      | Shared helper functions                           |
| globals.h        | Various type definitions and constants            |
| makefile         | Used by the build process                         |
| usb_ports.config | This is used by the port mapping, and MAY need to be updated if you change the USB hubs |
| setup_ap.sh      | Sets the Raspberry Pi WiFi into Access Point mode to allow direct access from a laptop without connecting via a WiFi hub |
| disable_ap.sh    | Sets the Raspberry Pi into normal WiFi mode for connections via a WiFi hub |


Compiling the program

```
cd ~/copier
make
```
To start the program
```
./server
```

## Installation

#### Raspbery Pi Configuration

SSH to: 192.168.0.244  or raspberrypi.local
Or in AP Mode: 192.168.4.1

username: pi  password: raspberry

#### Setting up a RAM drive on Raspberry Pi
```
 sudo mkdir /var/ramdrive 
 sudo nano /etc/fstab
   and add the line
    tmpfs /var/ramdrive tmpfs nodev,nosuid,size=2G 0 0 

 sudo mount -a
 sudo systemctl daemon-reload
``` 

#### To set a fixed IP Address for wifi
```
sudo nmcli con mod preconfigured ipv4.addresses 192.168.0.244/23
sudo nmcli con mod preconfigured ipv4.gateway 192.168.0.1
sudo nmcli con mod preconfigured ipv4.dns "192.168.0.1 8.8.8.8"
sudo nmcli con mod preconfigured ipv4.method manual
```

#### Setting up Samba file sharing
See https://pimylifeup.com/raspberry-pi-samba/

Username = pi
Password = raspberry

##### Enabling I2C for the LCD
```
sudo raspi-config
  (8 Update)
  (3 Interface options) -> (I5 I2C) -> YES
```  
  
#### Install Libraries
```
sudo apt install libudev-dev
sudo apt install libgpiod-dev
sudo apt install ffmpeg
```


#### Auto Start

1. Create a systemd Service File
```
	sudo nano /etc/systemd/system/usbcopier.service

	Add the following configuration to the file:

		[Unit]
		Description=Usb Copier Auto-Start Service
		commAfter=network.target

		[Service]
		ExecStart=/home/pi/copier/server
		WorkingDirectory=/home/pi/copier
		Restart=always
		RestartSec=5
		User=pi
		Group=pi
		StandardOutput=append:/var/log/usbcopier.log
		StandardError=append:/var/log/usbcopier.log
		Environment="PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

		[Install]
		WantedBy=multi-user.target
```

2. Create and set permissions for the log file:

```
 chmod +x /home/pi/copier/server
	sudo touch /var/log/usbcopier.log
	sudo chmod 664 /var/log/usbcopier.log
	sudo chown pi:pi /var/log/usbcopier.log
```	
	
3. Enable and Start the Service
```
	Reload systemd to recognize the new service file:
		sudo systemctl daemon-reload

	Enable the service to start on boot:
		sudo systemctl enable usbcopier.service
		sudo systemctl disable usbcopier.service

	Start the service usbcopier to test:
		sudo systemctl start usbcopier.service
		sudo systemctl stop usbcopier.service

5. Verify the Service
	sudo systemctl status usbcopier.service

6. Verify logs
	cat /var/log/usbcopier.log
```

## User Guide

#### LED Indicators

| LEDs                  | Meaning                                        |
|-----------------------|------------------------------------------------|
| None                  | No USB drive found                             |
| Amber                 | Ready to copy                                  |
| Flashing Amber        | Copying                                        |
| Fast Flashing Amber   | Verifying                                      |
| Green                 | Copy Finished Succesfully                      |
| Red                   | Copy Failed                                    |
| Red + Amber           | Copy OK but Verify Failed                      |
| Red+Amber+Green Flash | Insert Master USB here                         |

## Wifi Connections
See enable_wifi.sh. 

##### Setting up AP Mode
See enable_ap.sh. 

**Dont forget to change 'my-password'**
```
nmcli con add type wifi ifname wlan0 mode ap con-name Copier_AP ssid Copier_AP autoconnect yes
nmcli con modify Copier_AP 802-11-wireless.band bg
nmcli con modify Copier_AP 802-11-wireless.channel 7
nmcli con modify Copier_AP wifi-sec.key-mgmt wpa-psk
nmcli con modify Copier_AP wifi-sec.psk "my-password"
nmcli con modify Copier_AP ipv4.method shared ipv4.address 192.168.4.1/24
nmcli con modify Copier_AP ipv6.method disabled
nmcli con up Copier_AP
```


#### Enable Overlay mode and set boot partition to READ-ONLY

**Do this last, as any changes you make after this point will be lost when you reboot!**

```
sudo raspi-config
  (4 Performance Options)
  (P2 Overlay File System Enable/disable read-only file system)
     <YES>
Then boot partition to read-only
Reboot
```
