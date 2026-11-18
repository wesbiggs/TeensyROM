// SPDX-License-Identifier: MIT
//
// TLS spike: an extension host that fetches a list of HTTP and HTTPS URLs to
// the SD card.
//
// It runs when the user picks a .www file in the menu, which a /VMS package
// claims (see the package line below). Its inputs are:
//
//   <list>.www          the picked file: one http[s]://host[:port]/path per line;
//                       blank lines and lines starting with '#' are skipped
//   /VMS/<id>/ca.pem    the PEM root certificate(s) that may sign the servers'
//                       chains, in the claiming package's own folder
//
// Each URL follows up to five redirects, never from https:// to http://, so a
// line can hold a stable link such as a GitHub release download, whose signed
// target expires in minutes. Its body goes to download.tmp beside the list and
// is renamed to <name> there only once all of it has arrived, where <name> is the
// last segment of the line's path, query and fragment removed. A URL whose path
// ends in '/', or whose last segment is no FAT long name or names the list, its
// log or the temp file, saves as download.bin.
//
// A failed URL is skipped and the rest are still tried. <list>.log beside the
// list gets one "$<stage> <value> <ms> <url>" line per URL, value being the
// bytes saved on success. The menu shows `extension host returned
// ($50/<detail>)`, where detail is (stage << 24) | value: the first failure's,
// or stage 0 with the number of files saved. The stages are the Stage enum below.
//
// Build and install:
//
//   node tools/build-firmware.mjs --target tr-plus --host-sketch Source/Teensy/TlsHost --host-tls
//   python3 tools/bench/hostinstall.py build/firmware/TeensyROM+_<ver>_TlsHost.TRH
//
// The package that claims .www, with the same ca.pem copied into /VMS/WWW:
//
//   node tools/build-extension.mjs --id WWW --extensions www --services 0 \
//     --source vm/hello/hello.cpp --client-source Source/C64/VMHello/vmhello.a
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
#include "Common/C64Spans.h"
#include "Common/DMAControl.c"
#include "Common/IOHandlers.h"
#include "Common/MachineDetect.h"
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
   Launch,     // value: 0 no readable record, 1 no file selected
   Client,     // the package's client.crt would not load
};

static constexpr size_t MaxUrlsBytes = 16 * 1024;
static constexpr char TempName[] = "download.tmp";
static constexpr const char *DefaultName = "download.bin";
static constexpr size_t MaxNameLen = 255;   // a FAT long file name
// Set from the launch record: the selected list's folder, ending in '/', and
// the files this host writes there. Dir and the list's stem are both cut from
// the record's content, so together they fit in it, and ".log" adds 4.
static char Dir[sizeof(VmLaunchRecord::content)];
static const char *ListName;
static char TempPath[sizeof(Dir) + sizeof(TempName) - 1];
static char LogPath[sizeof(Dir) + 4];
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

// ---- The client link --------------------------------------------------------
//
// The C64 runs the WebGet client (Source/C64/WebGet/webget.a), presented as a
// 16 KiB EasyFlash cartridge. It speaks the stock host's IO2 link
// (vm/abi/README.md section 6): this host publishes packets in $DF00..$DFEF
// and takes input records, but drives the link itself rather than through the
// module runtime, so its downloads can block while the screen keeps updating
// from Idle(). Packet type 1 paints one row; input protocol $82 is one key.

// IOH_EasyFlash records the selected bank in CurrentEasyFlashBank under
// FeatVMHost; bank 58 opens the link.
static constexpr uint8_t ClientBank = 58;
static constexpr uint8_t RowPacket = 1;
static constexpr uint8_t KeyProtocol = 0x82;
static volatile bool LinkActive, ClientStarted;
static volatile uint8_t Keys[32];
static volatile uint8_t KeyHead, KeyTail;

