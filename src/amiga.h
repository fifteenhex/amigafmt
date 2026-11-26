/*
 * AmigaOS structures, as the programs see them.
 *
 * m68k Linux aligns 32-bit types to 2 bytes, exactly like the Amiga
 * compilers, so these lay out as in the NDK.  Layouts follow amitools'
 * vamos/libstructs (exec_.py, dos.py); key offsets are asserted below.
 */
#ifndef AMIGA_H
#define AMIGA_H

#include <stddef.h>
#include <stdint.h>

typedef uint32_t APTR;          /* an Amiga pointer, as a number */
typedef uint32_t BPTR;          /* address >> 2 */

#define MKBADDR(p)  ((BPTR)(uintptr_t)(p) >> 2)
#define BADDR(b)    ((void *)(uintptr_t)((uint32_t)(b) << 2))
#define PTR(x)      ((void *)(uintptr_t)(x))
#define APTR_OF(p)  ((APTR)(uintptr_t)(p))

#define DOSTRUE     (-1)
#define DOSFALSE    0

struct Node {
	APTR ln_Succ, ln_Pred;
	uint8_t ln_Type;
	int8_t ln_Pri;
	APTR ln_Name;
};

struct MinNode {
	APTR mln_Succ, mln_Pred;
};

struct List {
	APTR lh_Head, lh_Tail, lh_TailPred;
	uint8_t lh_Type, l_pad;
};

struct MinList {
	APTR mlh_Head, mlh_Tail, mlh_TailPred;
};

#define NT_TASK      1
#define NT_DEVICE    3
#define NT_MSGPORT   4
#define NT_MESSAGE   5
#define NT_RESOURCE  8
#define NT_LIBRARY   9
#define NT_MEMORY    10
#define NT_PROCESS   13

struct Library {
	struct Node lib_Node;
	uint8_t lib_Flags, lib_pad;
	uint16_t lib_NegSize, lib_PosSize, lib_Version, lib_Revision;
	APTR lib_IdString;
	uint32_t lib_Sum;
	uint16_t lib_OpenCnt;
};

struct MsgPort {
	struct Node mp_Node;
	uint8_t mp_Flags, mp_SigBit;
	APTR mp_SigTask;
	struct List mp_MsgList;
};

struct Message {
	struct Node mn_Node;
	APTR mn_ReplyPort;
	uint16_t mn_Length;
};

struct Task {
	struct Node tc_Node;
	uint8_t tc_Flags, tc_State;
	int8_t tc_IDNestCnt, tc_TDNestCnt;
	uint32_t tc_SigAlloc, tc_SigWait, tc_SigRecvd, tc_SigExcept;
	uint16_t tc_TrapAlloc, tc_TrapAble;
	APTR tc_ExceptData, tc_ExceptCode, tc_TrapData, tc_TrapCode;
	APTR tc_SPReg, tc_SPLower, tc_SPUpper, tc_Switch, tc_Launch;
	struct List tc_MemEntry;
	APTR tc_UserData;
};

struct MemHeader {
	struct Node mh_Node;
	uint16_t mh_Attributes;
	APTR mh_First, mh_Lower, mh_Upper;
	uint32_t mh_Free;
};

/* ---- expansion ---- */

struct ExpansionRom {
	uint8_t er_Type, er_Product, er_Flags, er_Reserved03;
	uint16_t er_Manufacturer;
	uint32_t er_SerialNumber;
	uint16_t er_InitDiagVec;
	uint8_t er_Reserved0c, er_Reserved0d, er_Reserved0e, er_Reserved0f;
};

struct ConfigDev {
	struct Node cd_Node;
	uint8_t cd_Flags, cd_Pad;
	struct ExpansionRom cd_Rom;
	APTR cd_BoardAddr;
	uint32_t cd_BoardSize;
	uint16_t cd_SlotAddr, cd_SlotSize;
	APTR cd_Driver, cd_NextCD;
	uint32_t cd_Unused[4];
};

struct CurrentBinding {
	APTR cb_ConfigDev, cb_FileName, cb_ProductString, cb_ToolTypes;
};

struct IntVector {
	APTR iv_Data, iv_Code, iv_Node;
};

struct SoftIntList {
	struct List sh_List;
	uint16_t sh_Pad;
};

