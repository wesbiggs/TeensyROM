// SPDX-License-Identifier: MIT
//
// TLS spike: an extension host that fetches a list of HTTPS URLs to the SD card.
//
// Inputs are two files on the SD card:
//
//   /tls/urls.txt  one http[s]://host[:port]/path per line; blank lines and lines
//                  starting with '#' are skipped
//   /tls/ca.pem    the PEM root certificate(s) that may sign the servers' chains
//
// Each URL follows up to five redirects, never from https:// to http://, so a
// line can hold a stable link such as a GitHub release download, whose signed target
// expires in minutes. Its body goes to /tls/download.tmp and is renamed to
// /tls/<name> only once all of it has arrived, where <name> is the last segment
// of the line's path, query and fragment removed. A URL whose path ends in '/',
// or whose last segment is no FAT long name or names one of the files above,
// saves as download.bin.
//
// A failed URL is skipped and the rest are still tried. /tls/results.txt gets
// one "$<stage> <value> <ms> <url>" line per URL, value being the bytes saved on
// success. The menu shows `extension host returned ($50/<detail>)`, where detail
// is (stage << 24) | value: the first failure's, or stage 0 with the number of
// files saved. The stages are the Stage enum below.
//
// Build and install:
//
//   node tools/build-firmware.mjs --target tr-plus --host-sketch Source/Teensy/TlsHost --host-tls
//   python3 tools/bench/hostinstall.py build/firmware/TeensyROM+_<ver>_TlsHost.TRH
//
// Known limits of the mbedTLS 2.12.0 that Teensyduino 1.61.0 bundles inside FNET:
// TLS 1.2 only, and MBEDTLS_HAVE_TIME is off, so certificate validity dates are
// not checked. HostTlsConfig.h raises its 10 KiB record limit to TLS's 16 KiB.

#include <SD.h>
#include <EEPROM.h>
#include "Min_TeensyROM.h"
#include "Common/Common_Defs.h"
#include "Common/Menu_Regs.h"
#include "Common/DriveDirLoad.h"
#include "Common/IOHandlers.h"
#include "Common/VMHostABI.h"
#include "Common/VMFail.h"
#include "Common/NativeEthernet/src/NativeEthernet.h"
#include "Common/NativeEthernet/src/NativeDns.h"
#include "TlsUrl.h"
#include "third_party/mbedtls-2.12.0/src/mbedtls/platform.h"
#include "third_party/mbedtls-2.12.0/src/mbedtls/ssl.h"
#include "third_party/mbedtls-2.12.0/src/mbedtls/ctr_drbg.h"
#include "third_party/mbedtls-2.12.0/src/mbedtls/x509_crt.h"
#include "third_party/mbedtls-2.12.0/src/mbedtls/memory_buffer_alloc.h"

uint8_t RAM_Image[RAM_ImageSize];
volatile uint8_t BtnPressed = false;
volatile uint8_t EmulateVicCycles = false;
uint8_t CurrentIOHandler = IOH_None;
StructMenuItem DriveDirMenu;
char DriveDirPath[MaxPathLength];
uint16_t LOROM_Mask, HIROM_Mask;
Stream *CmdChannel = &Serial;

#include "Common/ISRs.c"

__attribute__((used, section(".vmhostid")))
const VmHostId vmHostId = { VM_HOSTID_MAGIC, VM_ABI, 0, 0, "TLS spike", VM_HOST_CODE_FLOOR };

// RAM2 is the module arena, and this host loads no module, so it takes the
// arena at runtime: the host linker script refuses RAM2 globals, and the host
// heap is 16 KiB of DTCM. Everything stays below VM_FAIL_BASE.
static constexpr uint32_t Ram2Base    = 0x20200000u;
static constexpr uint32_t NetHeapSize = 128 * 1024;   // holds RecvWindow of queued segments
static constexpr uint32_t TlsHeapSize = 160 * 1024;
static_assert(Ram2Base + NetHeapSize + TlsHeapSize <= VM_FAIL_BASE, "RAM2 carve-out reaches the fail record");