// From IOH_EasyFlash's IO2 handler, in the PHI2 interrupt: only register
// traffic happens here. A key is taken, and $DFFC set to its token, only while
// the queue has room, so a client that sees $DFFC unchanged offers it again.
bool VMHostIO2(uint8_t address, bool read)
{
   if (!LinkActive || CurrentEasyFlashBank != ClientBank) return false;
   if (read) { DataPortWriteWaitLog(EZFlashRAM[address]); return true; }
   const uint8_t value = DataPortWaitRead();
   TraceLogAddValidData(value);
   if (address == 0xf6 || (address >= 0xf8 && address <= 0xfa) || address >= 0xfd) EZFlashRAM[address] = value;
   if (address != 0xf4) return true;
   if (value == 1) ClientStarted = true;
   if (value != 3 || !EZFlashRAM[0xfe] || EZFlashRAM[0xfe] == EZFlashRAM[0xfc] || EZFlashRAM[0xfd] != KeyProtocol) return true;
   if ((0xa5 ^ EZFlashRAM[0xf8] ^ EZFlashRAM[0xf9] ^ EZFlashRAM[0xfa] ^ EZFlashRAM[0xfd] ^ EZFlashRAM[0xfe]) != EZFlashRAM[0xff]) return true;
   const uint8_t next = (KeyHead + 1) % sizeof(Keys);
   if (next == KeyTail) return true;
   Keys[KeyHead] = EZFlashRAM[0xf8];
   KeyHead = next;
   EZFlashRAM[0xfc] = EZFlashRAM[0xfe];
   return true;
}

FLASHMEM static int NextKey()
{
   if (KeyTail == KeyHead) return -1;
   const uint8_t key = Keys[KeyTail];
   KeyTail = (KeyTail + 1) % sizeof(Keys);
   return key;
}

// The screen the client shows, kept here and sent a changed row at a time.
static uint8_t Screen[25][40];
static uint8_t RowColour[25];
static uint32_t Dirty;

namespace Link {
static VmPacket packet;
static uint8_t sequence;
static bool pending;
#include "VMHostWire.h"
}

FLASHMEM static void LinkService()
{
   using namespace Link;
   if (!LinkActive || !ClientStarted) return;
   EZFlashRAM[0xf5] = 2;
   if (pending && EZFlashRAM[0xf6] == sequence) pending = false;
   if (pending || !Dirty) return;
   const uint8_t row = __builtin_ctz(Dirty);
   Dirty &= ~(1u << row);
   packet.type = RowPacket;
   packet.flags = 0;
   packet.length = 42;
   packet.reserved = 0;
   packet.payload[0] = row;
   packet.payload[1] = RowColour[row];
   memcpy(packet.payload + 2, Screen[row], 40);
   sequence = sequence == 255 ? 1 : sequence + 1;
   uint8_t bytes[240];
   const unsigned size = encodePacket(bytes);
   for (unsigned i = 0; i < size; i++) EZFlashRAM[i] = bytes[i];
   pending = true;
   __asm__ volatile("dmb":::"memory");
   EZFlashRAM[0xf7] = sequence;
}

// IOH_EasyFlash's polling handler calls this from loop(); Idle() calls it
// from every wait.
void VMHostPoll() { LinkService(); }

// ASCII to a screen code in the lower/upper case set the client selects.
FLASHMEM static uint8_t ScreenCode(char c)
{
   if (c >= 'a' && c <= 'z') return c - 'a' + 1;
   if (c >= 'A' && c <= 'Z') return c - 'A' + 65;
   if (c >= ' ' && c <= '?') return c;
   if (c == '@') return 0;
   if (c == '[') return 27;
   if (c == ']') return 29;
   if (c == '_') return 100;
   return '?';
}

// A typed key as ASCII, or 0 for one a URL has no use for.
FLASHMEM static char KeyChar(uint8_t key)
{
   if (key >= 0x41 && key <= 0x5a) return key - 0x41 + 'a';
   if (key >= 0xc1 && key <= 0xda) return key - 0xc1 + 'A';
   if (key >= 0x20 && key <= 0x40) return key;
   if (key == 0x5b || key == 0x5d) return key;
   if (key == 0x5f) return '_';   // the left-arrow key
   return 0;
}

