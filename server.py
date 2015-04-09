#!/usr/bin/env python
# -*- coding: UTF8 -*-
import zmq

'''
the whole point of all of this is to simply multiplex
the getting of requests and the sending of responses.
both of these operations are very dumb, just forward
the bytes on. ends up meaning that not being parallel
doesn't impact performance and lets you stay non-blocking
'''

TOPIC= '*'

#get a request from a client and pass on to the backend to fulfill it
def handle_request(request_socket, push_socket):
  #get the message details, should be something like ['connection_identity', '']
  message_parts  = req_rep_socket.recv_multipart(zmq.NOBLOCK)
  if len(message_parts) != 2 or len(message_parts[0]) == 0 or len(message_parts[1]) != 0:
    raise Exception('Invalid request')
  #recv the rest of the request
  #TODO: make this more robust to DOS attacks (limit long requests)
  while message_parts[-1].endswith('\r\n\r\n') == False:
    message_parts.extend(req_rep_socket.recv_multipart(zmq.NOBLOCK))

  #mash the bits together after the identity frames
  identity = message_parts[0]
  request = ''.join(message_parts[3:])
  print repr(request)
  #send to backend for processing. tell it the topic to publish the response on
  push_socket.send_multipart([identity, TOPIC, request], zmq.NOBLOCK)

#get response from the backend and send on to requesting client
def handle_response(sub_socket, req_rep_socket):
  #get the message details
  message_parts = sub_socket.recv_multipart(zmq.NOBLOCK)
  #reply with junk for now
  response_socket.send_multipart([identity,
    'HTTP/1.0 200 OK\r\nContent-Type:text/html;charset=utf-8\r\nContent-Length: 19\r\n\r\nöppis ganz bsunners\r\n'], zmq.NOBLOCK)
  #close connection
  response_socket.send_multipart([identity, ''], zmq.NOBLOCK)
 
if __name__ == '__main__':

  #basically a thread for IO
  context = zmq.Context() 

  #receive requests from clients
  req_rep_socket = context.socket(zmq.ROUTER)
  req_rep_socket.router_raw = True
  req_rep_socket.set_hwm(0)
  req_rep_socket.bind('tcp://*:8002')

  #push work into the backend
  push_socket = context.socket(zmq.PUSH)
  push_socket.set_hwm(0)
  push_socket.bind('ipc://requests')  

  #get back results from the backend
  sub_socket = context.socket(zmq.SUB)
  sub_socket.set_hwm(0)
  sub_socket.setsockopt(zmq.SUBSCRIBE, TOPIC)
  sub_socket.bind('ipc://responses')

  #multiplexing through this guy
  poller = zmq.Poller()
  poller.register(req_rep_socket, zmq.POLLIN)
  poller.register(sub_socket, zmq.POLLIN)

  #go until we quit
  while True:
    #wait for activity on one of the sockets
    try:
      sockets = dict(poller.poll())
    except KeyboardInterrupt:
      break

    #look for new requests
    if req_rep_socket in sockets:
      handle_request(req_rep_socket, push_socket)

    #look for new responses
    if sub_socket in sockets:
      handle_response(sub_socket, req_rep_socket)
