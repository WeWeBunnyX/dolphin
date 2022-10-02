// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// AID / AUDIO_DMA controls pushing audio out to the SRC and then the speakers.
// The audio DMA pushes audio through a small FIFO 32 bytes at a time, as
// needed.

// The SRC behind the fifo eats stereo 16-bit data at a sample rate of 32khz,
// that is, 4 bytes at 32 khz, which is 32 bytes at 4 khz. We thereforce
// schedule an event that runs at 4khz, that eats audio from the fifo. Thus, we
// have homebrew audio.

// The AID interrupt is set when the fifo STARTS a transfer. It latches address
// and count into internal registers and starts copying. This means that the
// interrupt handler can simply set the registers to where the next buffer is,
// and start filling it. When the DMA is complete, it will automatically
// relatch and fire a new interrupt.

// Then there's the DSP... what likely happens is that the
// fifo-latched-interrupt handler kicks off the DSP, requesting it to fill up
// the just used buffer through the AXList (or whatever it might be called in
// Nintendo games).

#include "Core/HW/DSP.h"

#include <memory>

#include "AudioCommon/AudioCommon.h"

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/MemoryUtil.h"

#include "Core/ConfigManager.h"
#include "Core/CoreTiming.h"
#include "Core/DSPEmulator.h"
#include "Core/HW/HSP/HSP.h"
#include "Core/HW/MMIO.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace DSP
{
// register offsets
enum
{
  DSP_MAIL_TO_DSP_HI = 0x5000,
  DSP_MAIL_TO_DSP_LO = 0x5002,
  DSP_MAIL_FROM_DSP_HI = 0x5004,
  DSP_MAIL_FROM_DSP_LO = 0x5006,
  DSP_CONTROL = 0x500A,
  DSP_INTERRUPT_CONTROL = 0x5010,
  AR_INFO = 0x5012,  // These names are a good guess at best
  AR_MODE = 0x5016,  //
  AR_REFRESH = 0x501a,
  AR_DMA_MMADDR_H = 0x5020,
  AR_DMA_MMADDR_L = 0x5022,
  AR_DMA_ARADDR_H = 0x5024,
  AR_DMA_ARADDR_L = 0x5026,
  AR_DMA_CNT_H = 0x5028,
  AR_DMA_CNT_L = 0x502A,
  AUDIO_DMA_START_HI = 0x5030,
  AUDIO_DMA_START_LO = 0x5032,
  AUDIO_DMA_BLOCKS_LENGTH = 0x5034,  // Ever used?
  AUDIO_DMA_CONTROL_LEN = 0x5036,
  AUDIO_DMA_BLOCKS_LEFT = 0x503A,
};

// UARAMCount
union UARAMCount
{
  u32 Hex = 0;
  struct
  {
    u32 count : 31;
    u32 dir : 1;  // 0: MRAM -> ARAM 1: ARAM -> MRAM
  };
};

// Blocks are 32 bytes.
union UAudioDMAControl
{
  u16 Hex = 0;
  struct
  {
    u16 NumBlocks : 15;
    u16 Enable : 1;
  };
};

// AudioDMA
struct AudioDMA
{
  u32 current_source_address = 0;
  u16 remaining_blocks_count = 0;
  u32 SourceAddress = 0;
  UAudioDMAControl AudioDMAControl;
};

// ARAM_DMA
struct ARAM_DMA
{
  u32 MMAddr = 0;
  u32 ARAddr = 0;
  UARAMCount Cnt;
};

// So we may abstract GC/Wii differences a little
struct ARAMInfo
{
  bool wii_mode = false;  // Wii EXRAM is managed in Memory:: so we need to skip statesaving, etc
  u32 size = ARAM_SIZE;
  u32 mask = ARAM_MASK;
  u8* ptr = nullptr;  // aka audio ram, auxiliary ram, MEM2, EXRAM, etc...
};

union ARAM_Info
{
  u16 Hex = 0;
  struct
  {
    u16 size : 6;
    u16 unk : 1;
    u16 : 9;
  };
};

struct DSPState::Data
{
  ARAMInfo aram;
  AudioDMA audio_dma;
  ARAM_DMA aram_dma;
  UDSPControl dsp_control;
  ARAM_Info aram_info;
  // Contains bitfields for some stuff we don't care about (and nothing ever reads):
  //  CAS latency/burst length/addressing mode/write mode
  // We care about the LSB tho. It indicates that the ARAM controller has finished initializing
  u16 aram_mode;
  u16 aram_refresh;
  int dsp_slice = 0;

  std::unique_ptr<DSPEmulator> dsp_emulator;

  bool is_lle = false;

  CoreTiming::EventType* event_type_generate_dsp_interrupt;
  CoreTiming::EventType* event_type_complete_aram;
};

DSPState::DSPState() : m_data(std::make_unique<Data>())
{
}

DSPState::~DSPState() = default;

// time given to LLE DSP on every read of the high bits in a mailbox
constexpr int DSP_MAIL_SLICE = 72;

void DoState(PointerWrap& p)
{
  auto& state = Core::System::GetInstance().GetDSPState().GetData();

  if (!state.aram.wii_mode)
    p.DoArray(state.aram.ptr, state.aram.size);
  p.DoPOD(state.dsp_control);
  p.DoPOD(state.audio_dma);
  p.DoPOD(state.aram_dma);
  p.Do(state.aram_info);
  p.Do(state.aram_mode);
  p.Do(state.aram_refresh);
  p.Do(state.dsp_slice);

  state.dsp_emulator->DoState(p);
}

static void UpdateInterrupts();
static void Do_ARAM_DMA();
static void GenerateDSPInterrupt(u64 DSPIntType, s64 cyclesLate = 0);

static void CompleteARAM(u64 userdata, s64 cyclesLate)
{
  auto& state = Core::System::GetInstance().GetDSPState().GetData();
  state.dsp_control.DMAState = 0;
  GenerateDSPInterrupt(INT_ARAM);
}

DSPEmulator* GetDSPEmulator()
{
  auto& state = Core::System::GetInstance().GetDSPState().GetData();
  return state.dsp_emulator.get();
}

void Init(bool hle)
{
  auto& state = Core::System::GetInstance().GetDSPState().GetData();
  Reinit(hle);
  state.event_type_generate_dsp_interrupt =
      CoreTiming::RegisterEvent("DSPint", GenerateDSPInterrupt);
  state.event_type_complete_aram = CoreTiming::RegisterEvent("ARAMint", CompleteARAM);
}

void Reinit(bool hle)
{
  auto& state = Core::System::GetInstance().GetDSPState().GetData();
  state.dsp_emulator = CreateDSPEmulator(hle);
  state.is_lle = state.dsp_emulator->IsLLE();

  if (SConfig::GetInstance().bWii)
  {
    state.aram.wii_mode = true;
    state.aram.size = Memory::GetExRamSizeReal();
    state.aram.mask = Memory::GetExRamMask();
    state.aram.ptr = Memory::m_pEXRAM;
  }
  else
  {
    // On the GameCube, ARAM is accessible only through this interface.
    state.aram.wii_mode = false;
    state.aram.size = ARAM_SIZE;
    state.aram.mask = ARAM_MASK;
    state.aram.ptr = static_cast<u8*>(Common::AllocateMemoryPages(state.aram.size));
  }

  state.audio_dma = {};
  state.aram_dma = {};

  state.dsp_control.Hex = 0;
  state.dsp_control.DSPHalt = 1;

  state.aram_info.Hex = 0;
  state.aram_mode = 1;       // ARAM Controller has init'd
  state.aram_refresh = 156;  // 156MHz
}

void Shutdown()
{
  auto& state = Core::System::GetInstance().GetDSPState().GetData();

  if (!state.aram.wii_mode)
  {
    Common::FreeMemoryPages(state.aram.ptr, state.aram.size);
    state.aram.ptr = nullptr;
  }

  state.dsp_emulator->Shutdown();
  state.dsp_emulator.reset();
}

void RegisterMMIO(MMIO::Mapping* mmio, u32 base)
{
  static constexpr u16 WMASK_NONE = 0x0000;
  static constexpr u16 WMASK_AR_INFO = 0x007f;
  static constexpr u16 WMASK_AR_REFRESH = 0x07ff;
  static constexpr u16 WMASK_AR_HI_RESTRICT = 0x03ff;
  static constexpr u16 WMASK_AR_CNT_DIR_BIT = 0x8000;
  static constexpr u16 WMASK_AUDIO_HI_RESTRICT_GCN = 0x03ff;
  static constexpr u16 WMASK_AUDIO_HI_RESTRICT_WII = 0x1fff;
  static constexpr u16 WMASK_LO_ALIGN_32BIT = 0xffe0;

  auto& state = Core::System::GetInstance().GetDSPState().GetData();

  // Declare all the boilerplate direct MMIOs.
  struct
  {
    u32 addr;
    u16* ptr;
    u16 wmask;
  } directly_mapped_vars[] = {
      // This register is read-only
      {AR_MODE, &state.aram_mode, WMASK_NONE},

      // For these registers, only some bits can be set
      {AR_INFO, &state.aram_info.Hex, WMASK_AR_INFO},
      {AR_REFRESH, &state.aram_refresh, WMASK_AR_REFRESH},

      // For AR_DMA_*_H registers, only bits 0x03ff can be set
      // For AR_DMA_*_L registers, only bits 0xffe0 can be set
      {AR_DMA_MMADDR_H, MMIO::Utils::HighPart(&state.aram_dma.MMAddr), WMASK_AR_HI_RESTRICT},
      {AR_DMA_MMADDR_L, MMIO::Utils::LowPart(&state.aram_dma.MMAddr), WMASK_LO_ALIGN_32BIT},
      {AR_DMA_ARADDR_H, MMIO::Utils::HighPart(&state.aram_dma.ARAddr), WMASK_AR_HI_RESTRICT},
      {AR_DMA_ARADDR_L, MMIO::Utils::LowPart(&state.aram_dma.ARAddr), WMASK_LO_ALIGN_32BIT},
      // For this register, the topmost (dir) bit can also be set
      {AR_DMA_CNT_H, MMIO::Utils::HighPart(&state.aram_dma.Cnt.Hex),
       WMASK_AR_HI_RESTRICT | WMASK_AR_CNT_DIR_BIT},
      // AR_DMA_CNT_L triggers DMA

      // For AUDIO_DMA_START_HI, only bits 0x03ff can be set on GCN and 0x1fff on Wii
      // For AUDIO_DMA_START_LO, only bits 0xffe0 can be set
      // AUDIO_DMA_START_HI requires a complex write handler
      {AUDIO_DMA_START_LO, MMIO::Utils::LowPart(&state.audio_dma.SourceAddress),
       WMASK_LO_ALIGN_32BIT},
  };
  for (auto& mapped_var : directly_mapped_vars)
  {
    mmio->Register(base | mapped_var.addr, MMIO::DirectRead<u16>(mapped_var.ptr),
                   mapped_var.wmask != WMASK_NONE ?
                       MMIO::DirectWrite<u16>(mapped_var.ptr, mapped_var.wmask) :
                       MMIO::InvalidWrite<u16>());
  }

  // DSP mail MMIOs call DSP emulator functions to get results or write data.
  mmio->Register(base | DSP_MAIL_TO_DSP_HI, MMIO::ComplexRead<u16>([](u32) {
                   auto& state = Core::System::GetInstance().GetDSPState().GetData();
                   if (state.dsp_slice > DSP_MAIL_SLICE && state.is_lle)
                   {
                     state.dsp_emulator->DSP_Update(DSP_MAIL_SLICE);
                     state.dsp_slice -= DSP_MAIL_SLICE;
                   }
                   return state.dsp_emulator->DSP_ReadMailBoxHigh(true);
                 }),
                 MMIO::ComplexWrite<u16>([](u32, u16 val) {
                   auto& state = Core::System::GetInstance().GetDSPState().GetData();
                   state.dsp_emulator->DSP_WriteMailBoxHigh(true, val);
                 }));
  mmio->Register(base | DSP_MAIL_TO_DSP_LO, MMIO::ComplexRead<u16>([](u32) {
                   auto& state = Core::System::GetInstance().GetDSPState().GetData();
                   return state.dsp_emulator->DSP_ReadMailBoxLow(true);
                 }),
                 MMIO::ComplexWrite<u16>([](u32, u16 val) {
                   auto& state = Core::System::GetInstance().GetDSPState().GetData();
                   state.dsp_emulator->DSP_WriteMailBoxLow(true, val);
                 }));
  mmio->Register(base | DSP_MAIL_FROM_DSP_HI, MMIO::ComplexRead<u16>([](u32) {
                   auto& state = Core::System::GetInstance().GetDSPState().GetData();
                   if (state.dsp_slice > DSP_MAIL_SLICE && state.is_lle)
                   {
                     state.dsp_emulator->DSP_Update(DSP_MAIL_SLICE);
                     state.dsp_slice -= DSP_MAIL_SLICE;
                   }
                   return state.dsp_emulator->DSP_ReadMailBoxHigh(false);
                 }),
                 MMIO::InvalidWrite<u16>());
  mmio->Register(base | DSP_MAIL_FROM_DSP_LO, MMIO::ComplexRead<u16>([](u32) {
                   auto& state = Core::System::GetInstance().GetDSPState().GetData();
                   return state.dsp_emulator->DSP_ReadMailBoxLow(false);
                 }),
                 MMIO::InvalidWrite<u16>());

  mmio->Register(
      base | DSP_CONTROL, MMIO::ComplexRead<u16>([](u32) {
        auto& state = Core::System::GetInstance().GetDSPState().GetData();
        return (state.dsp_control.Hex & ~DSP_CONTROL_MASK) |
               (state.dsp_emulator->DSP_ReadControlRegister() & DSP_CONTROL_MASK);
      }),
      MMIO::ComplexWrite<u16>([](u32, u16 val) {
        auto& state = Core::System::GetInstance().GetDSPState().GetData();

        UDSPControl tmpControl;
        tmpControl.Hex = (val & ~DSP_CONTROL_MASK) |
                         (state.dsp_emulator->DSP_WriteControlRegister(val) & DSP_CONTROL_MASK);

        // Not really sure if this is correct, but it works...
        // Kind of a hack because DSP_CONTROL_MASK should make this bit
        // only viewable to DSP emulator
        if (val & 1 /*DSPReset*/)
        {
          state.audio_dma.AudioDMAControl.Hex = 0;
        }

        // Update DSP related flags
        state.dsp_control.DSPReset = tmpControl.DSPReset;
        state.dsp_control.DSPAssertInt = tmpControl.DSPAssertInt;
        state.dsp_control.DSPHalt = tmpControl.DSPHalt;
        state.dsp_control.DSPInitCode = tmpControl.DSPInitCode;
        state.dsp_control.DSPInit = tmpControl.DSPInit;

        // Interrupt (mask)
        state.dsp_control.AID_mask = tmpControl.AID_mask;
        state.dsp_control.ARAM_mask = tmpControl.ARAM_mask;
        state.dsp_control.DSP_mask = tmpControl.DSP_mask;

        // Interrupt
        if (tmpControl.AID)
          state.dsp_control.AID = 0;
        if (tmpControl.ARAM)
          state.dsp_control.ARAM = 0;
        if (tmpControl.DSP)
          state.dsp_control.DSP = 0;

        // unknown
        state.dsp_control.pad = tmpControl.pad;
        if (state.dsp_control.pad != 0)
        {
          PanicAlertFmt(
              "DSPInterface (w) DSP state (CC00500A) gets a value with junk in the padding {:08x}",
              val);
        }

        UpdateInterrupts();
      }));

  // ARAM MMIO controlling the DMA start.
  mmio->Register(base | AR_DMA_CNT_L,
                 MMIO::DirectRead<u16>(MMIO::Utils::LowPart(&state.aram_dma.Cnt.Hex)),
                 MMIO::ComplexWrite<u16>([](u32, u16 val) {
                   auto& state = Core::System::GetInstance().GetDSPState().GetData();
                   state.aram_dma.Cnt.Hex =
                       (state.aram_dma.Cnt.Hex & 0xFFFF0000) | (val & WMASK_LO_ALIGN_32BIT);
                   Do_ARAM_DMA();
                 }));

  mmio->Register(base | AUDIO_DMA_START_HI,
                 MMIO::DirectRead<u16>(MMIO::Utils::HighPart(&state.audio_dma.SourceAddress)),
                 MMIO::ComplexWrite<u16>([](u32, u16 val) {
                   auto& state = Core::System::GetInstance().GetDSPState().GetData();
                   *MMIO::Utils::HighPart(&state.audio_dma.SourceAddress) =
                       val & (SConfig::GetInstance().bWii ? WMASK_AUDIO_HI_RESTRICT_WII :
                                                            WMASK_AUDIO_HI_RESTRICT_GCN);
                 }));

  // Audio DMA MMIO controlling the DMA start.
  mmio->Register(
      base | AUDIO_DMA_CONTROL_LEN, MMIO::DirectRead<u16>(&state.audio_dma.AudioDMAControl.Hex),
      MMIO::ComplexWrite<u16>([](u32, u16 val) {
        auto& state = Core::System::GetInstance().GetDSPState().GetData();
        bool already_enabled = state.audio_dma.AudioDMAControl.Enable;
        state.audio_dma.AudioDMAControl.Hex = val;

        // Only load new values if were not already doing a DMA transfer,
        // otherwise just let the new values be autoloaded in when the
        // current transfer ends.
        if (!already_enabled && state.audio_dma.AudioDMAControl.Enable)
        {
          state.audio_dma.current_source_address = state.audio_dma.SourceAddress;
          state.audio_dma.remaining_blocks_count = state.audio_dma.AudioDMAControl.NumBlocks;

          INFO_LOG_FMT(AUDIO_INTERFACE, "Audio DMA configured: {} blocks from {:#010x}",
                       state.audio_dma.AudioDMAControl.NumBlocks, state.audio_dma.SourceAddress);

          // We make the samples ready as soon as possible
          void* address = Memory::GetPointer(state.audio_dma.SourceAddress);
          AudioCommon::SendAIBuffer((short*)address, state.audio_dma.AudioDMAControl.NumBlocks * 8);

          // TODO: need hardware tests for the timing of this interrupt.
          // Sky Crawlers crashes at boot if this is scheduled less than 87 cycles in the future.
          // Other Namco games crash too, see issue 9509. For now we will just push it to 200 cycles
          CoreTiming::ScheduleEvent(200, state.event_type_generate_dsp_interrupt, INT_AID);
        }
      }));

  // Audio DMA blocks remaining is invalid to write to, and requires logic on
  // the read side.
  mmio->Register(base | AUDIO_DMA_BLOCKS_LEFT, MMIO::ComplexRead<u16>([](u32) {
                   // remaining_blocks_count is zero-based.  DreamMix World Fighters will hang if it
                   // never reaches zero.
                   auto& state = Core::System::GetInstance().GetDSPState().GetData();
                   return (state.audio_dma.remaining_blocks_count > 0 ?
                               state.audio_dma.remaining_blocks_count - 1 :
                               0);
                 }),
                 MMIO::InvalidWrite<u16>());

  // 32 bit reads/writes are a combination of two 16 bit accesses.
  for (int i = 0; i < 0x1000; i += 4)
  {
    mmio->Register(base | i, MMIO::ReadToSmaller<u32>(mmio, base | i, base | (i + 2)),
                   MMIO::WriteToSmaller<u32>(mmio, base | i, base | (i + 2)));
  }
}

// UpdateInterrupts
static void UpdateInterrupts()
{
  auto& state = Core::System::GetInstance().GetDSPState().GetData();

  // For each interrupt bit in DSP_CONTROL, the interrupt enablemask is the bit directly
  // to the left of it. By doing:
  // (DSP_CONTROL>>1) & DSP_CONTROL & MASK_OF_ALL_INTERRUPT_BITS
  // We can check if any of the interrupts are enabled and active, all at once.
  bool ints_set = (((state.dsp_control.Hex >> 1) & state.dsp_control.Hex &
                    (INT_DSP | INT_ARAM | INT_AID)) != 0);

  ProcessorInterface::SetInterrupt(ProcessorInterface::INT_CAUSE_DSP, ints_set);
}

static void GenerateDSPInterrupt(u64 DSPIntType, s64 cyclesLate)
{
  auto& state = Core::System::GetInstance().GetDSPState().GetData();

  // The INT_* enumeration members have values that reflect their bit positions in
  // DSP_CONTROL - we mask by (INT_DSP | INT_ARAM | INT_AID) just to ensure people
  // don't call this with bogus values.
  state.dsp_control.Hex |= (DSPIntType & (INT_DSP | INT_ARAM | INT_AID));
  UpdateInterrupts();
}

// CALLED FROM DSP EMULATOR, POSSIBLY THREADED
void GenerateDSPInterruptFromDSPEmu(DSPInterruptType type, int cycles_into_future)
{
  auto& state = Core::System::GetInstance().GetDSPState().GetData();
  CoreTiming::ScheduleEvent(cycles_into_future, state.event_type_generate_dsp_interrupt, type,
                            CoreTiming::FromThread::ANY);
}

// called whenever SystemTimers thinks the DSP deserves a few more cycles
void UpdateDSPSlice(int cycles)
{
  auto& state = Core::System::GetInstance().GetDSPState().GetData();

  if (state.is_lle)
  {
    // use up the rest of the slice(if any)
    state.dsp_emulator->DSP_Update(state.dsp_slice);
    state.dsp_slice %= 6;
    // note the new budget
    state.dsp_slice += cycles;
  }
  else
  {
    state.dsp_emulator->DSP_Update(cycles);
  }
}

// This happens at 4 khz, since 32 bytes at 4khz = 4 bytes at 32 khz (16bit stereo pcm)
void UpdateAudioDMA()
{
  auto& state = Core::System::GetInstance().GetDSPState().GetData();

  static short zero_samples[8 * 2] = {0};
  if (state.audio_dma.AudioDMAControl.Enable)
  {
    // Read audio at g_audioDMA.current_source_address in RAM and push onto an
    // external audio fifo in the emulator, to be mixed with the disc
    // streaming output.

    if (state.audio_dma.remaining_blocks_count != 0)
    {
      state.audio_dma.remaining_blocks_count--;
      state.audio_dma.current_source_address += 32;
    }

    if (state.audio_dma.remaining_blocks_count == 0)
    {
      state.audio_dma.current_source_address = state.audio_dma.SourceAddress;
      state.audio_dma.remaining_blocks_count = state.audio_dma.AudioDMAControl.NumBlocks;

      if (state.audio_dma.remaining_blocks_count != 0)
      {
        // We make the samples ready as soon as possible
        void* address = Memory::GetPointer(state.audio_dma.SourceAddress);
        AudioCommon::SendAIBuffer((short*)address, state.audio_dma.AudioDMAControl.NumBlocks * 8);
      }
      GenerateDSPInterrupt(DSP::INT_AID);
    }
  }
  else
  {
    AudioCommon::SendAIBuffer(&zero_samples[0], 8);
  }
}

static void Do_ARAM_DMA()
{
  auto& state = Core::System::GetInstance().GetDSPState().GetData();

  state.dsp_control.DMAState = 1;

  // ARAM DMA transfer rate has been measured on real hw
  int ticksToTransfer = (state.aram_dma.Cnt.count / 32) * 246;
  CoreTiming::ScheduleEvent(ticksToTransfer, state.event_type_complete_aram);

  // Real hardware DMAs in 32byte chunks, but we can get by with 8byte chunks
  if (state.aram_dma.Cnt.dir)
  {
    // ARAM -> MRAM
    DEBUG_LOG_FMT(DSPINTERFACE, "DMA {:08x} bytes from ARAM {:08x} to MRAM {:08x} PC: {:08x}",
                  state.aram_dma.Cnt.count, state.aram_dma.ARAddr, state.aram_dma.MMAddr, PC);

    // Outgoing data from ARAM is mirrored every 64MB (verified on real HW)
    state.aram_dma.ARAddr &= 0x3ffffff;
    state.aram_dma.MMAddr &= 0x3ffffff;

    if (state.aram_dma.ARAddr < state.aram.size)
    {
      while (state.aram_dma.Cnt.count)
      {
        // These are logically separated in code to show that a memory map has been set up
        // See below in the write section for more information
        if ((state.aram_info.Hex & 0xf) == 3)
        {
          Memory::Write_U64_Swap(*(u64*)&state.aram.ptr[state.aram_dma.ARAddr & state.aram.mask],
                                 state.aram_dma.MMAddr);
        }
        else if ((state.aram_info.Hex & 0xf) == 4)
        {
          Memory::Write_U64_Swap(*(u64*)&state.aram.ptr[state.aram_dma.ARAddr & state.aram.mask],
                                 state.aram_dma.MMAddr);
        }
        else
        {
          Memory::Write_U64_Swap(*(u64*)&state.aram.ptr[state.aram_dma.ARAddr & state.aram.mask],
                                 state.aram_dma.MMAddr);
        }

        state.aram_dma.MMAddr += 8;
        state.aram_dma.ARAddr += 8;
        state.aram_dma.Cnt.count -= 8;
      }
    }
    else if (!state.aram.wii_mode)
    {
      while (state.aram_dma.Cnt.count)
      {
        Memory::Write_U64(HSP::Read(state.aram_dma.ARAddr), state.aram_dma.MMAddr);
        state.aram_dma.MMAddr += 8;
        state.aram_dma.ARAddr += 8;
        state.aram_dma.Cnt.count -= 8;
      }
    }
  }
  else
  {
    // MRAM -> ARAM
    DEBUG_LOG_FMT(DSPINTERFACE, "DMA {:08x} bytes from MRAM {:08x} to ARAM {:08x} PC: {:08x}",
                  state.aram_dma.Cnt.count, state.aram_dma.MMAddr, state.aram_dma.ARAddr, PC);

    // Incoming data into ARAM is mirrored every 64MB (verified on real HW)
    state.aram_dma.ARAddr &= 0x3ffffff;
    state.aram_dma.MMAddr &= 0x3ffffff;

    if (state.aram_dma.ARAddr < state.aram.size)
    {
      while (state.aram_dma.Cnt.count)
      {
        if ((state.aram_info.Hex & 0xf) == 3)
        {
          *(u64*)&state.aram.ptr[state.aram_dma.ARAddr & state.aram.mask] =
              Common::swap64(Memory::Read_U64(state.aram_dma.MMAddr));
        }
        else if ((state.aram_info.Hex & 0xf) == 4)
        {
          if (state.aram_dma.ARAddr < 0x400000)
          {
            *(u64*)&state.aram.ptr[(state.aram_dma.ARAddr + 0x400000) & state.aram.mask] =
                Common::swap64(Memory::Read_U64(state.aram_dma.MMAddr));
          }
          *(u64*)&state.aram.ptr[state.aram_dma.ARAddr & state.aram.mask] =
              Common::swap64(Memory::Read_U64(state.aram_dma.MMAddr));
        }
        else
        {
          *(u64*)&state.aram.ptr[state.aram_dma.ARAddr & state.aram.mask] =
              Common::swap64(Memory::Read_U64(state.aram_dma.MMAddr));
        }

        state.aram_dma.MMAddr += 8;
        state.aram_dma.ARAddr += 8;
        state.aram_dma.Cnt.count -= 8;
      }
    }
    else if (!state.aram.wii_mode)
    {
      while (state.aram_dma.Cnt.count)
      {
        HSP::Write(state.aram_dma.ARAddr, Memory::Read_U64(state.aram_dma.MMAddr));

        state.aram_dma.MMAddr += 8;
        state.aram_dma.ARAddr += 8;
        state.aram_dma.Cnt.count -= 8;
      }
    }
  }
}

// (shuffle2) I still don't believe that this hack is actually needed... :(
// Maybe the Wii Sports ucode is processed incorrectly?
// (LM) It just means that DSP reads via '0xffdd' on Wii can end up in EXRAM or main RAM
u8 ReadARAM(u32 address)
{
  auto& state = Core::System::GetInstance().GetDSPState().GetData();

  if (state.aram.wii_mode)
  {
    if (address & 0x10000000)
      return state.aram.ptr[address & state.aram.mask];
    else
      return Memory::Read_U8(address & Memory::GetRamMask());
  }
  else
  {
    return state.aram.ptr[address & state.aram.mask];
  }
}

void WriteARAM(u8 value, u32 address)
{
  auto& state = Core::System::GetInstance().GetDSPState().GetData();

  // TODO: verify this on Wii
  state.aram.ptr[address & state.aram.mask] = value;
}

u8* GetARAMPtr()
{
  auto& state = Core::System::GetInstance().GetDSPState().GetData();
  return state.aram.ptr;
}

}  // end of namespace DSP