enum Stage : uint8_t {
   Done = 0, SdInit, UrlFile, UrlParse, CaFile, CaParse, Trng, DrbgSeed, NetInit,
   Connect, TlsSetup, Handshake, Verify, Request, Response, HttpStatus, FileWrite,
   ReadTimeout, Truncated, Rename,
   Redirect,   // value: the status whose Location was missing or refused, or 0 for too many hops
   Dns,        // value: milliseconds the lookup took to fail
};

static constexpr const char *UrlsPath = "/tls/urls.txt";
static constexpr const char *ResultsPath = "/tls/results.txt";
static constexpr size_t MaxUrlsBytes = 16 * 1024;
static constexpr const char *CaPath   = "/tls/ca.pem";
static constexpr const char *TempPath = "/tls/download.tmp";
static constexpr char OutDir[]        = "/tls/";
static constexpr const char *DefaultName = "download.bin";
static constexpr size_t MaxNameLen = 255;   // a FAT long file name
static constexpr uint32_t IoTimeoutMs = 20000;
static constexpr uint32_t ConnectTimeoutMs = 10000;
// TCP keeps at most this much unread data in flight, so it caps throughput at
// RecvWindow per round trip: NativeEthernet's 2 KiB default held a 23 ms path
// to 88 KB/s.
static constexpr fnet_uint32_t RecvWindow = 32 * 1024;
static constexpr size_t LineSize = sizeof(Url::path) + 256;   // the request, or one header line
static constexpr size_t MaxHeadBytes = 32 * 1024;
static constexpr uint8_t MaxRedirects = 5;
FLASHMEM void ReturnToMenu(uint8_t stage, uint32_t value)
{
   VmFail::set(VmFail::HostReturned, ((uint32_t)stage << 24) | (value & 0xFFFFFF));
   EEPROM.write(VM_EEP_BOOTIND_ADDR, VM_BOOT_FROM_MIN);
   delay(10);
   RebootTR();
   while (true) ;
}

// The i.MX RT1062 TRNG, read directly. FNET's own mbedTLS entropy callback is
// fnet_rand(), a PRNG, which is not a seed for key generation.
FLASHMEM static bool TrngInit()
{
   CCM_CCGR6 |= CCM_CCGR6_TRNG(CCM_CCGR_ON);
   TRNG_MCTL = TRNG_MCTL_RST_DEF | TRNG_MCTL_PRGM;
   TRNG_MCTL &= ~TRNG_MCTL_PRGM;
   return !(TRNG_MCTL & TRNG_MCTL_ERR);
}

FLASHMEM static int TrngEntropy(void *, unsigned char *out, size_t len)
{
   while (len)
   {
      uint32_t start = millis();
      while (!(TRNG_MCTL & TRNG_MCTL_ENT_VAL))
      {
         if (TRNG_MCTL & TRNG_MCTL_ERR) return MBEDTLS_ERR_CTR_DRBG_ENTROPY_SOURCE_FAILED;
         if (millis() - start > 1000) return MBEDTLS_ERR_CTR_DRBG_ENTROPY_SOURCE_FAILED;
      }
      // Reading ENT15 starts the next generation, so all 16 words are read in order.
      for (uint8_t i = 0; i < 16; i++)
      {
         uint32_t word = (&TRNG_ENT0)[i];
         for (uint8_t b = 0; b < 4 && len; b++, len--) *out++ = (uint8_t)(word >> (8 * b));
      }
   }
   return 0;
}

// The TLS connection uses an FNET socket directly rather than EthernetClient,
// which sets the receive buffer to its own socket size before it connects, and
// FNET sizes the TCP window from that buffer when the connection opens.
FLASHMEM static fnet_socket_t TcpConnect(IPAddress ip, uint16_t port)
{
   fnet_socket_t sock = fnet_socket(AF_INET, SOCK_STREAM, 0);
   if (!sock) return nullptr;
   const fnet_int32_t noDelay = 1;
   fnet_socket_setopt(sock, SOL_SOCKET, SO_RCVBUF, &RecvWindow, sizeof(RecvWindow));
   fnet_socket_setopt(sock, IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof(noDelay));
   struct fnet_sockaddr_in addr;
   memset(&addr, 0, sizeof(addr));
   addr.sin_family = AF_INET;
   addr.sin_port = FNET_HTONS(port);
   addr.sin_addr.s_addr = (uint32_t)ip;
   if (fnet_socket_connect(sock, (struct fnet_sockaddr *)&addr, sizeof(addr)) != FNET_ERR)
   {
      uint32_t start = millis();
      while (millis() - start < ConnectTimeoutMs)
      {
         fnet_socket_state_t state;
         fnet_size_t size = sizeof(state);
         if (fnet_socket_getopt(sock, SOL_SOCKET, SO_STATE, &state, &size) == FNET_ERR) break;
         if (state == SS_CONNECTED) return sock;
         if (state != SS_CONNECTING) break;
         yield();
      }
   }
   fnet_socket_close(sock);
   return nullptr;
}

