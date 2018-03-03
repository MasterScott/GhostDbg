/*
  Copyright (c) 2018 Victor Sheinmann, Vicshann@gmail.com

  Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), 
  to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, 
  and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, 
  WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE. 
*/

#include "InjectDll.h" 


#pragma comment(linker,"/ENTRY:DLLMain")
#pragma comment(linker,"/NODEFAULTLIB")
    

// ---------- SETTINGS ------------------
UINT IPCSize = (1024*1024)*8;
bool AllowEjectOnDetach = false;
//---------------------------------------
//PHOOK(ProcRtlRestoreContext) HookRtlRestoreContext; 
//PHOOK(ProcKiUserExceptionDispatcher) HookKiUserExceptionDispatcher;
//PHOOK(ProcLdrInitializeThunk) HookLdrInitializeThunk;
PHOOK(ProcRtlDispatchException) HookRtlDispatchException; 
PHOOK(ProcNtUnmapViewOfSection) HookNtUnmapViewOfSection;     
PHOOK(ProcNtMapViewOfSection) HookNtMapViewOfSection;
PHOOK(ProcNtGetContextThread) HookNtGetContextThread;
PHOOK(ProcNtSetContextThread) HookNtSetContextThread;
PHOOK(ProcNtTerminateThread) HookNtTerminateThread;
PHOOK(ProcNtContinue) HookNtContinue;

bool ModInjected = false;
GhDbg::CDbgClient* Dbg = NULL;
DWORD   LastExcThID;    // Helps to reduce overhead of NtContinue hook

LPSTR   LibPathName = NULL;

PBYTE   ThisLibBase = NULL;
SIZE_T  ThisLibSize = 0;

PBYTE   MainExeBase = NULL;
SIZE_T  MainExeSize = 0;

BYTE  SysDirPath[MAX_PATH];
BYTE  StartUpDir[MAX_PATH];
BYTE  CfgFilePath[MAX_PATH];
BYTE  WorkFolder[MAX_PATH];