FLASHMEM static void Put(uint8_t row, uint8_t colour, const char *text)
{
   uint8_t i = 0;
   for (; i < 40 && text[i]; i++) Screen[row][i] = ScreenCode(text[i]);
   for (; i < 40; i++) Screen[row][i] = ' ';
   RowColour[row] = colour;
   Dirty |= 1u << row;
}

FLASHMEM static void Putf(uint8_t row, uint8_t colour, const char *format, ...)
{
   char text[64];
   va_list args;
   va_start(args, format);
   vsnprintf(text, sizeof(text), format, args);
   va_end(args);
   Put(row, colour, text);
}

// ---- The screen -------------------------------------------------------------

enum Colour : uint8_t { White = 1, Red = 2, Cyan = 3, Green = 5, Yellow = 7, LightRed = 10, Grey = 12, LightBlue = 14, LightGrey = 15 };
static constexpr uint8_t TitleRow = 0, FolderRow = 1, LogTop = 3, LogBottom = 17, StatusRow = 19, PromptRow = 21,
                         PromptRows = 3, HelpRow = 24;

FLASHMEM static void Status(const char *text) { Put(StatusRow, Yellow, text); }

// A finished URL's line, scrolling the log up.
FLASHMEM static void LogLine(uint8_t colour, const char *text)
{
   memmove(Screen[LogTop], Screen[LogTop + 1], (LogBottom - LogTop) * 40);
   memmove(RowColour + LogTop, RowColour + LogTop + 1, LogBottom - LogTop);
   for (uint8_t row = LogTop; row < LogBottom; row++) Dirty |= 1u << row;
   Put(LogBottom, colour, text);
}

// Every wait calls this, so the screen keeps up and RUN/STOP or the TR button
// can leave mid-download.
static bool Prompting;
static uint32_t Saved;
FLASHMEM static void Exit(uint8_t stage, uint32_t value);
FLASHMEM static void Idle()
{
   LinkService();
   FollowVideoStd();
   if (BtnPressed) Exit(Done, Saved);
   if (Prompting) return;
   for (int key; (key = NextKey()) >= 0; ) if (key == 3) Exit(Done, Saved);
}

// Progress, at most every quarter second: the link carries a row per packet.
static uint32_t ProgressStart, ProgressShown;
FLASHMEM static void Progress(uint32_t bytes, int32_t total)
{
   const uint32_t now = millis();
   if (now - ProgressShown < 250) return;
   ProgressShown = now;
   const uint32_t ms = now - ProgressStart;
   const uint32_t rate = ms ? bytes / ms : 0;   // bytes per ms is kB/s
   if (total > 0) Putf(StatusRow, Yellow, "%lu/%ld %lu%% %lukB/s", (unsigned long)bytes, (long)total,
                       (unsigned long)((uint64_t)bytes * 100 / (uint32_t)total), (unsigned long)rate);
   else Putf(StatusRow, Yellow, "%lu bytes %lukB/s", (unsigned long)bytes, (unsigned long)rate);
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
         Idle();
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
      Idle();
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
      Idle();
   }
}

// Host and path go into the request as they are, so a CR or LF in a Location
// would otherwise add header lines of the server's choosing.
FLASHMEM static bool Printable(const char *text)
{
   for (; *text; text++) if (*text <= ' ' || *text >= 0x7F) return false;
   return true;
}

