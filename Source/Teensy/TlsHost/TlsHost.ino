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
// It writes the response body to /tls/download.tmp and renames it to
// /tls/download.bin only once the whole body has arrived. The result comes back
// on the C64 menu as `extension host returned ($50/<detail>)`, where detail is
// (stage << 24) | value; stage 0 is success with value = bytes written. The
// stages are the Stage enum below.
//
// Build and install:
//
//   node tools/build-firmware.mjs --target tr-plus --host-sketch Source/Teensy/TlsHost --host-tls
//   python3 tools/bench/hostinstall.py build/firmware/TeensyROM+_<ver>_TlsHost.TRH
//
// Known limits of the mbedTLS 2.12.0 that Teensyduino 1.61.0 bundles inside FNET:
// TLS 1.2 only; MBEDTLS_SSL_MAX_CONTENT_LEN is fixed at 10 KiB in its config.h,
// so a server that ignores the 4 KiB max_fragment_length request and sends a
// full 16 KiB record fails the read; and MBEDTLS_HAVE_TIME is off, so certificate
// validity dates are not checked.

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
};

static constexpr const char *UrlPath  = "/tls/url.txt";
static constexpr const char *CaPath   = "/tls/ca.pem";
static constexpr const char *TempPath = "/tls/download.tmp";
static constexpr const char *OutPath  = "/tls/download.bin";
static constexpr uint32_t IoTimeoutMs = 20000;
static constexpr size_t SocketSize = 2048;   // EthernetClient::read narrows its count to int16_t

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
   return true;
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

// Status code and Content-Length (-1 when absent) from a complete header block.
// Parsed by hand: sscanf links the scanf and strtod families, about 14 KB of a
// slot this host already nearly fills.
FLASHMEM static int ParseHeaders(const char *head, int32_t &contentLength)
{
   contentLength = -1;
   if (strncmp(head, "HTTP/1.", 7) != 0) return 0;
   const char *code = strchr(head, ' ');
   if (!code) return 0;
   int status = strtol(code + 1, nullptr, 10);
   for (const char *line = strstr(head, "\r\n"); line; line = strstr(line + 2, "\r\n"))
   {
      if (strncasecmp(line + 2, "Content-Length:", 15) == 0) contentLength = strtol(line + 17, nullptr, 10);
   }
   return status;
}

