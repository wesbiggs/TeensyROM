// MIT License
// 
// Copyright (c) 2023 Travis Smith
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software 
// and associated documentation files (the "Software"), to deal in the Software without 
// restriction, including without limitation the rights to use, copy, modify, merge, publish, 
// distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom 
// the Software is furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in all copies or 
// substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING 
// BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND 
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, 
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, 
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#define DEBUG_MEMLOC   FLASHMEM

void IO1Hndlr_Debug(uint8_t Address, bool R_Wn);  
void PollingHndlr_Debug();                           
void InitHndlr_Debug();
static void FTReset();

stcIOHandlers IOHndlr_Debug =
{
  "Debug",             //Name of handler
  &InitHndlr_Debug,    //Called once at handler startup
  &IO1Hndlr_Debug,     //IO1 R/W handler
  NULL,                //IO2 R/W handler
  NULL,                //ROML Read handler, in addition to any ROM data sent
  NULL,                //ROMH Read handler, in addition to any ROM data sent
  &PollingHndlr_Debug, //Polled in main routine
  NULL,                //called at the end of EVERY c64 cycle
};

extern uint8_t ASIDidToReg[];

//MIDI input handlers for Debug _________________________________________________________________________

DEBUG_MEMLOC void DbgOnNoteOff(uint8_t channel, uint8_t note, uint8_t velocity)
{
   Serial.printf("8x Note Off, ch=%d, note=%d, velocity=%d\n", channel, note, velocity);
}

DEBUG_MEMLOC void DbgOnNoteOn(uint8_t channel, uint8_t note, uint8_t velocity)
{   
   Serial.printf("9x Note On, ch=%d, note=%d, velocity=%d\n", channel, note, velocity);
}

DEBUG_MEMLOC void DbgOnAfterTouchPoly(uint8_t channel, uint8_t note, uint8_t velocity)
{
   Serial.printf("Ax After Touch Poly, ch=%d, note=%d, velocity=%d\n", channel, note, velocity);
}

DEBUG_MEMLOC void DbgOnControlChange(uint8_t channel, uint8_t control, uint8_t value)
{
   Serial.printf("Bx Control Change, ch=%d, control=%d, value=%d\n", channel, control, value);
}

DEBUG_MEMLOC void DbgOnProgramChange(uint8_t channel, uint8_t program)
{   
   Serial.printf("Cx Program Change, ch=%d, program=%d\n", channel, program);
}

DEBUG_MEMLOC void DbgOnAfterTouch(uint8_t channel, uint8_t pressure)
{   
   Serial.printf("Dx After Touch, ch=%d, pressure=%d\n", channel, pressure);
}

DEBUG_MEMLOC void DbgOnPitchChange(uint8_t channel, int pitch) 
{
   Serial.printf("Ex Pitch Change, ch=%d, (int)pitch=%d\n", channel, pitch);
}

// F0 SysEx single call, message larger than buffer is truncated
DEBUG_MEMLOC void DbgOnSystemExclusive(uint8_t *data, unsigned int size) 
{
   Serial.printf("\nSysEx: size=%d, data=", size);
   for(uint16_t Cnt=0; Cnt<size; Cnt++) Serial.printf(" $%02x", data[Cnt]);
   Serial.println();
   
   // Implemented based on:   http://paulus.kapsi.fi/asid_protocol.txt
   
   unsigned int NumRegs = 0; //number of regs to write
   
   if(data[0] != 0xf0 || data[1] != 0x2d || data[size-1] != 0xf7)
   {
      Serial.println("-->Invalid ASID/SysEx format");
      return;
   }
   switch(data[2])
   {
      case 0x4c:
         Serial.println("Start playing");
         break;
      case 0x4d:
         Serial.println("Stop playback");
         break;
      case 0x4f:
         //display characters
         data[size-1] = 0; //replace 0xf7 with term
         Serial.printf("Display chars: \"%s\"\n", data+3);
         break;
      case 0x4e:
         //SID1 reg data
         Serial.println("SID1 reg data");
         for(uint8_t maskNum = 0; maskNum < 4; maskNum++)
         {
            for(uint8_t bitNum = 0; bitNum < 7; bitNum++)
            {
               if(data[3+maskNum] & (1<<bitNum))
               { //reg is to be written
                  uint8_t RegVal = data[11+NumRegs];
                  if(data[7+maskNum] & (1<<bitNum)) RegVal |= 0x80;
                  NumRegs++;
                  Serial.printf("#%d: reg $%02x = $%02x\n", NumRegs, ASIDidToReg[maskNum*7+bitNum], RegVal);
               }
            }
         }
         if(12+NumRegs > size)
         {
            Serial.println("-->More regs flagged than data available");    
         }
         break;
      default:
         Serial.printf("-->Unexpected msg type: $%02x\n", data[2]);
   }
}

