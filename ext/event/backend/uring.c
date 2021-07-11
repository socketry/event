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

#include "uring.h"
#include "backend.h"

#include <liburing.h>
#include <poll.h>
#include <time.h>

#include <pthread.h>

#include "pidfd.c"

#include <ruby/io/buffer.h>

static VALUE Event_Backend_URing = Qnil;

enum {URING_ENTRIES = 128};
enum {URING_MAX_EVENTS = 128};

struct Event_Backend_URing {
	VALUE loop;
	struct io_uring ring;
	
	pthread_t thread;
	pthread_cond_t submit;
	pthread_mutex_t guard;
};

void Event_Backend_URing_Type_mark(void *_data)
{
	struct Event_Backend_URing *data = _data;
	rb_gc_mark(data->loop);
}

static
void close_internal(struct Event_Backend_URing *data) {
	if (data->ring.ring_fd >= 0) {
		io_uring_queue_exit(&data->ring);
		data->ring.ring_fd = -1;
	}
	
	if (data->thread) {
		pthread_cancel(data->thread);
		pthread_join(data->thread, NULL);
		
		pthread_mutex_destroy(&data->guard);
		pthread_cond_destroy(&data->submit);
		
		data->thread = 0;
	}
}

void Event_Backend_URing_Type_free(void *_data)
{
	struct Event_Backend_URing *data = _data;
	
	close_internal(data);
	
	free(data);
}

size_t Event_Backend_URing_Type_size(const void *data)
{
	return sizeof(struct Event_Backend_URing);
}

static const rb_data_type_t Event_Backend_URing_Type = {
	.wrap_struct_name = "Event::Backend::URing",
	.function = {
		.dmark = Event_Backend_URing_Type_mark,
		.dfree = Event_Backend_URing_Type_free,
		.dsize = Event_Backend_URing_Type_size,
	},
	.data = NULL,
	.flags = RUBY_TYPED_FREE_IMMEDIATELY,
};

VALUE Event_Backend_URing_allocate(VALUE self) {
	struct Event_Backend_URing *data = NULL;
	VALUE instance = TypedData_Make_Struct(self, struct Event_Backend_URing, &Event_Backend_URing_Type, data);
	
	data->loop = Qnil;
	data->ring.ring_fd = -1;
	
	data->thread = 0;
	
	return instance;
}

static
void check(const char * message, int error) {
	if (error) {
		perror(message);
		abort();
	}
}

static
void submission_thread_cleanup(void *_data) {
	struct Event_Backend_URing *data = data;
	
	pthread_mutex_unlock(&data->guard);
}

static
void *submission_thread(void *_data) {
	struct Event_Backend_URing *data = _data;
	
	pthread_mutex_lock(&data->guard);
	pthread_cleanup_push(submission_thread_cleanup, _data);
	
	while (true) {
		pthread_cond_wait(&data->submit, &data->guard);
		fprintf(stderr, "submitting queue...\n");
		io_uring_submit(&data->ring);
	}
	
	pthread_cleanup_pop(1);
	
	return NULL;
}

static
void start_submission_thread(struct Event_Backend_URing *data) {
	check("start_submission_thread:pthread_mutex_init", 
		pthread_mutex_init(&data->guard, NULL)
	);
	
	check("start_submission_thread:pthread_cond_init",
		pthread_cond_init(&data->submit, NULL)
	);
	
	int error = pthread_create(&data->thread, NULL, submission_thread, data);
	
	if (!error) {
		data->thread = 0;
	} else {
		rb_sys_fail("start_submission_thread:pthread_create");
	}
}

inline static
void submit_lock(struct Event_Backend_URing *data) {
	pthread_mutex_lock(&data->guard);
}

inline static
void submit_unlock(struct Event_Backend_URing *data) {
	pthread_mutex_unlock(&data->guard);
}

inline static
void submit_unlock_signal(struct Event_Backend_URing *data) {
	pthread_mutex_unlock(&data->guard);
	pthread_cond_signal(&data->submit);
}

VALUE Event_Backend_URing_initialize(VALUE self, VALUE loop) {
	struct Event_Backend_URing *data = NULL;
	TypedData_Get_Struct(self, struct Event_Backend_URing, &Event_Backend_URing_Type, data);
	
	data->loop = loop;
	
	int result = io_uring_queue_init(URING_ENTRIES, &data->ring, 0);
	
	if (result < 0) {
		rb_syserr_fail(-result, "Event_Backend_URing_initialize:io_uring_queue_init");
	}
	
	rb_update_max_fd(data->ring.ring_fd);
	
	start_submission_thread(data);
	
	return self;
}

