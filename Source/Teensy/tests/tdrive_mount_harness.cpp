// SPDX-License-Identifier: MIT
// tdrive_mount_harness: IOH_TDrive.c's remote image selection (TDrive_RemoteSet, TDrive_RemoteIdleSet) on an in-memory card,
// driven from stdin one command a line:
//   file PATH SIZE   a file of SIZE bytes (decimal) on the card
//   set PATH         TDrive_RemoteSet on the SD unit (TDrive running): prints the result code, the mounted image or "none",
//                    and on a failure " : " and the reason the remote protocol sends after FailToken
//   idle PATH        TDrive_RemoteIdleSet on the SD unit (TR menu showing): the same, with the sidecar's text for the image
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#define FLASHMEM
#define FASTRUN
#define PROGMEM
#define Printf_dbg(...) ((void)0)
#define FILE_READ  0
#define FILE_WRITE 1
#define O_RDONLY   0
#define O_RDWR     2

static struct
{
   template <typename... Args> void printf(const char *, Args...) {}
   void println(const char *) {}
} Serial;
static uint32_t Now = 100000;
static uint32_t millis() { return Now; }
static struct { uint8_t Bytes[4300]; uint8_t read(int a) { return Bytes[a]; } void write(int a, uint8_t v) { Bytes[a] = v; } } EEPROM;
static void digitalWriteFast(int, bool) {}
static int digitalReadFast(int) { return 0; }
static void DataPortWriteWaitLog(uint8_t) {}
static uint8_t DataPortWaitRead() { return 0; }
static void TraceLogAddValidData(uint8_t) {}
static bool IsDotfile(const char *Name) { return Name[0] == '.'; }

// ---- the card ----
static std::map<std::string, std::vector<uint8_t>> Card;   // path -> contents; folders are not modeled beyond the root

struct File
{
   std::string Path;
   uint32_t Pos = 0;
   bool Open = false, Append = false;
   explicit operator bool() const { return Open; }
   bool isDirectory() const { return false; }
   uint32_t size() const { return (uint32_t)Card[Path].size(); }
   int read(void *Buf, size_t N)
   {
      std::vector<uint8_t> &D = Card[Path];
      size_t K = Pos < D.size() ? std::min(N, D.size() - Pos) : 0;
      memcpy(Buf, D.data() + Pos, K);
      Pos += (uint32_t)K;
      return (int)K;
   }
   size_t write(const void *Buf, size_t N)
   {
      std::vector<uint8_t> &D = Card[Path];
      D.insert(D.end(), (const uint8_t *)Buf, (const uint8_t *)Buf + N);
      return N;
   }
   void flush() {}
   void close() { Open = false; }
   const char *name() const { return Path.c_str() + Path.rfind('/') + 1; }
   File openNextFile() { return File(); }
};

struct FsVolume {};
struct FsFile
{
   std::string Path;
   uint32_t Pos = 0;
   bool Open = false;
   bool open(FsVolume *, const char *P, int)
   {
      if (!Card.count(P)) return false;
      Path = P; Pos = 0; Open = true;
      return true;
   }
   bool isOpen() const { return Open; }
   bool close() { Open = false; return true; }
   bool sync() { return true; }
   bool seekSet(uint32_t P) { Pos = P; return P <= Card[Path].size(); }
   int read(void *Buf, size_t N)
   {
      std::vector<uint8_t> &D = Card[Path];
      size_t K = Pos < D.size() ? std::min(N, D.size() - Pos) : 0;
      memcpy(Buf, D.data() + Pos, K);
      Pos += (uint32_t)K;
      return (int)K;
   }
   size_t write(const void *Buf, size_t N)
   {
      std::vector<uint8_t> &D = Card[Path];
      if (Pos + N > D.size()) D.resize(Pos + N);
      memcpy(D.data() + Pos, Buf, N);
      Pos += (uint32_t)N;
      return N;
   }
};

struct FS
{
   FsVolume sdfs, mscfs;
   File open(const char *P, int Mode = FILE_READ)
   {
      File F;
      if (!Card.count(P))
      {
         if (Mode != FILE_WRITE) return F;
         Card[P];
      }
      F.Path = P; F.Open = true;
      return F;
   }
   bool exists(const char *P) { return Card.count(P) != 0; }
   bool remove(const char *P) { return Card.erase(P) != 0; }
   bool mkdir(const char *) { return false; }
   bool rmdir(const char *) { return false; }
};
static FS SD, firstPartition;
static bool SDFullInit() { return true; }
static bool USBFileSystemWait() { return false; }

// ---- what the firmware around the handler provides ----
#define NumColorRefs 8
#define rwRegColorRefStart 42
#define rWRegCurrMenuWAIT 9
#define eepAdTDriveDevs 4235
enum { rmtSD = 1, rmtUSBDrive = 0 };
enum { IOH_TDriveAgent = 200 };
static volatile uint8_t IO1Bytes[256];
volatile uint8_t *IO1 = IO1Bytes;
char DriveDirPath[300] = "/";
volatile uint8_t BtnPressed;

#include "../MinimalBoot/Common/IO_Handlers/IOH_TDrive.c"

int main()
{
   vio = &vioStore;
   vio_init(vio, &vioHot);
   vdUnit[0].fs = &SD; vdUnit[0].vol = &SD.sdfs;
   tdrive_attach(&vio->vd, 0, NULL, 1);
   vioReady = true;

   char Command[16], Arg[256];
   while (scanf("%15s %255s", Command, Arg) == 2)
   {
      char Msg[64];
      if (!strcmp(Command, "file"))
      {
         unsigned long Size;
         if (scanf("%lu", &Size) != 1) return 1;
         Card[Arg] = std::vector<uint8_t>(Size, 0);
      }
      else if (!strcmp(Command, "set"))
      {
         int R = TDrive_RemoteSet(0, Arg, Msg, sizeof Msg);
         char Path[VD_PathLen];
         if (vdUnit[0].mounted) VD_HostPath(&vdUnit[0], vdUnit[0].imgName, Path, sizeof Path);
         printf("%d %s%s%s\n", R, vdUnit[0].mounted ? Path : "none", R ? " : " : "", R ? Msg : "");
      }
      else if (!strcmp(Command, "idle"))
      {
         int R = TDrive_RemoteIdleSet(0, Arg, Msg, sizeof Msg);
         auto It = Card.find(VD_Sidecar);
         printf("%d %s%s%s\n", R, It == Card.end() ? "none" : std::string(It->second.begin(), It->second.end()).c_str(),
                R ? " : " : "", R ? Msg : "");
      }
      else return 1;
   }
   return 0;
}