DEBUG_MEMLOC void DbgOnTimeCodeQuarterFrame(uint8_t data)
{
   Serial.printf("F1 TimeCodeQuarterFrame, data=%d\n", data);
   //could decode this, see example
}

DEBUG_MEMLOC void DbgOnSongPosition(uint16_t beats)       
{
   Serial.printf("F2 Song Position, (uint)beats=%d\n", beats);
}

DEBUG_MEMLOC void DbgOnSongSelect(uint8_t songnumber)     
{
   Serial.printf("F3 Song Select, songnumber=%d\n", songnumber);
}

DEBUG_MEMLOC void DbgOnTuneRequest(void)
{
   Serial.printf("F6 TuneRequest\n");
}

// F8-FF (except FD)
DEBUG_MEMLOC void DbgOnRealTimeSystem(uint8_t realtimebyte)     
{
   Serial.printf("%02x Real Time: ", realtimebyte);
   switch(realtimebyte)
   {
      case 0xF8:
         Serial.print("Timing Clock");
         break;
      case 0xF9:
         Serial.print("Measure End");
         break;
      case 0xFA:
         Serial.print("Start");
         break;
      case 0xFB:
         Serial.print("Continue");
         break;
      case 0xFC:
         Serial.print("Stop");
         break;
      case 0xFE:
         Serial.print("Active Sensing");
         break;
      case 0xFF:
         Serial.print("Reset");
         break;
      default:
         Serial.print("Unknown");
         break;
   }         
   Serial.println();
}

//______________________________________________________________________________________________

DEBUG_MEMLOC void InitHndlr_Debug()
{
   usbHostMIDI.setHandleNoteOff             (DbgOnNoteOff);             // 8x
   usbHostMIDI.setHandleNoteOn              (DbgOnNoteOn);              // 9x
   usbHostMIDI.setHandleAfterTouchPoly      (DbgOnAfterTouchPoly);      // Ax
   usbHostMIDI.setHandleControlChange       (DbgOnControlChange);       // Bx
   usbHostMIDI.setHandleProgramChange       (DbgOnProgramChange);       // Cx
   usbHostMIDI.setHandleAfterTouch          (DbgOnAfterTouch);          // Dx
   usbHostMIDI.setHandlePitchChange         (DbgOnPitchChange);         // Ex
   usbHostMIDI.setHandleSystemExclusive     (DbgOnSystemExclusive);     // F0   
   usbHostMIDI.setHandleTimeCodeQuarterFrame(DbgOnTimeCodeQuarterFrame);// F1
   usbHostMIDI.setHandleSongPosition        (DbgOnSongPosition);        // F2
   usbHostMIDI.setHandleSongSelect          (DbgOnSongSelect);          // F3
   usbHostMIDI.setHandleTuneRequest         (DbgOnTuneRequest);         // F6
   usbHostMIDI.setHandleRealTimeSystem      (DbgOnRealTimeSystem);      // F8-FF (except FD)
   // not catching F4, F5, F7 (end of SysEx), and FD                  

   usbDevMIDI.setHandleNoteOff             (DbgOnNoteOff);             // 8x
   usbDevMIDI.setHandleNoteOn              (DbgOnNoteOn);              // 9x
   usbDevMIDI.setHandleAfterTouchPoly      (DbgOnAfterTouchPoly);      // Ax
   usbDevMIDI.setHandleControlChange       (DbgOnControlChange);       // Bx
   usbDevMIDI.setHandleProgramChange       (DbgOnProgramChange);       // Cx
   usbDevMIDI.setHandleAfterTouch          (DbgOnAfterTouch);          // Dx
   usbDevMIDI.setHandlePitchChange         (DbgOnPitchChange);         // Ex
   usbDevMIDI.setHandleSystemExclusive     (DbgOnSystemExclusive);     // F0   
   usbDevMIDI.setHandleTimeCodeQuarterFrame(DbgOnTimeCodeQuarterFrame);// F1
   usbDevMIDI.setHandleSongPosition        (DbgOnSongPosition);        // F2
   usbDevMIDI.setHandleSongSelect          (DbgOnSongSelect);          // F3
   usbDevMIDI.setHandleTuneRequest         (DbgOnTuneRequest);         // F6
   usbDevMIDI.setHandleRealTimeSystem      (DbgOnRealTimeSystem);      // F8-FF (except FD)

   FTReset();
}

