#!/bin/sh
#GST_PLUGIN_PATH=/usr/local/lib/gstreamer-1.0
GST_PLUGIN_PATH=/usr/local/lib/gstreamer-0.10
export GST_PLUGIN_PATH
#
TEST_FILE_URL_PREFIX=file:///home/landerson/RUIHRI/git/gst-plugins-cl/dlnasrc/test/
export TEST_FILE_URL_PREFIX
#
#RUIH_GST_DTCP_DISABLE=true
#export RUIH_GST_DTCP_DISABLE
#
RUIH_GST_DTCP_KEY_STORAGE=/home/landerson/RUIHRI/git/gst-plugins-cl/dtcpip/
#RUIH_GST_DTCP_KEY_STORAGE=/media/truecrypt1/dll/prod_keys
export RUIH_GST_DTCP_KEY_STORAGE
#
RUIH_GST_DTCP_DLL=/home/landerson/RUIHRI/git/gst-plugins-cl/dtcpip/test/dtcpip_mock.so
#RUIH_GST_DTCP_DLL=/media/truecrypt1/dtcpip_v1.1_prod.so
export RUIH_GST_DTCP_DLL
#
GST_DEBUG=*:2,dlnasrc:4,dtcpip:5,dlnasouphttpsrc:4,playbin:3,playbin2:5,uridecodebin:3,filesrc:3,\
basesrc:4,pushsrc:1,baseparse:1,task:1,queue2:2,bin:1,\
mpegtsdemux:1,mpeg2dec:3,mpegvparse:3,tsdemux:1,mpeg2dec:1,mpegtsbase:3,xvimagesink:5,\
GST_EVENT:2,GST_SEGMENT:2,GST_ELEMENT_FACTORY:1,GST_ELEMENT_PAD:1,GST_STATES:1,GST_BUFFER:1,GST_BUS:1
export GST_DEBUG
#
LD_LIBRARY_PATH=/usr/local/lib
export LD_LIBRARY_PATH
#
GST_DEBUG_NO_COLOR=1
export GST_DEBUG_NO_COLOR
#
#./test file=clock.mpg
./test host=192.168.2.3 rrid=6 pipeline
#./test host=192.168.0.106 dtcp rrid=9 pipeline
#rate=8 wait=15 host=192.168.0.106 or 192.168.2.2 pipeline switch position seek file=clock.mpg file=two_videos.mkv file=scte20_7.mpg dtcp	
#
# To debug with gdb debugger, use this line
#gdb --args ./test host=192.168.2.2 rrid=8