FLASHMEM static int NetSend(void *ctx, const unsigned char *buf, size_t len)
{
   uint32_t start = millis();
   while (true)
   {
      fnet_ssize_t sent = fnet_socket_send((fnet_socket_t)ctx, buf, len, 0);
      if (sent == FNET_ERR) return MBEDTLS_ERR_SSL_CONN_EOF;
      if (sent > 0) return (int)sent;
      if (millis() - start > IoTimeoutMs) return MBEDTLS_ERR_SSL_TIMEOUT;
      yield();
   }
}

// mbedTLS's MBEDTLS_ERR_NET_CONN_RESET, from net_sockets.h, which FNET's
// mbedTLS port does not build.
static constexpr int NetReset = -0x0050;

// FNET's recv returns 0 while nothing has arrived and an error once the
// connection has ended and everything it carried has been read. The socket's
// state says how it ended: SS_CLOSING after the peer's FIN, SS_CLOSED after a
// reset or abort. Plain HTTP with no Content-Length has only that to tell a
// complete body from a cut one.
FLASHMEM static int NetRecv(void *ctx, unsigned char *buf, size_t len)
{
   uint32_t start = millis();
   while (true)
   {
      fnet_ssize_t got = fnet_socket_recv((fnet_socket_t)ctx, buf, len, 0);
      if (got == FNET_ERR)
      {
         fnet_socket_state_t state = SS_CLOSED;
         fnet_size_t size = sizeof(state);
         fnet_socket_getopt((fnet_socket_t)ctx, SOL_SOCKET, SO_STATE, &state, &size);
         return state == SS_CLOSING ? 0 : NetReset;   // 0 is EOF; mbedTLS reports it as CONN_EOF
      }
      if (got > 0) return (int)got;
      if (millis() - start > IoTimeoutMs) return MBEDTLS_ERR_SSL_TIMEOUT;
      yield();
   }
}

// Host and path go into the request as they are, so a CR or LF in a Location
// would otherwise add header lines of the server's choosing.
FLASHMEM static bool Printable(const char *text)
{
   for (; *text; text++) if (*text <= ' ' || *text >= 0x7F) return false;
   return true;
}

// A name one of this host's own files in OutDir has: saving over results.txt
// mid-list would wipe the earlier lines and append the later ones to the body.
FLASHMEM static bool NameReserved(const char *name, size_t len)
{
   for (const char *path : { TempPath, UrlsPath, ResultsPath, CaPath })
   {
      const char *own = path + sizeof(OutDir) - 1;
      if (strlen(own) == len && strncasecmp(name, own, len) == 0) return true;
   }
   return false;
}

// The file the body is saved as, named for the last segment of the URL's path.
// That is the name a user asked for: a redirect target, such as a GitHub
// release asset's, ends in an opaque identifier.
FLASHMEM static void OutputPath(const Url &url, char *path)
{
   size_t end = strcspn(url.path, "?#");
   const char *name = url.path + end;
   while (name[-1] != '/') name--;
   size_t len = url.path + end - name;
   bool usable = len > 0 && len <= MaxNameLen && strcspn(name, "\\:*\"<>|") >= len && strspn(name, ".") < len &&
      !NameReserved(name, len);
   strcpy(path, OutDir);
   if (usable) strncat(path, name, len);
   else strcat(path, DefaultName);
}