//The float hand-off test ______________________________________________________________________
// For measuring, on hardware, what answering a BASIC float routine from the cartridge costs: the
// C64 (Source/C64/FloatHandoff) writes a command to $DEF0, the TR holds it by DMA, and
// PollingHndlr_Debug reads the zero page, runs the port in BasicAccel/CbmFloat.h, writes the
// result back and lets the C64 go. The C64 times the hold and checks the result against the ROM.
// Only this handler answers these addresses, and only while it is the active one.

#define CBM_FLOAT_FN    FLASHMEM
#define CBM_FLOAT_DATA  PROGMEM
#include "../../../BasicAccel/CbmFloat.h"

#define FTReg_First  0xf0
enum FTRegs
{
   FTReg_Command = 0xf0, // W: command, R: status
   FTReg_Routine = 0xf1, // W/R: the routine FTCmd_Routine runs, FTRoutines index
   FTReg_A       = 0xf2, // R: the result's A, X, Y, P and Set (lo, hi)
   FTReg_LatencyLo = 0xf8, // R: nS from the command write to thread mode taking it up
   FTReg_ComputeLo = 0xfa, // R: nS in the port
   FTReg_WorkLo    = 0xfc, // R: nS from taking it up to letting the C64 go
   //$fe is rRegIOHSwapPoll during a handler swap
   FTReg_Ident   = 0xff, // R: FTIdent
};
#define FTIdent  'F'

enum FTCommands
{
   FTCmd_PingPoll = 1, // no hold: thread mode sets the status, the C64 polls for it
   FTCmd_PingHalt = 2, // hold the C64, take it up in thread mode, let it go
   FTCmd_Routine  = 3, // hold, read the zero page, run the routine, write back, let go
};

enum FTStatuses
{
   FTSt_Idle, FTSt_Busy, FTSt_Done, FTSt_Native, FTSt_DMAFailed, FTSt_NoDMA, FTSt_BadCommand,
};

static CbmFloatResult (* const FTRoutines[])(CbmCpu *) PROGMEM = { CbmFAddT, CbmFSubT, CbmFMultT, CbmFDivT, CbmQint, CbmAyint };

volatile uint8_t FTRegs[0x100 - FTReg_First];
volatile uint8_t FTPending;
volatile uint32_t FTCommandCyc;

static FLASHMEM void FTReset()
{
   memset((void *)FTRegs, 0, sizeof FTRegs);
   FTRegs[FTReg_Ident - FTReg_First] = FTIdent;
   FTPending = 0;
}

static FLASHMEM void FTPut16(uint8_t Reg, uint32_t Cycles)
{
   uint32_t nS = (uint64_t)Cycles * 1000 / (F_CPU_ACTUAL / 1000000);
   if (nS > 0xffff) nS = 0xffff;
   FTRegs[Reg - FTReg_First] = nS;
   FTRegs[Reg - FTReg_First + 1] = nS >> 8;
}

#ifdef Fab04_FullDMACapable
// The zero page the routines read and write, in the spans DMA moves: $22-$2A and $56-$70
static const uint8_t FTSpanFirst[2] PROGMEM = { 0x22, 0x56 };
static const uint8_t FTSpanLen[2] PROGMEM = { 9, 27 };

// The C64 is already held when this runs: DMA_S_StartImmediate asserted on the read cycle after
// its write to $DEF0. PerformDMA would start over with the safe freeze, which waits for a write
// then a read the held 6510 never makes, and so sits out DMA_TIMEOUT_CYCLES (5 mS) per span.
static FLASHMEM bool FTTransfer(DMA_Trans_RnW RnW, uint8_t First, uint8_t *Buffer, uint8_t Len)
{
   SetUpDMA(RnW, First, Buffer, Len, DMA_ADDR_INCREMENT);
   DMA_State = DMA_S_TransferReady;
   return FinishDMA();
}

