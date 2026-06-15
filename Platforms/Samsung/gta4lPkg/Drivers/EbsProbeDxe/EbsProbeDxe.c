/** @file
 *  EbsProbeDxe.c  -  fine-grained breadcrumb tracer for the winload->kernel handoff.
 *
 *  Each milestone lights its OWN horizontal BAND (top -> bottom), so reached
 *  milestones DON'T overwrite each other: photograph the screen at the end and
 *  read the whole pattern of lit bands = exactly which points the boot reached.
 *
 *  Post-ExitBootServices the only hookable surface is the UEFI Runtime Services,
 *  so the kernel-phase bands trace the kernel's early runtime calls. GetVariable
 *  is called many times early, so we light bands at call counts 1/2/4/8/16/32/64
 *  to show HOW FAR the kernel's early init progresses before it dies/hangs.
 *
 *   Band  Color      Milestone (furthest lit band = furthest reached)
 *   ----  ---------  ---------------------------------------------------
 *    0    blue       EndOfDxe                 (UEFI)
 *    1    cyan       ReadyToBoot              (UEFI, FB confirmed)
 *    2    green      ExitBootServices         (winload finished, handoff)
 *    3    YELLOW     GetVariable #1           *** KERNEL ALIVE post-EBS ***
 *    4    yellow     GetVariable #2
 *    5    yellow     GetVariable #4
 *    6    orange     GetVariable #8
 *    7    orange     GetVariable #16
 *    8    orange     GetVariable #32
 *    9    red        GetVariable #64
 *   10    red        GetVariable #128
 *   11    magenta    GetNextVariableName
 *   12    magenta    QueryVariableInfo
 *   13    white      GetTime                  (kernel read RTC)
 *   14    white      SetVariable              (kernel wrote a var)
 *   15    gray       GetNextHighMonotonicCount
 *   16    lime       SetVirtualAddressMap     (kernel runtime virtual phase)
 *   17    full-red   ResetSystem              (kernel asked firmware to reset = crash)
 **/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/DebugLib.h>
#include <Protocol/GraphicsOutput.h>

#define BANDS  20

STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL  *mGop     = NULL;
STATIC UINTN    mFbBase   = 0;
STATIC UINTN    mStride   = 1200;
STATIC UINTN    mWidth    = 1200;
STATIC UINTN    mHeight   = 2000;
STATIC BOOLEAN  mAfterEbs = FALSE;

STATIC EFI_GET_VARIABLE                  mOrigGetVariable = NULL;
STATIC EFI_GET_NEXT_VARIABLE_NAME        mOrigGetNextVar  = NULL;
STATIC EFI_QUERY_VARIABLE_INFO           mOrigQueryVar    = NULL;
STATIC EFI_SET_VARIABLE                  mOrigSetVariable = NULL;
STATIC EFI_GET_TIME                      mOrigGetTime     = NULL;
STATIC EFI_GET_NEXT_HIGH_MONO_COUNT      mOrigGetMono     = NULL;
STATIC EFI_SET_VIRTUAL_ADDRESS_MAP       mOrigSVAM        = NULL;
STATIC EFI_RESET_SYSTEM                  mOrigReset       = NULL;

STATIC EFI_EVENT  mRtbEvent = NULL;
STATIC EFI_EVENT  mEbsEvent = NULL;
STATIC EFI_EVENT  mVacEvent = NULL;

STATIC UINTN    mGetVarCount = 0;

// Light band [idx] (top->bottom) with a solid color. Bands don't overlap, so
// every reached milestone stays visible.
STATIC
VOID
Band (
  IN UINTN   idx,
  IN UINT32  color
  )
{
  volatile UINT32 *Fb;
  UINTN H, y0, y1, y, x;

  if (mFbBase == 0 || idx >= BANDS) {
    return;
  }
  Fb = (volatile UINT32 *)mFbBase;
  H  = mHeight / BANDS;
  y0 = idx * H;
  y1 = y0 + H;
  for (y = y0; y < y1; y++) {
    for (x = 0; x < mWidth; x++) {
      Fb[y * mStride + x] = color;
    }
  }
}