//===========================================================================
BOOL APIENTRY DLLMain(HMODULE hModule, DWORD ReasonCall, LPVOID lpReserved) 
{
 bool RemTh = (DWORD)hModule & 0x0FFF;      // Normal HMODULE would be aligned at 0x1000  
 if(RemTh || (ReasonCall > 3))    
  {
   //
   // TODO: Prevent multi-entering from different threads when APC injection method used
   //
   hModule      = InjLdr::ModFixInplaceSelf(hModule);   // After this we can access a static variables
   ReasonCall   = DLL_PROCESS_ATTACH;
   ModInjected  = true;   
  }        
 switch(ReasonCall)	    
  {			 
   case DLL_PROCESS_ATTACH:
    {     
     ThisLibBase  = (PBYTE)hModule;
	 MainExeBase  = (PBYTE)GetModuleHandleA(NULL);
     LibPathName  = (LPSTR)&SysDirPath;
     ThisLibSize  = GetRealModuleSize(ThisLibBase);
     MainExeSize  = GetRealModuleSize(MainExeBase);

     GetModuleFileNameA((HMODULE)hModule,(LPSTR)&StartUpDir,sizeof(StartUpDir));        
     GetSystemDirectoryA((LPSTR)&SysDirPath,sizeof(SysDirPath));
     lstrcatA((LPSTR)&SysDirPath,"\\");
     lstrcatA((LPSTR)&SysDirPath,GetFileName((LPSTR)&StartUpDir));
     GetModuleFileNameA((HMODULE)MainExeBase,(LPSTR)&StartUpDir,sizeof(StartUpDir));        
        
	 lstrcpyA((LPSTR)&WorkFolder, (LPSTR)&StartUpDir);
     TrimFilePath((LPSTR)&WorkFolder);
//     lstrcatA((LPSTR)&WorkFolder,".LOGS\\");

#ifndef NOLOG
     lstrcpyA((LPSTR)&LogFilePath,(LPSTR)&WorkFolder);
	 lstrcatA((LPSTR)&LogFilePath,GetFileName((LPSTR)&StartUpDir));
     lstrcatA((LPSTR)&LogFilePath,LOGFILE);
#endif
     lstrcpyA((LPSTR)&CfgFilePath,(LPSTR)&WorkFolder);
     lstrcatA((LPSTR)&CfgFilePath, CFGFILE);	

     CreateDirectoryPath((LPSTR)&WorkFolder);
     LoadConfiguration();	
	 if(LogMode & lmCons){AllocConsole();/*SetWinConsoleSizes(1000, 500, 1000, 500);*/}
	 LOGMSG("Time=%08X, Owner='%s'", (DWORD)GetTime64(),(LPSTR)&StartUpDir);	
     TrimFilePath((LPSTR)&StartUpDir);
     LOGMSG("WorkFolder: %s", (LPSTR)&WorkFolder);
     LOGMSG("StartUpDir: %s", (LPSTR)&StartUpDir);
     LOGMSG("SysDirPath: %s", (LPSTR)&SysDirPath);
//	 LOGMSG("HookMod=%p, RealMod=%p", hModule, hRealMod);
     if(!InitApplication())return false;    
    }
     break;									
   case DLL_THREAD_ATTACH:
     if(Dbg && Dbg->IsActive())Dbg->Report_CREATE_THREAD_DEBUG_EVENT(GetCurrentThreadId());     // For a simple testing
     break; 
   case DLL_THREAD_DETACH:
     if(Dbg && Dbg->IsActive())Dbg->Report_EXIT_THREAD_DEBUG_EVENT(GetCurrentThreadId(),0);     // For a simple testing
     break;
   case DLL_PROCESS_DETACH: 
     if(Dbg && Dbg->IsActive())Dbg->Report_EXIT_PROCESS_DEBUG_EVENT(0);      // For a simple testing
     UnInitApplication();     
	 break;

   default : return false;  
  }
 if(RemTh){LOGMSG("Terminating injected thread: %u", GetCurrentThreadId()); TerminateThread(GetCurrentThread(),0);}     //Stack frame may be incorrect
 return true;
}
//====================================================================================
void _stdcall LoadConfiguration(void)
{    
 CJSonItem Root;
 CMiniStr  str;
 str.FromFile((LPSTR)&CfgFilePath);
 bool BinFmt = (str.Length())?(CJSonItem::IsBinaryEncrypted(str.c_data()) >= 0):(0);
 LOGMSG("Loading config(Bin=%u): %s", BinFmt, (LPSTR)&CfgFilePath);
 if(str.Length())Root.FromString(str);
 CJSonItem* Params = EnsureJsnParam(jsObject, "Parameters", &Root);  
 LogMode       = EnsureJsnParam((int)LogMode,       "LogMode",       Params)->GetValInt();
 IPCSize       = EnsureJsnParam(IPCSize,            "IPCSize",       Params)->GetValInt();  
 AllowEjectOnDetach = EnsureJsnParam(AllowEjectOnDetach,         "AllowEjectOnDetach",    Params)->GetValBol();  
        
 CJSonItem* DbgParams = EnsureJsnParam(jsObject, "DbgClient", &Root);   
 if(Dbg)
  {
   Dbg->HideDbgState = EnsureJsnParam(Dbg->HideDbgState,        "HideDbgState",   DbgParams)->GetValBol(); 
   Dbg->AllowPrTerm  = EnsureJsnParam(Dbg->AllowPrTerm,         "AllowPrTerm",    DbgParams)->GetValBol(); 
   Dbg->AllowThTerm  = EnsureJsnParam(Dbg->AllowThTerm,         "AllowThTerm",    DbgParams)->GetValBol(); 
   Dbg->OnlyOwnSwBP  = EnsureJsnParam(Dbg->OnlyOwnSwBP,         "OnlyOwnSwBP",    DbgParams)->GetValBol(); 
   Dbg->OnlyOwnHwBP  = EnsureJsnParam(Dbg->OnlyOwnHwBP,         "OnlyOwnHwBP",    DbgParams)->GetValBol(); 
   Dbg->OnlyOwnTF    = EnsureJsnParam(Dbg->OnlyOwnTF,           "OnlyOwnTF",      DbgParams)->GetValBol(); 
   Dbg->SwBpVal      = EnsureJsnParam((UINT)Dbg->SwBpVal,       "SwBpVal",        DbgParams)->GetValBol(); 
  }
 LOGMSG("Saving config(Bin=%u): %s", BinFmt, (LPSTR)&CfgFilePath);
 str.Clear();
 if(BinFmt)Root.ToBinary(str,true);
 Root.ToString(str,true);
 str.ToFile((LPSTR)&CfgFilePath);  
}
//------------------------------------------------------------------------------------
void _stdcall SaveConfiguration(int BinFmt)
{
 CJSonItem Root;
 CMiniStr  str;
 str.FromFile((LPSTR)&CfgFilePath);
 bool VBinFmt = (str.Length() || (BinFmt < 0))?(CJSonItem::IsBinaryEncrypted(str.c_data()) >= 0):(BinFmt > 0);
 LOGMSG("Loading config(Bin=%u): %s", VBinFmt, (LPSTR)&CfgFilePath);
 if(str.Length())Root.FromString(str);
 CJSonItem* Params = EnsureJsnParam(jsObject, "Parameters", &Root);  
 LogMode       = SetJsnParamValue((int)LogMode,       "LogMode",       Params)->GetValInt();
 IPCSize       = SetJsnParamValue(IPCSize,            "IPCSize",       Params)->GetValInt();   
 AllowEjectOnDetach       = SetJsnParamValue(AllowEjectOnDetach,         "AllowEjectOnDetach",    Params)->GetValBol();   
     
 CJSonItem* DbgParams = EnsureJsnParam(jsObject, "DbgClient", &Root);   
 if(Dbg)
  {
   SetJsnParamValue(Dbg->HideDbgState,        "HideDbgState",   DbgParams)->GetValBol(); 
   SetJsnParamValue(Dbg->AllowPrTerm,         "AllowPrTerm",    DbgParams)->GetValBol(); 
   SetJsnParamValue(Dbg->AllowThTerm,         "AllowThTerm",    DbgParams)->GetValBol(); 
   SetJsnParamValue(Dbg->OnlyOwnSwBP,         "OnlyOwnSwBP",    DbgParams)->GetValBol(); 
   SetJsnParamValue(Dbg->OnlyOwnHwBP,         "OnlyOwnHwBP",    DbgParams)->GetValBol(); 
   SetJsnParamValue(Dbg->OnlyOwnTF,           "OnlyOwnTF",      DbgParams)->GetValBol(); 
   SetJsnParamValue((UINT)Dbg->SwBpVal,       "SwBpVal",        DbgParams)->GetValBol(); 
  }
 LOGMSG("Saving config(Bin=%u): %s", BinFmt, (LPSTR)&CfgFilePath);
 str.Clear();
 if(VBinFmt)Root.ToBinary(str,true);
 Root.ToString(str,true);
 str.ToFile((LPSTR)&CfgFilePath);
}
//------------------------------------------------------------------------------------
bool _stdcall InitApplication(void)
{
 LOGMSG("Enter");
 if(GhDbg::CDbgClient::IsExistForID(GetCurrentProcessId())){LOGMSG("Already injected!"); return false;}

/*#ifdef _AMD64_
 HookRtlRestoreContext.SetHook("RtlRestoreContext","ntdll.dll");
#endif
 HookKiUserExceptionDispatcher.SetHook("KiUserExceptionDispatcher","ntdll.dll");    
 HookLdrInitializeThunk.SetHook("LdrInitializeThunk","ntdll.dll");     */

 Dbg = new GhDbg::CDbgClient;
 Dbg->UsrReqCallback = &DbgUsrReqCallback;
 LoadConfiguration();
 LOGMSG("IPC created");
 HookNtMapViewOfSection.SetHook("NtMapViewOfSection","ntdll.dll");
 HookNtUnmapViewOfSection.SetHook("NtUnmapViewOfSection","ntdll.dll"); 
 HookRtlDispatchException.SetHook((PBYTE)GhDbg::SNtDll::FindRtlDispatchException());  
 HookNtGetContextThread.SetHook("NtGetContextThread","ntdll.dll");
 HookNtSetContextThread.SetHook("NtSetContextThread","ntdll.dll");
 HookNtTerminateThread.SetHook("NtTerminateThread","ntdll.dll");   
 HookNtContinue.SetHook("NtContinue","ntdll.dll"); 
 LOGMSG("Hooks set");             
 Dbg->Start(IPCSize);        // Start it from DLL Main to avoid of similair DLL being loaded again
 LOGMSG("IPC started");
 return true;
}
//------------------------------------------------------------------------------------
void _stdcall UnInitApplication(void)
{
 HookNtContinue.Remove();
 HookNtTerminateThread.Remove();
 HookRtlDispatchException.Remove();   
 HookNtUnmapViewOfSection.Remove();
 HookNtMapViewOfSection.Remove(); 
 HookNtSetContextThread.Remove();
 HookNtGetContextThread.Remove();
 LOGMSG("Hooks removed");
 if(Dbg)delete(Dbg);
 LOGMSG("IPC destroyed");
}
//------------------------------------------------------------------------------------
int _stdcall DbgUsrReqCallback(ShMem::CMessageIPC::SMsgHdr* Req, PVOID ArgA, UINT ArgB)
{
 if(Req->MsgID == GhDbg::miDbgGetConfigs)
  {
   ShMem::CArgPack<>* apo = (ShMem::CArgPack<>*)ArgA;
   if(ModInjected)apo->PushArgEx(AllowEjectOnDetach, "Allow Eject On Detach", GhDbg::CDbgClient::MakeCfgItemID(++ArgB,GhDbg::dtBool));    
   if(ModInjected){bool Nons = false; apo->PushArgEx(Nons, "Eject", GhDbg::CDbgClient::MakeCfgItemID(++ArgB,GhDbg::dtBool));}    
   return ArgB;
  }
 if(Req->MsgID == GhDbg::miDbgSetConfigs)
  {
   UINT CfgIdx = 0;
   UINT Type   = GhDbg::CDbgClient::ReadCfgItemID(ArgB, &CfgIdx);
   if(ArgA)
    {
     switch(CfgIdx)                            // New Process Injection
      {
       case 1:
         AllowEjectOnDetach = *(bool*)ArgA;
        break;
       case 2:  
         if(ModInjected)
          {
           LOGMSG("Ejecting by user!");
           UnInitApplication();
           LOGMSG("Uninit done. Unmapping...");
           InjLdr::UnmapAndTerminateSelf(ThisLibBase);
          }
        break;
      }
    }
   SaveConfiguration();
   return 0;
  }
 if(Req->MsgID == GhDbg::miDbgDetachNtfy)
  {
   if(AllowEjectOnDetach && ModInjected)
    {
     LOGMSG("Ejecting on Detach!");
     UnInitApplication();
     LOGMSG("Uninit done. Unmapping...");
     InjLdr::UnmapAndTerminateSelf(ThisLibBase);
    }
  }
 return 0;
}
//====================================================================================
//                           Debugger support
//------------------------------------------------------------------------------------
// x32:  [ESP]   = EXCEPTION_RECORD*    // Get it with '_ReturnAddress()'
//       [ESP+4] = CONTEXT*
//
// x64:  CONTEXT           // sizeof(CONTEXT) is 0x04E8  // Size of this block is 0x04F0 (On Win7 and Win10 x64) (Aligned to 16?)
//       EXCEPTION_RECORD
//
/*__declspec(noreturn) void _fastcall ProcKiUserExceptionDispatcher(void)  
{
 PBYTE FramePtr = (PBYTE)_AddressOfReturnAddress();   // No return address on stack    // Must match value of ESP on enter to this function
#ifdef _AMD64_
 PCONTEXT Context = (PCONTEXT)FramePtr;
 PEXCEPTION_RECORD ExceptionRecord = (PEXCEPTION_RECORD)&FramePtr[0x4F0];       // AlignFrwd(sizeof(CONTEXT),16)]  ??????????????????????????
#else
 PCONTEXT Context = (PCONTEXT)((PVOID*)FramePtr)[1];
 PEXCEPTION_RECORD ExceptionRecord = (PEXCEPTION_RECORD)((PVOID*)FramePtr)[0];
#endif 
 //
 HookKiUserExceptionDispatcher.OrigProc();     // Must be tail optimized - Requires optimization to be enabled (O1,O2,Ox)
}
//------------------------------------------------------------------------------------
__declspec(noreturn) VOID NTAPI ProcRtlRestoreContext(PCONTEXT ContextRecord, PEXCEPTION_RECORD ExceptionRecord)
{                    
 if(Dbg && Dbg->IsActive())Dbg->DebugThreadLoad(GetCurrentThreadId(), ContextRecord);
 HookRtlRestoreContext.OrigProc(ContextRecord, ExceptionRecord);   
} 
//------------------------------------------------------------------------------------
void NTAPI ProcLdrInitializeThunk(PVOID ArgA, PVOID ArgB, PVOID ArgC, PVOID ArgD)
{
 if(Dbg && Dbg->IsActive())Dbg->GetThread(GetCurrentThreadId());      
 HookLdrInitializeThunk.OrigProc(ArgA, ArgB, ArgC, ArgD);     // Must be tail optimized - Requires optimization to be enabled (O1,O2,Ox)
}  */
//------------------------------------------------------------------------------------
BOOLEAN NTAPI ProcRtlDispatchException(PEXCEPTION_RECORD ExceptionRecord, PCONTEXT ContextRecord)
{
 DWORD ThID = LastExcThID = GetCurrentThreadId();
 if(!Dbg || !Dbg->IsActive() || Dbg->IsDbgThreadID(ThID))return HookRtlDispatchException.OrigProc(ExceptionRecord, ContextRecord);
 if(Dbg->HandleException(ThID, ExceptionRecord, ContextRecord))return true;    // Handled by a debugger
 if(!Dbg->HideDbgState)HookRtlDispatchException.OrigProc(ExceptionRecord, ContextRecord);
 CONTEXT ForgedCtx;                            // Can it be detected that this is a copy of original CONTEXT and have a different address on stack?
 memcpy(&ForgedCtx,ContextRecord,sizeof(CONTEXT));
 Dbg->DebugThreadLoad(ThID, &ForgedCtx);       // Load any previous DRx modifications from internal buffer     
 BOOLEAN res = HookRtlDispatchException.OrigProc(ExceptionRecord, &ForgedCtx);
 Dbg->DebugThreadSave(ThID, &ForgedCtx);  // Save any modifications to DRx in a separate struct 
 Dbg->DebugRstExcContext(ContextRecord, &ForgedCtx);
 return res;    
}
//------------------------------------------------------------------------------------
// SEC_FILE             0x0800000     
// SEC_IMAGE            0x1000000     
// SEC_PROTECTED_IMAGE  0x2000000  
NTSTATUS NTAPI ProcNtMapViewOfSection(HANDLE SectionHandle, HANDLE ProcessHandle, PVOID* BaseAddress, ULONG_PTR ZeroBits, SIZE_T CommitSize, PLARGE_INTEGER SectionOffset, PSIZE_T ViewSize, SECTION_INHERIT InheritDisposition, ULONG AllocationType, ULONG Win32Protect)
{              
 NTSTATUS res = HookNtMapViewOfSection.OrigProc(SectionHandle,ProcessHandle,BaseAddress,ZeroBits,CommitSize,SectionOffset,ViewSize,InheritDisposition,AllocationType,Win32Protect); 
 if(!res && (ProcessHandle == NtCurrentProcess) && BaseAddress && *BaseAddress && ViewSize && *ViewSize && Dbg && Dbg->IsActive() && IsValidPEHeaderBlk(*BaseAddress, Dbg->IsMemAvailable(*BaseAddress)))    // Try to get the module`s name?
  {            
   LOGMSG("Module: Status=%08X, SectionHandle=%p, BaseAddress=%p, ViewSize=%08X, AllocationType=%08X, Win32Protect=%08X",res,SectionHandle,*BaseAddress,*ViewSize,AllocationType,Win32Protect);
   if(Dbg && Dbg->IsActive())Dbg->Report_LOAD_DLL_DEBUG_INFO(*BaseAddress);    // Events:TLS Callbacks must be disabled or xg4dbg will crash in 'cbLoadDll{ auto modInfo = ModInfoFromAddr(duint(base));}' (because it won`t check for NULL) if this mapping will be unmapped too soon
  } 
//   else {LOGMSG("Status=%08X, SectionHandle=%p, ViewSize=%08X, AllocationType=%08X, Win32Protect=%08X",res,SectionHandle,ViewSize,AllocationType,Win32Protect);}
 return res;
}
//------------------------------------------------------------------------------------
NTSTATUS NTAPI ProcNtUnmapViewOfSection(HANDLE ProcessHandle, PVOID BaseAddress)
{
 if((ProcessHandle == NtCurrentProcess) && Dbg && Dbg->IsActive() && IsValidPEHeaderBlk(BaseAddress, Dbg->IsMemAvailable(BaseAddress)))
  {
   LOGMSG("BaseAddress=%p",BaseAddress);
   if(Dbg && Dbg->IsActive() && Dbg->IsOtherConnections())Dbg->Report_UNLOAD_DLL_DEBUG_EVENT(BaseAddress); 
  }                                            
 return HookNtUnmapViewOfSection.OrigProc(ProcessHandle,BaseAddress);
}
//------------------------------------------------------------------------------------
// Called at start of a thread and at return from APC/Exception
// Protect CONTEXT here? Can be used separately from any kernel callback?
// On Win10 x64 it is called by RtlRestoreContext for normal exceptions but it is not used on Win7 x64  
// On x32 it is called from LdrInitializeThunk when a new thread created (User mode thread`s entry point) 
// Hooking LdrInitializeThunk is hard
//
NTSTATUS NTAPI ProcNtContinue(PCONTEXT ContextRecord, BOOLEAN TestAlert)        // NOTE: Too much overhead of exception processing with this(Twice 'GetThread' on breakpoints)
{
 if(Dbg && (GetCurrentThreadId() != LastExcThID) && Dbg->IsActive())Dbg->GetThread(GetCurrentThreadId());       // Report this thread if it is not in list yet
 return HookNtContinue.OrigProc(ContextRecord, TestAlert);   // Will not return
}
//------------------------------------------------------------------------------------
// Exit from a thread`s proc will also end up here  
NTSTATUS NTAPI ProcNtTerminateThread(HANDLE ThreadHandle, NTSTATUS ExitStatus)
{
 if(Dbg && Dbg->IsActive())Dbg->Report_EXIT_THREAD_DEBUG_EVENT(GetCurrentThreadId(),0);  
 return HookNtTerminateThread.OrigProc(ThreadHandle, ExitStatus);
}
//------------------------------------------------------------------------------------    
NTSTATUS NTAPI ProcNtGetContextThread(HANDLE ThreadHandle, PCONTEXT Context)
{  
 NTSTATUS res = HookNtGetContextThread.OrigProc(ThreadHandle, Context); 
 if(!res && Dbg && Dbg->IsActive() || !Dbg->HideDbgState)Dbg->DebugThreadLoad(GetCurrentThreadId(), Context);   // Load into CONTEXT a previously saved DRx instead of currently read ones
 return res;
}
//------------------------------------------------------------------------------------    
NTSTATUS NTAPI ProcNtSetContextThread(HANDLE ThreadHandle, PCONTEXT Context)   // Do not let DRx to be changed by this
{
 if(!Dbg || !Dbg->IsActive() || !Dbg->HideDbgState)return HookNtSetContextThread.OrigProc(ThreadHandle, Context);
 CONTEXT FCtx;      // Copy of CONTEXT where CONTEXT_DEBUG_REGISTERS is removed (CONTEXT_DEBUG_REGISTERS must be preserved in original Context in case it may be checked)
 Dbg->DebugThreadSave(GetCurrentThreadId(), Context); 
 memcpy(&FCtx,Context,sizeof(CONTEXT));
 FCtx.ContextFlags &= ~0x00000010;   // CONTEXT_DEBUG_REGISTERS     // TF is allowed to change? 
 return HookNtSetContextThread.OrigProc(ThreadHandle, &FCtx);
}
//------------------------------------------------------------------------------------      
// ntdll.NtTerminateProcess


