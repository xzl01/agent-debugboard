#!/usr/bin/ruby
# frozen_string_literal: true

require "json"
require "socket"

listen_host = ENV.fetch("LINKR_NCM_FORWARDER_HOST", "127.0.0.1")
listen_port = Integer(ENV.fetch("LINKR_NCM_FORWARDER_PORT", "0"), 10)
target_host = ARGV.fetch(0)
target_port = Integer(ARGV.fetch(1), 10)

server = TCPServer.new(listen_host, listen_port)
actual_port = server.local_address.ip_port

STDOUT.sync = true
STDERR.sync = true
puts JSON.generate({ ready: true, host: listen_host, port: actual_port })

shutdown = proc do
  server.close unless server.closed?
  exit! 0
rescue IOError, SystemCallError
  exit! 0
end

Signal.trap("INT", &shutdown)
Signal.trap("TERM", &shutdown)

def copy_socket(source, destination)
  IO.copy_stream(source, destination)
  destination.close_write
rescue Errno::ECONNRESET, Errno::EPIPE, IOError
  nil
end

begin
  loop do
    downstream = server.accept
    Thread.new(downstream) do |client|
      upstream = nil
      begin
        upstream = Socket.tcp(target_host, target_port, connect_timeout: 4)
        client.setsockopt(Socket::IPPROTO_TCP, Socket::TCP_NODELAY, 1)
        upstream.setsockopt(Socket::IPPROTO_TCP, Socket::TCP_NODELAY, 1)

        to_upstream = Thread.new { copy_socket(client, upstream) }
        to_downstream = Thread.new { copy_socket(upstream, client) }
        to_upstream.join
        to_downstream.join
      rescue Errno::ECONNREFUSED, Errno::EHOSTUNREACH, Errno::ENETDOWN,
             Errno::ENETUNREACH, Errno::ETIMEDOUT => error
        warn "upstream #{target_host}:#{target_port} unavailable: #{error.message}"
      ensure
        upstream&.close unless upstream&.closed?
        client.close unless client.closed?
      end
    end
  end
rescue IOError, Errno::EBADF
  # The listener was closed by the signal handler.
end