static FLASHMEM uint8_t FTRunRoutine(uint32_t *ComputeCyc)
{
   uint8_t Routine = FTRegs[FTReg_Routine - FTReg_First];
   if (Routine >= sizeof FTRoutines / sizeof FTRoutines[0]) return FTSt_BadCommand;

   CbmCpu Cpu;
   uint8_t Before[256];
   memset(&Cpu, 0, sizeof Cpu);
   for (int i = 0; i < 2; i++)
      if (!FTTransfer(DMA_READ, FTSpanFirst[i], &Cpu.Zp[FTSpanFirst[i]], FTSpanLen[i])) return FTSt_DMAFailed;
   memcpy(Before, Cpu.Zp, sizeof Before);

   // The caller leaves A = $61 with N/Z from it, as at the ROM entries
   Cpu.A = Cpu.Zp[0x61];
   Cpu.P = (Cpu.A ? 0 : CBM_FLAG_Z) | (Cpu.A & CBM_FLAG_N);

   uint32_t Start = ARM_DWT_CYCCNT;
   CbmFloatResult Result = FTRoutines[Routine](&Cpu);
   *ComputeCyc = ARM_DWT_CYCCNT - Start;
   if (Result != CbmFloat_Done) return FTSt_Native;

   for (int i = 0; i < 2; i++)
      if (memcmp(&Cpu.Zp[FTSpanFirst[i]], &Before[FTSpanFirst[i]], FTSpanLen[i]) &&
          !FTTransfer(DMA_WRITE, FTSpanFirst[i], &Cpu.Zp[FTSpanFirst[i]], FTSpanLen[i])) return FTSt_DMAFailed;

   uint8_t *Out = (uint8_t *)&FTRegs[FTReg_A - FTReg_First];
   Out[0] = Cpu.A; Out[1] = Cpu.X; Out[2] = Cpu.Y; Out[3] = Cpu.P;
   Out[4] = Cpu.Set; Out[5] = Cpu.Set >> 8;
   return FTSt_Done;
}
#endif

static FLASHMEM void FTService()
{
   uint8_t Command = FTPending;
   bool Held = Command == FTCmd_PingHalt || Command == FTCmd_Routine;
#ifdef Fab04_FullDMACapable
   if (Held && DMA_State != DMA_S_ActiveReady) return;  //not stopped yet: the next loop()
#endif
   uint32_t TakenUp = ARM_DWT_CYCCNT;
   FTPending = 0;
   uint32_t ComputeCyc = 0;
   uint8_t Status = FTSt_Done;
#ifdef Fab04_FullDMACapable
   if (Command == FTCmd_Routine) Status = FTRunRoutine(&ComputeCyc);
#else
   if (Held) Status = FTSt_NoDMA;
#endif
   if (!Held && Command != FTCmd_PingPoll) Status = FTSt_BadCommand;
   FTPut16(FTReg_LatencyLo, TakenUp - FTCommandCyc);
   FTPut16(FTReg_ComputeLo, ComputeCyc);
   FTPut16(FTReg_WorkLo, ARM_DWT_CYCCNT - TakenUp);
   FTRegs[FTReg_Command - FTReg_First] = Status;
#ifdef Fab04_FullDMACapable
   if (Held) CloseDMA();
#endif
}

void IO1Hndlr_Debug(uint8_t Address, bool R_Wn)
{
   #ifndef DbgIOTraceLog
      BigBuf[BigBufCount] = Address; //initialize w/ address
   #endif
   if (R_Wn) //High (IO1 Read)
   {
      if (Address >= FTReg_First) DataPortWriteWaitLog(FTRegs[Address - FTReg_First]);
   }
   else  // IO1 write
   {
      uint8_t Data = DataPortWaitRead();
      BigBuf[BigBufCount] |= (Data<<8) | IOTLDataValid;
      if (Address == FTReg_Command)
      {
         FTCommandCyc = ARM_DWT_CYCCNT;
         FTRegs[FTReg_Command - FTReg_First] = FTSt_Busy;
         FTPending = Data;
#ifdef Fab04_FullDMACapable
         if (Data == FTCmd_PingHalt || Data == FTCmd_Routine) DMA_State = DMA_S_StartImmediate;
#endif
      }
      else if (Address == FTReg_Routine) FTRegs[FTReg_Routine - FTReg_First] = Data;
   }
   #ifndef DbgIOTraceLog
      if (R_Wn) BigBuf[BigBufCount] |= IOTLRead;
      if (BigBufCount < BigBufSize-1) BigBufCount++;
   #endif
}

FLASHMEM void PollingHndlr_Debug()
{
   usbHostMIDI.read();
   usbDevMIDI.read();
   if (FTPending) FTService();
}

