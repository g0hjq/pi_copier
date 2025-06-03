#!/bin/bash

# Delete any existing AP configuration
nmcli con delete Copier_AP 2>/dev/null

# Create AP configuration
nmcli con add type wifi ifname wlan0 mode ap con-name Copier_AP ssid Copier_AP autoconnect yes
nmcli con modify Copier_AP 802-11-wireless.band bg
nmcli con modify Copier_AP 802-11-wireless.channel 11
nmcli con modify Copier_AP wifi-sec.key-mgmt wpa-psk
nmcli con modify Copier_AP wifi-sec.psk "LetMeIn123"
nmcli con modify Copier_AP ipv4.method shared ipv4.address 192.168.4.1/24
nmcli con modify Copier_AP ipv6.method disabled
nmcli con up Copier_AP
