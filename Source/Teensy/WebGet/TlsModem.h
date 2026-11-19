// SPDX-License-Identifier: MIT
//
// The modem: picking a .tls file runs this host as a SwiftLink, so C64
// programs can reach the network through it, TLS included.
//
// A .tls file is a .prg renamed. The host starts the C64 through BootStub
// below, which turns the cartridge off and runs the KERNAL's reset, so a C128
// comes up in C64 mode. Once BASIC is at READY it DMAs the program in, and for
// one that loads at $0801 types RUN. An empty .tls loads nothing: BASIC with
// the modem, for a program loaded from a real drive.
//
// The SwiftLink is the main image's own, IOH_Swiftlink.c and its Swift_*.c
// included below unchanged, with the AT commands they answer. What they take
// from the main image this file supplies, `client` among it: a ModemClient,
// which answers EthernetClient's calls on a plain FNET socket or an mbedTLS
// session over one. Their hooks add the dial strings that use TLS:
//
//   ATDT https://<host>[:<port>]/<path>   one GET, read-only: CONNECT, the body,
//   ATDT http://...                       then NO CARRIER. Redirects followed as
//                                         WebGet follows them; a body that ends
//                                         short ends "*cut short*" first.
//   ATDT tls://<host>[:<port>]            a TLS session both ways (port 443
//                                         unless given), for a BBS that speaks it
//
// ATDT <host>:<port> is the stock plain connection. ATBROWSE answers ERROR:
// this host is no web browser, and the browser was written for the main
// image's 320 KiB receive queue, which RAM2 cannot hold beside TLS. This
// queue is 32 KiB, and ModemClient::available() gives it flow control.

static bool ModemMode;

// ---- What the SwiftLink files take from the main image ----------------------

uint8_t nfcState;   // the main image pauses NFC through it; this host has none
#define nfcStateBitPaused 2

#include "ModemText.h"

#define ToPETSCII(x) HostToPETSCII(x)

FLASHMEM void FreeDriveDirMenu() {}   // this host lists no directory
inline bool IsDotfile(const char *Name) { return Name[0] == '.'; }   // DriveDirLoad.h's, which is full-build only
bool EthernetInit(void (*MsgOut)(const char *));   // Swift_ATcommands.c calls it before IOH_Swiftlink.c defines it
FLASHMEM void SetRandomSeed() { randomSeed(ARM_DWT_CYCCNT); }
FLASHMEM uint32_t RAM2BytesFree() { return 0; }   // read only by debug prints

// The browser's, which ATBROWSE's refusal leaves unreachable.
FLASHMEM void RemoteLaunch(RegMenuTypes, const char *, bool) {}
class NoDrive : public FS
{
public:
   File open(const char *, uint8_t) override { return File(); }
   bool exists(const char *) override { return false; }
   bool mkdir(const char *) override { return false; }
   bool rename(const char *, const char *) override { return false; }
   bool remove(const char *) override { return false; }
   bool rmdir(const char *) override { return false; }
   uint64_t usedSize() override { return 0; }
   uint64_t totalSize() override { return 0; }
};
static NoDrive firstPartition;

class ModemClient
{
public:
   bool begin();   // the buffers, on the heap: DTCM has no room for them
   // As EthernetClient answers them, for the SwiftLink files.
   int connect(const char *host, uint16_t port);
   int connect(IPAddress ip, uint16_t port);
   uint8_t connected();
   int available();
   int read();
   int read(uint8_t *buf, size_t size);
   size_t write(const uint8_t *buf, size_t size);
   int availableForWrite();
   void stop();
   void flush() {}
   int printf(const char *format, ...);

   // This host's: `url` fetched (fetch) or opened both ways.
   Result dial(Url &url, bool fetch);

private:
   enum class Kind : uint8_t { Idle, Stream, Fetch };
   Kind kind = Kind::Idle;
   bool tls = false, ended = false;
   fnet_socket_t sock = nullptr;
   int32_t remaining = -1;   // a fetch's body bytes still to come, -1 with no Content-Length
   uint32_t lastData = 0;
   uint16_t inPos = 0, inLen = 0;
   static constexpr uint16_t InSize = 1024;
   struct Buffers { mbedtls_ssl_context ssl; uint8_t in[InSize]; } *b = nullptr;

   bool open(IPAddress ip, uint16_t port);
   void fill();
   void finish(bool cut);
};
ModemClient client;

