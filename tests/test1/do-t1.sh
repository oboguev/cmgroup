sudo cat /proc/slabinfo >slabinfo.1
./test1 t1
sudo cat /proc/slabinfo >slabinfo.2
meld slabinfo.1 slabinfo.2
rm slabinfo.1 slabinfo.2

