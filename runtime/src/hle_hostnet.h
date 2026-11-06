// Host TCP/UDP sockets + GetAddrInfo with Nextendo DNS redirect, used when online_services_active().
#pragma once

#include <nx/cpu.h>

namespace nx {

bool hostnet_active();
bool hostnet_owns_fd(int fd);

void hostnet_Socket(CPU& c);
void hostnet_Close(CPU& c);
void hostnet_Bind(CPU& c);
void hostnet_Connect(CPU& c);
void hostnet_Send(CPU& c);
void hostnet_SendTo(CPU& c);
void hostnet_Recv(CPU& c);
void hostnet_RecvFrom(CPU& c);
void hostnet_GetAddrInfo(CPU& c);
void hostnet_FreeAddrInfo(CPU& c);
void hostnet_GetLastErrno(CPU& c);
void hostnet_SetLastErrno(CPU& c);
void hostnet_Fcntl(CPU& c);
void hostnet_Shutdown(CPU& c);
void hostnet_SetSockOpt(CPU& c);
void hostnet_GetSockOpt(CPU& c);
void hostnet_GetSockName(CPU& c);
void hostnet_GetPeerName(CPU& c);
void hostnet_GetHostByName(CPU& c);
void hostnet_Poll(CPU& c);

bool hostnet_is_nonblocking(int guest_fd);
uint32_t hostnet_local_ip();  // network order; 0 if unknown

// Native SOCKET for SSL (kBadSock if unknown).
#ifdef _WIN32
using hostnet_sock_t = uintptr_t;
#else
using hostnet_sock_t = int;
#endif
hostnet_sock_t hostnet_native_fd(int guest_fd);

}  // namespace nx
