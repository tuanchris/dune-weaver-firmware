/*
  WebServer.h - Dead simple web-server.
  Supports only one simultaneous client, knows how to handle GET and POST.

  Copyright (c) 2014 Ivan Grokhotkov. All rights reserved.

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
  Modified 8 May 2015 by Hristo Gochkov (proper post and file upload handling)
*/


#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <functional>
#include <memory>
#include <WiFi.h>
#include "HTTP_Method.h"
#include "Uri.h"

enum HTTPUploadStatus { UPLOAD_FILE_START, UPLOAD_FILE_WRITE, UPLOAD_FILE_END,
                        UPLOAD_FILE_ABORTED };
enum HTTPRawStatus { RAW_START, RAW_WRITE, RAW_END, RAW_ABORTED };
enum HTTPClientStatus { HC_NONE, HC_WAIT_READ, HC_WAIT_CLOSE };
enum HTTPAuthMethod { BASIC_AUTH, DIGEST_AUTH };

#define HTTP_DOWNLOAD_UNIT_SIZE 1436

#ifndef HTTP_UPLOAD_BUFLEN
#define HTTP_UPLOAD_BUFLEN 1436
#endif

#ifndef HTTP_RAW_BUFLEN
#define HTTP_RAW_BUFLEN 1436
#endif

// DW fork: the server handles ONE client at a time, so these waits are
// head-of-line blocking — a client that connects and never sends (an aborted
// app request, a phone's speculative connection) parks the whole server for
// the full wait while new connections rot in the accept backlog. Under a
// request storm during a WiFi scan that serializes into minutes of outage
// (see handleClient). A LAN client that hasn't sent its request line within
// 1 s is gone; don't wait 5. POST/SEND waits stay at 5 s — mid-body stalls on
// slow uploads are real and give up work already in progress.
#define HTTP_MAX_DATA_WAIT 1000 //ms to wait for the client to send the request
#define HTTP_MAX_POST_WAIT 5000 //ms to wait for POST data to arrive
#define HTTP_MAX_SEND_WAIT 5000 //ms to wait for data chunk to be ACKed
#define HTTP_MAX_CLOSE_WAIT 250 //ms to wait for the client to close the connection

#define CONTENT_LENGTH_UNKNOWN ((size_t) -1)
#define CONTENT_LENGTH_NOT_SET ((size_t) -2)

class WebServer;

typedef struct {
  HTTPUploadStatus status;
  String  filename;
  String  name;
  String  type;
  size_t  totalSize;    // file size
  size_t  currentSize;  // size of data currently in buf
  uint8_t buf[HTTP_UPLOAD_BUFLEN];
} HTTPUpload;

typedef struct
{
  HTTPRawStatus status;
  size_t  totalSize;   // content size
  size_t  currentSize; // size of data currently in buf
  uint8_t buf[HTTP_UPLOAD_BUFLEN];
  void    *data;       // additional data
} HTTPRaw;

#include "detail/RequestHandler.h"

namespace fs {
class FS;
}

class WebServer
{
public:
  WebServer(IPAddress addr, int port = 80);
  WebServer(int port = 80);
  virtual ~WebServer();

  virtual void begin();
  virtual void begin(uint16_t port);
  virtual void handleClient();

  virtual void close();
  void stop();

  bool authenticate(const char * username, const char * password);
  void requestAuthentication(HTTPAuthMethod mode = BASIC_AUTH, const char* realm = NULL, const String& authFailMsg = String("") );

  typedef std::function<void(void)> THandlerFunction;
  void on(const Uri &uri, THandlerFunction fn);
  void on(const Uri &uri, HTTPMethod method, THandlerFunction fn); 
  void on(const Uri &uri, HTTPMethod method, THandlerFunction fn, THandlerFunction ufn); //ufn handles file uploads
  void addHandler(RequestHandler* handler);
  void serveStatic(const char* uri, fs::FS& fs, const char* path, const char* cache_header = NULL );
  void onNotFound(THandlerFunction fn);  //called when handler is not assigned
  void onFileUpload(THandlerFunction ufn); //handle file uploads