STATIC
VOID
RefreshFb (
  VOID
  )
{
  if ((mGop != NULL) && (mGop->Mode != NULL)) {
    mFbBase = (UINTN)mGop->Mode->FrameBufferBase;
    if (mGop->Mode->Info != NULL) {
      mWidth  = mGop->Mode->Info->HorizontalResolution;
      mHeight = mGop->Mode->Info->VerticalResolution;
      mStride = mGop->Mode->Info->PixelsPerScanLine;
      if (mStride == 0) { mStride = mWidth; }
    }
  }
}

// ---- Runtime service hooks (only paint AFTER EBS = the kernel) ----

STATIC
EFI_STATUS
EFIAPI
HookGetVariable (
  IN CHAR16 *Name, IN EFI_GUID *Guid,
  OUT UINT32 *Attr OPTIONAL, IN OUT UINTN *Size, OUT VOID *Data OPTIONAL
  )
{
  if (mAfterEbs) {
    mGetVarCount++;
    switch (mGetVarCount) {
      case 1:   Band (3,  0xFFFFFF00); break;  // YELLOW = KERNEL ALIVE
      case 2:   Band (4,  0xFFFFFF00); break;
      case 4:   Band (5,  0xFFFFFF00); break;
      case 8:   Band (6,  0xFFFF8000); break;  // orange
      case 16:  Band (7,  0xFFFF8000); break;
      case 32:  Band (8,  0xFFFF8000); break;
      case 64:  Band (9,  0xFFFF0000); break;  // red
      case 128: Band (10, 0xFFFF0000); break;
      default: break;
    }
  }
  return mOrigGetVariable (Name, Guid, Attr, Size, Data);
}

STATIC
EFI_STATUS
EFIAPI
HookGetNextVar (
  IN OUT UINTN *NameSize, IN OUT CHAR16 *Name, IN OUT EFI_GUID *Guid
  )
{
  if (mAfterEbs) { Band (11, 0xFFFF00FF); }   // magenta
  return mOrigGetNextVar (NameSize, Name, Guid);
}

STATIC
EFI_STATUS
EFIAPI
HookQueryVar (
  IN UINT32 Attr, OUT UINT64 *MaxStore, OUT UINT64 *RemStore, OUT UINT64 *MaxSize
  )
{
  if (mAfterEbs) { Band (12, 0xFFFF00FF); }
  return mOrigQueryVar (Attr, MaxStore, RemStore, MaxSize);
}

STATIC
EFI_STATUS
EFIAPI
HookGetTime (
  OUT EFI_TIME *Time, OUT EFI_TIME_CAPABILITIES *Caps OPTIONAL
  )
{
  if (mAfterEbs) { Band (13, 0xFFFFFFFF); }   // white
  return mOrigGetTime (Time, Caps);
}

STATIC
EFI_STATUS
EFIAPI
HookSetVariable (
  IN CHAR16 *Name, IN EFI_GUID *Guid, IN UINT32 Attr, IN UINTN Size, IN VOID *Data
  )
{
  if (mAfterEbs) { Band (14, 0xFFFFFFFF); }
  return mOrigSetVariable (Name, Guid, Attr, Size, Data);
}

STATIC
EFI_STATUS
EFIAPI
HookGetMono (
  OUT UINT32 *HighCount
  )
{
  if (mAfterEbs) { Band (15, 0xFF808080); }   // gray
  return mOrigGetMono (HighCount);
}

STATIC
EFI_STATUS
EFIAPI
HookSVAM (
  IN UINTN MapSize, IN UINTN DescSize, IN UINT32 DescVer, IN EFI_MEMORY_DESCRIPTOR *Map
  )
{
  Band (16, 0xFF00FF00);   // lime = kernel runtime virtual phase
  return mOrigSVAM (MapSize, DescSize, DescVer, Map);
}

STATIC
VOID
EFIAPI
HookReset (
  IN EFI_RESET_TYPE Type, IN EFI_STATUS St, IN UINTN Size, IN VOID *Data OPTIONAL
  )
{
  if (mAfterEbs) { Band (17, 0xFFFF0000); }   // crash -> firmware reset
  mOrigReset (Type, St, Size, Data);
}

// ---- Event callbacks ----

