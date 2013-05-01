all:
	gcc -Wall -pedantic -DDEBUG -o fakeserial fakeserial.c
	gcc -Wall -pedantic -DDEBUG -o udp-broker udp-broker.c