struct ExecBase {
	struct Library LibNode;
	uint16_t SoftVer;
	int16_t LowMemChkSum;
	uint32_t ChkBase;
	APTR ColdCapture, CoolCapture, WarmCapture, SysStkUpper, SysStkLower;
	uint32_t MaxLocMem;
	APTR DebugEntry, DebugData, AlertData, MaxExtMem;
	uint16_t ChkSum;
	struct IntVector IntVects[16];
	APTR ThisTask;
	uint32_t IdleCount, DispCount;
	uint16_t Quantum, Elapsed, SysFlags;
	int8_t IDNestCnt, TDNestCnt;
	uint16_t AttnFlags, AttnResched;
	APTR ResModules, TaskTrapCode, TaskExceptCode, TaskExitCode;
	uint32_t TaskSigAlloc;
	uint16_t TaskTrapAlloc;
	struct List MemList, ResourceList, DeviceList, IntrList, LibList;
	struct List PortList, TaskReady, TaskWait;
	struct SoftIntList SoftInts[5];
	uint32_t LastAlert[4];
	uint8_t VBlankFrequency, PowerSupplyFrequency;
	struct List SemaphoreList;
	APTR KickMemPtr, KickTagPtr, KickCheckSum;
	uint16_t ex_Pad0;
	uint32_t ex_LaunchPoint;
	APTR ex_RamLibPrivate;
	uint32_t ex_EClockFrequency, ex_CacheControl, ex_TaskID;
	uint32_t ex_Reserved1[5];
	APTR ex_MMULock;
	uint32_t ex_Reserved2[3];
	struct MinList ex_MemHandlers;
	APTR ex_MemHandler;
};

#define AFF_68010 (1 << 0)
#define AFF_68020 (1 << 1)
#define AFF_68030 (1 << 2)
#define AFF_68040 (1 << 3)
#define AFF_68881 (1 << 4)
#define AFF_68882 (1 << 5)
#define AFF_FPU40 (1 << 6)
#define AFF_68060 (1 << 7)

struct DateStamp {
	int32_t ds_Days, ds_Minute, ds_Tick;
};

struct FileLock {
	BPTR fl_Link;
	uint32_t fl_Key;
	int32_t fl_Access;
	APTR fl_Task;
	BPTR fl_Volume;
};

struct FileHandle {
	APTR fh_Link, fh_Port, fh_Type;
	int32_t fh_Buf, fh_Pos, fh_End;
	int32_t fh_Funcs, fh_Func2, fh_Func3;
	int32_t fh_Args, fh_Arg2;
};

struct CommandLineInterface {
	int32_t cli_Result2;
	BPTR cli_SetName, cli_CommandDir;
	int32_t cli_ReturnCode;
	BPTR cli_CommandName;
	int32_t cli_FailLevel;
	BPTR cli_Prompt, cli_StandardInput, cli_CurrentInput, cli_CommandFile;
	int32_t cli_Interactive, cli_Background;
	BPTR cli_CurrentOutput;
	int32_t cli_DefaultStack;
	BPTR cli_StandardOutput, cli_Module;
};

struct Process {
	struct Task pr_Task;
	struct MsgPort pr_MsgPort;
	int16_t pr_Pad;
	BPTR pr_SegList;
	int32_t pr_StackSize;
	APTR pr_GlobVec;
	int32_t pr_TaskNum;
	BPTR pr_StackBase;
	int32_t pr_Result2;
	BPTR pr_CurrentDir, pr_CIS, pr_COS;
	APTR pr_ConsoleTask, pr_FileSystemTask;
	BPTR pr_CLI;
	APTR pr_ReturnAddr, pr_PktWait, pr_WindowPtr;
	BPTR pr_HomeDir;
	int32_t pr_Flags;
	APTR pr_ExitCode;
	int32_t pr_ExitData;
	APTR pr_Arguments;
	struct MinList pr_LocalVars;
	uint32_t pr_ShellPrivate;
	BPTR pr_CES;
};

struct FileInfoBlock {
	uint32_t fib_DiskKey;
	int32_t fib_DirEntryType;
	char fib_FileName[108];
	int32_t fib_Protection, fib_EntryType;
	uint32_t fib_Size;
	int32_t fib_NumBlocks;
	struct DateStamp fib_Date;
	char fib_Comment[80];
	uint16_t fib_OwnerUID, fib_OwnerGID;
	char fib_Reserved[32];
};

