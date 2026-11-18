// SPDX-License-Identifier: MIT
//
// TLS spike: an extension host that fetches one HTTPS URL to the SD card.
//
// It exists to answer whether Ethernet plus mbedTLS fits a TR+ host image and
// completes a verified handshake, before any downloader is designed. Inputs are
// two files on the SD card:
//
//   /tls/url.txt   one line, https://host[:port]/path
//   /tls/ca.pem    the PEM root certificate(s) that may sign the server's chain
//
// It follows up to five redirects to other https:// URLs, so url.txt can hold a
// stable link such as a GitHub release download, whose signed target expires in
// minutes. It writes the response body to /tls/download.tmp and renames it to
// /tls/<name> only once the whole body has arrived, where <name> is the last
// segment of url.txt's path, query and fragment removed. A URL whose path ends
// in '/', or whose last segment is no FAT long name, saves as download.bin. The result comes back
// on the C64 menu as `extension host returned ($50/<detail>)`, where detail is
// (stage << 24) | value; stage 0 is success with value = the milliseconds the
// body took to arrive. The stages are the Stage enum below. /tls/timing.txt gets
// one "<phase> <ms> <extra>" line per phase: boot, setup and total give millis()
// since power-on, the rest durations; head's extra is the HTTP status, body's
// the bytes, total's the stage. A "host <name>" line opens each connection.
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
static constexpr uint32_t NetHeapSize = 64 * 1024;
static constexpr uint32_t TlsHeapSize = 160 * 1024;
static_assert(Ram2Base + NetHeapSize + TlsHeapSize <= VM_FAIL_BASE, "RAM2 carve-out reaches the fail record");

enum Stage : uint8_t {
   Done = 0, SdInit, UrlFile, UrlParse, CaFile, CaParse, Trng, DrbgSeed, NetInit,
   Connect, TlsSetup, Handshake, Verify, Request, Response, HttpStatus, FileWrite,
   ReadTimeout, Truncated, Rename,
   Redirect,   // value: the status whose Location was missing or refused, or 0 for too many hops
   Dns,        // value: milliseconds the lookup took to fail
};

static constexpr const char *UrlPath  = "/tls/url.txt";
static constexpr const char *CaPath   = "/tls/ca.pem";
static constexpr const char *TempPath = "/tls/download.tmp";
static constexpr char OutDir[]        = "/tls/";
static constexpr const char *DefaultName = "download.bin";
static constexpr size_t MaxNameLen = 255;   // a FAT long file name
static constexpr uint32_t IoTimeoutMs = 20000;
static constexpr size_t SocketSize = 2048;   // EthernetClient::read narrows its count to int16_t
static constexpr size_t LineSize = sizeof(Url::path) + 256;   // the request, or one header line
static constexpr size_t MaxHeadBytes = 32 * 1024;
static constexpr uint8_t MaxRedirects = 5;
static constexpr const char *TimingPath = "/tls/timing.txt";

// Each phase's milliseconds, written to TimingPath on the way back to the menu,
// whatever the outcome, once the SD card is up.
static char Timing[1024];
static size_t TimingLen;
static bool SdReady;

FLASHMEM static void LogTime(const char *phase, uint32_t ms, uint32_t extra)
{
   int n = snprintf(Timing + TimingLen, sizeof(Timing) - TimingLen, "%s %lu %lu\n", phase, (unsigned long)ms, (unsigned long)extra);
   if (n > 0) TimingLen = min(TimingLen + n, sizeof(Timing) - 1);
}

FLASHMEM static void LogHost(const char *host)
{
   int n = snprintf(Timing + TimingLen, sizeof(Timing) - TimingLen, "host %s\n", host);
   if (n > 0) TimingLen = min(TimingLen + n, sizeof(Timing) - 1);
}

