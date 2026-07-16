/* socklib.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#if WIN32
#include <winsock2.h>
#else
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#endif

#if __UNIX__
#include <arpa/inet.h>
#elif __BEOS__
#include <be/net/netdb.h>   
#endif

#include "srtypes.h"
#include "socklib.h"
#include "threadlib.h"
#include "sr_compat.h"
#include "debug.h"

#if defined(HAVE_OPENSSL)
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#endif


#if WIN32
#define DEFAULT_TIMEOUT		(15 * 1000)
#define FIRST_READ_TIMEOUT	(30 * 1000)
#endif


/****************************************************************************
 * Function definitions
 ****************************************************************************/
error_code
socklib_init()
{
#if WIN32
    WORD wVersionRequested;
    WSADATA wsaData;
    int err;

    wVersionRequested = MAKEWORD( 2, 2 );
    err = WSAStartup( wVersionRequested, &wsaData );
    if ( err != 0 )
        return SR_ERROR_WIN32_INIT_FAILURE;
#endif

    return SR_SUCCESS;
}

/* Try to find the local interface to bind to */
error_code
read_interface (char *if_name, uint32_t *addr)
{
#if defined (WIN32)
    return -1;
#else
    int fd;
    struct ifreq ifr;

    memset(&ifr, 0, sizeof(struct ifreq));
    if((fd = socket(AF_INET, SOCK_DGRAM, 0)) >= 0) {
	ifr.ifr_addr.sa_family = AF_INET;
	strcpy(ifr.ifr_name, if_name);
	if (ioctl(fd, SIOCGIFADDR, &ifr) == 0)
	    *addr = ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr.s_addr;
	else {
	    close(fd);
	    return -2;
	}
    } else 
	return -1;
    close(fd);
    return 0;
#endif
}

#if defined(HAVE_OPENSSL)
/*
 * Wrap an already-connected socket in a TLS session.  When ssl_verify is
 * non-zero the server certificate and hostname are validated against the
 * system trust store; otherwise the connection is encrypted but unverified
 * (the default, matching the opt-in --ssl-verify behavior).
 */
