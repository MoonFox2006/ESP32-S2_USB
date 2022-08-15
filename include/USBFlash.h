#pragma once

#include <esp_vfs_fat.h>
#include <USBMSC.h>

class USBFlash {
public:
  USBFlash() : _msc(USBMSC()), _vfat(-1) {
    _this = this;
  }
  ~USBFlash() {
    end();
  }

  bool init(const char *path = "/fatfs", const char *label = "ffat");
  bool begin();
  void end();

protected:
  static bool onStartStop(uint8_t power_condition, bool start, bool load_eject);
  static int32_t onRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize);
  static int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize);

  static USBFlash *_this;

  USBMSC _msc;
  wl_handle_t _vfat;
};

bool USBFlash::init(const char *path, const char *label) {
  esp_vfs_fat_mount_config_t mount_config = {
    .format_if_mount_failed = true,
    .max_files = 5,
    .allocation_unit_size = CONFIG_WL_SECTOR_SIZE
  };

  if (esp_vfs_fat_spiflash_mount(path, label, &mount_config, &_vfat) != ESP_OK) {
    _vfat = -1;
    return false;
  }
  return true;
}

bool USBFlash::begin() {
  if (_vfat != -1) {
    _msc.vendorID("ESP32");
    _msc.productID("USB_MSC");
    _msc.productRevision("1.0");
    _msc.onStartStop(&USBFlash::onStartStop);
    _msc.onRead(&USBFlash::onRead);
    _msc.onWrite(&USBFlash::onWrite);
    _msc.mediaPresent(true);
    return _msc.begin(wl_size(_vfat) / wl_sector_size(_vfat), wl_sector_size(_vfat));
  }
  return false;
}

inline void USBFlash::end() {
  _msc.end();
}

bool USBFlash::onStartStop(uint8_t power_condition, bool start, bool load_eject) {
  if (load_eject && (! start)) {
    _this->_msc.end();
  }
  return true;
}

int32_t USBFlash::onRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
  wl_read(_this->_vfat, wl_sector_size(_this->_vfat) * lba + offset, buffer, bufsize);
  return bufsize;
}

int32_t USBFlash::onWrite(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
  wl_erase_range(_this->_vfat, wl_sector_size(_this->_vfat) * lba + offset, bufsize);
  wl_write(_this->_vfat, wl_sector_size(_this->_vfat) * lba + offset, buffer, bufsize);
  return bufsize;
}

USBFlash *USBFlash::_this = nullptr;