FLASHMEM void ReturnToMenu(uint8_t stage, uint32_t value)
{
   if (SdReady)
   {
      LogTime("total", millis(), stage);
      SD.remove(TimingPath);
      File f = SD.open(TimingPath, FILE_WRITE);
      if (f) { f.write(Timing, TimingLen); f.close(); }
   }
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

FLASHMEM static int NetSend(void *ctx, const unsigned char *buf, size_t len)
{
   EthernetClient *client = (EthernetClient *)ctx;
   if (!client->connected()) return MBEDTLS_ERR_SSL_CONN_EOF;
   size_t sent = client->write(buf, len);
   return sent ? (int)sent : MBEDTLS_ERR_SSL_CONN_EOF;
}

FLASHMEM static int NetRecv(void *ctx, unsigned char *buf, size_t len)
{
   EthernetClient *client = (EthernetClient *)ctx;
   uint32_t start = millis();
   while (client->available() <= 0)
   {
      if (!client->connected()) return 0;   // EOF; mbedTLS reports it as CONN_EOF
      if (millis() - start > IoTimeoutMs) return MBEDTLS_ERR_SSL_TIMEOUT;
      yield();
   }
   if (len > SocketSize) len = SocketSize;
   int got = client->read(buf, len);
   return got > 0 ? got : MBEDTLS_ERR_SSL_WANT_READ;
}

// Host and path go into the request as they are, so a CR or LF in a Location
// would otherwise add header lines of the server's choosing.
FLASHMEM static bool Printable(const char *text)
{
   for (; *text; text++) if (*text <= ' ' || *text >= 0x7F) return false;
   return true;
}

// The file the body is saved as, named for the last segment of url.txt's path.
// That is the name a user asked for: a redirect target, such as a GitHub
// release asset's, ends in an opaque identifier.
FLASHMEM static void OutputPath(const Url &url, char *path)
{
   size_t end = strcspn(url.path, "?#");
   const char *name = url.path + end;
   while (name[-1] != '/') name--;
   size_t len = url.path + end - name;
   bool usable = len > 0 && len <= MaxNameLen && strcspn(name, "\\:*\"<>|") >= len && strspn(name, ".") < len &&
      !(len == 12 && strncasecmp(name, "download.tmp", 12) == 0);
   strcpy(path, OutDir);
   if (usable) strncat(path, name, len);
   else strcat(path, DefaultName);
}

FLASHMEM static bool ParseUrl(const char *text, Url &url)
{
   if (strncmp(text, "https://", 8) != 0) return false;
   const char *host = text + 8;
   const char *hostEnd = host + strcspn(host, ":/");
   size_t hostLen = hostEnd - host;
   if (hostLen == 0 || hostLen >= sizeof(url.host)) return false;
   memcpy(url.host, host, hostLen);
   url.host[hostLen] = 0;
   url.port = 443;
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
   Ethernet.setSocketNum(2);
   Ethernet.setSocketSize(SocketSize);
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
// parses. An http:// target fails here, so a redirect never leaves TLS.
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
   if (!ParseUrl(value, next)) return false;
   url = next;
   return true;
}

FLASHMEM static void FailBody(File &out, Stage stage, uint32_t value)
{
   if (out) { out.close(); SD.remove(TempPath); }
   ReturnToMenu(stage, value);
}

// One GET on a fresh connection. Returns true once the body is in TempPath, with
// its size in `bodyBytes`, or false for a redirect, with `url` now its target.
// `line` (LineSize bytes) holds the request and then each header line in turn.
FLASHMEM static bool Exchange(Url &url, mbedtls_ssl_config &conf, char *line, uint32_t &bodyBytes, uint32_t &bodyMs)
{
   LogHost(url.host);
   // A new client per connection: EthernetClient counts unread bytes in
   // _remaining, which neither stop() nor connect() clears, so a reused client
   // hands the next connection's first reads the bytes the last one left unread.
   EthernetClient client;
   // Resolved here rather than by connect(host, port), which returns the same 0
   // for a failed lookup and a failed connection. Connect's value is the time
   // it took, which tells a refusal from the 10 s connect timeout.
   IPAddress remote;
   DNSClient dns;
   dns.begin(Ethernet.dnsServerIP());
   uint32_t start = millis();
   if (!dns.getHostByName(url.host, remote)) ReturnToMenu(Dns, millis() - start);
   LogTime("dns", millis() - start, 0);
   start = millis();
   if (!client.connect(remote, url.port)) ReturnToMenu(Connect, millis() - start);
   LogTime("connect", millis() - start, 0);
   start = millis();

   mbedtls_ssl_context ssl;
   mbedtls_ssl_init(&ssl);
   int ret = mbedtls_ssl_setup(&ssl, &conf);
   if (ret == 0) ret = mbedtls_ssl_set_hostname(&ssl, url.host);
   if (ret != 0) ReturnToMenu(TlsSetup, -ret);
   mbedtls_ssl_set_bio(&ssl, &client, NetSend, NetRecv, nullptr);

   while ((ret = mbedtls_ssl_handshake(&ssl)) != 0)
   {
      if (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) ReturnToMenu(Verify, mbedtls_ssl_get_verify_result(&ssl));
      if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) ReturnToMenu(Handshake, -ret);
   }
   LogTime("handshake", millis() - start, 0);
   start = millis();

   int reqLen = snprintf(line, LineSize,
      "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: TeensyROM\r\nConnection: close\r\n\r\n", url.path, url.host);
   if (reqLen <= 0 || reqLen >= (int)LineSize) ReturnToMenu(Request, 0);
   for (int off = 0; off < reqLen; )
   {
      ret = mbedtls_ssl_write(&ssl, (const unsigned char *)line + off, reqLen - off);
      if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
      if (ret <= 0) ReturnToMenu(Request, -ret);
      off += ret;
   }

   // Headers are taken a line at a time, keeping only the status, Content-Length
   // and a redirect's Location: github.com's redirect alone sends 5.7 KB of them.
   // A line longer than `line` keeps its start, enough for every header but
   // Location, which is then refused rather than followed cut short.
   int status = 0;
   int32_t contentLength = -1;
   bool location = false, overflow = false, inBody = false, closeNotify = false;
   size_t lineLen = 0, headBytes = 0;
   File out;
   bodyBytes = 0;
   unsigned char buf[1024];
   while (true)
   {
      ret = mbedtls_ssl_read(&ssl, buf, sizeof(buf));
      if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
      if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) { closeNotify = true; break; }
      if (ret == 0 || ret == MBEDTLS_ERR_SSL_CONN_EOF) break;
      if (ret == MBEDTLS_ERR_SSL_TIMEOUT) FailBody(out, ReadTimeout, bodyBytes);
      if (ret < 0) FailBody(out, Response, -ret);

      size_t at = 0;
      while (!inBody && at < (size_t)ret)
      {
         char c = buf[at++];
         if (++headBytes > MaxHeadBytes) ReturnToMenu(Response, headBytes);
         if (c != '\n')
         {
            if (lineLen < LineSize - 1) line[lineLen++] = c; else overflow = true;
            continue;
         }
         while (lineLen && strchr("\r \t", line[lineLen - 1])) lineLen--;
         line[lineLen] = 0;
         if (status == 0)
         {
            status = ParseStatus(line);
            if (status == 0) ReturnToMenu(Response, 0);
         }
         else if (lineLen == 0) inBody = true;
         else if (strncasecmp(line, "Content-Length:", 15) == 0) contentLength = strtol(line + 15, nullptr, 10);
         else if (IsRedirect(status) && strncasecmp(line, "Location:", 9) == 0) location = !overflow && ResolveLocation(line + 9, url);
         lineLen = 0;
         overflow = false;
         if (!inBody) continue;

         LogTime("head", millis() - start, status);
         start = millis();
         if (IsRedirect(status))
         {
            if (!location) ReturnToMenu(Redirect, status);
            mbedtls_ssl_close_notify(&ssl);
            mbedtls_ssl_free(&ssl);
            client.stop();
            return false;
         }
         if (status != 200) ReturnToMenu(HttpStatus, status);
         SD.remove(TempPath);
         out = SD.open(TempPath, FILE_WRITE);
         if (!out) ReturnToMenu(FileWrite, 0);
      }
      size_t bodyLen = ret - at;
      if (bodyLen && out.write(buf + at, bodyLen) != bodyLen) FailBody(out, FileWrite, bodyBytes);
      bodyBytes += bodyLen;
   }

   // Without close_notify or a Content-Length to check against, a cut connection
   // looks the same as a complete body, so neither is taken as complete.
   bool complete = inBody && (contentLength >= 0 ? bodyBytes == (uint32_t)contentLength : closeNotify);
   if (!complete) FailBody(out, Truncated, bodyBytes);
   out.close();
   bodyMs = millis() - start;
   LogTime("body", bodyMs, bodyBytes);
   return true;
}

