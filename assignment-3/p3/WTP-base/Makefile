all: sender receiver

sender: wSender.c
	gcc -o wSender wSender.c ../starter_files/PacketHeader.h ../starter_files/crc32.h

receiver: wReceiver.c
	gcc -o wReceiver wReceiver.c ../starter_files/PacketHeader.h ../starter_files/crc32.h

clean:
	rm wSender
	rm wReceiver
