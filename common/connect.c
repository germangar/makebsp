#include "connect.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

static int broadcastSocket = -1;

void Broadcast_Setup(const char *dest) {
    char address[256];
    char *colon;
    int port = 39000;
    struct sockaddr_in server;

    if (!dest || !dest[0]) return;

    strncpy(address, dest, sizeof(address) - 1);
    address[sizeof(address) - 1] = '\0';
    
    colon = strchr(address, ':');
    if (colon) {
        *colon = '\0';
        port = atoi(colon + 1);
    }

#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return;
#endif

    broadcastSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (broadcastSocket < 0) return;

    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = inet_addr(address);
    server.sin_port = htons(port);

    if (connect(broadcastSocket, (struct sockaddr *)&server, sizeof(server)) < 0) {
#ifdef _WIN32
        closesocket(broadcastSocket);
#else
        close(broadcastSocket);
#endif
        broadcastSocket = -1;
        return;
    }

    // Send init string
    const char *initStr = "<?xml version=\"1.0\"?><q3map_feedback version=\"1\">";
    int len = strlen(initStr);
    send(broadcastSocket, (const char *)&len, 4, 0); 
    send(broadcastSocket, initStr, len, 0);
}

void Broadcast_Print(int level, const char *msg) {
    if (broadcastSocket < 0 || !msg) return;
    
    char buffer[4096];
    
    // Convert angle brackets to prevent breaking XML
    char safeMsg[2048];
    int j = 0;
    for (int i = 0; msg[i] && j < sizeof(safeMsg) - 5; i++) {
        if (msg[i] == '<') {
            safeMsg[j++] = '&'; safeMsg[j++] = 'l'; safeMsg[j++] = 't'; safeMsg[j++] = ';';
        } else if (msg[i] == '>') {
            safeMsg[j++] = '&'; safeMsg[j++] = 'g'; safeMsg[j++] = 't'; safeMsg[j++] = ';';
        } else if (msg[i] == '&') {
            safeMsg[j++] = '&'; safeMsg[j++] = 'a'; safeMsg[j++] = 'm'; safeMsg[j++] = 'p'; safeMsg[j++] = ';';
        } else {
            safeMsg[j++] = msg[i];
        }
    }
    safeMsg[j] = '\0';
    
    snprintf(buffer, sizeof(buffer), "<message level=\"%d\">%s</message>", level, safeMsg);
    
    int len = strlen(buffer);
    send(broadcastSocket, (const char *)&len, 4, 0);
    send(broadcastSocket, buffer, len, 0);
}

void Broadcast_Shutdown(void) {
    if (broadcastSocket >= 0) {
#ifdef _WIN32
        shutdown(broadcastSocket, SD_SEND);
        Sleep(100);
        closesocket(broadcastSocket);
#else
        shutdown(broadcastSocket, SHUT_WR);
        usleep(100000);
        close(broadcastSocket);
#endif
        broadcastSocket = -1;
    }
}
