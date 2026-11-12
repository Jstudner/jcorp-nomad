# Changelog for USB Host MSC driver

All notable changes to this component will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.2.0] - 2026-04-08

### Added

- Added global suspend/resume support
- Added support for ESP32-S31

## [1.1.4] - 2025-09-24

### Added

- Added public API support for formatting
- Added support for ESP32-H4

## [1.1.3] - 2024-10-29

### Added

- Implemented request sense, to get sense data from USB device in case of an error

### Fixed

- Fixed initialization of some flash drives, which require more time to initialize (https://github.com/espressif/esp-idf/issues/14319)

## [1.1.2] - 2024-01-25

### Added

- Added support for ESP32-P4

### Changed

- Reverted zero-copy bulk transfers. Data are now copied to USB buffers with negligible effect on performance

## [1.1.1]

### Fixed

- Fix `msc_host_get_device_info` for devices without Serial Number string descriptor https://github.com/espressif/esp-idf/issues/12163
- Fix regression from version 1.1.0 that files could not be opened in PSRAM https://github.com/espressif/idf-extra-components/issues/202
- Fix MSC driver event handling without background task

## [1.1.0] - 2023-06-29 [YANKED]

### Changed

- Significantly increase performance with Virtual File System by allowing longer transfers
- Optimize used heap memory by reusing the Virtual File System buffer
- Optimize CPU usage by putting the background MSC task to 'Blocked' state indefinitely when there is nothing to do

### Fixed

- Fix MSC commands for devices on interface numbers other than zero
- Replace unsafe debug functions for direct access of MSC sectors with private SCSI commands

## [1.0.4] - 2023-04-14

### Added

- Claim support for USB composite devices

## [1.0.2] - 2022-10-01

### Fixed

- Increase transfer timeout to 5 seconds to handle slower flash disks

## [1.0.1] - 2022-09-13

### Fixed

- Fix compatibility with IDF v4.4

## [1.0.0] - 2022-08-10

- Initial version