// The SwiftLink files' buffers go on mbedTLS's heap: this host's own is 16 KiB
// of DTCM.
#define malloc(size) mbedtls_calloc(1, (size))
#define free(ptr) mbedtls_free(ptr)
#define RxQueueNumBlocks 4
#define SwiftHostATDecls ATRespCode AT_HostDial(char *); ATRespCode AT_HostBrowse(char *);
#define SwiftHostATCommands "dt", &AT_HostDial, "browse", &AT_HostBrowse,
#define SwiftHostATHelp \
   AddToPETSCIIStrToRxQueueLN(" ATDT https://<URL>  Fetch, read-only"); \
   AddToPETSCIIStrToRxQueueLN(" ATDT tls://<Host>:<Port>  TLS connect");
#include "Common/IO_Handlers/IOH_Swiftlink.c"
#undef malloc
#undef free

// ---- The TLS setup, made on the first TLS dial ------------------------------

// On the heap, for DTCM's sake.
struct ModemTls
{
   mbedtls_x509_crt ca;
   mbedtls_ctr_drbg_context drbg;
   mbedtls_ssl_config conf;
};
static ModemTls *Tls;

FLASHMEM static void FreeTls(ModemTls *t)
{
   mbedtls_ssl_config_free(&t->conf);
   mbedtls_ctr_drbg_free(&t->drbg);
   mbedtls_x509_crt_free(&t->ca);
   mbedtls_free(t);
}

// A failure keeps nothing, so a corrected ca.pem is read on the next dial.
FLASHMEM static Result ModemTlsSetup()
{
   if (Tls) return Result{ Done, 0 };
   ModemTls *t = (ModemTls *)mbedtls_calloc(1, sizeof(ModemTls));
   if (!t) return Result{ TlsSetup, 0 };
   mbedtls_x509_crt_init(&t->ca);
   mbedtls_ctr_drbg_init(&t->drbg);
   mbedtls_ssl_config_init(&t->conf);

   char caPath[sizeof(LaunchRecord.root) + sizeof("/ca.pem") - 1];
   snprintf(caPath, sizeof(caPath), "%s/ca.pem", LaunchRecord.root);
   size_t caLen = 0;
   Result r{ Done, 0 };
   int ret = 0;
   unsigned char *caPem = ReadFile(caPath, caLen, 64 * 1024);
   if (!caPem) r = Result{ CaFile, 0 };
   else
   {
      ret = mbedtls_x509_crt_parse(&t->ca, caPem, caLen);
      mbedtls_free(caPem);
      // A positive ret is the number of certificates in the file that did not parse.
      if (ret != 0) r = Result{ CaParse, (uint32_t)(ret < 0 ? -ret : ret) };
   }
   if (r.stage == Done && !TrngInit()) r = Result{ Trng, TRNG_MCTL };
   if (r.stage == Done && (ret = mbedtls_ctr_drbg_seed(&t->drbg, TrngEntropy, nullptr, (const unsigned char *)"TeensyROM", 9)) != 0)
      r = Result{ DrbgSeed, (uint32_t)-ret };
   if (r.stage == Done && (ret = mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                                             MBEDTLS_SSL_PRESET_DEFAULT)) != 0)
      r = Result{ TlsSetup, (uint32_t)-ret };
   if (r.stage != Done) { FreeTls(t); return r; }
   mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
   mbedtls_ssl_conf_ca_chain(&t->conf, &t->ca, nullptr);
   mbedtls_ssl_conf_rng(&t->conf, mbedtls_ctr_drbg_random, &t->drbg);
   Tls = t;
   return r;
}

// NetRecv without the wait: nothing yet is WANT_READ, so a session's reads
// return to the SwiftLink's poll.
FLASHMEM static int RecvNow(void *ctx, unsigned char *buf, size_t len)
{
   fnet_ssize_t got = fnet_socket_recv((fnet_socket_t)ctx, buf, len, 0);
   if (got > 0) return (int)got;
   if (got == 0) return MBEDTLS_ERR_SSL_WANT_READ;
   fnet_socket_state_t state = SS_CLOSED;
   fnet_size_t size = sizeof(state);
   fnet_socket_getopt((fnet_socket_t)ctx, SOL_SOCKET, SO_STATE, &state, &size);
   return state == SS_CLOSING ? 0 : NetReset;
}

// ---- ModemClient --------------------------------------------------------------