#define ST_ROOT      1
#define ST_USERDIR   2
#define ST_SOFTLINK  3
#define ST_LINKDIR   4
#define ST_FILE      (-3)
#define ST_LINKFILE  (-4)
#define ST_PIPEFILE  (-5)

/* protection bits: RWED are active LOW */
#define FIBF_DELETE   (1 << 0)
#define FIBF_EXECUTE  (1 << 1)
#define FIBF_WRITE    (1 << 2)
#define FIBF_READ     (1 << 3)
#define FIBF_ARCHIVE  (1 << 4)
#define FIBF_PURE     (1 << 5)
#define FIBF_SCRIPT   (1 << 6)

struct InfoData {
	int32_t id_NumSoftErrors, id_UnitNumber, id_DiskState;
	int32_t id_NumBlocks, id_NumBlocksUsed, id_BytesPerBlock, id_DiskType;
	BPTR id_VolumeNode;
	int32_t id_InUse;
};

struct RootNode {
	BPTR rn_TaskArray, rn_ConsoleSegment;
	struct DateStamp rn_Time;
	int32_t rn_RestartSeg;
	BPTR rn_Info, rn_FileHandlerSegment;
	struct MinList rn_CliList;
	APTR rn_BootProc;
	BPTR rn_ShellSegment;
	int32_t rn_Flags;
};

struct DosLibrary {
	struct Library dl_lib;
	APTR dl_Root, dl_GV;
	int32_t dl_A2, dl_A5, dl_A6;
	APTR dl_Errors, dl_TimeReq, dl_UtilityBase, dl_IntuitionBase;
};

struct CSource {
	APTR CS_Buffer;
	int32_t CS_Length, CS_CurChr;
};

struct RDArgs {
	struct CSource RDA_Source;
	int32_t RDA_DAList;
	APTR RDA_Buffer;
	int32_t RDA_BufSiz;
	APTR RDA_ExtHelp;
	int32_t RDA_Flags;
};

#define RDAF_STDIN     (1 << 0)
#define RDAF_NOALLOC   (1 << 1)
#define RDAF_NOPROMPT  (1 << 2)

struct AChain {
	APTR an_Child, an_Parent;
	BPTR an_Lock;
	struct FileInfoBlock an_Info;
	int8_t an_Flags;
	uint8_t an_String[1];
};

struct AnchorPath {
	APTR ap_Base, ap_Last;
	int32_t ap_BreakBits, ap_FoundBreak;
	int8_t ap_Flags, ap_Reserved;
	int16_t ap_Strlen;
	struct FileInfoBlock ap_Info;
	char ap_Buf[1];
};

#define APF_DOWILD      (1 << 0)
#define APF_ITSWILD     (1 << 0)
#define APF_DODIR       (1 << 2)
#define APF_DIDDIR      (1 << 3)
#define APF_NOMEMERR    (1 << 4)
#define APF_DODOT       (1 << 5)
#define APF_DirChanged  (1 << 6)
#define APF_FollowHLinks (1 << 7)

struct DateTime {
	struct DateStamp dat_Stamp;
	uint8_t dat_Format, dat_Flags;
	APTR dat_StrDay, dat_StrDate, dat_StrTime;
};

struct ExAllControl {
	uint32_t eac_Entries, eac_LastKey;
	APTR eac_MatchString, eac_MatchFunc;
};

struct TagItem {
	uint32_t ti_Tag, ti_Data;
};

struct Hook {
	struct MinNode h_MinNode;
	APTR h_Entry, h_SubEntry, h_Data;
};

struct ClockData {
	uint16_t sec, min, hour, mday, month, year, wday;
};

#define SIGBREAKF_CTRL_C (1 << 12)
#define SIGBREAKF_CTRL_D (1 << 13)
#define SIGBREAKF_CTRL_E (1 << 14)
#define SIGBREAKF_CTRL_F (1 << 15)
#define SIGF_DOS         (1 << 8)

