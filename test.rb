#!/usr/bin/env ruby

require_relative 'lib/event'

require 'fiber'
require 'socket'

local, remote = UNIXSocket.pair
selector = Event::Backend::URing.new(Fiber.current)

f1 = Fiber.new do
	buffer = IO::Buffer.new(128)
	length = selector.io_read(Fiber.current, local, buffer, 1)
	pp ["f1", length, buffer]
end

f2 = Fiber.new do
	buffer = IO::Buffer.new(128)
	offset = buffer.copy("Hello World", 0)
	length = selector.io_write(Fiber.current, remote, buffer, offset)
	pp ["f2", length, buffer]
end

f1.transfer
f2.transfer

pp ["select", selector.select(1)]

