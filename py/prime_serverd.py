#!/usr/bin/env python

import sys
import threading
from BaseHTTPServer import HTTPServer, BaseHTTPRequestHandler
from SocketServer import ThreadingMixIn
from cgi import urlparse

#enable threaded server
class ThreadedHTTPServer(ThreadingMixIn, HTTPServer):
  pass

#custom handler for getting routes
class PrimeHandler(BaseHTTPRequestHandler):

  def __init__(self, *args):
    BaseHTTPRequestHandler.__init__(self, *args)

  #parse the request because we dont get this for free!
  def handle_request(self):
    #split the query from the path
    try:
      split = urlparse.urlsplit(self.path)
    except:
      raise Exception('Try a url with 2 components: is_prime?possible_prime=SOME_NUMBER')

    if split.path != '/is_prime':
      raise Exception('Try a valid path: is_prime')

    #get a dict and unexplode non-list entries
    try:
      params = urlparse.parse_qs(split.query)
      if len(params['possible_prime']) > 1:
        raise Exception('Only one possible_prime at a time')
      prime = int(params['possible_prime'][0])
    except Exception as e:
      raise e

    #prime computation
    divisor = 2;
    high = prime;
    while divisor < high:
      if prime % divisor == 0:
        break
      high = prime / divisor
      divisor += 1
    if divisor < high:
      prime = 2

    #hand it back
    return str(prime) 

  #send a success
  def succeed(self, response):
    self.send_response(200)

    #set some basic info
    self.send_header('Access-Control-Allow-Origin','*')
    self.send_header('Content-type', 'text/plain;charset=utf-8')
    self.send_header('Content-length', len(response))
    self.end_headers()

    #hand it back
    self.wfile.write(response)

  #send a fail
  def fail(self, error):
    self.send_response(400)

    #set some basic info
    self.send_header('Access-Control-Allow-Origin','*')
    self.send_header('Content-type', 'text/plain;charset=utf-8')
    self.send_header('Content-length', len(error))
    self.end_headers()

    #hand it back
    self.wfile.write(str(error))

  #handle the request
  def do_GET(self):
    #get out the bits we care about
    try:
      response = self.handle_request()
      self.succeed(response)
    except Exception as e:
      self.fail(str(e))

#go off and wait for connections
if __name__ == '__main__':
  #setup the server
  server = ('0.0.0.0', 8002)
  PrimeHandler.protocol_version = 'HTTP/1.0'
  httpd = ThreadedHTTPServer(server, PrimeHandler)

  try:
    httpd.serve_forever()
  except KeyboardInterrupt:
    httpd.server_close()
