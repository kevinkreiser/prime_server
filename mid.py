#!/usr/bin/env python
# -*- coding: UTF8 -*-
import zmq

'''
whole point of all of this is simply to be an intermediary work node.
we will get a request, do some work on it and either:

encounter an error - we pub on failure channel
complete last step - we pub on success channel
complete mid step - we push semi-complete work to next stage
'''

#get a request from a client and pass on to the backend to fulfill it
def handle_request(request_socket, push_socket, pub_socket):
  #get the message details, should be something like ['connection_identity', 'request']
  message_parts  = request_socket.recv_multipart(zmq.NOBLOCK)
  if len(message_parts) != 2 or len(message_parts[0]) == 0:
    return
  identity = message_parts[0]
  request = message_parts[1]
  if len(request) == 0:
    pub_socket.send_multipart([b'E', identity, 'failure_message'])
    return
    
  print repr(request)
  #random failure
  if ord(identity[1]) % 2 == 0:
   pub_socket.send_multipart([b'E', identity, 'failure_message'])
  #deeper into the stack
  elif push_socket is not None:
    push_socket.send_multipart([identity, request[0:len(request)/2 + 1]])
  #done
  else:
    pub_socket.send_multipart([b'S', identity, 'success_message'])

if __name__ == '__main__':

  #basically a thread for IO
  context = zmq.Context.instance()

  #receive requests from clients
  pull_socket = context.socket(zmq.PULL)
  pull_socket.set_hwm(0)
  pull_socket.connect('ipc://requests')

  #get back results from the backend, subscribing to the 'S'uccess and 'E'rror channels
  pub_socket = context.socket(zmq.PUB)
  pub_socket.set_hwm(0)
  pub_socket.connect('ipc://responses')

  #to forward work to the next stage of processing
  push_socket = None #context.socket(zmq.PUSH)
  #push_socket.set_hwm(0)
  #push_socket.bind('ipc://')

  #multiplexing through this guy
  poller = zmq.Poller()
  poller.register(pull_socket, zmq.POLLIN)

  #go until we quit
  while True:
    #wait for activity on one of the sockets
    try:
      sockets = dict(poller.poll())
    except KeyboardInterrupt:
      break

    #look for new requests
    if pull_socket in sockets:
      handle_request(pull_socket, push_socket, pub_socket)
