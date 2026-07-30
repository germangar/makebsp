#include "connect.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

    if (broadcastSocket >= 0) return;

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
    int len = strlen(initStr) + 1;
    char outBuffer[256];
    memcpy(outBuffer, &len, 4);
    memcpy(outBuffer + 4, initStr, len);
    send(broadcastSocket, outBuffer, len + 4, 0);
}

static time_t lastBroadcastTime = 0;

static int bufferedDots = 0;

void Broadcast_Print(int level, const char *msg) {
    if (broadcastSocket < 0 || !msg) return;
    
    if (strcmp(msg, ".") == 0) {
        bufferedDots++;
        if (bufferedDots < 32) return;
    }

    if (bufferedDots > 0 && strcmp(msg, ".") != 0) {
        char temp[64];
        memset(temp, '.', bufferedDots);
        temp[bufferedDots] = '\0';
        bufferedDots = 0;
        Broadcast_Print(level, temp);
    }
    
    const char *actualMsg = msg;
    char dotBuf[64];
    if (strcmp(msg, ".") == 0) {
        memset(dotBuf, '.', bufferedDots);
        dotBuf[bufferedDots] = '\0';
        bufferedDots = 0;
        actualMsg = dotBuf;
    }

    lastBroadcastTime = time(NULL);

    char buffer[8192];
    
    // Convert angle brackets to prevent breaking XML
    char safeMsg[4096];
    int j = 0;
    for (int i = 0; actualMsg[i] && j < sizeof(safeMsg) - 10; i++) {
        if (actualMsg[i] == '<') {
            safeMsg[j++] = '&'; safeMsg[j++] = 'l'; safeMsg[j++] = 't'; safeMsg[j++] = ';';
        } else if (actualMsg[i] == '>') {
            safeMsg[j++] = '&'; safeMsg[j++] = 'g'; safeMsg[j++] = 't'; safeMsg[j++] = ';';
        } else if (actualMsg[i] == '&') {
            safeMsg[j++] = '&'; safeMsg[j++] = 'a'; safeMsg[j++] = 'm'; safeMsg[j++] = 'p'; safeMsg[j++] = ';';
        } else {
            safeMsg[j++] = actualMsg[i];
        }
    }
    safeMsg[j] = '\0';
    
    snprintf(buffer, sizeof(buffer), "<message level=\"%d\">%s</message>", level, safeMsg);
    
    int len = strlen(buffer) + 1;
    char outBuffer[8196]; // 8192 buffer + 4 byte header
    memcpy(outBuffer, &len, 4);
    memcpy(outBuffer + 4, buffer, len);
    
    if (send(broadcastSocket, outBuffer, len + 4, 0) <= 0) {
        Broadcast_Shutdown();
        return;
    }
}

void Broadcast_Shutdown(void) {
    if (broadcastSocket >= 0) {
        if (bufferedDots > 0) {
            char temp[64];
            memset(temp, '.', bufferedDots);
            temp[bufferedDots] = '\0';
            bufferedDots = 0;
            Broadcast_Print(1, temp); // Flush remaining dots
        }
        
        // Send closing tag
        const char *closeStr = "</q3map_feedback>";
        int len = strlen(closeStr) + 1;
        // We ignore errors here because we are shutting down anyway
        char outBuffer[256];
        memcpy(outBuffer, &len, 4);
        memcpy(outBuffer + 4, closeStr, len);
        send(broadcastSocket, outBuffer, len + 4, 0);

        // Graceful shutdown to ensure the final tag is actually sent
#ifdef _WIN32
        shutdown(broadcastSocket, SD_SEND);
        
        // Drain the socket to wait for the peer to acknowledge the close
        char discardBuf[256];
        while (recv(broadcastSocket, discardBuf, sizeof(discardBuf), 0) > 0) {
            // Do nothing, just drain
        }
        closesocket(broadcastSocket);
#else
        shutdown(broadcastSocket, SHUT_WR);
        
        char discardBuf[256];
        while (recv(broadcastSocket, discardBuf, sizeof(discardBuf), 0) > 0) {
            // Do nothing, just drain
        }
        close(broadcastSocket);
#endif
        broadcastSocket = -1;
    }
}

void Broadcast_KeepAlive(void) {
    if (broadcastSocket < 0) return;
    time_t now = time(NULL);
    if (now - lastBroadcastTime >= 3) {
        Broadcast_Print(1, "");
    }
}
