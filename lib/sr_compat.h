/* compat.h
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
#ifndef __COMPAT_H__
#define __COMPAT_H__

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <semaphore.h>
#include <pthread.h>

//
// This file handles all portablity issues with streamripper
//

// File Routines
////////////////////////////////////////// 


#define FHANDLE	int
// #define OpenFile(_filename_)	open(_filename_, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)
// #define CloseFile(_fhandle_) 	close(_fhandle_)
// #define TruncateFile(_filename_)	CloseFile(open(_filename_, O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH))
// #define MoveFile(_oldfile_, _newfile_)     rename(_oldfile_, _newfile_)
// #define DeleteFile(_file_)  	(!unlink(_file_))
#define INVALID_FHANDLE 	-1


// Thread Routines

#define THREAD_HANDLE		pthread_t
#define BeginThread(_thandle_, callback, arg) \
               pthread_create(&_thandle_, NULL, \
                          (void *)callback, (void *)arg)
#define WaitForThread(_thandle_)	pthread_join(_thandle_, NULL)
#define DestroyThread(_thandle_)	// is there one for unix?

#if defined (__APPLE__)
/* macOS deprecated (and never really supported) unnamed POSIX semaphores,
   so use GCD dispatch semaphores there.  dispatch_semaphore_t is an opaque
   pointer, which also makes threadlib_create_sem()'s return-by-value safe. */
#include <dispatch/dispatch.h>
#define HSEM		dispatch_semaphore_t
#define	SemInit(_s_)	((_s_) = dispatch_semaphore_create (0))
#define	SemWait(_s_)	dispatch_semaphore_wait ((_s_), DISPATCH_TIME_FOREVER)
#define	SemPost(_s_)	dispatch_semaphore_signal ((_s_))
#define	SemDestroy(_s_)	dispatch_release ((_s_))
#else
#define HSEM		sem_t
#define	SemInit(_s_)	sem_init(&(_s_), 0, 0)
#define	SemWait(_s_)	sem_wait(&(_s_))
#define	SemPost(_s_)	sem_post(&(_s_))
#define	SemDestroy(_s_)	sem_destroy(&(_s_))
#endif

#define Sleep(x) 	usleep(1000*x)


// Socket Routines
////////////////////////////////////////// 

#define closesocket     close
#define SOCKET_ERROR	-1
#define WSACleanup()

// Other stuff
////////////////////////////////////////// 


#endif // __COMPAT_H__
