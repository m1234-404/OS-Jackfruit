# Multi-Container Runtime

## Project Overview
This project is a lightweight Linux container runtime written in C with a supervisor and a kernel memory monitor. It supports multiple containers with isolation, logging, IPC, and scheduling experiments.

## Features
- Multi-container supervisor
- CLI: start, run, ps, logs, stop
- PID, mount, UTS namespace isolation
- chroot filesystem isolation
- /proc support in containers
- FIFO-based IPC
- Pipe-based bounded buffer logging
- Kernel memory monitor (soft/hard limits)
- Scheduling experiments using nice values

## Build
cd boilerplate
make

## Load Kernel Module
sudo insmod monitor.ko
ls /dev/container_monitor

## Run Supervisor (Terminal 1)
sudo ./engine supervisor ./rootfs-base

## Run Commands (Terminal 2)

cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta

sudo ./engine start alpha ./rootfs-alpha "echo alpha"
sudo ./engine start beta ./rootfs-beta "echo beta"

sudo ./engine ps

sudo ./engine logs alpha
sudo ./engine logs beta

sudo ./engine run gamma ./rootfs-alpha "echo run-test"

sudo ./engine stop alpha

## Scheduling Experiments
sudo ./engine start cpu1 ./rootfs-alpha "yes > /dev/null"
sudo ./engine start cpu2 ./rootfs-beta "yes > /dev/null"

sudo ./engine start high ./rootfs-alpha "yes > /dev/null" --nice -10
sudo ./engine start low ./rootfs-beta "yes > /dev/null" --nice 10

ps -eo pid,ni,cmd | grep yes

## Memory Test
sudo ./engine start memtest ./rootfs-alpha "stress-ng --vm 1 --vm-bytes 50M --timeout 5s"
dmesg | tail -n 50

## Cleanup
sudo ./engine stop alpha
sudo ./engine stop beta
sudo rmmod monitor

## Files
engine.c
monitor.c
monitor_ioctl.h
Makefile
