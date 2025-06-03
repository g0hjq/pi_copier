#!/bin/bash

nmcli con down "Copier_AP"
nmcli con delete "Copier_AP"
nmcli con up preconfigured

