/*
 * selector.h
 *
 * MontaVista IPMI interface code for timers and file waiting.
 *
 * Author: MontaVista Software, Inc.
 *         Corey Minyard <minyard@mvista.com>
 *         source@mvista.com
 *
 * Copyright 2002 MontaVista Software Inc.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public License
 *  as published by the Free Software Foundation; either version 2 of
 *  the License, or (at your option) any later version.
 *
 *
 *  THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 *  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 *  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 *  OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 *  ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
 *  TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 *  USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this program; if not, write to the Free
 *  Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#ifndef SELECTOR
#define SELECTOR
#include <sys/time.h> /* For timeval */

/* The main data structure used by the selector. */
struct selector_s;
typedef struct selector_s selector_t;

/* You have to create a selector before you can use it. */
int sel_alloc_selector(selector_t **new_selector);

/* Used to destroy a selector. */
int sel_free_selector(selector_t *new_selector);


/* A function to call when select sees something on a file
   descriptor. */
typedef void (*sel_fd_handler_t)(int fd, void *data);

/* Set the handlers for a file descriptor.  The "data" parameter is
   not used, it is just passed to the exception handlers. */
void sel_set_fd_handlers(selector_t       *sel,
			 int              fd,
			 void             *data,
			 sel_fd_handler_t read_handler,
			 sel_fd_handler_t write_handler,
			 sel_fd_handler_t except_handler);

/* Remove the handlers for a file descriptor.  This will also disable
   the handling of all I/O for the fd. */
void sel_clear_fd_handlers(selector_t *sel,
			   int        fd);

/* Turn on and off handling for I/O from a file descriptor. */
#define SEL_FD_HANDLER_ENABLED	0
#define SEL_FD_HANDLER_DISABLED	1
void sel_set_fd_read_handler(selector_t *sel, int fd, int state);
void sel_set_fd_write_handler(selector_t *sel, int fd, int state);
void sel_set_fd_except_handler(selector_t *sel, int fd, int state);

struct sel_timer_s;
typedef struct sel_timer_s sel_timer_t;

typedef void (*sel_timeout_handler_t)(selector_t  *sel,
				      sel_timer_t *timer,
				      void        *data);

int sel_alloc_timer(selector_t            *sel,
		    sel_timeout_handler_t handler,
		    void                  *user_data,
		    sel_timer_t           **new_timer);

int sel_free_timer(sel_timer_t *timer);

int sel_start_timer(sel_timer_t    *timer,
		    struct timeval *timeout);

int sel_stop_timer(sel_timer_t *timer);

/* This is the main loop for the program. */
void sel_select_loop(selector_t *sel);

#endif /* SELECTOR */