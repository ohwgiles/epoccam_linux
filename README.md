epoccam_linux
-------------
C program to communicate with Kinoni's EpocCam mobile application
Copyright (c) 2014, Oliver Giles

EpocCam is produced by Kinoni (http://www.kinoni.com), who generously
provided useful documentation and support for this project.

Using EpocCam, you can use your mobile phone as a wireless webcam.
This application provides a linux compatibility wrapper.

Status
------
epoccam_linux is nearly stable, and is suitable for casual use.

epoccam_linux depends on alsa, libavcodec, libavformat, v4l and gtk.

Running
-------
In order to emulate a webcam driver that will be recognised by even
closed-source software such as Skype, you should install v4l2loopback.

epoccam_linux will fail to launch without a v4l2loopback device node
being present.

v4l2loopback is available at https://github.com/umlaeute/v4l2loopback,
as well as in the Ubuntu Universe Repository and in the Arch User
Repository.

Next steps
----------
Fix latency issues
Handle video flags (mirrored/inverted)
