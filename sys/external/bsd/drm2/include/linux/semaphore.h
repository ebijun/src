/*-
 * Copyright (c) 2026 Jun Ebihara <jun@NetBSD.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _LINUX_SEMAPHORE_H_
#define _LINUX_SEMAPHORE_H_

#include <sys/types.h>
#include <sys/condvar.h>
#include <sys/mutex.h>

struct semaphore {
	kmutex_t	lock;
	kcondvar_t	cv;
	unsigned int	count;
};

static inline void
sema_init(struct semaphore *sem, int val)
{
	mutex_init(&sem->lock, MUTEX_DEFAULT, IPL_NONE);
	cv_init(&sem->cv, "sema");
	sem->count = val;
}

static inline void
down(struct semaphore *sem)
{
	mutex_enter(&sem->lock);
	while (sem->count == 0)
		cv_wait(&sem->cv, &sem->lock);
	sem->count--;
	mutex_exit(&sem->lock);
}

static inline void
up(struct semaphore *sem)
{
	mutex_enter(&sem->lock);
	sem->count++;
	cv_signal(&sem->cv);
	mutex_exit(&sem->lock);
}

#endif /* _LINUX_SEMAPHORE_H_ */