  String uri() { return _currentUri; }
  HTTPMethod method() { return _currentMethod; }
  virtual WiFiClient client() { return _currentClient; }

  // DW fork: liveness signals for the self-heal watchdog (Web_Server::poll()).
  // lastHandledMillis() is when a request last completed _handleRequest();
  // hasPendingClient() is whether a connection is waiting in the accept queue.
  unsigned long lastHandledMillis() const { return _lastHandledMs; }
  bool hasPendingClient() { return _server.hasClient(); }
  void restartListener(); // flush a rotted accept queue; handlers survive

  // DW fork: low-heap guard. When free heap is below floorBytes, requests are
  // answered 503 before their handler runs (a handler whose allocations fail
  // mid-flight stalls the single-threaded server and queued clients pin even
  // more memory). exempt(uri) returning true bypasses the guard for cheap
  // must-work routes (status poll, stop). floorBytes 0 disables (default).
  void setLowHeapGuard(uint32_t floorBytes, bool (*exempt)(const String& uri)) {
    _lowHeapFloor = floorBytes;
    _lowHeapExempt = exempt;
  }

  // DW fork: hard heap floor. Below it, a newly accepted connection is
  // RST-closed before its request is even read.  The soft guard above still
  // has to accept + parse a request to answer its 503, and each such client
  // pins ~2 KB of lwIP buffers while it waits — under a genuine heap crater
  // the shedding itself deepens the hole.  Applies to every route (pre-parse,
  // so there is no URI to exempt on); clients retry with backoff.  0 disables
  // (default).
  void setHardHeapFloor(uint32_t floorBytes) { _heapHardFloor = floorBytes; }

  // DW fork: per-request trace hook, called at the top of _handleRequest()
  // (before the low-heap guard, so shed requests are traced too).  Diagnosis
  // aid for heap-drain hunts: the FluidNC side logs uri + heap per request.
  void onRequestTrace(void (*fn)(const char* uri)) { _onRequestTrace = fn; }
  HTTPUpload& upload() { return *_currentUpload; }
  HTTPRaw& raw() { return *_currentRaw; }

  String pathArg(unsigned int i); // get request path argument by number
  String arg(String name);        // get request argument value by name
  String arg(int i);              // get request argument value by number
  String argName(int i);          // get request argument name by number
  int args();                     // get arguments count
  bool hasArg(String name);       // check if argument exists
  void collectHeaders(const char* headerKeys[], const size_t headerKeysCount); // set the request headers to collect
  String header(String name);     // get request header value by name
  String header(int i);           // get request header value by number
  String headerName(int i);       // get request header name by number
  int headers();                  // get header count
  bool hasHeader(String name);    // check if header exists

  int clientContentLength() { return _clientContentLength; }      // return "content-length" of incoming HTTP header from "_currentClient"

  String hostHeader();            // get request host header if available or empty String if not

  // send response to the client
  // code - HTTP response code, can be 200 or 404
  // content_type - HTTP content type, like "text/plain" or "image/png"
  // content - actual content body
  void send(int code, const char* content_type = NULL, const String& content = String(""));
  void send(int code, char* content_type, const String& content);
  void send(int code, const String& content_type, const String& content);
  void send(int code, const char* content_type, const char* content);

  void send_P(int code, PGM_P content_type, PGM_P content);
  void send_P(int code, PGM_P content_type, PGM_P content, size_t contentLength);

  void enableDelay(boolean value);
  void enableCORS(boolean value = true);
  void enableCrossOrigin(boolean value = true);

  void setContentLength(const size_t contentLength);
  void sendHeader(const String& name, const String& value, bool first = false);
  void sendContent(const String& content);
  void sendContent(const char* content, size_t contentLength);
  void sendContent_P(PGM_P content);
  void sendContent_P(PGM_P content, size_t size);

  static String urlDecode(const String& text);

