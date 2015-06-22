#!/usr/bin/env python

import sys
import threading
from BaseHTTPServer import HTTPServer, BaseHTTPRequestHandler
from SocketServer import ThreadingMixIn
from cgi import urlparse
import json
from StringIO import StringIO

#enable threaded server
class ThreadedHTTPServer(ThreadingMixIn, HTTPServer):
  pass

#custom handler for getting routes
class RequestHandler(BaseHTTPRequestHandler):

  def __init__(self, *args):
    BaseHTTPRequestHandler.__init__(self, *args)

  #parse the request because we dont get this for free!
  def handle_request(self):
    #turn the request into json for some echo output
    split = self.path.split('?')
    query = {}
    if len(split) == 2:
      self.path = split[0]
      query = urlparse.parse_qs(split[1])
    d = {'method': self.command, 'path': self.path, 'version': self.request_version, 'headers': self.headers.dict, 'query': query}
    return json.dumps(d, separators=(',',':'))

  #send a success
  def succeed(self, response):
    self.send_response(200)

    #set some basic info
    self.send_header('Access-Control-Allow-Origin','*')
    self.send_header('Content-type', 'application/json;charset=utf-8')
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
  RequestHandler.protocol_version = 'HTTP/1.0'
  httpd = ThreadedHTTPServer(server, RequestHandler)

  try:
    httpd.serve_forever()
  except KeyboardInterrupt:
    httpd.server_close()
