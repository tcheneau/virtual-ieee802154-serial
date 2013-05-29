all:
	gcc -std=c99 -Wall -pedantic -o fakeserial fakeserial.c thirdparty/crc.c
	gcc -std=c99 -Wall -pedantic -o udp-broker udp-broker.c