  template<typename T>
  size_t streamFile(T &file, const String& contentType, const int code = 200) {
    _streamFileCore(file.size(), file.name(), contentType, code);
    return _currentClient.write(file);
  }

protected:
  // DW fork: abort the client on a short/failed write.  WiFiClient::write
  // already blocks up to HTTP_MAX_SEND_WAIT waiting for the reader; a short
  // return means the client stalled or vanished mid-response (e.g. the app
  // cancelling a preview download).  Without this, EVERY remaining chunk of
  // the response burns another full send-wait on the same dead socket -- a
  // 50 KB chunked response = minutes of blockage in the single-threaded
  // poller (observed: 30 s deaf spells via the stall watchdog, and a 120 s
  // task-WDT panic).  RST (not FIN) is correct mid-response: the response is
  // already broken and linger-0 skips TIME_WAIT.  After stop(), the write
  // calls of the rest of the response return immediately.
  virtual size_t _currentClientWrite(const char* b, size_t l) {
    size_t w = _currentClient.write( b, l );
    if (w != l) { _abortDeadClient(); }
    return w;
  }
  virtual size_t _currentClientWrite_P(PGM_P b, size_t l) {
    size_t w = _currentClient.write_P( b, l );
    if (w != l) { _abortDeadClient(); }
    return w;
  }
  void _addRequestHandler(RequestHandler* handler);
  void _handleRequest();
  void _lingerAbort(); // DW fork: RST-close the current client (abort paths only)
  void _abortDeadClient(); // DW fork: RST + stop a client that stalled mid-response
  void _finalizeResponse();
  bool _parseRequest(WiFiClient& client);
  void _parseArguments(String data);
  static String _responseCodeToString(int code);
  bool _parseForm(WiFiClient& client, String boundary, uint32_t len);
  bool _parseFormUploadAborted();
  void _uploadWriteByte(uint8_t b);
  int _uploadReadByte(WiFiClient& client);
  void _prepareHeader(String& response, int code, const char* content_type, size_t contentLength);
  bool _collectHeader(const char* headerName, const char* headerValue);

  void _streamFileCore(const size_t fileSize, const String & fileName, const String & contentType, const int code = 200);

  String _getRandomHexString();
  // for extracting Auth parameters
  String _extractParam(String& authReq,const String& param,const char delimit = '"');

  struct RequestArgument {
    String key;
    String value;
  };

  boolean     _corsEnabled;
  WiFiServer  _server;

  WiFiClient  _currentClient;
  HTTPMethod  _currentMethod;
  String      _currentUri;
  uint8_t     _currentVersion;
  HTTPClientStatus _currentStatus;
  unsigned long _statusChange;
  unsigned long _lastHandledMs; // DW fork: see lastHandledMillis()
  uint32_t    _lowHeapFloor = 0;                            // DW fork: see setLowHeapGuard()
  bool        (*_lowHeapExempt)(const String& uri) = nullptr; // DW fork
  uint32_t    _heapHardFloor = 0;                           // DW fork: see setHardHeapFloor()
  void        (*_onRequestTrace)(const char* uri) = nullptr;  // DW fork: see onRequestTrace()
  boolean     _nullDelay;

  RequestHandler*  _currentHandler;
  RequestHandler*  _firstHandler;
  RequestHandler*  _lastHandler;
  THandlerFunction _notFoundHandler;
  THandlerFunction _fileUploadHandler;

  int              _currentArgCount;
  RequestArgument* _currentArgs;
  int              _postArgsLen;
  RequestArgument* _postArgs;

  std::unique_ptr<HTTPUpload> _currentUpload;
  std::unique_ptr<HTTPRaw>    _currentRaw;

  int              _headerKeysCount;
  RequestArgument* _currentHeaders;
  size_t           _contentLength;
  int              _clientContentLength;	// "Content-Length" from header of incoming POST or GET request
  String           _responseHeaders;

  String           _hostHeader;
  bool             _chunked;

  String           _snonce;  // Store noance and opaque for future comparison
  String           _sopaque;
  String           _srealm;  // Store the Auth realm between Calls

};


#endif //ESP8266WEBSERVER_H