static error_code
socklib_ssl_handshake (HSOCKET *socket_handle, const char *host,
		       int ssl_verify)
{
    SSL_CTX *ctx = NULL;
    SSL *ssl = NULL;
    long verify_result;

    ctx = SSL_CTX_new (TLS_client_method ());
    if (!ctx) {
	debug_printf ("SSL_CTX_new failed\n");
	return SR_ERROR_SSL_INIT_FAILED;
    }
    /* Require at least TLS 1.2 for modern streams. */
    SSL_CTX_set_min_proto_version (ctx, TLS1_2_VERSION);

    if (ssl_verify) {
	SSL_CTX_set_verify (ctx, SSL_VERIFY_PEER, NULL);
	if (SSL_CTX_set_default_verify_paths (ctx) != 1) {
	    debug_printf ("Warning: could not load system CA certificates\n");
	}
    } else {
	SSL_CTX_set_verify (ctx, SSL_VERIFY_NONE, NULL);
    }

    ssl = SSL_new (ctx);
    if (!ssl) {
	debug_printf ("SSL_new failed\n");
	SSL_CTX_free (ctx);
	return SR_ERROR_SSL_INIT_FAILED;
    }

    /* SNI: many virtual-hosted https servers require this. */
    SSL_set_tlsext_host_name (ssl, host);

    if (ssl_verify) {
	/* Check that the cert actually matches the hostname. */
	SSL_set_hostflags (ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
	if (SSL_set1_host (ssl, host) != 1) {
	    debug_printf ("SSL_set1_host failed\n");
	    SSL_free (ssl);
	    SSL_CTX_free (ctx);
	    return SR_ERROR_SSL_INIT_FAILED;
	}
    }

    if (SSL_set_fd (ssl, socket_handle->s) != 1) {
	debug_printf ("SSL_set_fd failed\n");
	SSL_free (ssl);
	SSL_CTX_free (ctx);
	return SR_ERROR_SSL_INIT_FAILED;
    }

    if (SSL_connect (ssl) != 1) {
	debug_printf ("SSL_connect (handshake) failed\n");
	SSL_free (ssl);
	SSL_CTX_free (ctx);
	return SR_ERROR_SSL_HANDSHAKE_FAILED;
    }

    if (ssl_verify) {
	verify_result = SSL_get_verify_result (ssl);
	if (verify_result != X509_V_OK) {
	    debug_printf ("Certificate verification failed: %ld\n",
			  verify_result);
	    SSL_shutdown (ssl);
	    SSL_free (ssl);
	    SSL_CTX_free (ctx);
	    return SR_ERROR_SSL_HANDSHAKE_FAILED;
	}
    }

    debug_printf ("TLS handshake OK, cipher = %s\n", SSL_get_cipher (ssl));
    socket_handle->ssl = ssl;
    socket_handle->ssl_ctx = ctx;
    return SR_SUCCESS;
}
#endif /* HAVE_OPENSSL */

/*
 * open's a tcp connection to host at port, host can be a dns name or IP,
 * socket_handle gets assigned to the handle for the connection.  When
 * use_ssl is set, a TLS session is negotiated on top of the connection.
 */
error_code
socklib_open (HSOCKET *socket_handle, char *host, int port,
	      char *if_name, int timeout, int use_ssl, int ssl_verify)
{
    int rc;
    struct sockaddr_in address, local;
    struct hostent *hp;
    int len;

    if (!socket_handle || !host)
	return SR_ERROR_INVALID_PARAM;

#if defined(HAVE_OPENSSL)
    /* Free TLS objects left over from a previous connection on this handle.
       We deliberately defer the free to here (instead of socklib_close) so a
       reader thread that is still inside SSL_read is never racing an SSL_free
       issued from another thread during teardown: by the time we re-open a
       connection, that reader thread has already been joined.  SSL_free(NULL)
       / SSL_CTX_free(NULL) are safe no-ops on the first open. */
    if (socket_handle->ssl) {
	SSL_free ((SSL *) socket_handle->ssl);
    }
    if (socket_handle->ssl_ctx) {
	SSL_CTX_free ((SSL_CTX *) socket_handle->ssl_ctx);
    }
#endif
    socket_handle->ssl = NULL;
    socket_handle->ssl_ctx = NULL;

    if (use_ssl) {
#if defined(HAVE_OPENSSL)
	/* nothing to do here; handshake happens after connect() below */
#else
	return SR_ERROR_SSL_NOT_COMPILED;
#endif
    }

    /* On error:
       Unix returns -1 and sets errno.
       Windows??? */
    socket_handle->s = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_handle->s == SOCKET_ERROR) {
	debug_printf ("socket() failed\n");
	WSACleanup ();
	return SR_ERROR_CANT_CREATE_SOCKET;
    }

    if (if_name) {
	if (read_interface (if_name, &local.sin_addr.s_addr) != 0)
	    local.sin_addr.s_addr = htonl(INADDR_ANY);
	local.sin_family = AF_INET;
	local.sin_port = htons(0);
	/* On error:
	   Unix returns -1 and sets errno.
	   Windows??? */
	debug_printf ("Calling bind\n");
	if (bind(socket_handle->s, (struct sockaddr *)&local, 
		 sizeof(local)) == SOCKET_ERROR) {
	    debug_printf ("Bind failed\n");
	    WSACleanup ();
	    closesocket (socket_handle->s);
	    return SR_ERROR_CANT_BIND_ON_INTERFACE;
	}
    }

    if ((address.sin_addr.s_addr = inet_addr(host)) == INADDR_NONE) {
	debug_printf ("Calling gethostbyname\n");
	hp = gethostbyname (host);
	if (hp) {
	    memcpy (&address.sin_addr, hp->h_addr_list[0], hp->h_length);
	} else {
	    debug_printf ("resolving hostname: %s failed\n", host);
	    WSACleanup ();
	    /* GCS Added... */
	    closesocket (socket_handle->s);
	    return SR_ERROR_CANT_RESOLVE_HOSTNAME;
	}
    }
    address.sin_family = AF_INET;
    address.sin_port = htons((unsigned short)port);
    len = sizeof(address);

    debug_printf ("Calling connect\n");
    rc = connect (socket_handle->s, (struct sockaddr *)&address, len);
    debug_printf ("Connect complete\n");
    if (rc == SOCKET_ERROR) {
	debug_printf ("connect failed\n");
	/* GCS Added... */
	WSACleanup ();
	closesocket (socket_handle->s);
	return SR_ERROR_CONNECT_FAILED;
    }

#ifdef WIN32
    {
	struct timeval tv = {timeout, 0};
	rc = setsockopt (socket_handle->s, SOL_SOCKET,  SO_RCVTIMEO, 
			 (char *)&tv, sizeof(struct timeval));
	if (rc == SOCKET_ERROR) {
	    debug_printf ("setsockopt failed\n");
	    /* GCS Added... */
	    WSACleanup ();
	    closesocket (socket_handle->s);
	    return SR_ERROR_CANT_SET_SOCKET_OPTIONS;
	}
    }
#endif

    socket_handle->closed = FALSE;

    if (use_ssl) {
#if defined(HAVE_OPENSSL)
	error_code sr;
	sr = socklib_ssl_handshake (socket_handle, host, ssl_verify);
	if (sr != SR_SUCCESS) {
	    closesocket (socket_handle->s);
	    socket_handle->closed = TRUE;
	    return sr;
	}
#else
	closesocket (socket_handle->s);
	socket_handle->closed = TRUE;
	return SR_ERROR_SSL_NOT_COMPILED;
#endif
    }

    return SR_SUCCESS;
}