#define MEMF_PUBLIC   (1 << 0)
#define MEMF_CHIP     (1 << 1)
#define MEMF_FAST     (1 << 2)
#define MEMF_LOCAL    (1 << 8)
#define MEMF_24BITDMA (1 << 9)
#define MEMF_CLEAR    (1 << 16)
#define MEMF_LARGEST  (1 << 17)
#define MEMF_TOTAL    (1 << 19)

#define MODE_OLDFILE    1005
#define MODE_NEWFILE    1006
#define MODE_READWRITE  1004
#define SHARED_LOCK     (-2)
#define EXCLUSIVE_LOCK  (-1)
#define OFFSET_BEGINNING (-1)
#define OFFSET_CURRENT  0
#define OFFSET_END      1

#define ERROR_NO_FREE_STORE          103
#define ERROR_BAD_TEMPLATE           114
#define ERROR_BAD_NUMBER             115
#define ERROR_REQUIRED_ARG_MISSING   116
#define ERROR_KEY_NEEDS_ARG          117
#define ERROR_TOO_MANY_ARGS          118
#define ERROR_UNMATCHED_QUOTES       119
#define ERROR_LINE_TOO_LONG          120
#define ERROR_FILE_NOT_OBJECT        121
#define ERROR_OBJECT_IN_USE          202
#define ERROR_OBJECT_EXISTS          203
#define ERROR_DIR_NOT_FOUND          204
#define ERROR_OBJECT_NOT_FOUND       205
#define ERROR_BAD_STREAM_NAME        206
#define ERROR_OBJECT_TOO_LARGE       207
#define ERROR_ACTION_NOT_KNOWN       209
#define ERROR_INVALID_COMPONENT_NAME 210
#define ERROR_INVALID_LOCK           211
#define ERROR_OBJECT_WRONG_TYPE      212
#define ERROR_DISK_WRITE_PROTECTED   214
#define ERROR_RENAME_ACROSS_DEVICES  215
#define ERROR_DIRECTORY_NOT_EMPTY    216
#define ERROR_TOO_MANY_LEVELS        217
#define ERROR_DEVICE_NOT_MOUNTED     218
#define ERROR_SEEK_ERROR             219
#define ERROR_DISK_FULL              221
#define ERROR_DELETE_PROTECTED       222
#define ERROR_WRITE_PROTECTED        223
#define ERROR_READ_PROTECTED         224
#define ERROR_NO_MORE_ENTRIES        232
#define ERROR_IS_SOFT_LINK           233
#define ERROR_BAD_HUNK               235
#define ERROR_NOT_IMPLEMENTED        236
#define ERROR_BUFFER_OVERFLOW        303
#define ERROR_BREAK                  304


/* ---- graphics ---- */

struct TextAttr {
	APTR ta_Name;
	uint16_t ta_YSize;
	uint8_t ta_Style, ta_Flags;
};

struct TextFont {
	struct Message tf_Message;      /* ln_Name is the font name */
	uint16_t tf_YSize;
	uint8_t tf_Style, tf_Flags;
	uint16_t tf_XSize, tf_Baseline, tf_BoldSmear, tf_Accessors;
	uint8_t tf_LoChar, tf_HiChar;
	APTR tf_CharData;
	uint16_t tf_Modulo;
	APTR tf_CharLoc, tf_CharSpace, tf_CharKern;
};

struct BitMap {
	uint16_t BytesPerRow, Rows;
	uint8_t Flags, Depth;
	uint16_t pad;
	APTR Planes[8];
};

struct RastPort {
	APTR Layer, BitMap, AreaPtrn, TmpRas, AreaInfo, GelsInfo;
	uint8_t Mask;
	int8_t FgPen, BgPen, AOlPen, DrawMode, AreaPtSz, linpatcnt, dummy;
	uint16_t Flags, LinePtrn;
	int16_t cp_x, cp_y;
	uint8_t minterms[8];
	int16_t PenWidth, PenHeight;
	APTR Font;
	uint8_t AlgoStyle, TxFlags;
	uint16_t TxHeight, TxWidth, TxBaseline;
	int16_t TxSpacing;
	APTR RP_User;
	uint32_t longreserved[2];
	uint16_t wordreserved[7];
	uint8_t reserved[8];
};

#define JAM1        0
#define JAM2        1
#define COMPLEMENT  2
#define INVERSVID   4