FLASHMEM bool ModemClient::begin()
{
   b = (Buffers *)mbedtls_calloc(1, sizeof(Buffers));
   if (!b) return false;
   mbedtls_ssl_init(&b->ssl);
   return true;
}

FLASHMEM bool ModemClient::open(IPAddress ip, uint16_t port)
{
   stop();
   sock = TcpConnect(ip, port);
   if (!sock) return false;
   kind = Kind::Stream;
   lastData = millis();
   return true;
}

FLASHMEM int ModemClient::connect(IPAddress ip, uint16_t port) { return open(ip, port); }

FLASHMEM int ModemClient::connect(const char *host, uint16_t port)
{
   IPAddress remote;
   DNSClient dns;
   dns.begin(Ethernet.dnsServerIP());
   if (dns.getHostByName(host, remote) != 1) return 0;
   return open(remote, port);
}

// Up until everything received has reached the C64, so the SwiftLink's NO
// CARRIER comes after the last byte rather than flushing the queue to send it.
FLASHMEM uint8_t ModemClient::connected()
{
   if (kind == Kind::Idle) return 0;
   if (ended && inPos == inLen && RxQueueUsed == 0) { stop(); return 0; }
   return 1;
}

FLASHMEM void ModemClient::finish(bool cut)
{
   ended = true;
   if (cut && kind == Kind::Fetch) AddToPETSCIIStrToRxQueueLN("*cut short*");
}

// One read, never a wait. A fetch with no Content-Length is complete only on
// what WebGet takes as complete: TLS's close_notify, or plain HTTP's FIN.
FLASHMEM void ModemClient::fill()
{
   inPos = inLen = 0;
   int got = tls ? mbedtls_ssl_read(&b->ssl, b->in, InSize) : RecvNow(sock, b->in, InSize);
   if (got > 0)
   {
      lastData = millis();
      if (kind == Kind::Fetch && remaining >= 0)
      {
         if (got > remaining) got = remaining;
         remaining -= got;
      }
      inLen = got;
      if (remaining == 0) finish(false);
      return;
   }
   if (got == MBEDTLS_ERR_SSL_WANT_READ || got == MBEDTLS_ERR_SSL_WANT_WRITE)
   {
      if (kind == Kind::Fetch && millis() - lastData > IoTimeoutMs) finish(true);
      return;
   }
   finish(kind == Kind::Fetch && !(remaining < 0 && (tls ? got == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY : got == 0)));
}

// The SwiftLink's poll takes all that is available, so this holds back while
// its queue could not take a buffer more: the TCP window closes behind it, and
// the sender waits for the C64.
FLASHMEM int ModemClient::available()
{
   if (inPos < inLen) return inLen - inPos;
   if (kind == Kind::Idle || ended) return 0;
   if (RxQueueSize - RxQueueUsed < InSize + 64)   // and room for "*cut short*"
   {
      lastData = millis();   // the wait is the C64's, not the server's
      return 0;
   }
   fill();
   return inLen - inPos;
}

FLASHMEM int ModemClient::read() { return available() ? b->in[inPos++] : -1; }

FLASHMEM int ModemClient::read(uint8_t *buf, size_t size)
{
   size_t got = 0;
   while (got < size && available())
   {
      size_t n = min((size_t)(inLen - inPos), size - got);
      memcpy(buf + got, b->in + inPos, n);
      inPos += n;
      got += n;
   }
   return got;
}

// Whole or not at all: NetSend waits, and mbedtls_ssl_write must be called
// again with the same data after WANT_WRITE, which the SwiftLink's partial
// writes would not do. A fetch is read-only, so what the C64 sends is dropped.
FLASHMEM size_t ModemClient::write(const uint8_t *buf, size_t size)
{
   if (kind == Kind::Idle || ended) return 0;
   if (kind == Kind::Fetch) return size;
   size_t sent = 0;
   const uint32_t start = millis();
   while (sent < size)
   {
      int ret = tls ? mbedtls_ssl_write(&b->ssl, buf + sent, size - sent) : NetSend(sock, buf + sent, size - sent);
      if ((ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) && millis() - start < IoTimeoutMs) continue;
      if (ret <= 0) { ended = true; break; }
      sent += ret;
   }
   return sent;
}

// The SwiftLink sends at once while this is the socket's whole size.
FLASHMEM int ModemClient::availableForWrite() { return (kind != Kind::Idle && !ended) ? FNET_SOCKET_DEFAULT_SIZE : 0; }

