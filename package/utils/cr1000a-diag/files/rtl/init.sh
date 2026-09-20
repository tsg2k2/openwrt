#!/bin/ash
cp /lib/libubus.so.* /lib/libubus.so
cp /lib/libubox.so.* /lib/libubox.so
ln -s /dev/spidev1.0 /dev/spidev32765.0

control_fifo=/tmp/control_rtl9303.fifo

mkfifo $control_fifo
tail -f $control_fifo | /lib/rtl/usrApp &
sleep 30

#echo "port set port all state disable" > $control_fifo
echo "exit" > $control_fifo


