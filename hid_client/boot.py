import storage
import usb_hid

#enable USB HID (Keyboard)
usb_hid.enable((usb_hid.Device.KEYBOARD,))

#remount the filesystem so the storage drive is writable by the victim computer, and READ-ONLY to the RP2040 internal script execution loop
storage.remount("/", readonly=False)
