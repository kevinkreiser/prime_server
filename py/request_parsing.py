from BaseHTTPServer import BaseHTTPRequestHandler
from StringIO import StringIO
from cgi import urlparse
import json
import zmq

class HTTPRequest(BaseHTTPRequestHandler):
  def __init__(self, request_text):
    self.rfile = StringIO(request_text)
    self.raw_requestline = self.rfile.readline()
    self.error_code = self.error_message = None
    self.parse_request()

  def send_error(self, code, message):
    self.error_code = code
    self.error_message = message

#get a request from a client and pass on to the backend to fulfill it
def handle_request(req_rep_socket):
  #get the message details, should be something like ['connection_identity', '']
  message_parts  = req_rep_socket.recv_multipart(zmq.NOBLOCK)
  if len(message_parts) != 2 or len(message_parts[0]) == 0 or len(message_parts[1]) != 0:
    raise Exception('Invalid request')
  #recv the rest of the request
  #TODO: make this more robust to DOS attacks (limit long requests)
  try:
    while message_parts[-1].endswith('\r\n\r\n') == False:
      message_parts.extend(req_rep_socket.recv_multipart(zmq.NOBLOCK))
  except:
    pass

  #mash the bits together after the identity frames
  identity = message_parts[0]
  request_text = ''.join(message_parts[3:])

  #turn the request into json for some echo output
  request = HTTPRequest(request_text)
  split = request.path.split('?')
  query = {}
  if len(split) == 2:
    request.path = split[0]    
    query = urlparse.parse_qs(split[1])
  d = {'error_code': request.error_code, 'method': request.command, 'path': request.path, 'version': request.request_version, 'headers': request.headers.dict, 'query': query}
  response = json.dumps(d, separators=(',',':'))

  #reply with junk for now
  req_rep_socket.send_multipart([identity,
    'HTTP/1.0 200 OK\r\nContent-Type: application/json;charset=utf-8\r\nContent-Length: %s\r\n\r\n%s\r\n' % (len(response), response)], zmq.NOBLOCK)
  #close connection
  req_rep_socket.send_multipart([identity, ''], zmq.NOBLOCK)

if __name__ == '__main__':

  #basically a thread for IO
  context = zmq.Context.instance()

  #receive requests from clients
  req_rep_socket = context.socket(zmq.ROUTER)
  req_rep_socket.router_raw = True
  req_rep_socket.set_hwm(0)
  req_rep_socket.bind('tcp://*:8002')

  #multiplexing through this guy
  poller = zmq.Poller()
  poller.register(req_rep_socket, zmq.POLLIN)

  #go until we quit
  while True:
    #wait for activity on one of the sockets
    try:
      sockets = dict(poller.poll())
    except KeyboardInterrupt:
      break

    #look for new requests
    if req_rep_socket in sockets:
      handle_request(req_rep_socket)