#pragma optimize( "", off )

#pragma code_seg()

//====================================================================================
//								 WRAPPER FUNCTIONS
//------------------------------------------------------------------------------------
namespace ProxyExport
{
// winspool.drv
/*APIWRAPPER(LibPathName, GetDefaultPrinterA)
APIWRAPPER(LibPathName, AbortPrinter)
APIWRAPPER(LibPathName, AddFormA)
APIWRAPPER(LibPathName, AddFormW)
APIWRAPPER(LibPathName, AddJobA)
APIWRAPPER(LibPathName, AddJobW)
APIWRAPPER(LibPathName, AddMonitorA)
APIWRAPPER(LibPathName, AddMonitorW)
APIWRAPPER(LibPathName, AddPortA)
APIWRAPPER(LibPathName, AddPortExA)
APIWRAPPER(LibPathName, AddPortExW)
APIWRAPPER(LibPathName, AddPortW)
APIWRAPPER(LibPathName, AddPrintProcessorA)
APIWRAPPER(LibPathName, AddPrintProcessorW)
APIWRAPPER(LibPathName, AddPrintProvidorA)
APIWRAPPER(LibPathName, AddPrintProvidorW)
APIWRAPPER(LibPathName, AddPrinterA)
APIWRAPPER(LibPathName, AddPrinterConnection2A)
APIWRAPPER(LibPathName, AddPrinterConnection2W)
APIWRAPPER(LibPathName, AddPrinterConnectionA)
APIWRAPPER(LibPathName, AddPrinterConnectionW)
APIWRAPPER(LibPathName, AddPrinterDriverA)
APIWRAPPER(LibPathName, AddPrinterDriverExA)
APIWRAPPER(LibPathName, AddPrinterDriverExW)
APIWRAPPER(LibPathName, AddPrinterDriverW)
APIWRAPPER(LibPathName, AddPrinterW)
APIWRAPPER(LibPathName, AdvancedDocumentPropertiesA)
APIWRAPPER(LibPathName, AdvancedDocumentPropertiesW)
APIWRAPPER(LibPathName, AdvancedSetupDialog)
APIWRAPPER(LibPathName, CheckSignatureInFile)
APIWRAPPER(LibPathName, ClosePrinter)
APIWRAPPER(LibPathName, CloseSpoolFileHandle)
APIWRAPPER(LibPathName, CommitSpoolData)
APIWRAPPER(LibPathName, ConfigurePortA)
APIWRAPPER(LibPathName, ConfigurePortW)
APIWRAPPER(LibPathName, ConnectToPrinterDlg)
APIWRAPPER(LibPathName, ConvertAnsiDevModeToUnicodeDevmode)
APIWRAPPER(LibPathName, ConvertUnicodeDevModeToAnsiDevmode)
APIWRAPPER(LibPathName, CorePrinterDriverInstalledA)
APIWRAPPER(LibPathName, CorePrinterDriverInstalledW)
APIWRAPPER(LibPathName, CreatePrintAsyncNotifyChannel)
APIWRAPPER(LibPathName, CreatePrinterIC)
APIWRAPPER(LibPathName, DeleteFormA)
APIWRAPPER(LibPathName, DeleteFormW)
APIWRAPPER(LibPathName, DeleteMonitorA)
APIWRAPPER(LibPathName, DeleteMonitorW)
APIWRAPPER(LibPathName, DeletePortA)
APIWRAPPER(LibPathName, DeletePortW)
APIWRAPPER(LibPathName, DeletePrintProcessorA)
APIWRAPPER(LibPathName, DeletePrintProcessorW)
APIWRAPPER(LibPathName, DeletePrintProvidorA)
APIWRAPPER(LibPathName, DeletePrintProvidorW)
APIWRAPPER(LibPathName, DeletePrinter)
APIWRAPPER(LibPathName, DeletePrinterConnectionA)
APIWRAPPER(LibPathName, DeletePrinterConnectionW)
APIWRAPPER(LibPathName, DeletePrinterDataA)
APIWRAPPER(LibPathName, DeletePrinterDataExA)
APIWRAPPER(LibPathName, DeletePrinterDataExW)
APIWRAPPER(LibPathName, DeletePrinterDataW)
APIWRAPPER(LibPathName, DeletePrinterDriverA)
APIWRAPPER(LibPathName, DeletePrinterDriverExA)
APIWRAPPER(LibPathName, DeletePrinterDriverExW)
APIWRAPPER(LibPathName, DeletePrinterDriverPackageA)
APIWRAPPER(LibPathName, DeletePrinterDriverPackageW)
APIWRAPPER(LibPathName, DeletePrinterDriverW)
APIWRAPPER(LibPathName, DeletePrinterIC)
APIWRAPPER(LibPathName, DeletePrinterKeyA)
APIWRAPPER(LibPathName, DeletePrinterKeyW)
APIWRAPPER(LibPathName, DevQueryPrint)
APIWRAPPER(LibPathName, DevQueryPrintEx)
APIWRAPPER(LibPathName, DeviceCapabilitiesA)
APIWRAPPER(LibPathName, DeviceCapabilitiesW)
APIWRAPPER(LibPathName, DeviceMode)
APIWRAPPER(LibPathName, DevicePropertySheets)
APIWRAPPER(LibPathName, DocumentEvent)
APIWRAPPER(LibPathName, DocumentPropertiesA)
APIWRAPPER(LibPathName, DocumentPropertiesW)
APIWRAPPER(LibPathName, DocumentPropertySheets)
APIWRAPPER(LibPathName, EndDocPrinter)
APIWRAPPER(LibPathName, EndPagePrinter)
APIWRAPPER(LibPathName, EnumFormsA)
APIWRAPPER(LibPathName, EnumFormsW)
APIWRAPPER(LibPathName, EnumJobsA)
APIWRAPPER(LibPathName, EnumJobsW)
APIWRAPPER(LibPathName, EnumMonitorsA)
APIWRAPPER(LibPathName, EnumMonitorsW)
APIWRAPPER(LibPathName, EnumPortsA)
APIWRAPPER(LibPathName, EnumPortsW)
APIWRAPPER(LibPathName, EnumPrintProcessorDatatypesA)
APIWRAPPER(LibPathName, EnumPrintProcessorDatatypesW)
APIWRAPPER(LibPathName, EnumPrintProcessorsA)
APIWRAPPER(LibPathName, EnumPrintProcessorsW)
APIWRAPPER(LibPathName, EnumPrinterDataA)
APIWRAPPER(LibPathName, EnumPrinterDataExA)
APIWRAPPER(LibPathName, EnumPrinterDataExW)
APIWRAPPER(LibPathName, EnumPrinterDataW)
APIWRAPPER(LibPathName, EnumPrinterDriversA)
APIWRAPPER(LibPathName, EnumPrinterDriversW)
APIWRAPPER(LibPathName, EnumPrinterKeyA)
APIWRAPPER(LibPathName, EnumPrinterKeyW)
APIWRAPPER(LibPathName, EnumPrintersA)
APIWRAPPER(LibPathName, EnumPrintersW)
APIWRAPPER(LibPathName, ExtDeviceMode)
APIWRAPPER(LibPathName, FindClosePrinterChangeNotification)
APIWRAPPER(LibPathName, FindFirstPrinterChangeNotification)
APIWRAPPER(LibPathName, FindNextPrinterChangeNotification)
APIWRAPPER(LibPathName, FlushPrinter)
APIWRAPPER(LibPathName, FreePrinterNotifyInfo)
APIWRAPPER(LibPathName, GetCorePrinterDriversA)
APIWRAPPER(LibPathName, GetCorePrinterDriversW)
APIWRAPPER(LibPathName, GetDefaultPrinterW)
APIWRAPPER(LibPathName, GetFormA)
APIWRAPPER(LibPathName, GetFormW)
APIWRAPPER(LibPathName, GetJobA)
APIWRAPPER(LibPathName, GetJobW)
APIWRAPPER(LibPathName, GetPrintExecutionData)
APIWRAPPER(LibPathName, GetPrintProcessorDirectoryA)
APIWRAPPER(LibPathName, GetPrintProcessorDirectoryW)
APIWRAPPER(LibPathName, GetPrinterA)
APIWRAPPER(LibPathName, GetPrinterDataA)
APIWRAPPER(LibPathName, GetPrinterDataExA)
APIWRAPPER(LibPathName, GetPrinterDataExW)
APIWRAPPER(LibPathName, GetPrinterDataW)
APIWRAPPER(LibPathName, GetPrinterDriver2A)
APIWRAPPER(LibPathName, GetPrinterDriver2W)
APIWRAPPER(LibPathName, GetPrinterDriverA)
APIWRAPPER(LibPathName, GetPrinterDriverDirectoryA)
APIWRAPPER(LibPathName, GetPrinterDriverDirectoryW)
APIWRAPPER(LibPathName, GetPrinterDriverPackagePathA)
APIWRAPPER(LibPathName, GetPrinterDriverPackagePathW)
APIWRAPPER(LibPathName, GetPrinterDriverW)
APIWRAPPER(LibPathName, GetPrinterW)
APIWRAPPER(LibPathName, GetSpoolFileHandle)
APIWRAPPER(LibPathName, InstallPrinterDriverFromPackageA)
APIWRAPPER(LibPathName, InstallPrinterDriverFromPackageW)
APIWRAPPER(LibPathName, IsValidDevmodeA)
APIWRAPPER(LibPathName, IsValidDevmodeW)
APIWRAPPER(LibPathName, OpenPrinter2A)
APIWRAPPER(LibPathName, OpenPrinter2W)
APIWRAPPER(LibPathName, OpenPrinterA)
APIWRAPPER(LibPathName, OpenPrinterW)
APIWRAPPER(LibPathName, PerfClose)
APIWRAPPER(LibPathName, PerfCollect)
APIWRAPPER(LibPathName, PerfOpen)
APIWRAPPER(LibPathName, PlayGdiScriptOnPrinterIC)
APIWRAPPER(LibPathName, PrinterMessageBoxA)
APIWRAPPER(LibPathName, PrinterMessageBoxW)
APIWRAPPER(LibPathName, PrinterProperties)
APIWRAPPER(LibPathName, QueryColorProfile)
APIWRAPPER(LibPathName, QueryRemoteFonts)
APIWRAPPER(LibPathName, QuerySpoolMode)
APIWRAPPER(LibPathName, ReadPrinter)
APIWRAPPER(LibPathName, RegisterForPrintAsyncNotifications)
APIWRAPPER(LibPathName, ReportJobProcessingProgress)
APIWRAPPER(LibPathName, ResetPrinterA)
APIWRAPPER(LibPathName, ResetPrinterW)
APIWRAPPER(LibPathName, ScheduleJob)
APIWRAPPER(LibPathName, SeekPrinter)
APIWRAPPER(LibPathName, SetDefaultPrinterA)
APIWRAPPER(LibPathName, SetDefaultPrinterW)
APIWRAPPER(LibPathName, SetFormA)
APIWRAPPER(LibPathName, SetFormW)
APIWRAPPER(LibPathName, SetJobA)
APIWRAPPER(LibPathName, SetJobW)
APIWRAPPER(LibPathName, SetPortA)
APIWRAPPER(LibPathName, SetPortW)
APIWRAPPER(LibPathName, SetPrinterA)
APIWRAPPER(LibPathName, SetPrinterDataA)
APIWRAPPER(LibPathName, SetPrinterDataExA)
APIWRAPPER(LibPathName, SetPrinterDataExW)
APIWRAPPER(LibPathName, SetPrinterDataW)
APIWRAPPER(LibPathName, SetPrinterW)
APIWRAPPER(LibPathName, SplDriverUnloadComplete)
APIWRAPPER(LibPathName, SpoolerDevQueryPrintW)
APIWRAPPER(LibPathName, SpoolerPrinterEvent)
APIWRAPPER(LibPathName, StartDocDlgA)
APIWRAPPER(LibPathName, StartDocDlgW)
APIWRAPPER(LibPathName, StartDocPrinterA)
APIWRAPPER(LibPathName, StartDocPrinterW)
APIWRAPPER(LibPathName, StartPagePrinter)
APIWRAPPER(LibPathName, SystemFunction035)
APIWRAPPER(LibPathName, UnRegisterForPrintAsyncNotifications)
APIWRAPPER(LibPathName, UploadPrinterDriverPackageA)
APIWRAPPER(LibPathName, UploadPrinterDriverPackageW)
APIWRAPPER(LibPathName, WaitForPrinterChange)
APIWRAPPER(LibPathName, WritePrinter)
APIWRAPPER(LibPathName, XcvDataW) */

// cryptsp.dll
/*APIWRAPPER(LibPathName, CryptAcquireContextA)
APIWRAPPER(LibPathName, CryptAcquireContextW)
APIWRAPPER(LibPathName, CryptContextAddRef)
APIWRAPPER(LibPathName, CryptCreateHash)
APIWRAPPER(LibPathName, CryptDecrypt)
APIWRAPPER(LibPathName, CryptDeriveKey)
APIWRAPPER(LibPathName, CryptDestroyHash)
APIWRAPPER(LibPathName, CryptDestroyKey)
APIWRAPPER(LibPathName, CryptDuplicateHash)
APIWRAPPER(LibPathName, CryptDuplicateKey)
APIWRAPPER(LibPathName, CryptEncrypt)
APIWRAPPER(LibPathName, CryptEnumProviderTypesA)
APIWRAPPER(LibPathName, CryptEnumProviderTypesW)
APIWRAPPER(LibPathName, CryptEnumProvidersA)
APIWRAPPER(LibPathName, CryptEnumProvidersW)
APIWRAPPER(LibPathName, CryptExportKey)
APIWRAPPER(LibPathName, CryptGenKey)
APIWRAPPER(LibPathName, CryptGenRandom)
APIWRAPPER(LibPathName, CryptGetDefaultProviderA)
APIWRAPPER(LibPathName, CryptGetDefaultProviderW)
APIWRAPPER(LibPathName, CryptGetHashParam)
APIWRAPPER(LibPathName, CryptGetKeyParam)
APIWRAPPER(LibPathName, CryptGetProvParam)
APIWRAPPER(LibPathName, CryptGetUserKey)
APIWRAPPER(LibPathName, CryptHashData)
APIWRAPPER(LibPathName, CryptHashSessionKey)
APIWRAPPER(LibPathName, CryptImportKey)
APIWRAPPER(LibPathName, CryptReleaseContext)
APIWRAPPER(LibPathName, CryptSetHashParam)
APIWRAPPER(LibPathName, CryptSetKeyParam)
APIWRAPPER(LibPathName, CryptSetProvParam)
APIWRAPPER(LibPathName, CryptSetProviderA)
APIWRAPPER(LibPathName, CryptSetProviderExA)
APIWRAPPER(LibPathName, CryptSetProviderExW)
APIWRAPPER(LibPathName, CryptSetProviderW)
APIWRAPPER(LibPathName, CryptSignHashA)
APIWRAPPER(LibPathName, CryptSignHashW)
APIWRAPPER(LibPathName, CryptVerifySignatureA)
APIWRAPPER(LibPathName, CryptVerifySignatureW)  */

// version.dll
/*APIWRAPPER(LibPathName, GetFileVersionInfoA)
APIWRAPPER(LibPathName, GetFileVersionInfoByHandle)
APIWRAPPER(LibPathName, GetFileVersionInfoExA)
APIWRAPPER(LibPathName, GetFileVersionInfoExW)
APIWRAPPER(LibPathName, GetFileVersionInfoSizeA)
APIWRAPPER(LibPathName, GetFileVersionInfoSizeExA)
APIWRAPPER(LibPathName, GetFileVersionInfoSizeExW)
APIWRAPPER(LibPathName, GetFileVersionInfoSizeW)
APIWRAPPER(LibPathName, GetFileVersionInfoW)
APIWRAPPER(LibPathName, VerFindFileA)
APIWRAPPER(LibPathName, VerFindFileW)
APIWRAPPER(LibPathName, VerInstallFileA)
APIWRAPPER(LibPathName, VerInstallFileW)
APIWRAPPER(LibPathName, VerLanguageNameA)
APIWRAPPER(LibPathName, VerLanguageNameW)
APIWRAPPER(LibPathName, VerQueryValueA)
APIWRAPPER(LibPathName, VerQueryValueW) */ 

}
//====================================================================================