VALUE Event_Backend_URing_close(VALUE self) {
	struct Event_Backend_URing *data = NULL;
	TypedData_Get_Struct(self, struct Event_Backend_URing, &Event_Backend_URing_Type, data);
	
	close_internal(data);
	
	return Qnil;
}

struct io_uring_sqe * io_get_sqe(struct Event_Backend_URing *data) {
	struct io_uring_sqe *sqe = io_uring_get_sqe(&data->ring);
	
	while (sqe == NULL) {
		io_uring_submit(&data->ring);
		sqe = io_uring_get_sqe(&data->ring);
	}
	
	// fprintf(stderr, "io_get_sqe -> %p\n", sqe);
	
	return sqe;
}

struct process_wait_arguments {
	struct Event_Backend_URing *data;
	pid_t pid;
	int flags;
	int descriptor;
};

static
VALUE process_wait_transfer(VALUE _arguments) {
	struct process_wait_arguments *arguments = (struct process_wait_arguments *)_arguments;
	
	Event_Backend_fiber_transfer(arguments->data->loop);
	
	return Event_Backend_process_status_wait(arguments->pid);
}

static
VALUE process_wait_ensure(VALUE _arguments) {
	struct process_wait_arguments *arguments = (struct process_wait_arguments *)_arguments;
	
	close(arguments->descriptor);
	
	return Qnil;
}

VALUE Event_Backend_URing_process_wait(VALUE self, VALUE fiber, VALUE pid, VALUE flags) {
	struct Event_Backend_URing *data = NULL;
	TypedData_Get_Struct(self, struct Event_Backend_URing, &Event_Backend_URing_Type, data);
	
	struct process_wait_arguments process_wait_arguments = {
		.data = data,
		.pid = NUM2PIDT(pid),
		.flags = NUM2INT(flags),
	};
	
	process_wait_arguments.descriptor = pidfd_open(process_wait_arguments.pid, 0);
	rb_update_max_fd(process_wait_arguments.descriptor);
	
	submit_lock(data);
	struct io_uring_sqe *sqe = io_get_sqe(data);
	assert(sqe);
	
	io_uring_prep_poll_add(sqe, process_wait_arguments.descriptor, POLLIN|POLLHUP|POLLERR);
	io_uring_sqe_set_data(sqe, (void*)fiber);
	submit_unlock_signal(data);
	
	return rb_ensure(process_wait_transfer, (VALUE)&process_wait_arguments, process_wait_ensure, (VALUE)&process_wait_arguments);
}

static inline
short poll_flags_from_events(int events) {
	short flags = 0;
	
	if (events & READABLE) flags |= POLLIN;
	if (events & PRIORITY) flags |= POLLPRI;
	if (events & WRITABLE) flags |= POLLOUT;
	
	flags |= POLLERR;
	flags |= POLLHUP;
	
	return flags;
}

static inline
int events_from_poll_flags(short flags) {
	int events = 0;
	
	if (flags & POLLIN) events |= READABLE;
	if (flags & POLLPRI) events |= PRIORITY;
	if (flags & POLLOUT) events |= WRITABLE;
	
	return events;
}

struct io_wait_arguments {
	struct Event_Backend_URing *data;
	VALUE fiber;
	short flags;
};

static
VALUE io_wait_rescue(VALUE _arguments, VALUE exception) {
	struct io_wait_arguments *arguments = (struct io_wait_arguments *)_arguments;
	struct Event_Backend_URing *data = arguments->data;
	
	submit_lock(data);
	struct io_uring_sqe *sqe = io_get_sqe(data);
	assert(sqe);
	
	// fprintf(stderr, "poll_remove(%p, %p)\n", sqe, (void*)arguments->fiber);
	
	io_uring_prep_poll_remove(sqe, (void*)arguments->fiber);
	io_uring_submit(&data->ring);
	submit_unlock(data);
	
	rb_exc_raise(exception);
};

static
VALUE io_wait_transfer(VALUE _arguments) {
	struct io_wait_arguments *arguments = (struct io_wait_arguments *)_arguments;
	struct Event_Backend_URing *data = arguments->data;

	VALUE result = Event_Backend_fiber_transfer(data->loop);
	
	// We explicitly filter the resulting events based on the requested events.
	// In some cases, poll will report events we didn't ask for.
	short flags = arguments->flags & NUM2INT(result);
	
	return INT2NUM(events_from_poll_flags(flags));
};