FLASHMEM static void Fetch()
{
   if (!SD.begin(BUILTIN_SDCARD)) ReturnToMenu(SdInit, 0);
   SdReady = true;
   LogTime("boot", millis(), 0);

   mbedtls_memory_buffer_alloc_init((unsigned char *)(Ram2Base + NetHeapSize), TlsHeapSize);
   char *line = (char *)mbedtls_calloc(1, LineSize);
   static Url url;
   if (!line) ReturnToMenu(TlsSetup, 0);

   {
      File f = SD.open(UrlPath, FILE_READ);
      if (!f) ReturnToMenu(UrlFile, 0);
      // A card read error comes back as SdFat's -1 cast to size_t.
      size_t n = f.read(line, LineSize - 1);
      f.close();
      if (n > LineSize - 1) ReturnToMenu(UrlFile, 1);
      line[n] = 0;
      line[strcspn(line, "\r\n")] = 0;
   }
   if (!ParseUrl(line, url)) ReturnToMenu(UrlParse, 0);
   static char outPath[sizeof(OutDir) + MaxNameLen];
   OutputPath(url, outPath);

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

   LogTime("setup", millis(), 0);
   uint32_t start = millis();
   if (!EthernetUp()) ReturnToMenu(NetInit, Ethernet.linkStatus());
   LogTime("ethernet", millis() - start, 0);

   mbedtls_ssl_config conf;
   mbedtls_ssl_config_init(&conf);
   ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
   if (ret != 0) ReturnToMenu(TlsSetup, -ret);
   mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
   mbedtls_ssl_conf_ca_chain(&conf, &ca, nullptr);
   mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);

   uint32_t bodyBytes = 0, bodyMs = 0;
   for (uint8_t hops = 0; !Exchange(url, conf, line, bodyBytes, bodyMs); )
   {
      if (++hops > MaxRedirects) ReturnToMenu(Redirect, 0);
   }

   SD.remove(outPath);
   if (!SD.rename(TempPath, outPath)) ReturnToMenu(Rename, bodyBytes);
   ReturnToMenu(Done, bodyMs);
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