STATIC
VOID
EFIAPI
OnExitBootServices (
  IN EFI_EVENT Event, IN VOID *Context
  )
{
  RefreshFb ();
  Band (2, 0xFF00FF00);   // green = winload finished
  mAfterEbs = TRUE;
}

STATIC
VOID
EFIAPI
OnVirtualAddressChange (
  IN EFI_EVENT Event, IN VOID *Context
  )
{
  gRT->ConvertPointer (0, (VOID **)&mOrigGetVariable);
  gRT->ConvertPointer (0, (VOID **)&mOrigGetNextVar);
  gRT->ConvertPointer (0, (VOID **)&mOrigQueryVar);
  gRT->ConvertPointer (0, (VOID **)&mOrigSetVariable);
  gRT->ConvertPointer (0, (VOID **)&mOrigGetTime);
  gRT->ConvertPointer (0, (VOID **)&mOrigGetMono);
  gRT->ConvertPointer (0, (VOID **)&mOrigReset);
  // NB: mFbBase is MMIO (not in the runtime map) so we do NOT convert it;
  // post-SVAM bands may not paint, but pre-SVAM kernel bands already told the story.
}

STATIC
VOID
EFIAPI
OnReadyToBoot (
  IN EFI_EVENT Event, IN VOID *Context
  )
{
  EFI_STATUS Status;
  UINT32     Crc;

  gBS->CloseEvent (Event);

  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&mGop);
  if (EFI_ERROR (Status)) { mGop = NULL; }
  RefreshFb ();
  Band (1, 0xFF00FFFF);   // cyan = ReadyToBoot

  mOrigGetVariable = gRT->GetVariable;
  mOrigGetNextVar  = gRT->GetNextVariableName;
  mOrigQueryVar    = gRT->QueryVariableInfo;
  mOrigSetVariable = gRT->SetVariable;
  mOrigGetTime     = gRT->GetTime;
  mOrigGetMono     = gRT->GetNextHighMonotonicCount;
  mOrigSVAM        = gRT->SetVirtualAddressMap;
  mOrigReset       = gRT->ResetSystem;

  gRT->GetVariable              = HookGetVariable;
  gRT->GetNextVariableName      = HookGetNextVar;
  gRT->QueryVariableInfo        = HookQueryVar;
  gRT->SetVariable              = HookSetVariable;
  gRT->GetTime                  = HookGetTime;
  gRT->GetNextHighMonotonicCount = HookGetMono;
  gRT->SetVirtualAddressMap     = HookSVAM;
  gRT->ResetSystem              = HookReset;

  gRT->Hdr.CRC32 = 0;
  gBS->CalculateCrc32 (gRT, gRT->Hdr.HeaderSize, &Crc);
  gRT->Hdr.CRC32 = Crc;

  gBS->CreateEvent (EVT_SIGNAL_EXIT_BOOT_SERVICES, TPL_NOTIFY,
                    OnExitBootServices, NULL, &mEbsEvent);
  gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_NOTIFY, OnVirtualAddressChange,
                      NULL, &gEfiEventVirtualAddressChangeGuid, &mVacEvent);

  DEBUG ((DEBUG_ERROR, "[ebsprobe] RTB FB=0x%lx %dx%d stride=%d\n",
          (UINT64)mFbBase, mWidth, mHeight, mStride));
}

STATIC
VOID
EFIAPI
OnEndOfDxe (
  IN EFI_EVENT Event, IN VOID *Context
  )
{
  gBS->CloseEvent (Event);
  // Try to get the FB this early too (may be set up already)
  if (gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&mGop) == EFI_SUCCESS) {
    RefreshFb ();
    Band (0, 0xFF0000FF);   // blue = EndOfDxe
  }
}

EFI_STATUS
EFIAPI
EbsProbeDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_EVENT eod;

  gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, OnEndOfDxe,
                      NULL, &gEfiEndOfDxeEventGroupGuid, &eod);

  return gBS->CreateEventEx (
                EVT_NOTIFY_SIGNAL, TPL_CALLBACK, OnReadyToBoot,
                NULL, &gEfiEventReadyToBootGuid, &mRtbEvent
                );
}