// A name one of this host's own files in Dir has: saving over the list or its
// log would lose them, and the temp file is renamed onto the name.
FLASHMEM static bool NameReserved(const char *name, size_t len)
{
   for (const char *own : { TempName, ListName, (const char *)LogPath + strlen(Dir) })
   {
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
   strcpy(path, Dir);
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
   if (size > limit) { f.close(); return nullptr; }
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
   Putf(StatusRow, Yellow, "looking up %s", url.host);
   // Dns's and Connect's values are the milliseconds they took, which tells a
   // refusal from a timeout.
   uint32_t start = millis();
   if (!dns.getHostByName(url.host, remote)) FAIL(Dns, millis() - start);
   start = millis();
   Putf(StatusRow, Yellow, "connecting to %s", url.host);
   Idle();
   c.sock = TcpConnect(remote, url.port);
   if (!c.sock) FAIL(Connect, millis() - start);

   int ret;
   c.tls = url.tls;
   if (c.tls)
   {
      Status("securing the connection");
      Idle();
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
   Status("waiting for the server");
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
         ProgressStart = millis();
         ProgressShown = 0;
         if (!c.out) FAIL(FileWrite, 0);
      }
      size_t bodyLen = ret - at;
      if (bodyLen && c.out.write(buf + at, bodyLen) != bodyLen) FAIL(FileWrite, bodyBytes);
      bodyBytes += bodyLen;
      Progress(bodyBytes, contentLength);
   }

   // With no Content-Length, a TLS body is complete only on close_notify: an
   // attacker can forge a TCP close, but not the alert. Plain HTTP has nothing
   // better than the peer's FIN, which NetRecv tells from a reset.
   bool complete = inBody && (contentLength >= 0 ? bodyBytes == (uint32_t)contentLength : (c.tls ? closeNotify : eof));
   if (!complete) FAIL(Truncated, bodyBytes);
   c.out.close();
   return Fetched;
}

// One URL from the list, through its redirects, to its file on the card.
// Success's value is the bytes saved.
// The name the last download saved, or would have, as.
static const char *LastName;

FLASHMEM static Result Download(const char *text, mbedtls_ssl_config &conf, char *line)
{
   static Url url;
   if (!ParseUrl(text, url)) return Result{ UrlParse, 0 };
   static char outPath[sizeof(Dir) + MaxNameLen + 1];
   OutputPath(url, outPath);
   LastName = outPath + strlen(Dir);

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
   File f = SD.open(LogPath, FILE_WRITE);
   if (!f) return;
   char head[40];
   snprintf(head, sizeof(head), "$%02X %lu %lu ", r.stage, (unsigned long)r.value, (unsigned long)ms);
   f.write(head, strlen(head));
   f.write(url, strlen(url));
   f.write("\n", 1);
   f.close();
}

// The prompt: "> " and the tail of what has been typed that fits its rows,
// with a reverse-video cursor after it.
static constexpr size_t PromptWidth = PromptRows * 40 - 3;
FLASHMEM static void DrawPrompt(const char *text, size_t len)
{
   const char *shown = text + (len > PromptWidth ? len - PromptWidth : 0);
   uint8_t cells[PromptRows * 40];
   memset(cells, ' ', sizeof(cells));
   cells[0] = '>';
   size_t at = 2;
   for (; *shown; shown++) cells[at++] = ScreenCode(*shown);
   cells[at] = 0xa0;   // reverse space
   for (uint8_t row = 0; row < PromptRows; row++)
   {
      memcpy(Screen[PromptRow + row], cells + row * 40, 40);
      RowColour[PromptRow + row] = White;
      Dirty |= 1u << (PromptRow + row);
   }
}

// One line typed at the prompt, or nullptr when the user leaves.
FLASHMEM static const char *ReadUrl()
{
   static char typed[sizeof(Url::path) + sizeof(Url::host) + 16];
   size_t len = 0;
   typed[0] = 0;
   DrawPrompt(typed, len);
   Prompting = true;
   while (true)
   {
      Idle();
      const int key = NextKey();
      if (key < 0) continue;
      if (key == 3) { Prompting = false; return nullptr; }                 // RUN/STOP
      if (key == 13 && len) { Prompting = false; return typed; }          // RETURN
      if (key == 20 && len) typed[--len] = 0;                             // DEL
      else if (key == 147) typed[len = 0] = 0;                            // CLR
      else if (const char c = KeyChar(key); c && len < sizeof(typed) - 1) { typed[len++] = c; typed[len] = 0; }
      DrawPrompt(typed, len);
   }
}