struct AreaInfo {
	APTR VctrTbl, VctrPtr, FlagTbl, FlagPtr;
	int16_t Count, MaxCount, FirstX, FirstY;
};

struct ViewPort {
	APTR Next, ColorMap, DspIns, SprIns, ClrIns, UCopIns;
	int16_t DWidth, DHeight, DxOffset, DyOffset;
	uint16_t Modes;
	uint8_t SpritePriorities, ExtendedModes;
	APTR RasInfo;
};

/* ---- intuition ---- */

struct Screen {
	APTR NextScreen, FirstWindow;
	int16_t LeftEdge, TopEdge, Width, Height, MouseY, MouseX;
	uint16_t Flags;
	APTR Title, DefaultTitle;
	int8_t BarHeight, BarVBorder, BarHBorder, MenuVBorder, MenuHBorder;
	int8_t WBorTop, WBorLeft, WBorRight, WBorBottom;
	APTR Font;                      /* struct TextAttr * */
	struct ViewPort ViewPort;
	struct RastPort RastPort;
	struct BitMap BitMap;
	uint8_t LayerInfo[102];
	APTR FirstGadget;
	uint8_t DetailPen, BlockPen;
	uint16_t SaveColor0;
	APTR BarLayer, ExtData, UserData;
};

struct Window {
	APTR NextWindow;
	int16_t LeftEdge, TopEdge, Width, Height, MouseY, MouseX;
	int16_t MinWidth, MinHeight;
	uint16_t MaxWidth, MaxHeight;
	uint32_t Flags;
	APTR MenuStrip, Title, FirstRequest, DMRequest;
	int16_t ReqCount;
	APTR WScreen, RPort;
	int8_t BorderLeft, BorderTop, BorderRight, BorderBottom;
	APTR BorderRPort, FirstGadget, Parent, Descendant, Pointer;
	int8_t PtrHeight, PtrWidth, XOffset, YOffset;
	uint32_t IDCMPFlags;
	APTR UserPort, WindowPort, MessageKey;
	uint8_t DetailPen, BlockPen;
	APTR CheckMark, ScreenTitle;
	int16_t GZZMouseX, GZZMouseY, GZZWidth, GZZHeight;
	APTR ExtData, UserData, WLayer, IFont;
	uint32_t MoreFlags;
};

struct NewWindow {
	int16_t LeftEdge, TopEdge, Width, Height;
	uint8_t DetailPen, BlockPen;
	uint32_t IDCMPFlags, Flags;
	APTR FirstGadget, CheckMark, Title, Screen, BitMap;
	int16_t MinWidth, MinHeight;
	uint16_t MaxWidth, MaxHeight, Type;
	APTR Extension;                 /* ExtNewWindow, with WFLG_NW_EXTENDED */
};

struct IntuiMessage {
	struct Message ExecMessage;
	uint32_t Class;
	uint16_t Code, Qualifier;
	APTR IAddress;
	int16_t MouseX, MouseY;
	uint32_t Seconds, Micros;
	APTR IDCMPWindow, SpecialLink;
};

struct Gadget {
	APTR NextGadget;
	int16_t LeftEdge, TopEdge, Width, Height;
	uint16_t Flags, Activation, GadgetType;
	APTR GadgetRender, SelectRender, GadgetText;
	int32_t MutualExclude;
	APTR SpecialInfo;
	uint16_t GadgetID;
	APTR UserData;
};

struct PropInfo {
	uint16_t Flags, HorizPot, VertPot, HorizBody, VertBody;
	uint16_t CWidth, CHeight, HPotRes, VPotRes, LeftBorder, TopBorder;
};

struct StringInfo {
	APTR Buffer, UndoBuffer;
	int16_t BufferPos, MaxChars, DispPos, UndoPos, NumChars, DispCount;
	int16_t CLeft, CTop;
	APTR Extension;
	int32_t LongInt;
	APTR AltKeyMap;
};

struct IntuiText {
	uint8_t FrontPen, BackPen, DrawMode, pad;
	int16_t LeftEdge, TopEdge;
	APTR ITextFont, IText, NextText;
};

struct Border {
	int16_t LeftEdge, TopEdge;
	uint8_t FrontPen, BackPen, DrawMode, Count;
	APTR XY, NextBorder;
};