FLASHMEM void ModemClient::stop()
{
   mbedtls_ssl_free(&b->ssl);
   mbedtls_ssl_init(&b->ssl);
   if (sock) fnet_socket_close(sock);
   sock = nullptr;
   kind = Kind::Idle;
   tls = ended = false;
   remaining = -1;
   inPos = inLen = 0;
}

FLASHMEM int ModemClient::printf(const char *format, ...)
{
   char text[512];
   va_list args;
   va_start(args, format);
   int len = vsnprintf(text, sizeof(text), format, args);
   va_end(args);
   if (len < 0) return 0;
   return write((const uint8_t *)text, min((size_t)len, sizeof(text) - 1));
}

#undef FAIL
#define FAIL(s, v) do { stop(); return Result{ (s), (uint32_t)(v) }; } while (0)

// Connected and, for TLS, verified; for a fetch, through its redirects to a
// 200 with the start of its body in `in`. Every failure leaves it stopped.
FLASHMEM Result ModemClient::dial(Url &url, bool fetch)
{
   char *line = fetch ? (char *)mbedtls_calloc(1, LineSize) : nullptr;
   if (fetch && !line) return Result{ Request, 0 };
   struct Freed { char *p; ~Freed() { mbedtls_free(p); } } freed{ line };

   for (uint8_t hops = 0; ; hops++)
   {
      if (url.tls)   // here, not once: an http:// fetch can redirect to https://
      {
         Result setup = ModemTlsSetup();
         if (setup.stage != Done) { stop(); return setup; }
      }
      IPAddress remote;
      DNSClient dns;
      dns.begin(Ethernet.dnsServerIP());
      uint32_t start = millis();
      if (dns.getHostByName(url.host, remote) != 1) FAIL(Dns, millis() - start);
      start = millis();
      if (!open(remote, url.port)) FAIL(Connect, millis() - start);
      tls = url.tls;
      int ret;
      if (tls)
      {
         ret = mbedtls_ssl_setup(&b->ssl, &Tls->conf);
         if (ret == 0) ret = mbedtls_ssl_set_hostname(&b->ssl, url.host);
         if (ret != 0) FAIL(TlsSetup, -ret);
         mbedtls_ssl_set_bio(&b->ssl, sock, NetSend, NetRecv, nullptr);
         while ((ret = mbedtls_ssl_handshake(&b->ssl)) != 0)
         {
            if (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) FAIL(Verify, mbedtls_ssl_get_verify_result(&b->ssl));
            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) FAIL(Handshake, -ret);
         }
      }
      if (!fetch)
      {
         if (tls) mbedtls_ssl_set_bio(&b->ssl, sock, NetSend, RecvNow, nullptr);
         return Result{ Done, 0 };
      }

      char port[8] = "";
      if (url.port != (url.tls ? 443 : 80)) snprintf(port, sizeof(port), ":%u", url.port);
      int reqLen = snprintf(line, LineSize,
         "GET %s HTTP/1.0\r\nHost: %s%s\r\nUser-Agent: TeensyROM\r\nConnection: close\r\n\r\n", url.path, url.host, port);
      if (reqLen <= 0 || reqLen >= (int)LineSize) FAIL(Request, 0);
      for (int off = 0; off < reqLen; )
      {
         ret = tls ? mbedtls_ssl_write(&b->ssl, (const unsigned char *)line + off, reqLen - off)
                   : NetSend(sock, (const unsigned char *)line + off, reqLen - off);
         if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
         if (ret <= 0) FAIL(Request, -ret);
         off += ret;
      }

      // The headers as Exchange takes them, a buffer at a time, so what follows
      // the blank line is the start of the body, already in `in`.
      int status = 0;
      remaining = -1;
      bool location = false, overflow = false, inBody = false;
      size_t lineLen = 0, headBytes = 0;
      while (!inBody)
      {
         ret = tls ? mbedtls_ssl_read(&b->ssl, b->in, InSize) : NetRecv(sock, b->in, InSize);
         if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
         if (ret == MBEDTLS_ERR_SSL_TIMEOUT) FAIL(ReadTimeout, 0);
         if (ret <= 0) FAIL(Response, ret < 0 ? -ret : 0);
         size_t at = 0;
         while (!inBody && at < (size_t)ret)
         {
            char ch = b->in[at++];
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
            else if (strncasecmp(line, "Content-Length:", 15) == 0) remaining = strtol(line + 15, nullptr, 10);
            else if (IsRedirect(status) && strncasecmp(line, "Location:", 9) == 0) location = !overflow && ResolveLocation(line + 9, url);
            lineLen = 0;
            overflow = false;
         }
         inPos = at;
         inLen = ret;
      }
      if (IsRedirect(status))
      {
         if (!location) FAIL(Redirect, status);
         if (hops == MaxRedirects) FAIL(Redirect, 0);
         continue;   // open() stops this connection
      }
      if (status != 200) FAIL(HttpStatus, status);

      kind = Kind::Fetch;
      lastData = millis();
      if (tls) mbedtls_ssl_set_bio(&b->ssl, sock, NetSend, RecvNow, nullptr);
      if (remaining >= 0)
      {
         if (inLen - inPos > remaining) inLen = inPos + remaining;
         remaining -= inLen - inPos;
         if (remaining == 0) ended = true;
      }
      return Result{ Done, 0 };
   }
}
#undef FAIL