FLASHMEM static bool ParseUrl(const char *text, Url &url)
{
   const char *host;
   if (strncmp(text, "https://", 8) == 0) { url.tls = true; url.port = 443; host = text + 8; }
   else if (strncmp(text, "http://", 7) == 0) { url.tls = false; url.port = 80; host = text + 7; }
   else return false;
   const char *hostEnd = host + strcspn(host, ":/");
   size_t hostLen = hostEnd - host;
   if (hostLen == 0 || hostLen >= sizeof(url.host)) return false;
   memcpy(url.host, host, hostLen);
   url.host[hostLen] = 0;
   const char *rest = hostEnd;
   if (*rest == ':')
   {
      char *end;
      unsigned long port = strtoul(rest + 1, &end, 10);
      if (end == rest + 1 || port == 0 || port > 65535) return false;
      url.port = (uint16_t)port;
      rest = end;
   }
   if (*rest == 0) rest = "/";
   if (*rest != '/' || strlen(rest) >= sizeof(url.path)) return false;
   strcpy(url.path, rest);
   return Printable(url.host) && Printable(url.path);
}

// Reads a whole SD file into the TLS heap. mbedtls_x509_crt_parse wants a PEM
// buffer that includes its terminating NUL in the length.
FLASHMEM static unsigned char *ReadFile(const char *path, size_t &len, size_t limit)
{
   File f = SD.open(path, FILE_READ);
   if (!f) return nullptr;
   size_t size = f.size();
   if (size == 0 || size > limit) { f.close(); return nullptr; }
   unsigned char *buf = (unsigned char *)mbedtls_calloc(1, size + 1);
   if (!buf) { f.close(); return nullptr; }
   len = f.read(buf, size);
   f.close();
   if (len != size) { mbedtls_free(buf); return nullptr; }
   buf[len++] = 0;
   return buf;
}

FLASHMEM static bool EthernetUp()
{
   uint8_t mac[6];
   EEPreadNBuf(eepAdMyMAC, mac, 6);
   uint16_t timeout;
   EEPROM.get(eepAdDHCPTimeout, timeout);
   Ethernet.setStackHeap((uint8_t *)Ram2Base, NetHeapSize);
   // NativeEthernet's own sockets go unused (DNS runs on FNET's resolver), but
   // begin() allocates their buffers on the 16 KiB host heap regardless.
   Ethernet.setSocketNum(1);
   Ethernet.setSocketSize(512);
   if (EEPROM.read(eepAdDHCPEnabled))
   {
      uint16_t respTimeout;
      EEPROM.get(eepAdDHCPRespTO, respTimeout);
      return Ethernet.begin(mac, timeout, respTimeout) != 0;
   }
   uint32_t ip, dns, gateway, mask;
   EEPROM.get(eepAdMyIP, ip);
   EEPROM.get(eepAdDNSIP, dns);
   EEPROM.get(eepAdGtwyIP, gateway);
   EEPROM.get(eepAdMaskIP, mask);
   return Ethernet.begin(mac, ip, dns, gateway, mask, timeout) != 0;
}

// The status code from an HTTP/1.x status line, or 0 when it is not one.
FLASHMEM static int ParseStatus(const char *line)
{
   if (strncmp(line, "HTTP/1.", 7) != 0 || line[8] != ' ') return 0;
   char *end;
   long status = strtol(line + 9, &end, 10);
   return (end == line + 12 && status >= 100) ? (int)status : 0;
}

