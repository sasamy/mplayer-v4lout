# mplayer-v4lout
Mplayer video output driver for v4l2 output devices to use the hardware capabilities of the processor to scale and color space conversion 

## How to add this driver to mplayer

0009-vo-v4lout-sk.patch patch for mplayer version 1.3.0. 

When using buildroot - just copy this patch to the directory buildroot-xxx/package/mplayer/

## How to use

Tested on processors of the imx series, containing PxP - imx6ull, imx233

Example: 
```
mplayer -vo v4lout:vdev=/dev/video1:fbdev=/dev/fb3:l=8:t=16:w=128:h=64 sample_video.avi
```
or simply
```
mplayer -vo v4lout sample_video.avi
```
```
Options:
  vdev - video out device(default: /dev/video0)
  fbdev - framebuffer device (screeninfo, default: /dev/fb0)
  t,l - top and left of the cropped window (default: 0,0)
  w,h - width and height of the cropped window (default: full screen)
```
