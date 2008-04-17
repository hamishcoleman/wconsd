wconsd.c - serial port server service for Windows NT

Copyright (c) 2003 Benjamin Schweizer <gopher at h07 dot org>
              1998 Stephen Early <Stephen.Early@cl.cam.ac.uk>



* Installation:

wconsd.exe comes as a Windows Service. You must install this service
before you can use it. This is done by the command:

  wconsd.exe -i c:\path\to\wconsd.exe

Now you can start/stop it over the services tab in the Control Center.


* Usage:

wconsd.exe listens on localhost:9600. You can connect with your
favourite terminal (PuTTY in my case;). Setup a raw connection -
no terminal controls will be handeled.
When you've connected there is a online help:

  port, speed, data, parity, stop
  help, status, copyright
  open, close, autoclose

port   [1..16]                     set port id (com1, ...)
speed  [300..115200]               set port speed
data   [5|6|7|8]                   set data bits
parity [no|odd|even|mark|space]    set parity
stop   [one|one5|two]              set stop bits

help                               print this help
status                             print port status
copyright                          print copyright notice and gpl

open                               opens comport
close                              closes comport
autoclose [false|true]             close comport on socket lost?

The defaults are: com1,9600bps,8n1


* Uninstallation

  wconsd.exe -r
