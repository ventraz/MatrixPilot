//
//  SIL-udb.h
//  MatrixPilot-SIL
//
//  Created by Ben Levitt on 2/1/13.
//  Copyright (c) 2013 MatrixPilot. All rights reserved.
//

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "UDBSocket.h"
#include "options.h"

UDBSocket stdioSocket;
UDBSocket udpSocket;

uint8_t readSockets(void);


void printHelp(void)
{
	printf("usage: silcat -h                print Help\n");
	printf("       silcat -s [port]         udp Server\n");
	printf("       silcat -c [port [host]]  udp Client\n");
	printf("       silcat -l [port [baud]]  seriaL\n");
	printf("       port: udp port number, or serial port\n");
	printf("       host: udp host to connect to, e.g. 127.0.0.1. (For -c only)\n");
	printf("       baud: serial port speed\n");
	printf("       (With no arguments, will connect to MatrixPilot-SIL's telemetry port.)\n");
}


int main(int argc, char** argv)
{
	UDBSocketType socketType = (!SILSIM_TELEMETRY_RUN_AS_SERVER) ? UDBSocketUDPServer : UDBSocketUDPClient;
	uint32_t udpPort = SILSIM_TELEMETRY_PORT;
	char *udpHost = SILSIM_TELEMETRY_HOST;
	char *serialPort = NULL;
	uint32_t serialBaud = 0;
	
	uint8_t argPos = 0;
	
	int i;
	for (i = 1; i < argc; i++) {
		if (argv[i][0] == '-') {
			if (argv[i][1] == 'h') {
				printHelp();
				return 0;
			}
			else if (argv[i][1] == 's') {
				socketType = UDBSocketUDPServer;
			}
			else if (argv[i][1] == 'c') {
				socketType = UDBSocketUDPClient;
			}
			else if (argv[i][1] == 'l') {
				socketType = UDBSocketSerial;
			}
		}
		else {
			if (argPos == 0) {
				if (socketType == UDBSocketSerial) {
					serialPort = argv[i];
				}
				else {
					udpPort = atoi(argv[i]);
				}
			}
			else if (argPos == 1) {
				if (socketType == UDBSocketUDPClient) {
					udpHost = argv[i];
				}
				else {
					serialBaud = atoi(argv[i]);
				}
			}
			else {
				printHelp();
				return 0;
			}
			argPos++;
		}
	}
	
	stdioSocket = UDBSocket_init(UDBSocketStandardInOut, 0, NULL, NULL, 0);
	udpSocket = UDBSocket_init(socketType, udpPort, udpHost, serialPort, serialBaud);
	
	while (1) {
		if (!readSockets()) {
			usleep(1000);
		}
	}
}


#define BUFLEN 512

uint8_t readSockets(void)
{
	uint8_t buffer[BUFLEN];
	int32_t bytesRead;
	uint8_t didRead = 0;
	
	if (udpSocket) {
		bytesRead = UDBSocket_read(udpSocket, buffer, BUFLEN);
		if (bytesRead < 0) {
			UDBSocket_close(udpSocket);
			udpSocket = NULL;
			printf("ERROR: read failed\n");
			exit(1);
		}
		else if (bytesRead > 0) {
			bytesRead = UDBSocket_write(stdioSocket, buffer, bytesRead);
			didRead = 1;
		}
	}
	
	if (stdioSocket) {
		bytesRead = UDBSocket_read(stdioSocket, buffer, BUFLEN);
		if (bytesRead > 0) {
			bytesRead = UDBSocket_write(udpSocket, buffer, bytesRead);
			if (bytesRead < 0) {
				UDBSocket_close(udpSocket);
				udpSocket = NULL;
				printf("ERROR: write failed\n");
				exit(1);
			}
			didRead = 1;
		}
	}
		
	return didRead;
}