FLASHMEM static void Fetch()
{
   if (!SD.begin(BUILTIN_SDCARD)) ReturnToMenu(SdInit, 0);

   mbedtls_memory_buffer_alloc_init((unsigned char *)(Ram2Base + NetHeapSize), TlsHeapSize);

   char line[400]{};
   {
      File f = SD.open(UrlPath, FILE_READ);
      if (!f) ReturnToMenu(UrlFile, 0);
      size_t n = f.read(line, sizeof(line) - 1);
      f.close();
      line[n] = 0;
      line[strcspn(line, "\r\n")] = 0;
   }
   static Url url;
   if (!ParseUrl(line, url)) ReturnToMenu(UrlParse, 0);

   mbedtls_x509_crt ca;
   mbedtls_x509_crt_init(&ca);
   size_t caLen = 0;
   unsigned char *caPem = ReadFile(CaPath, caLen, 64 * 1024);
   if (!caPem) ReturnToMenu(CaFile, 0);
   int ret = mbedtls_x509_crt_parse(&ca, caPem, caLen);
   mbedtls_free(caPem);
   if (ret != 0) ReturnToMenu(CaParse, -ret);

   if (!TrngInit()) ReturnToMenu(Trng, TRNG_MCTL);
   mbedtls_ctr_drbg_context drbg;
   mbedtls_ctr_drbg_init(&drbg);
   ret = mbedtls_ctr_drbg_seed(&drbg, TrngEntropy, nullptr, (const unsigned char *)"TeensyROM", 9);
   if (ret != 0) ReturnToMenu(DrbgSeed, -ret);

   if (!EthernetUp()) ReturnToMenu(NetInit, Ethernet.linkStatus());

   static EthernetClient client;
   if (!client.connect(url.host, url.port)) ReturnToMenu(Connect, url.port);

   mbedtls_ssl_config conf;
   mbedtls_ssl_context ssl;
   mbedtls_ssl_config_init(&conf);
   mbedtls_ssl_init(&ssl);
   ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
   if (ret == 0)
   {
      mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
      mbedtls_ssl_conf_ca_chain(&conf, &ca, nullptr);
      mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
      ret = mbedtls_ssl_conf_max_frag_len(&conf, MBEDTLS_SSL_MAX_FRAG_LEN_4096);
   }
   if (ret == 0) ret = mbedtls_ssl_setup(&ssl, &conf);
   if (ret == 0) ret = mbedtls_ssl_set_hostname(&ssl, url.host);
   if (ret != 0) ReturnToMenu(TlsSetup, -ret);
   mbedtls_ssl_set_bio(&ssl, &client, NetSend, NetRecv, nullptr);

   while ((ret = mbedtls_ssl_handshake(&ssl)) != 0)
   {
      if (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) ReturnToMenu(Verify, mbedtls_ssl_get_verify_result(&ssl));
      if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) ReturnToMenu(Handshake, -ret);
   }

   char request[512];
   int reqLen = snprintf(request, sizeof(request),
      "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: TeensyROM\r\nConnection: close\r\n\r\n", url.path, url.host);
   if (reqLen <= 0 || reqLen >= (int)sizeof(request)) ReturnToMenu(Request, 0);
   for (int off = 0; off < reqLen; )
   {
      ret = mbedtls_ssl_write(&ssl, (const unsigned char *)request + off, reqLen - off);
      if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
      if (ret <= 0) ReturnToMenu(Request, -ret);
      off += ret;
   }

   SD.remove(TempPath);
   File out = SD.open(TempPath, FILE_WRITE);
   if (!out) ReturnToMenu(FileWrite, 0);

   static char head[2048];
   size_t headLen = 0;
   bool inBody = false;
   int32_t contentLength = -1;
   uint32_t bodyBytes = 0;
   unsigned char buf[1024];
   bool closeNotify = false;
   while (true)
   {
      ret = mbedtls_ssl_read(&ssl, buf, sizeof(buf));
      if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
      if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) { closeNotify = true; break; }
      if (ret == 0 || ret == MBEDTLS_ERR_SSL_CONN_EOF) break;
      if (ret == MBEDTLS_ERR_SSL_TIMEOUT) { out.close(); SD.remove(TempPath); ReturnToMenu(ReadTimeout, bodyBytes); }
      if (ret < 0) { out.close(); SD.remove(TempPath); ReturnToMenu(Response, -ret); }

      const unsigned char *body = buf;
      size_t bodyLen = ret;
      if (!inBody)
      {
         size_t take = min(bodyLen, sizeof(head) - 1 - headLen);
         memcpy(head + headLen, buf, take);
         headLen += take;
         head[headLen] = 0;
         char *end = strstr(head, "\r\n\r\n");
         if (!end)
         {
            if (headLen == sizeof(head) - 1) { out.close(); SD.remove(TempPath); ReturnToMenu(Response, headLen); }
            continue;
         }
         size_t headerBytes = end + 4 - head;
         end[2] = 0;
         int status = ParseHeaders(head, contentLength);
         if (status != 200) { out.close(); SD.remove(TempPath); ReturnToMenu(HttpStatus, status); }
         inBody = true;
         // What of this read lies past the header block is body.
         size_t consumedBefore = headLen - take;
         body = buf + (headerBytes - consumedBefore);
         bodyLen = ret - (headerBytes - consumedBefore);
      }
      if (bodyLen && out.write(body, bodyLen) != bodyLen) { out.close(); SD.remove(TempPath); ReturnToMenu(FileWrite, bodyBytes); }
      bodyBytes += bodyLen;
   }
   out.close();

   // Without close_notify or a Content-Length to check against, a cut connection
   // looks the same as a complete body, so neither is taken as complete.
   bool complete = inBody && (contentLength >= 0 ? bodyBytes == (uint32_t)contentLength : closeNotify);
   if (!complete) { SD.remove(TempPath); ReturnToMenu(Truncated, bodyBytes); }

   SD.remove(OutPath);
   if (!SD.rename(TempPath, OutPath)) ReturnToMenu(Rename, bodyBytes);
   ReturnToMenu(Done, bodyBytes);
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
