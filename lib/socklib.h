#ifndef __socklib_h__
#define __socklib_h__

#include "srtypes.h"
#include "rip_manager.h"

#ifndef INADDR_NONE
#define INADDR_NONE (-1)
#endif

error_code socklib_init ();
error_code socklib_open (HSOCKET *socket_handle, char *host, int port, char *if_name, int timeout, int use_ssl, int ssl_verify);
/* Negotiate TLS on an already-connected socket (e.g. an HTTP CONNECT proxy
   tunnel).  Unlike socklib_open, this does NOT create a new connection. */
error_code socklib_start_tls (HSOCKET *socket_handle, char *host, int ssl_verify);
void socklib_close (HSOCKET *socket_handle);
void socklib_cleanup ();
error_code
socklib_read_header(RIP_MANAGER_INFO* rmi, HSOCKET *socket_handle, 
		    char *buffer, int size);
error_code
socklib_recvall (RIP_MANAGER_INFO* rmi, HSOCKET *socket_handle, 
		 char* buffer, int size, int timeout);
int socklib_sendall (HSOCKET *socket_handle, char* buffer, int size);
error_code read_interface (char *if_name, uint32_t *addr);

#endif	//__socklib_h__