// ---- Bus and cartridge, as the stock host sets them up (VMBoot.ino) ----------

// The client cartridge from the launching package, checked as the stock host
// checks it (VMHostBoot), then presented to the C64 as a 16 KiB EasyFlash.
FLASHMEM static bool StartClient(const VmLaunchRecord &launch)
{
   char path[sizeof(launch.root) + 16];
   snprintf(path, sizeof(path), "%s/client.crt", launch.root);
   File f = SD.open(path, FILE_READ);
   if (!f) return false;
   uint8_t header[64], chip[16], descriptor[128];
   bool ok = f.read(header, 64) == 64 && !memcmp(header, "C64 CARTRIDGE   ", 16) && header[23] == 32;
   for (unsigned i = 0; ok && i < 2; i++)
      ok = f.read(chip, 16) == 16 && !memcmp(chip, "CHIP", 4) && !chip[10] && !chip[11] && chip[12] == (i ? 0xa0 : 0x80) &&
           !chip[13] && chip[14] == 0x20 && !chip[15] && f.read(RAM_Image + i * 8192, 8192) == 8192;
   const char *id = strrchr(launch.root, '/') + 1;
   ok = ok && f.read(chip, 16) == 16 && !memcmp(chip, "CHIP", 4) && !chip[10] && chip[11] == 1 && chip[12] == 0x80 && !chip[13] &&
        f.read(descriptor, 128) == 128 && !memcmp(descriptor, "VMH1", 4) && descriptor[4] == VM_ABI &&
        memchr(descriptor + 16, 0, 24) && !strcmp((char *)descriptor + 16, id) &&
        vm_crc32(descriptor, 124) == *(uint32_t *)(descriptor + 124) &&
        vm_crc32(RAM_Image, 16384) == *(uint32_t *)(descriptor + 8);
   f.close();
   if (!ok) return false;
   NumCrtChips = 0;
   memset(EZFlashRAM, 0, sizeof(EZFlashRAM));
   CurrentEasyFlashBank = 0;
   SetGameAssert;
   SetExROMDeassert;
   for (unsigned i = 0; i < 64; i++) { BankDecode[i][0] = RAM_Image; BankDecode[i][1] = RAM_Image + 8192; }
   LOROM_Image = RAM_Image;
   HIROM_Image = RAM_Image + 8192;
   LOROM_Mask = HIROM_Mask = 8191;
   CurrentIOHandler = IOH_EasyFlash;
   EmulateVicCycles = false;
   memcpy(EZFlashRAM + 0xf0, "M3TP", 4);
   LinkActive = true;
   doReset = true;
   return true;
}

// ---- The session ------------------------------------------------------------

static VmLaunchRecord LaunchRecord;

// Every way out: the record the menu shows, and no temp file left behind.
FLASHMEM static void Exit(uint8_t stage, uint32_t value)
{
   if (TempPath[0]) SD.remove(TempPath);
   ReturnToMenu(stage, value);
}

// A failure the session cannot go on from: on screen until the user leaves,
// then in the record the menu shows.
FLASHMEM static void Stop(uint8_t stage, uint32_t value, const char *what)
{
   Putf(StatusRow, LightRed, "%s ($%02X/%lX)", what, stage, (unsigned long)value);
   Prompting = true;
   while (true)
   {
      Idle();
      const int key = NextKey();
      if (key == 3 || key == 13) Exit(stage, value);
   }
}

// One URL, downloaded and logged both on screen and in <list>.log.
FLASHMEM static void FetchOne(const char *text, mbedtls_ssl_config &conf, char *line)
{
   const char *shown = strstr(text, "://");
   shown = shown ? shown + 3 : text;
   const uint32_t start = millis();
   Result r = Download(text, conf, line);
   LogResult(r, millis() - start, text);
   char entry[48];
   if (r.stage == Done)
   {
      Saved++;
      snprintf(entry, sizeof(entry), "%-8lu %s", (unsigned long)r.value, LastName);
      LogLine(Green, entry);
   }
   else
   {
      snprintf(entry, sizeof(entry), "$%02X %-4lu %s", r.stage, (unsigned long)r.value, shown);
      LogLine(LightRed, entry);
   }
   Status("ready");
}

