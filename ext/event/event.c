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

#include "event.h"
#include "selector/selector.h"

VALUE Event = Qnil;
VALUE Event_Selector = Qnil;

void Init_event()
{
	#ifdef HAVE_RB_EXT_RACTOR_SAFE
	rb_ext_ractor_safe(true);
	#endif
	
	Event = rb_define_module("Event");
	Event_Selector = rb_define_module_under(Event, "Selector");
	
	Init_Event_Selector(Event_Selector);
	
	#ifdef EVENT_SELECTOR_URING
	Init_Event_Selector_URing(Event_Selector);
	#endif
	
	#ifdef EVENT_SELECTOR_EPOLL
	Init_Event_Selector_EPoll(Event_Selector);
	#endif
	
	#ifdef EVENT_SELECTOR_KQUEUE
	Init_Event_Selector_KQueue(Event_Selector);
	#endif
}