FLASHMEM static bool IsRedirect(int status)
{
   return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

// A Location value, absolute or host-relative, applied to `url` only when it
// parses. An https:// URL's redirect to http:// fails here, so a download that
// started on TLS never leaves it.
FLASHMEM static bool ResolveLocation(const char *value, Url &url)
{
   value += strspn(value, " \t");
   if (value[0] == '/' && value[1] != '/')
   {
      if (strlen(value) >= sizeof(url.path) || !Printable(value)) return false;
      strcpy(url.path, value);
      return true;
   }
   static Url next;
   if (!ParseUrl(value, next) || (url.tls && !next.tls)) return false;
   url = next;
   return true;
}

// What one connection holds, released however the exchange ends: a failed
// download leaves no temp file, socket or TLS buffers behind for the next one.
struct Connection
{
   bool tls = false;
   fnet_socket_t sock = nullptr;
   mbedtls_ssl_context ssl;
   File out;
   Connection() { mbedtls_ssl_init(&ssl); }
   ~Connection();
   int read(unsigned char *buf, size_t len);
   int write(const unsigned char *buf, size_t len);
};

FLASHMEM Connection::~Connection()
{
   if (out) { out.close(); SD.remove(TempPath); }
   mbedtls_ssl_free(&ssl);
   if (sock) fnet_socket_close(sock);
}

FLASHMEM int Connection::read(unsigned char *buf, size_t len)
{
   return tls ? mbedtls_ssl_read(&ssl, buf, len) : NetRecv(sock, buf, len);
}

FLASHMEM int Connection::write(const unsigned char *buf, size_t len)
{
   return tls ? mbedtls_ssl_write(&ssl, buf, len) : NetSend(sock, buf, len);
}

#define FAIL(s, v) do { result = Result{ (s), (uint32_t)(v) }; return Failed; } while (0)

// One GET on a fresh connection. Fetched leaves the body in TempPath with its
// size in `bodyBytes`; Redirected leaves `url` at the redirect's target.
// `line` (LineSize bytes) holds the request and then each header line in turn.
FLASHMEM static Outcome Exchange(Url &url, mbedtls_ssl_config &conf, char *line, uint32_t &bodyBytes, Result &result)
{
   Connection c;
   IPAddress remote;
   DNSClient dns;
   dns.begin(Ethernet.dnsServerIP());
   // Dns's and Connect's values are the milliseconds they took, which tells a
   // refusal from a timeout.
   uint32_t start = millis();
   if (!dns.getHostByName(url.host, remote)) FAIL(Dns, millis() - start);
   start = millis();
   c.sock = TcpConnect(remote, url.port);
   if (!c.sock) FAIL(Connect, millis() - start);

   int ret;
   c.tls = url.tls;
   if (c.tls)
   {
      ret = mbedtls_ssl_setup(&c.ssl, &conf);
      if (ret == 0) ret = mbedtls_ssl_set_hostname(&c.ssl, url.host);
      if (ret != 0) FAIL(TlsSetup, -ret);
      mbedtls_ssl_set_bio(&c.ssl, c.sock, NetSend, NetRecv, nullptr);
      while ((ret = mbedtls_ssl_handshake(&c.ssl)) != 0)
      {
         if (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) FAIL(Verify, mbedtls_ssl_get_verify_result(&c.ssl));
         if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) FAIL(Handshake, -ret);
      }
   }

   // A port other than the scheme's own goes in Host, which a virtual host
   // on a non-standard port may need to find the site.
   char port[8] = "";
   if (url.port != (url.tls ? 443 : 80)) snprintf(port, sizeof(port), ":%u", url.port);
   int reqLen = snprintf(line, LineSize,
      "GET %s HTTP/1.0\r\nHost: %s%s\r\nUser-Agent: TeensyROM\r\nConnection: close\r\n\r\n", url.path, url.host, port);
   if (reqLen <= 0 || reqLen >= (int)LineSize) FAIL(Request, 0);
   for (int off = 0; off < reqLen; )
   {
      ret = c.write((const unsigned char *)line + off, reqLen - off);
      if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
      if (ret <= 0) FAIL(Request, -ret);
      off += ret;
   }

   // Headers are taken a line at a time, keeping only the status, Content-Length
   // and a redirect's Location: github.com's redirect alone sends 5.7 KB of them.
   // A line longer than `line` keeps its start, enough for every header but
   // Location, which is then refused rather than followed cut short.
   int status = 0;
   int32_t contentLength = -1;
   bool location = false, overflow = false, inBody = false, closeNotify = false, eof = false;
   size_t lineLen = 0, headBytes = 0;
   bodyBytes = 0;
   unsigned char buf[4096];
   while (true)
   {
      ret = c.read(buf, sizeof(buf));
      if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
      if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) { closeNotify = true; break; }
      if (ret == 0 || ret == MBEDTLS_ERR_SSL_CONN_EOF) { eof = true; break; }
      // A reset after the last byte still leaves a Content-Length body complete.
      if (ret == NetReset) break;
      if (ret == MBEDTLS_ERR_SSL_TIMEOUT) FAIL(ReadTimeout, bodyBytes);
      if (ret < 0) FAIL(Response, -ret);

      size_t at = 0;
      while (!inBody && at < (size_t)ret)
      {
         char ch = buf[at++];
         if (++headBytes > MaxHeadBytes) FAIL(Response, headBytes);
         if (ch != '\n')
         {
            if (lineLen < LineSize - 1) line[lineLen++] = ch; else overflow = true;
            continue;
         }
         while (lineLen && strchr("\r \t", line[lineLen - 1])) lineLen--;
         line[lineLen] = 0;
         if (status == 0)
         {
            status = ParseStatus(line);
            if (status == 0) FAIL(Response, 0);
         }
         else if (lineLen == 0) inBody = true;
         else if (strncasecmp(line, "Content-Length:", 15) == 0) contentLength = strtol(line + 15, nullptr, 10);
         else if (IsRedirect(status) && strncasecmp(line, "Location:", 9) == 0) location = !overflow && ResolveLocation(line + 9, url);
         lineLen = 0;
         overflow = false;
         if (!inBody) continue;

         if (IsRedirect(status))
         {
            if (!location) FAIL(Redirect, status);
            if (c.tls) mbedtls_ssl_close_notify(&c.ssl);
            return Redirected;
         }
         if (status != 200) FAIL(HttpStatus, status);
         SD.remove(TempPath);
         c.out = SD.open(TempPath, FILE_WRITE);
         if (!c.out) FAIL(FileWrite, 0);
      }
      size_t bodyLen = ret - at;
      if (bodyLen && c.out.write(buf + at, bodyLen) != bodyLen) FAIL(FileWrite, bodyBytes);
      bodyBytes += bodyLen;
   }

   // With no Content-Length, a TLS body is complete only on close_notify: an
   // attacker can forge a TCP close, but not the alert. Plain HTTP has nothing
   // better than the peer's FIN, which NetRecv tells from a reset.
   bool complete = inBody && (contentLength >= 0 ? bodyBytes == (uint32_t)contentLength : (c.tls ? closeNotify : eof));
   if (!complete) FAIL(Truncated, bodyBytes);
   c.out.close();
   return Fetched;
}