FLASHMEM static void Session()
{
   Put(TitleRow, LightBlue, "TeensyROM web downloader");
   Putf(FolderRow, Grey, "into %s", Dir);
   Put(HelpRow, Grey, "return fetches, run/stop leaves");
   for (uint8_t row = LogTop; row <= LogBottom; row++) Put(row, Green, "");
   Status("starting");

   mbedtls_memory_buffer_alloc_init((unsigned char *)(Ram2Base + NetHeapSize), TlsHeapSize);
   char *line = (char *)mbedtls_calloc(1, LineSize);
   if (!line) Stop(TlsSetup, 0, "out of memory");

   size_t urlsLen = 0;
   char *urls = (char *)ReadFile(LaunchRecord.content, urlsLen, MaxUrlsBytes);
   if (!urls) Stop(UrlFile, 0, "cannot read the list");

   char caPath[sizeof(LaunchRecord.root) + sizeof("/ca.pem") - 1];
   snprintf(caPath, sizeof(caPath), "%s/ca.pem", LaunchRecord.root);
   mbedtls_x509_crt ca;
   mbedtls_x509_crt_init(&ca);
   size_t caLen = 0;
   unsigned char *caPem = ReadFile(caPath, caLen, 64 * 1024);
   if (!caPem) Stop(CaFile, 0, "no ca.pem in the package");
   int ret = mbedtls_x509_crt_parse(&ca, caPem, caLen);
   mbedtls_free(caPem);
   // A positive ret is the number of certificates in the file that did not parse.
   if (ret != 0) Stop(CaParse, ret < 0 ? -ret : ret, "bad ca.pem");

   if (!TrngInit()) Stop(Trng, TRNG_MCTL, "no random source");
   mbedtls_ctr_drbg_context drbg;
   mbedtls_ctr_drbg_init(&drbg);
   ret = mbedtls_ctr_drbg_seed(&drbg, TrngEntropy, nullptr, (const unsigned char *)"TeensyROM", 9);
   if (ret != 0) Stop(DrbgSeed, -ret, "no random source");

   Status("starting the network");
   if (!EthernetUp()) Stop(NetInit, Ethernet.linkStatus(), "no network");

   mbedtls_ssl_config conf;
   mbedtls_ssl_config_init(&conf);
   ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
   if (ret != 0) Stop(TlsSetup, -ret, "tls setup failed");
   mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
   mbedtls_ssl_conf_ca_chain(&conf, &ca, nullptr);
   mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);

   SD.remove(LogPath);
   for (char *next = urls; *next; )
   {
      char *text = next;
      size_t len = strcspn(text, "\n");
      next = text + len + (text[len] == '\n');
      text[len] = 0;
      while (len && strchr("\r \t", text[len - 1])) text[--len] = 0;
      text += strspn(text, " \t");
      if (*text == 0 || *text == '#') continue;
      FetchOne(text, conf, line);
   }

   Status("ready");
   while (const char *text = ReadUrl()) FetchOne(text, conf, line);
   Exit(Done, Saved);
}

// ---- setup() and loop(), as the stock host's (VMBoot.ino) ---------------------

extern "C" uint32_t set_arm_clock(uint32_t frequency);

