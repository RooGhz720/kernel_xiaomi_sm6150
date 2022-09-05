# Halium-boot

bootimg generator for Halium ports 

## Build

```
mka halium-boot
```

### Build with a local initramfs

Sometimes you might want to make changes to the initramfs locally, such as to fix a device-specific quirk. To do this, follow these instructions:

1. Download the current initramfs from [initramfs-tools-halium releases](https://github.com/Halium/initramfs-tools-halium/releases/tag/continuous)
1. Create a working directory for the files: `mkdir halium-initramfs`
1. Extract the image: `zcat initrd.img-touch-armhf | cpio -D halium-initramfs -idmv`
1. Make your edits to the files in `halium-initramfs/`. The file that you will probably want to edit is `scripts/halium`
1. Repack the image: `cd halium-initramfs/ && find . | cpio -H newc -o | gzip -9 > ../initramfs.gz && cd ..`
1. Place the `initramfs.gz` file you just created in your *device tree*. This is the directory in `device/[manufacturer]/[board]` for your device. 
1. Add `BOARD_USE_LOCAL_INITRD := true` to the end of your `BoardConfig.mk`.
1. Set up your environment and build with `mka halium-boot`

Don't forget to remove the `initramfs.gz` file and remove `BOARD_USE_LOCAL_INITRD := true` after you have proposed your changes to [initramfs-tools-halium](https://github.com/halium/initramfs-tools-halium). This ensures that your port is in line with other Halium ports.

If you'd prefer to rebuild a whole initramfs from source, see the build instructions in [initramfs-tools-halium](https://github.com/halium/initramfs-tools-halium#build-an-initramfs-image). Once you've built the image, rename it to `initramfs.gz` and follow the instructions from step 6 above.

## Initrd debugging:

```
sudo fastboot boot halium-boot.img -c break=[level]
```

Levels

* modules
* premount
* mount
* mountroot
* bottom

example:

```
sudo fastboot boot halium-boot.img -c break=premount
```

And use telnet to login:

```
telnet 192.168.2.15
```