// ---- The dial strings ----------------------------------------------------------

// The line as typed, case and all: the SwiftLink lower-cases what it keeps in
// TxMsg, and a URL's path is case-sensitive. ModemPoll keeps this copy at the
// same offsets, so a pointer into TxMsg finds its own text here.
static char TypedLine[TxMsgMaxSize];

FLASHMEM static const char *StageText(uint8_t stage)
{
   switch (stage)
   {
      case CaFile:      return PSTR("no ca.pem");
      case CaParse:     return PSTR("bad ca.pem");
      case Trng:
      case DrbgSeed:    return PSTR("no random source");
      case Dns:         return PSTR("name not found");
      case Connect:     return PSTR("no connection");
      case TlsSetup:    return PSTR("tls setup failed");
      case Handshake:   return PSTR("secure connection failed");
      case Verify:      return PSTR("certificate not trusted");
      case Request:     return PSTR("request not sent");
      case Response:    return PSTR("response not understood");
      case HttpStatus:  return PSTR("server refused");
      case ReadTimeout: return PSTR("server stopped sending");
      case Redirect:    return PSTR("redirect refused");
   }
   return PSTR("failed");
}

FLASHMEM ATRespCode AT_HostDial(char *CmdArg)
{
   const char *typed = TypedLine + (CmdArg - TxMsg);
   const bool fetch = strncasecmp(typed, "https://", 8) == 0 || strncasecmp(typed, "http://", 7) == 0;
   const bool stream = strncasecmp(typed, "tls://", 6) == 0;
   if (!fetch && !stream) return AT_DT(CmdArg);

   SwiftRegStatus |= SwiftStatusDCD;
   ATRespCode resp = AT_C(NULL);
   if (resp != ATRC_OK) return resp;

   // tls://host:port takes the https:// parse, and nothing after the port.
   char text[TxMsgMaxSize + 2];
   if (stream) snprintf(text, sizeof(text), "https://%.*s", (int)strlen(CmdArg) - 6, typed + 6);
   else snprintf(text, sizeof(text), "%.*s", (int)strlen(CmdArg), typed);
   for (char *c = text; *c != ':'; c++) *c = tolower(*c);   // ParseUrl takes the scheme in lower case
   size_t len = strlen(text);
   if (len && text[len - 1] == '"') text[--len] = 0;
   Url *url = (Url *)mbedtls_calloc(1, sizeof(Url));
   if (!url) return ATRC_ERROR;
   struct Freed { Url *p; ~Freed() { mbedtls_free(p); } } freed{ url };
   if (!ParseUrl(text, *url) || (stream && strcmp(url->path, "/") != 0))
   {
      AddVerboseToPETSCIIStrToRxQueueLN("not a URL");
      return ATRC_ERROR;
   }

   char msg[64];
   snprintf(msg, sizeof(msg), "Trying %s...", url->host);
   AddVerboseToPETSCIIStrToRxQueueLN(msg);
   FlushRxQueue();
   Result r = client.dial(*url, fetch);
   if (r.stage == Done) return ATRC_CONNECT;
   snprintf(msg, sizeof(msg), "*%s ($%02X/%lu)*", StageText(r.stage), r.stage, (unsigned long)r.value);
   AddVerboseToPETSCIIStrToRxQueueLN(msg);
   return ATRC_NO_ANSWER;
}

