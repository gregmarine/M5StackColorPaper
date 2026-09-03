# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "0.1.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(papercolor_frame)

# Builds a valid, empty, pre-formatted FAT filesystem image for the "storage"
# partition. hal_storage.cpp mounts that partition with do_not_format=true (it
# refuses to auto-format on first boot), so a brand-new board needs this
# image written once:  idf.py -p <PORT> storage-flash
# It is deliberately NOT flashed as part of `idf.py flash` (no
# FLASH_IN_PROJECT), so reflashing the firmware keeps the photos already on
# the drive. main/fatfs_image/ is intentionally empty.
fatfs_create_spiflash_image(storage main/fatfs_image)
