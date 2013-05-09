all:
	gcc -Wall -pedantic -o fakeserial fakeserial.c
	gcc -Wall -pedantic -DDEBUG -o udp-broker udp-broker.c