FLASHMEM ATRespCode AT_HostBrowse(char *)
{
   AddVerboseToPETSCIIStrToRxQueueLN("no browser in this host");
   return ATRC_ERROR;
}

// The SwiftLink's poll, after keeping the case of a key it is about to take
// into TxMsg: the same conditions, the same offset, so TypedLine lines up.
FLASHMEM static void ModemPoll()
{
   if ((SwiftRegStatus & SwiftStatusTxEmpty) == 0 && SwiftTxOutLen < SwiftTxOutSize &&
       !(client.connected() && !BrowserMode) && TxMsgOffset < TxMsgMaxSize)
   {
      const uint8_t key = SwiftTxBuf;
      uint8_t lower = key & 0x7f;
      if (lower & 0x40) lower |= 0x20;
      if (!(TxMsgOffset && (lower == 0x08 || lower == 0x14))) TypedLine[TxMsgOffset] = CaseKept(key);
   }
   PollingHndlr_SwiftLink();
}

// ---- Starting the C64 ------------------------------------------------------------

// A cartridge that turns itself off: EasyFlash bank 0's ROMH, in Ultimax mode,
// which a C128 boots in C64 mode. It copies the last five instructions to $0200
// and runs them there, as the KERNAL replaces the ROM they ran from:
//
//   E000  78        sei               0200  A9 80     lda #$80
//   E001  D8        cld               0202  8D 02 DE  sta $de02   ; EasyFlash control: LED on, cartridge off
//   E002  A2 FF     ldx #$ff          0205  A9 A5     lda #$a5
//   E004  9A        txs               0207  8D 00 DF  sta $df00   ; StubDone, in EZFlashRAM
//   E005  A2 0C     ldx #12           020A  6C FC FF  jmp ($fffc) ; the KERNAL's reset
//   E007  BD 13 E0  lda $e013,x
//   E00A  9D 00 02  sta $0200,x
//   E00D  CA        dex
//   E00E  10 F7     bpl $e007
//   E010  4C 00 02  jmp $0200
//   E013            (the 13 bytes at $0200)
//   E020  40        rti               ; NMI and IRQ, until the KERNAL's vectors
//
// $DF00 is written with the cartridge already off: IO2 still reaches this
// host's EasyFlash handler, and the flag tells the host the C64 no longer
// reads the ROM, so the SwiftLink can take IO1.
static const uint8_t BootStub[] PROGMEM = {
   0x78, 0xD8, 0xA2, 0xFF, 0x9A, 0xA2, 0x0C, 0xBD, 0x13, 0xE0, 0x9D, 0x00, 0x02, 0xCA, 0x10, 0xF7,
   0x4C, 0x00, 0x02,
   0xA9, 0x80, 0x8D, 0x02, 0xDE, 0xA9, 0xA5, 0x8D, 0x00, 0xDF, 0x6C, 0xFC, 0xFF,
   0x40,
};
static constexpr uint8_t StubDone = 0xA5;
static constexpr uint16_t StubRti = 0x20;
static_assert(sizeof(BootStub) == StubRti + 1, "the RTI ends the stub");

// The program from the .tls file, read and checked before the C64 is touched,
// so a bad file returns to the menu with the machine as it was.
static uint8_t *Prg;
static uint32_t PrgLen;
static uint16_t PrgLoad;

// BASIC's own pointers ($2B TXTTAB, $2D VARTAB) and line link after NEW, which
// is the cold start's last step before READY: the program goes in after it.
static constexpr uint16_t BasicStart = 0x0801;

FLASHMEM static bool PeekC64(uint16_t addr, uint8_t *buf, uint32_t len)
{
   return PerformDMA(DMA_READ, addr, buf, len, DMA_ADDR_INCREMENT) && CloseDMA();
}

FLASHMEM static bool PokeC64(uint16_t addr, uint8_t *buf, uint32_t len)
{
   return PerformDMA(DMA_WRITE, addr, buf, len, DMA_ADDR_INCREMENT) && CloseDMA();
}

FLASHMEM static void ModemWait(uint32_t ms)
{
   const uint32_t start = millis();
   while (millis() - start < ms)
   {
      FollowVideoStd();
      if (BtnPressed) Exit(Done, 0);
   }
}