VALUE Event_Backend_URing_io_wait(VALUE self, VALUE fiber, VALUE io, VALUE events) {
	struct Event_Backend_URing *data = NULL;
	TypedData_Get_Struct(self, struct Event_Backend_URing, &Event_Backend_URing_Type, data);
	
	int descriptor = Event_Backend_io_descriptor(io);
	
	submit_lock(data);
	struct io_uring_sqe *sqe = io_get_sqe(data);
	assert(sqe);
	
	short flags = poll_flags_from_events(NUM2INT(events));
	
	// fprintf(stderr, "poll_add(%p, %d, %d, %p)\n", sqe, descriptor, flags, (void*)fiber);
	
	io_uring_prep_poll_add(sqe, descriptor, flags);
	io_uring_sqe_set_data(sqe, (void*)fiber);
	// fprintf(stderr, "io_uring_submit\n");
	// io_uring_submit(&data->ring);
	submit_unlock(data);
	
	struct io_wait_arguments io_wait_arguments = {
		.data = data,
		.fiber = fiber,
		.flags = flags
	};
	
	return rb_rescue(io_wait_transfer, (VALUE)&io_wait_arguments, io_wait_rescue, (VALUE)&io_wait_arguments);
}

#ifdef HAVE_RUBY_IO_BUFFER_H

static int io_read(struct Event_Backend_URing *data, VALUE fiber, int descriptor, char *buffer, size_t length) {
	submit_lock(data);
	struct io_uring_sqe *sqe = io_get_sqe(data);
	assert(sqe);
	
	io_uring_prep_read(sqe, descriptor, buffer, length, 0);
	io_uring_sqe_set_data(sqe, (void*)fiber);
	// io_uring_submit(&data->ring);
	submit_unlock_signal(data);

	VALUE result = Event_Backend_fiber_transfer(data->loop);
	return RB_NUM2INT(result);
}

VALUE Event_Backend_URing_io_read(VALUE self, VALUE fiber, VALUE io, VALUE buffer, VALUE _length) {
	struct Event_Backend_URing *data = NULL;
	TypedData_Get_Struct(self, struct Event_Backend_URing, &Event_Backend_URing_Type, data);
	
	int descriptor = Event_Backend_io_descriptor(io);
	
	void *base;
	size_t size;
	rb_io_buffer_get_mutable(buffer, &base, &size);
	
	size_t offset = 0;
	size_t length = NUM2SIZET(_length);
	
	while (length > 0) {
		size_t maximum_size = size - offset;
		int result = io_read(data, fiber, descriptor, (char*)base+offset, maximum_size);
		
		if (result == 0) {
			break;
		} else if (result > 0) {
			offset += result;
			if ((size_t)result > length) break;
			length -= result;
		} else if (-result == EAGAIN || -result == EWOULDBLOCK) {
			Event_Backend_URing_io_wait(self, fiber, io, RB_INT2NUM(READABLE));
		} else {
			rb_syserr_fail(-result, strerror(-result));
		}
	}
	
	return SIZET2NUM(offset);
}

static
int io_write(struct Event_Backend_URing *data, VALUE fiber, int descriptor, char *buffer, size_t length) {
	submit_lock(data);
	struct io_uring_sqe *sqe = io_get_sqe(data);
	assert(sqe);
	
	io_uring_prep_write(sqe, descriptor, buffer, length, 0);
	io_uring_sqe_set_data(sqe, (void*)fiber);
	// io_uring_submit(&data->ring);
	submit_unlock_signal(data);
	
	return NUM2INT(Event_Backend_fiber_transfer(data->loop));
}

VALUE Event_Backend_URing_io_write(VALUE self, VALUE fiber, VALUE io, VALUE buffer, VALUE _length) {
	struct Event_Backend_URing *data = NULL;
	TypedData_Get_Struct(self, struct Event_Backend_URing, &Event_Backend_URing_Type, data);
	
	int descriptor = Event_Backend_io_descriptor(io);
	
	const void *base;
	size_t size;
	rb_io_buffer_get_immutable(buffer, &base, &size);
	
	size_t offset = 0;
	size_t length = NUM2SIZET(_length);
	
	if (length > size) {
		rb_raise(rb_eRuntimeError, "Length exceeds size of buffer!");
	}
	
	while (length > 0) {
		int result = io_write(data, fiber, descriptor, (char*)base+offset, length);
		
		if (result >= 0) {
			length -= result;
			offset += result;
		} else if (-result == EAGAIN || -result == EWOULDBLOCK) {
			Event_Backend_URing_io_wait(self, fiber, io, RB_INT2NUM(WRITABLE));
		} else {
			rb_syserr_fail(-result, strerror(-result));
		}
	}
	
	return SIZET2NUM(offset);
}

#endif

static
struct __kernel_timespec * make_timeout(VALUE duration, struct __kernel_timespec *storage) {
	if (duration == Qnil) {
		return NULL;
	}
	
