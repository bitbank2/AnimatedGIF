AnimatedGIF<br>
-----------------------------------
Copyright (c) 2020 BitBank Software, Inc.<br>
Written by Larry Bank<br>
bitbank@pobox.com<br>
<br>
![AnimatedGIF](/demo.jpg?raw=true "AnimatedGIF")
<br>
I started working with image and video files around 1989 and soon turned my interest into a Document Imaging product based on my own imaging library. Over the years I added support for more and more formats until I had supported all of the standard ones, including DICOM. I sold my Document Imaging business in 1997, but still found uses for my imaging code in other projects such as retro gaming. I recently went looking to see if there was a good GIF player for Arduino and only found Adafruit's library. Unfortunately it only runs on their ATSAMD51 boards. I thought it would be possible to use my old code to create a universal GIF player that could run on any MCU/SoC with at least 32K of RAM, so I started modifying my old GIF code for this purpose. The focus of this project is speed and to use as little RAM as possible, so there are some limitations.<br>

Limitations<br>
-----------<br>
To save memory, the code limits the maximum image width to 320 pixels. This is just a constant defined in AnimatedGIF.h, so you can set it larger if you like. Animated GIF images have a lot of options to save space when encoding the changes from frame to frame. Many of the tools which generate animated GIFs don't make use of every option and that's helpful because the frame disposal options are not supported in the library code. They can be implemented in your drawing callback if you want. The reason they're not provided with this code is because one of the image disposal options requires you to keep an entire copy of the previous frame and this would prevent it from working on low memory MCUs. If would also require dynamic memory allocation which is another area I avoided with this code.<br>

Features:<br>
---------<br>
- Supports any MCU with at least 24K of RAM (Cortex-M0+ is the simplest I've tested).<br>
- Optimized for speed; the main limitation will be how fast you can copy the pixels to the display. You can use SPI+DMA to help.<br>
- GIF image data can come from memory (FLASH/RAM), SDCard or any media you provide.<br>
- GIF files can be any length, (e.g. hundreds of megabytes)
- Simple C++ class and callback design allows you to easily add GIF support to any application.<br>
- The C code doing the heavy lifting is completely portable and has no external dependencies.<br>
- Does not use dynamic memory (malloc/free/new/delete), so it's easy to build it for a minimal bare metal system.<br>
<br>

Getting GIF files to play:<br>
--------------------------<br>
You'll notice that the images provided in the test_images folder have been turned into C code. Each byte is now in the form 0xAB so that it can be compiled into your program and stored in FLASH memory alongside your other code. You can use a command line tool called xxd to convert a binary file into this type of text. Make sure to add a **const** modifier in front of the GIF data array to ensure that it gets written to FLASH and not RAM by your build environment.<br>

The Callback functions:<br>
-----------------------<br>
One of the reasons that this is apparently the first universal GIF library for Arduino is because the lack of available RAM and myriad display options would make it difficult to support all MCUs and displays properly. I decided that to solve this issue, I would isolate the GIF decoding from the display and file I/O with callback functions. This allows the core code to run on any system, but you need to help it a little. At a minimum, your code must provide a function to draw (or store) each scan line of image. If you're playing a GIF file from memory, this is the only function you need to provide. In the examples folder there are multiple sketches to show how this is done on various display libraries. For reading from SD cards, 4 other functions must be provided: open, close, read, seek. There is an example for implementing these in the examples folder as well.<br>

If you find this code useful, please consider sending a donation or becoming a Github sponsor.

[![paypal](https://www.paypalobjects.com/en_US/i/btn/btn_donateCC_LG.gif)](https://www.paypal.com/cgi-bin/webscr?cmd=_s-xclick&hosted_button_id=SR4F44J2UR8S4)

