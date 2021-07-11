// Copyright, 2021, by Samuel G. D. Williams. <http://www.codeotaku.com>
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include <ruby.h>
#include <ruby/thread.h>
#include <ruby/io.h>

#ifdef HAVE_RUBY_IO_BUFFER_H
#include <ruby/io/buffer.h>
#endif

#include <time.h>

enum Event {
	READABLE = 1,
	PRIORITY = 2,
	WRITABLE = 4,
	ERROR = 8,
	HANGUP = 16
};

void Init_Event_Backend();

#ifdef HAVE__RB_FIBER_TRANSFER
#define Event_Backend_fiber_transfer(fiber) rb_fiber_transfer(fiber, 0, NULL)
#define Event_Backend_fiber_transfer_result(fiber, argument) rb_fiber_transfer(fiber, 1, &argument)
#else
VALUE Event_Backend_fiber_transfer(VALUE fiber);
VALUE Event_Backend_fiber_transfer_result(VALUE fiber, VALUE argument);
#endif

#ifdef HAVE_RB_IO_DESCRIPTOR
#define Event_Backend_io_descriptor(io) rb_io_descriptor(io)
#else
int Event_Backend_io_descriptor(VALUE io);
#endif

#ifdef HAVE_RB_PROCESS_STATUS_WAIT
#define Event_Backend_process_status_wait(pid) rb_process_status_wait(pid)
#else
VALUE Event_Backend_process_status_wait(rb_pid_t pid);
#endif

int Event_Backend_nonblock_set(int file_descriptor);
void Event_Backend_nonblock_restore(int file_descriptor, int flags);

void Event_Backend_elapsed_time(struct timespec* start, struct timespec* stop, struct timespec *duration);
void Event_Backend_current_time(struct timespec *time);

#define PRINTF_TIMESPEC "%lld.%.9ld"
#define PRINTF_TIMESPEC_ARGS(ts) (long long)((ts).tv_sec), (ts).tv_nsec