struct Image {
	int16_t LeftEdge, TopEdge, Width, Height, Depth;
	APTR ImageData;
	uint8_t PlanePick, PlaneOnOff;
	APTR NextImage;
};

struct Menu {
	APTR NextMenu;
	int16_t LeftEdge, TopEdge, Width, Height;
	uint16_t Flags;
	APTR MenuName, FirstItem;
	int16_t JazzX, JazzY, BeatX, BeatY;
};

struct MenuItem {
	APTR NextItem;
	int16_t LeftEdge, TopEdge, Width, Height;
	uint16_t Flags;
	int32_t MutualExclude;
	APTR ItemFill, SelectFill;
	int8_t Command, pad;
	APTR SubItem;
	uint16_t NextSelect;
};

struct DrawInfo {
	uint16_t dri_Version, dri_NumPens;
	APTR dri_Pens, dri_Font;
	uint16_t dri_Depth;
	uint16_t dri_ResX, dri_ResY;
	uint32_t dri_Flags;
	APTR dri_CheckMark, dri_AmigaKey;
	uint32_t dri_Reserved[5];
};

struct EasyStruct {
	uint32_t es_StructSize, es_Flags;
	APTR es_Title, es_TextFormat, es_GadgetFormat;
};

/* ---- workbench ---- */

struct WBArg {
	BPTR wa_Lock;
	APTR wa_Name;
};

struct WBStartup {
	struct Message sm_Message;
	APTR sm_Process;
	BPTR sm_Segment;
	int32_t sm_NumArgs;
	APTR sm_ToolWindow, sm_ArgList;
};

struct DiskObject {
	uint16_t do_Magic, do_Version;
	struct Gadget do_Gadget;
	uint8_t do_Type, pad;
	APTR do_DefaultTool, do_ToolTypes;
	int32_t do_CurrentX, do_CurrentY;
	APTR do_DrawerData, do_ToolWindow;
	int32_t do_StackSize;
};

#define GSZ(s, n) _Static_assert(sizeof(struct s) == (n), "sizeof " #s)
GSZ(TextFont, 52); GSZ(BitMap, 40); GSZ(RastPort, 100); GSZ(ViewPort, 40);
GSZ(Screen, 346); GSZ(Window, 136); GSZ(NewWindow, 52); GSZ(IntuiMessage, 52);
GSZ(Gadget, 44); GSZ(PropInfo, 22); GSZ(StringInfo, 36); GSZ(IntuiText, 20);
GSZ(Border, 16); GSZ(Image, 20); GSZ(Menu, 30); GSZ(MenuItem, 34);
GSZ(WBStartup, 40); GSZ(DiskObject, 78);
#undef GSZ

#define CHK(s, f, o) _Static_assert(offsetof(struct s, f) == (o), #s "." #f)
CHK(Library, lib_OpenCnt, 32);
CHK(ExecBase, ThisTask, 276);
CHK(ExecBase, AttnFlags, 296);
CHK(ExecBase, LibList, 378);
CHK(ExecBase, ex_EClockFrequency, 568);
CHK(ExecBase, ex_MemHandler, 628);
CHK(Task, tc_SPLower, 58);
CHK(Task, tc_UserData, 88);
CHK(Process, pr_MsgPort, 92);
CHK(Process, pr_Result2, 148);
CHK(Process, pr_CLI, 172);
CHK(Process, pr_Arguments, 204);
CHK(Process, pr_CES, 224);
CHK(FileInfoBlock, fib_Size, 124);
CHK(FileInfoBlock, fib_Comment, 144);
CHK(CommandLineInterface, cli_Module, 60);
CHK(AnchorPath, ap_Info, 20);
CHK(AnchorPath, ap_Buf, 280);
CHK(DosLibrary, dl_Root, 34);
CHK(Window, RPort, 50);
CHK(Window, UserPort, 86);
CHK(Window, GZZWidth, 112);
CHK(Window, IFont, 128);
CHK(Screen, Font, 40);
CHK(Screen, RastPort, 84);
CHK(Screen, BitMap, 184);
CHK(Screen, FirstGadget, 326);
CHK(RastPort, Font, 52);
CHK(RastPort, TxBaseline, 62);
CHK(TextFont, tf_CharData, 34);
#undef CHK

#endif