// One URL from urls.txt, through its redirects, to its file on the card.
// Success's value is the bytes saved.
FLASHMEM static Result Download(const char *text, mbedtls_ssl_config &conf, char *line)
{
   static Url url;
   if (!ParseUrl(text, url)) return Result{ UrlParse, 0 };
   static char outPath[sizeof(OutDir) + MaxNameLen];
   OutputPath(url, outPath);

   Result result{ Done, 0 };
   uint32_t bodyBytes = 0;
   for (uint8_t hops = 0; ; hops++)
   {
      Outcome outcome = Exchange(url, conf, line, bodyBytes, result);
      if (outcome == Failed) return result;
      if (outcome == Fetched) break;
      if (hops == MaxRedirects) return Result{ Redirect, 0 };
   }
   SD.remove(outPath);
   if (!SD.rename(TempPath, outPath)) { SD.remove(TempPath); return Result{ Rename, bodyBytes }; }
   return Result{ Done, bodyBytes };
}

// One "$<stage> <value> <ms> <url>" line per URL, so a list's outcome is on
// the card even though the menu shows only one result.
FLASHMEM static void LogResult(const Result &r, uint32_t ms, const char *url)
{
   File f = SD.open(ResultsPath, FILE_WRITE);
   if (!f) return;
   char head[40];
   snprintf(head, sizeof(head), "$%02X %lu %lu ", r.stage, (unsigned long)r.value, (unsigned long)ms);
   f.write(head, strlen(head));
   f.write(url, strlen(url));
   f.write("\n", 1);
   f.close();
}

