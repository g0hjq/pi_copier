# pi_copier
A Raspberry Pi based USB flash drive copier for talking newspapers and the like.

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


## Installation

## User Guide

