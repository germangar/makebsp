#ifndef CONNECT_H
#define CONNECT_H

void Broadcast_Setup(const char *dest);
void Broadcast_Print(int level, const char *msg);
void Broadcast_KeepAlive(void);
void Broadcast_Shutdown(void);

#endif // CONNECT_H