/* Negotiate TLS on an already-connected socket (does NOT open a new
   connection).  Used to upgrade an HTTP CONNECT proxy tunnel to TLS. */
error_code
socklib_start_tls (HSOCKET *socket_handle, char *host, int ssl_verify)
{
    if (!socket_handle || !host)
	return SR_ERROR_INVALID_PARAM;
    if (socket_handle->closed)
	return SR_ERROR_SOCKET_CLOSED;
#if defined(HAVE_OPENSSL)
    return socklib_ssl_handshake (socket_handle, host, ssl_verify);
#else
    return SR_ERROR_SSL_NOT_COMPILED;
#endif
}

void
socklib_cleanup()
{
    WSACleanup();
}

void
socklib_close(HSOCKET *socket_handle)
{
    /* NOTE on TLS: socklib_close may be called from a different thread than
       the one reading the socket (see rip_manager_stop), so we must NOT
       SSL_free the session here -- that would be a use-after-free against a
       reader still inside SSL_read.  Closing the underlying fd is enough to
       unblock the reader; the SSL/SSL_CTX objects are freed later, from
       socklib_open, once the reader thread has been joined. */
    closesocket(socket_handle->s);
    socket_handle->closed = TRUE;
}

error_code
socklib_read_header(RIP_MANAGER_INFO* rmi, HSOCKET *socket_handle, 
		    char *buffer, int size)
{
    int i;
#ifdef WIN32
    int timeout;
#endif
    int ret;
    char *t;

    if (socket_handle->closed)
	return SR_ERROR_SOCKET_CLOSED;

#ifdef WIN32
    timeout = 2 * rmi->prefs->timeout * 1000;  /* Convert sec to msec */
    if (setsockopt (socket_handle->s, SOL_SOCKET,  SO_RCVTIMEO, (char *)&timeout, sizeof(int)) == SOCKET_ERROR)
	return SR_ERROR_CANT_SET_SOCKET_OPTIONS;
#endif

    memset(buffer, 0, size);
    for (i = 0; i < size; i++)
    {
	ret = socklib_recvall (rmi, socket_handle, &buffer[i], 1, 0);
	if (ret < 0) {
	    return ret;
	}

	if (ret == 0) {
	    debug_printf("http header:\n%s\n", buffer);
	    return SR_ERROR_NO_HTTP_HEADER;
	}

	if (socket_handle->closed)
	    return SR_ERROR_SOCKET_CLOSED;

#if defined (commentout)
	/* This is too restrictive.  Some servers do not send this. */
	if (!strstr(buffer, "icy-") && !strstr(buffer,"ice-"))
	    continue;
#endif

	t = buffer + (i > 3 ? i - 3: i);

	if (strncmp(t, "\r\n\r\n", 4) == 0)
	    break;
	/* Allegedly live365 used to do this */
	if (strncmp(t, "\n\0\r\n", 4) == 0)
	    break;
    }

    if (i == size) {
	debug_printf("http header:\n%s\n", buffer);
	return SR_ERROR_NO_HTTP_HEADER;
    }

    buffer[i] = '\0';

#ifdef WIN32
    timeout = rmi->prefs->timeout * 1000;  /* Convert sec to msec */
    if (setsockopt (socket_handle->s, SOL_SOCKET,  SO_RCVTIMEO, (char *)&timeout, sizeof(timeout)) == SOCKET_ERROR)
	return SR_ERROR_CANT_SET_SOCKET_OPTIONS;
#endif

    return SR_SUCCESS;
}