	if (FIXNUM_P(duration)) {
		storage->tv_sec = NUM2TIMET(duration);
		storage->tv_nsec = 0;
		
		return storage;
	}
	
	else if (RB_FLOAT_TYPE_P(duration)) {
		double value = RFLOAT_VALUE(duration);
		time_t seconds = value;
		
		storage->tv_sec = seconds;
		storage->tv_nsec = (value - seconds) * 1000000000L;
		
		return storage;
	}
	
	rb_raise(rb_eRuntimeError, "unable to convert timeout");
}

static
int timeout_nonblocking(struct __kernel_timespec *timespec) {
	return timespec && timespec->tv_sec == 0 && timespec->tv_nsec == 0;
}

struct select_arguments {
	struct Event_Backend_URing *data;
	
	int result;
	
	struct __kernel_timespec storage;
	struct __kernel_timespec *timeout;
};

static
void * select_internal(void *_arguments) {
	struct select_arguments * arguments = (struct select_arguments *)_arguments;
	
	submit_lock(arguments->data);
	io_uring_submit(&arguments->data->ring);
	
	struct io_uring_cqe *cqe = NULL;
	arguments->result = io_uring_wait_cqe_timeout(&arguments->data->ring, &cqe, arguments->timeout);
	submit_unlock(arguments->data);
	
	return NULL;
}

static
int select_internal_without_gvl(struct select_arguments *arguments) {
	rb_thread_call_without_gvl(select_internal, (void *)arguments, RUBY_UBF_IO, 0);
	
	if (arguments->result == -ETIME) {
		arguments->result = 0;
	} else if (arguments->result < 0) {
		rb_syserr_fail(-arguments->result, "select_internal_without_gvl:io_uring_wait_cqes");
	} else {
		// At least 1 event is waiting:
		arguments->result = 1;
	}
	
	return arguments->result;
}

static inline
unsigned select_process_completions(struct io_uring *ring) {
	unsigned completed = 0;
	unsigned head;
	struct io_uring_cqe *cqe;
	
	io_uring_for_each_cqe(ring, head, cqe) {
		++completed;
		
		// If the operation was cancelled, or the operation has no user data (fiber):
		if (cqe->res == -ECANCELED || cqe->user_data == 0 || cqe->user_data == LIBURING_UDATA_TIMEOUT) {
			continue;
		}
		
		VALUE fiber = (VALUE)cqe->user_data;
		VALUE result = INT2NUM(cqe->res);
		
		// fprintf(stderr, "cqe res=%d user_data=%p\n", cqe->res, (void*)cqe->user_data);

		Event_Backend_fiber_transfer_result(fiber, result);
	}
	
	if (completed) {
		io_uring_cq_advance(ring, completed);
	}
	
	return completed;
}

VALUE Event_Backend_URing_select(VALUE self, VALUE duration) {
	struct Event_Backend_URing *data = NULL;
	TypedData_Get_Struct(self, struct Event_Backend_URing, &Event_Backend_URing_Type, data);
	
	int result = select_process_completions(&data->ring);
	
	if (result < 0) {
		rb_syserr_fail(-result, strerror(-result));
	} else if (result == 0) {
		// We might need to wait for events:
		struct select_arguments arguments = {
			.data = data,
			.timeout = NULL,
		};
		
		arguments.timeout = make_timeout(duration, &arguments.storage);
		
		if (!timeout_nonblocking(arguments.timeout)) {
			result = select_internal_without_gvl(&arguments);
		} else {
			io_uring_submit(&data->ring);
		}
	}
	
	result = select_process_completions(&data->ring);
	
	return INT2NUM(result);
}

void Init_Event_Backend_URing(VALUE Event_Backend) {
	Event_Backend_URing = rb_define_class_under(Event_Backend, "URing", rb_cObject);
	
	rb_define_alloc_func(Event_Backend_URing, Event_Backend_URing_allocate);
	rb_define_method(Event_Backend_URing, "initialize", Event_Backend_URing_initialize, 1);
	rb_define_method(Event_Backend_URing, "select", Event_Backend_URing_select, 1);
	rb_define_method(Event_Backend_URing, "close", Event_Backend_URing_close, 0);
	
	rb_define_method(Event_Backend_URing, "io_wait", Event_Backend_URing_io_wait, 3);
	
#ifdef HAVE_RUBY_IO_BUFFER_H
	rb_define_method(Event_Backend_URing, "io_read", Event_Backend_URing_io_read, 4);
	rb_define_method(Event_Backend_URing, "io_write", Event_Backend_URing_io_write, 4);
#endif
	
	rb_define_method(Event_Backend_URing, "process_wait", Event_Backend_URing_process_wait, 3);
}