FLASHMEM static bool BasicReady()
{
   uint8_t vector[2], pointers[4], link[2];
   return PeekC64(0x0302, vector, 2) && PeekC64(0x002B, pointers, 4) && PeekC64(BasicStart, link, 2) &&
          vector[0] == 0x83 && vector[1] == 0xA4 &&                       // IMAIN: BASIC's main loop
          pointers[0] == 0x01 && pointers[1] == 0x08 && pointers[2] == 0x03 && pointers[3] == 0x08 &&
          link[0] == 0 && link[1] == 0;
}

// The launched file, the heaps and the SwiftLink's buffers, then the boot
// cartridge. The C64 is not touched before the file has passed.
FLASHMEM static void ModemSetup(const VmLaunchRecord &launch)
{
   NetSetup();
   mbedtls_memory_buffer_alloc_init((unsigned char *)(Ram2Base + NetHeapSize), HeapSize);
   if (!client.begin()) ReturnToMenu(TlsSetup, 0);

   File f = SD.open(launch.content, FILE_READ);
   if (!f) ReturnToMenu(Program, 0);
   const uint32_t size = f.size();
   if (!PrgSizeFits(size)) { f.close(); ReturnToMenu(Program, 0); }
   if (size)
   {
      Prg = (uint8_t *)mbedtls_calloc(1, size);
      if (!Prg || f.read(Prg, size) != size) { f.close(); ReturnToMenu(Program, 0); }
      PrgLoad = Prg[0] | (Prg[1] << 8);
      PrgLen = size - 2;
      if (!PrgPlaceFits(PrgLoad, PrgLen)) { f.close(); ReturnToMenu(Program, PrgLoad); }
   }
   f.close();

   InitHndlr_SwiftLink();
   for (unsigned i = 0; i < 2; i++) memset(RAM_Image + i * 8192, 0xFF, 8192);
   memcpy_P(RAM_Image + 8192, BootStub, sizeof(BootStub));
   for (unsigned v = 0x1FFA; v < 0x2000; v += 2) { RAM_Image[8192 + v] = StubRti; RAM_Image[8192 + v + 1] = 0xE0; }
   RAM_Image[8192 + 0x1FFC] = 0x00;   // reset: $E000
   PresentEasyFlash();
   ModemMode = true;
   doReset = true;
}

// Runs once the reset is released: the stub, BASIC, the program, then the
// modem until the TR button.
FLASHMEM static void ModemSession()
{
   uint32_t start = millis();
   while (EZFlashRAM[0] != StubDone)
   {
      if (millis() - start > 3000) Exit(Boot, 0);
      FollowVideoStd();
      if (BtnPressed) Exit(Done, 0);
   }
   IOHandler[IOH_None] = &IOHndlr_SwiftLink;   // the slot this host never selects otherwise
   CurrentIOHandler = IOH_None;
   SetGameDeassert;
   SetExROMDeassert;

   // A KERNAL boot takes about two seconds; DMA while its RAM test runs is no
   // use, and right after a C128's reset it can hang the boot.
   ModemWait(500);
   start = millis();
   while (!BasicReady())
   {
      if (millis() - start > 10000) Exit(Boot, 1);
      ModemWait(100);
   }
   ModemWait(100);   // past READY's print, into the input loop

   if (Prg)
   {
      bool ok = PokeC64(PrgLoad, Prg + 2, PrgLen);
      if (ok && PrgLoad == BasicStart)
      {
         // VARTAB, ARYTAB and STREND at its end, as LOAD leaves them, and RUN in
         // the keyboard buffer. On the stack, not in flash: the DMA reads them
         // from the PHI2 interrupt.
         const uint16_t end = BasicStart + PrgLen;
         uint8_t pointers[6] = { (uint8_t)end, (uint8_t)(end >> 8), (uint8_t)end, (uint8_t)(end >> 8), (uint8_t)end, (uint8_t)(end >> 8) };
         uint8_t run[4] = { 'R', 'U', 'N', 13 };
         uint8_t count = sizeof(run);
         ok = PokeC64(0x002D, pointers, sizeof(pointers)) && PokeC64(0x0277, run, sizeof(run)) && PokeC64(0x00C6, &count, 1);
      }
      mbedtls_free(Prg);
      Prg = nullptr;
      if (!ok) Exit(Boot, 2);
   }

   while (true)
   {
      FollowVideoStd();
      if (BtnPressed) Exit(Done, 0);
      ModemPoll();
   }
}