error_code
socklib_recvall (RIP_MANAGER_INFO* rmi, HSOCKET *socket_handle, 
		 char* buffer, int size, int timeout)
{
    int ret = 0, read = 0;
    int sock;
    fd_set fds;
    struct timeval tv;
    
    sock = socket_handle->s;
    FD_ZERO(&fds);
    while(size) {
	int ssl_has_pending = 0;

	if (socket_handle->closed)
	    return SR_ERROR_SOCKET_CLOSED;

#if defined(HAVE_OPENSSL)
	/* OpenSSL may already hold decrypted bytes from a previously read
	   TLS record; select() on the raw fd wouldn't see them. */
	if (socket_handle->ssl
	    && SSL_pending ((SSL *) socket_handle->ssl) > 0)
	    ssl_has_pending = 1;
#endif

	if (timeout > 0 && !ssl_has_pending) {
	    /* Wait up to 'timeout' seconds for data on socket to be
	       ready for read */
#if __UNIX__
	    FD_SET(rmi->abort_pipe[0], &fds);
#endif
	    FD_SET(sock, &fds);
	    tv.tv_sec = timeout;
	    tv.tv_usec = 0;
	    ret = select (sock + 1, &fds, NULL, NULL, &tv);
	    if (ret == SOCKET_ERROR) {
		/* This happens when I kill winamp while ripping */
		return SR_ERROR_SELECT_FAILED;
	    }
	    if (ret == 0) {
		return SR_ERROR_TIMEOUT;
	    }
#if __UNIX__
	    if (FD_ISSET(rmi->abort_pipe[0], &fds)) {
		debug_printf ("socklib_recvall detected write to abort pipe.\n");
		return SR_ERROR_ABORT_PIPE_SIGNALLED;
	    }
#endif
	}

#if defined(HAVE_OPENSSL)
	if (socket_handle->ssl) {
	    ret = SSL_read ((SSL *) socket_handle->ssl, &buffer[read], size);
	    if (ret <= 0) {
		int ssl_err = SSL_get_error ((SSL *) socket_handle->ssl, ret);
		if (ssl_err == SSL_ERROR_ZERO_RETURN) {
		    /* Peer closed the TLS session cleanly. */
		    ret = 0;
		} else if (ssl_err == SSL_ERROR_WANT_READ
			   || ssl_err == SSL_ERROR_WANT_WRITE) {
		    /* Need more I/O (e.g. renegotiation); loop and retry. */
		    continue;
		} else {
		    debug_printf ("SSL_read failed, ssl_err = %d\n", ssl_err);
		    return SR_ERROR_RECV_FAILED;
		}
	    }
	} else
#endif
	{
	    ret = recv(socket_handle->s, &buffer[read], size, 0);
	}
	debug_printf ("RECV req %5d bytes, got %5d bytes\n", size, ret);

        if (ret == SOCKET_ERROR) {
	    debug_printf ("RECV failed, errno = %d\n", errno);
	    debug_printf ("Err = %s\n",strerror(errno));
	    return SR_ERROR_RECV_FAILED;
	}

	/* Got zero bytes on blocking read.  For unix this is an
	   orderly shutdown. */
	if (ret == 0) {
	    debug_printf ("recv received zero bytes!\n");
	    break;
	}

	read += ret;
	size -= ret;
    }

    return read;
}

int
socklib_sendall (HSOCKET *socket_handle, char* buffer, int size)
{
    int ret = 0, sent = 0;

    while(size) {
	if (socket_handle->closed)
	    return SR_ERROR_SOCKET_CLOSED;

#if defined(HAVE_OPENSSL)
	if (socket_handle->ssl) {
	    ret = SSL_write ((SSL *) socket_handle->ssl, &buffer[sent], size);
	    if (ret <= 0) {
		int ssl_err = SSL_get_error ((SSL *) socket_handle->ssl, ret);
		if (ssl_err == SSL_ERROR_WANT_READ
		    || ssl_err == SSL_ERROR_WANT_WRITE)
		    continue;
		debug_printf ("SSL_write failed, ssl_err = %d\n", ssl_err);
		return SR_ERROR_SEND_FAILED;
	    }
	} else
#endif
	{
	    ret = send(socket_handle->s, &buffer[sent], size, 0);
	    if (ret == SOCKET_ERROR)
		return SR_ERROR_SEND_FAILED;
	}

	if (ret == 0)
	    break;

	sent += ret;
	size -= ret;
    }

    return sent;
}