FLASHMEM static void Fetch()
{
   if (!SD.begin(BUILTIN_SDCARD)) ReturnToMenu(SdInit, 0);
   SD.remove(ResultsPath);

   mbedtls_memory_buffer_alloc_init((unsigned char *)(Ram2Base + NetHeapSize), TlsHeapSize);
   char *line = (char *)mbedtls_calloc(1, LineSize);
   if (!line) ReturnToMenu(TlsSetup, 0);

   size_t urlsLen = 0;
   char *urls = (char *)ReadFile(UrlsPath, urlsLen, MaxUrlsBytes);
   if (!urls) ReturnToMenu(UrlFile, 0);

   mbedtls_x509_crt ca;
   mbedtls_x509_crt_init(&ca);
   size_t caLen = 0;
   unsigned char *caPem = ReadFile(CaPath, caLen, 64 * 1024);
   if (!caPem) ReturnToMenu(CaFile, 0);
   int ret = mbedtls_x509_crt_parse(&ca, caPem, caLen);
   mbedtls_free(caPem);
   // A positive ret is the number of certificates in the file that did not parse.
   if (ret != 0) ReturnToMenu(CaParse, ret < 0 ? -ret : ret);

   if (!TrngInit()) ReturnToMenu(Trng, TRNG_MCTL);
   mbedtls_ctr_drbg_context drbg;
   mbedtls_ctr_drbg_init(&drbg);
   ret = mbedtls_ctr_drbg_seed(&drbg, TrngEntropy, nullptr, (const unsigned char *)"TeensyROM", 9);
   if (ret != 0) ReturnToMenu(DrbgSeed, -ret);

   if (!EthernetUp()) ReturnToMenu(NetInit, Ethernet.linkStatus());

   mbedtls_ssl_config conf;
   mbedtls_ssl_config_init(&conf);
   ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
   if (ret != 0) ReturnToMenu(TlsSetup, -ret);
   mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
   mbedtls_ssl_conf_ca_chain(&conf, &ca, nullptr);
   mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);

   // Every URL is tried; the menu shows the first failure, or the count saved.
   uint32_t saved = 0;
   Result first{ Done, 0 };
   for (char *next = urls; *next; )
   {
      char *text = next;
      size_t len = strcspn(text, "\n");
      next = text + len + (text[len] == '\n');
      text[len] = 0;
      while (len && strchr("\r \t", text[len - 1])) text[--len] = 0;
      text += strspn(text, " \t");
      if (*text == 0 || *text == '#') continue;

      uint32_t start = millis();
      Result r = Download(text, conf, line);
      LogResult(r, millis() - start, text);
      if (r.stage == Done) saved++;
      else if (first.stage == Done) first = r;
   }
   if (first.stage != Done) ReturnToMenu(first.stage, first.value);
   ReturnToMenu(Done, saved);
}

void setup()
{
   SetLEDOn;
   for (uint8_t PinNum = 0; PinNum < sizeof(OutputPins); PinNum++) pinMode(OutputPins[PinNum], OUTPUT);
   SetDMADeassert;
   SetIRQDeassert;
   SetNMIDeassert;
   SetResetAssert;
   pinMode(Menu_Btn_In_PIN, INPUT_PULLUP);

   if (ReadButton == 0) runMainTRApp();
   uint32_t magic = 0;
   EEPROM.get(VM_EEP_MAGIC_ADDR, magic);
   if (magic != VM_EEP_MAGIC) runMainTRApp();
   char marker[5]{};
   for (uint8_t i = 0; i < 4; i++) marker[i] = EEPROM.read(VM_EEP_BOOTNAME_ADDR + i);
   if (strcmp(marker, VM_HOST_MARKER) != 0) runMainTRApp();

   Fetch();
}

void loop()
{
}

void EEPwriteNBuf(uint16_t addr, const uint8_t* buf, uint8_t len)
{
   while (len--) EEPROM.write(addr+len, buf[len]);
}

void EEPwriteStr(uint16_t addr, const char* buf)
{
   EEPwriteNBuf(addr, (uint8_t*)buf, strlen(buf)+1);
}

void EEPreadNBuf(uint16_t addr, uint8_t* buf, uint16_t len)
{
   while (len--) buf[len] = EEPROM.read(addr+len);
}

void EEPreadStr(uint16_t addr, char* buf)
{
   uint16_t CharNum = 0;
   do
   {
      buf[CharNum] = EEPROM.read(addr+CharNum);
   } while (buf[CharNum++] !=0);
}
