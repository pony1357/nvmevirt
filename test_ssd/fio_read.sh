fio --directory=/home/kimi/test_ssd/mnt --direct=1 --ioengine=libaio  --rw=randread --bs=4k --size=256M --time_based=1 --runtime=20 --numjobs=1 --name read_test --iodepth=4