void setup()
{
   set_arm_clock(816000000);
   SetLEDOn;
   for (uint8_t PinNum = 0; PinNum < sizeof(OutputPins); PinNum++) pinMode(OutputPins[PinNum], OUTPUT);
#ifdef Fab04_FullDMACapable
   SetAddrPortDirIn;
   SetAddrBufsIn;
#endif
#ifdef Fab04_DataBufAlwaysEnabled
   SetDataPortDirIn;
   SetDataBufIn;
#else
   DataBufDisable;
   SetDataPortDirOut;
#endif
   SetDMADeassert;
   SetIRQDeassert;
   SetNMIDeassert;
#ifdef Fab04_BiDirReset
   pinMode(BiDir_Reset_PIN, INPUT_PULLUP);
   attachInterrupt(digitalPinToInterrupt(BiDir_Reset_PIN), isrButton, FALLING);
#endif
   SetResetAssert;
#ifdef Fab04_SpecialButton
   pinMode(Special_Btn_In_PIN, INPUT_PULLUP);
#endif
   for (uint8_t PinNum = 0; PinNum < sizeof(InputPins); PinNum++) pinMode(InputPins[PinNum], INPUT);
   pinMode(Menu_Btn_In_PIN, INPUT_PULLUP);
   pinMode(PHI2_PIN, INPUT_PULLUP);
   attachInterrupt(digitalPinToInterrupt(Menu_Btn_In_PIN), isrButton, FALLING);
   attachInterrupt(digitalPinToInterrupt(PHI2_PIN), isrPHI2, RISING);
   NVIC_SET_PRIORITY(IRQ_GPIO6789, 16);   // above Ethernet's timer, which would otherwise cost bus cycles

   if (ReadButton == 0) runMainTRApp();
   uint32_t magic = 0;
   EEPROM.get(VM_EEP_MAGIC_ADDR, magic);
   if (magic != VM_EEP_MAGIC) runMainTRApp();
   char marker[5]{};
   for (uint8_t i = 0; i < 4; i++) marker[i] = EEPROM.read(VM_EEP_BOOTNAME_ADDR + i);
   if (strcmp(marker, VM_HOST_MARKER) != 0) runMainTRApp();

   // PAL/NTSC timed here with the machine in reset; C64 or C128 comes from the
   // pair the main image left.
   if (TimeVideoStdAtBoot() != 0xff) SetVideoStdDMATiming(MeasuredVidStd);
   uint8_t machine[2];
   EEPreadNBuf(VM_EEP_MACHINE_ADDR, machine, 2);
   const int Machine = vm_launch_machine(machine);
   if (Machine >= 0)
      SetVideoStdDMATiming(MeasuredVidStd == 0xff ? Machine : (uint8_t)((Machine & ~rvtcNTSC) | MeasuredVidStd));

   if (!SD.begin(BUILTIN_SDCARD)) ReturnToMenu(SdInit, 0);

   // The main image wrote the launch record before rebooting: the package's
   // folder, and the .www file the user picked.
   {
      File f = SD.open("/VMS/launch.vml", FILE_READ);
      bool read = f && f.size() == sizeof(LaunchRecord) && f.read(&LaunchRecord, sizeof(LaunchRecord)) == sizeof(LaunchRecord);
      if (f) f.close();
      if (!read || !vm_launch_valid(LaunchRecord)) ReturnToMenu(Launch, 0);
   }
   if (!LaunchRecord.content[0]) ReturnToMenu(Launch, 1);
   ListName = strrchr(LaunchRecord.content, '/') + 1;
   memcpy(Dir, LaunchRecord.content, ListName - LaunchRecord.content);
   Dir[ListName - LaunchRecord.content] = 0;
   const char *dot = strrchr(ListName, '.');
   const size_t stem = dot ? (size_t)(dot - ListName) : strlen(ListName);
   snprintf(TempPath, sizeof(TempPath), "%s%s", Dir, TempName);
   snprintf(LogPath, sizeof(LogPath), "%s%.*s.log", Dir, (int)stem, ListName);

   if (!StartClient(LaunchRecord)) ReturnToMenu(Client, 0);
   BtnPressed = false;
}

void loop()
{
   FollowVideoStd();
   if (BtnPressed) Exit(Done, Saved);
   if (doReset)
   {
      SetResetAssert;
      delay(50);
#ifdef Fab04_BiDirReset
      SetResetInput;
      delay(50);
#else
      SetResetDeassert;
#endif
      doReset = false;
      BtnPressed = false;
      Phi2ResetReleased();
   }
   LinkService();
   if (ClientStarted) Session();   // does not return
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
