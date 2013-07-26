gstreamer-tests
===============

A simple gstreamer test program to get buffers from audio (input/file) channels separately and do "something" (??)

pipeline:
(filesrc ! autoaudiosrc) ! audioconvert ! audioresample ! capsfilter ! deinterleave ! queue0 ! appsink0 ! ??
                                                                                          \
                                                                                           `queue1 ! appsink1 ! ??

dependencies: 
````````````
glib 2.0
gstreamer 0.10
cmake ??

build:
``````
mkdir build && cd build
cmake..
make

running:
```````
1) Read buffers from audio file

$ ./inputtest <audio file path> 

or

2)  Read buffers from audio input (microphone)

$ ./inputtest
