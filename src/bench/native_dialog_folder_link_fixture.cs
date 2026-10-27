using System;
using System.Text;
using System.IO;
using System.Threading;
using System.Diagnostics;
using System.Runtime.InteropServices;

[StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] struct Filter { [MarshalAs(UnmanagedType.LPWStr)] public string name; [MarshalAs(UnmanagedType.LPWStr)] public string spec; }
[ComImport, Guid("43826D1E-E718-42EE-BC55-A1E261C37BFE"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)] interface IShellItem {
 [PreserveSig] int BindToHandler(IntPtr a, ref Guid b, ref Guid c, out IntPtr d);
 [PreserveSig] int GetParent(out IShellItem p);
 [PreserveSig] int GetDisplayName(uint how, out IntPtr name);
 [PreserveSig] int GetAttributes(uint mask, out uint attr);
 [PreserveSig] int Compare(IShellItem other, uint hint, out int order);
}
[ComImport, Guid("42f85136-db7e-439c-85f1-e4075d135fc8"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)] interface IFileDialog {
 [PreserveSig] int Show(IntPtr owner);
 [PreserveSig] int SetFileTypes(uint n, [MarshalAs(UnmanagedType.LPArray, SizeParamIndex=0)] Filter[] filters);
 [PreserveSig] int SetFileTypeIndex(uint i);
 [PreserveSig] int GetFileTypeIndex(out uint i);
 [PreserveSig] int Advise([MarshalAs(UnmanagedType.Interface)] IFileDialogEvents e, out uint cookie);
 [PreserveSig] int Unadvise(uint cookie);
 [PreserveSig] int SetOptions(uint o);
 [PreserveSig] int GetOptions(out uint o);
 [PreserveSig] int SetDefaultFolder(IShellItem s);
 [PreserveSig] int SetFolder(IShellItem s);
 [PreserveSig] int GetFolder(out IShellItem s);
 [PreserveSig] int GetCurrentSelection(out IShellItem s);
 [PreserveSig] int SetFileName([MarshalAs(UnmanagedType.LPWStr)] string s);
 [PreserveSig] int GetFileName([MarshalAs(UnmanagedType.LPWStr)] out string s);
 [PreserveSig] int SetTitle([MarshalAs(UnmanagedType.LPWStr)] string s);
 [PreserveSig] int SetOkButtonLabel([MarshalAs(UnmanagedType.LPWStr)] string s);
 [PreserveSig] int SetFileNameLabel([MarshalAs(UnmanagedType.LPWStr)] string s);
 [PreserveSig] int GetResult(out IShellItem s);
 [PreserveSig] int AddPlace(IShellItem s, uint fdap);
 [PreserveSig] int SetDefaultExtension([MarshalAs(UnmanagedType.LPWStr)] string s);
 [PreserveSig] int Close(int hr);
 [PreserveSig] int SetClientGuid(ref Guid g);
 [PreserveSig] int ClearClientData();
 [PreserveSig] int SetFilter(IntPtr f);
}
[ComVisible(true), Guid("973510DB-7D7F-452B-8975-74A85828D354"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)] interface IFileDialogEvents {
 [PreserveSig] int OnFileOk(IFileDialog d);
 [PreserveSig] int OnFolderChanging(IFileDialog d, IShellItem s);
 [PreserveSig] int OnFolderChange(IFileDialog d);
 [PreserveSig] int OnSelectionChange(IFileDialog d);
 [PreserveSig] int OnShareViolation(IFileDialog d, IShellItem s, out uint response);
 [PreserveSig] int OnTypeChange(IFileDialog d);
 [PreserveSig] int OnOverwrite(IFileDialog d, IShellItem s, out uint response);
}
[ComVisible(true), ClassInterface(ClassInterfaceType.None)] sealed class Events : IFileDialogEvents {
 readonly string status; int count=0; int folderChanges=0;
 public Events(string s) { status=s; }
 public void Write(IFileDialog d) {
  IShellItem item; string folder="", file=""; uint index=0;
  if(d.GetFolder(out item)>=0 && item!=null) { IntPtr p; if(item.GetDisplayName(0x80058000,out p)>=0 && p!=IntPtr.Zero) {folder=Marshal.PtrToStringUni(p);Marshal.FreeCoTaskMem(p);} Marshal.ReleaseComObject(item); }
  d.GetFileName(out file); d.GetFileTypeIndex(out index);
  try {File.WriteAllText(status, folder+"\n"+(file??"")+"\n"+index+"\n"+count+"\n"+folderChanges+"\n",Encoding.UTF8);} catch(IOException) {}
 }
 public int OnFileOk(IFileDialog d) {count++;Write(d);return 1;}
 public int OnFolderChanging(IFileDialog d,IShellItem s){return 0;}
 public int OnFolderChange(IFileDialog d){folderChanges++;Write(d);return 0;}
 public int OnSelectionChange(IFileDialog d){Write(d);return 0;}
 public int OnShareViolation(IFileDialog d,IShellItem s,out uint r){r=0;return 0;}
 public int OnTypeChange(IFileDialog d){Write(d);return 0;}
 public int OnOverwrite(IFileDialog d,IShellItem s,out uint r){r=0;return 0;}
}
static class Probe {
 const uint READOBJECTS=1, CREATEWINDOW=2, WRITEOBJECTS=0x80, ENUMERATE=0x40, HOOKCONTROL=8;
 [DllImport("user32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern IntPtr CreateDesktop(string n,IntPtr d,IntPtr dev,uint flags,uint access,IntPtr sa);
 [DllImport("user32.dll",SetLastError=true)] static extern bool CloseDesktop(IntPtr d);
 [DllImport("user32.dll",SetLastError=true)] static extern bool SetThreadDesktop(IntPtr d);
 [DllImport("user32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern IntPtr OpenDesktop(string n,uint flags,bool inherit,uint access);
 [DllImport("user32.dll")] static extern bool EnumDesktopWindows(IntPtr desk, EnumProc f,IntPtr data);
 [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr parent,EnumProc f,IntPtr data);
 delegate bool EnumProc(IntPtr w,IntPtr data);
 [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr w,out uint pid);
 [DllImport("user32.dll")] static extern IntPtr GetParent(IntPtr w);
 [DllImport("user32.dll")] static extern int GetDlgCtrlID(IntPtr w);
 [StructLayout(LayoutKind.Sequential)] struct POINT {public int x,y;} 
 [DllImport("user32.dll")] static extern bool ScreenToClient(IntPtr w, ref POINT p);
 [StructLayout(LayoutKind.Sequential)] struct RECT { public int left,top,right,bottom; }
 [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr w,out RECT r);
 [DllImport("user32.dll")] static extern bool IsChild(IntPtr root,IntPtr w);
 [DllImport("user32.dll")] static extern bool IsWindow(IntPtr w);
 [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr w);
 [StructLayout(LayoutKind.Sequential)] struct GUIThreadInfo { public int cbSize; public uint flags; public IntPtr active,focus,capture,menuOwner,moveSize,caret; public RECT caretRect; }
 [DllImport("user32.dll")] static extern bool GetGUIThreadInfo(uint thread,ref GUIThreadInfo info);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern IntPtr RemoveProp(IntPtr window,string name);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr w,StringBuilder b,int max);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr w,StringBuilder b,int max);
 [DllImport("user32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern IntPtr SendMessageTimeout(IntPtr w,uint m,IntPtr a,string b,uint flags,uint timeout,out IntPtr result);
 [DllImport("user32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern IntPtr SendMessageTimeout(IntPtr w,uint m,IntPtr a,StringBuilder b,uint flags,uint timeout,out IntPtr result);
 [DllImport("user32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern IntPtr SendMessageTimeout(IntPtr w,uint m,IntPtr a,IntPtr b,uint flags,uint timeout,out IntPtr result);
 [DllImport("user32.dll")] static extern bool PostMessage(IntPtr w,uint m,IntPtr a,IntPtr b);
 [StructLayout(LayoutKind.Sequential)] struct CwpStruct { public IntPtr lParam,wParam; public uint message; public IntPtr hwnd; }
 delegate IntPtr HookProc(int code,IntPtr wParam,IntPtr lParam);
 [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern IntPtr CreateFileMapping(IntPtr file,IntPtr security,uint protect,uint sizeHigh,uint sizeLow,string name);
 [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
 [DllImport("user32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern IntPtr SetWindowsHookEx(int kind,HookProc callback,IntPtr module,uint thread);
 [DllImport("user32.dll")] static extern bool UnhookWindowsHookEx(IntPtr hook);
 [DllImport("user32.dll")] static extern IntPtr CallNextHookEx(IntPtr hook,int code,IntPtr wParam,IntPtr lParam);
 delegate void TimerProc(IntPtr window,uint message,UIntPtr id,uint time);
 [DllImport("user32.dll",SetLastError=true)] static extern UIntPtr SetTimer(IntPtr window,UIntPtr id,uint interval,TimerProc callback);
 [DllImport("user32.dll")] static extern bool KillTimer(IntPtr window,UIntPtr id);
 [DllImport("shell32.dll",CharSet=CharSet.Unicode,PreserveSig=true)] static extern int SHCreateItemFromParsingName(string path,IntPtr bind,ref Guid iid,[MarshalAs(UnmanagedType.Interface)] out IShellItem item);
 static string Class(IntPtr w){var b=new StringBuilder(128);GetClassName(w,b,b.Capacity);return b.ToString();}
 static string Title(IntPtr w){var b=new StringBuilder(1024);GetWindowText(w,b,b.Capacity);return b.ToString();}
 static bool RunDriver(string driver,string args) {
  var process=Process.Start(new ProcessStartInfo(driver,args) {UseShellExecute=false,CreateNoWindow=true,RedirectStandardOutput=true,RedirectStandardError=true});
  bool finished=process.WaitForExit(12000);
  if(!finished){process.Kill();process.WaitForExit();}
  string output=process.StandardOutput.ReadToEnd(); string error=process.StandardError.ReadToEnd();
  int code=process.ExitCode; process.Dispose();
  Console.WriteLine("driver_exit="+code+" timed_out="+(!finished)+" output="+output.Trim()+" error="+error.Trim());
  return finished&&code==0;
 }
 static int Child(string mode,string root,string title,string desktopName,string blockKind,string eventPrefix) {
  IntPtr desktop=OpenDesktop(desktopName,0,false,READOBJECTS|CREATEWINDOW|WRITEOBJECTS|ENUMERATE|HOOKCONTROL);
  if(desktop==IntPtr.Zero || !SetThreadDesktop(desktop)) { Console.WriteLine("child desktop fail: "+Marshal.GetLastWin32Error()); return 10; }
  Guid clsid=new Guid(mode=="save"?"C0B4E2F3-BA21-4773-8DBA-335EC946EB8B":"DC1C5A9C-E88A-4DDE-A5A1-60F82A20AEF7");
  var d=(IFileDialog)Activator.CreateInstance(Type.GetTypeFromCLSID(clsid));
  uint options; d.GetOptions(out options);
  if(mode=="folder") d.SetOptions(options|0x20|0x40);
  else {d.SetFileTypes(2,new Filter[]{new Filter{name="Text",spec="*.txt"},new Filter{name="All",spec="*.*"}});d.SetFileTypeIndex(2);d.SetFileName("verification-only.txt");}
  Guid iid=new Guid("43826D1E-E718-42EE-BC55-A1E261C37BFE");IShellItem item;
  if(SHCreateItemFromParsingName(Path.Combine(root,"start"),IntPtr.Zero,ref iid,out item)>=0){d.SetFolder(item);Marshal.ReleaseComObject(item);}
  if(mode!="folder")d.SetFileName("verification-only.txt"); d.SetTitle(title); var events=new Events(Path.Combine(root,"status-"+mode+".txt"));uint cookie;
  int hr=d.Advise(events,out cookie);if(hr<0)return 11;
  // Optional fault hook belongs exclusively to this fixture's GUI thread.
  // A unique private event arms it only after the native dialog is ready.
  EventWaitHandle armed=null,entered=null,release=null; HookProc hookCallback=null; IntPtr hook=IntPtr.Zero; bool intercepted=false;
  if(blockKind!=null) {
   if(blockKind!="settext"&&blockKind!="enter")return 13;
   try {armed=EventWaitHandle.OpenExisting(eventPrefix+"-armed");entered=EventWaitHandle.OpenExisting(eventPrefix+"-entered");release=EventWaitHandle.OpenExisting(eventPrefix+"-release");}
   catch(WaitHandleCannotBeOpenedException){return 14;}
   hookCallback=delegate(int code,IntPtr wp,IntPtr lp) {
    if(code>=0&&!intercepted&&armed.WaitOne(0)&&lp!=IntPtr.Zero) {
     CwpStruct info=(CwpStruct)Marshal.PtrToStructure(lp,typeof(CwpStruct));
     bool wanted=(blockKind=="settext"&&info.message==12)||(blockKind=="enter"&&info.message==0x100&&info.wParam.ToInt64()==13);
     if(wanted&&Class(info.hwnd)=="Edit"&&GetDlgCtrlID(info.hwnd)==41477) {
      if(blockKind=="settext")wanted=info.lParam!=IntPtr.Zero && Marshal.PtrToStringUni(info.lParam)==Path.Combine(root,"target");
      bool address=false,combo=false;
      for(IntPtr p=GetParent(info.hwnd);p!=IntPtr.Zero;p=GetParent(p)) {
       if(Class(p)=="ComboBoxEx32")combo=true;
       if(Class(p)=="Address Band Root"&&GetDlgCtrlID(p)==41477){address=true;break;}
      }
      if(wanted&&address&&combo){intercepted=true;entered.Set();release.WaitOne(10000);}
     }
    }
    return CallNextHookEx(hook,code,wp,lp);
   };
   hook=SetWindowsHookEx(4,hookCallback,IntPtr.Zero,GetCurrentThreadId()); if(hook==IntPtr.Zero)return 15;
  }
  // Thread timer runs on the same STA as Show's modal message loop. Reading
  // only in early OnFolderChange captures initialization before the filename
  // edit is populated. Seed once after Show starts, then sample actual COM state.
  bool seeded=false; TimerProc timer=delegate(IntPtr w,uint msg,UIntPtr id,uint tick) {
   try { if(!seeded){seeded=true;if(mode!="folder")Console.WriteLine("set_filename_hr="+d.SetFileName("verification-only.txt"));} events.Write(d); }
   catch(Exception ex){Console.WriteLine("timer_error="+ex.GetType().Name+":"+ex.Message);}
  };
  UIntPtr timerId=SetTimer(IntPtr.Zero,UIntPtr.Zero,200,timer); if(timerId==UIntPtr.Zero){d.Unadvise(cookie);Marshal.ReleaseComObject(d);return 12;}
  try {d.Show(IntPtr.Zero);} finally {KillTimer(IntPtr.Zero,timerId);if(hook!=IntPtr.Zero)UnhookWindowsHookEx(hook);if(armed!=null)armed.Dispose();if(entered!=null)entered.Dispose();if(release!=null)release.Dispose();d.Unadvise(cookie);GC.KeepAlive(timer);GC.KeepAlive(hookCallback);GC.KeepAlive(events);Marshal.ReleaseComObject(d);CloseDesktop(desktop);} return 0;
 }
 static string Read(string p){try{return File.ReadAllText(p,Encoding.UTF8);}catch(IOException){return "";}catch(UnauthorizedAccessException){return "";}}
 static bool RunBlocked(IntPtr desktop,string desktopName,string exe,string root,string kind) {
  string token=Guid.NewGuid().ToString("N"); string prefix="Local\\PulseFolderProbe_"+token;
  string mappingName=prefix+"-"+kind+"-mapping",marker="Pulse.FolderLink."+Guid.NewGuid().ToString("B");
  using(var armed=new EventWaitHandle(false,EventResetMode.ManualReset,prefix+"-armed"))
  using(var entered=new EventWaitHandle(false,EventResetMode.ManualReset,prefix+"-entered"))
  using(var release=new EventWaitHandle(false,EventResetMode.ManualReset,prefix+"-release")) {
   IntPtr mapping=CreateFileMapping(new IntPtr(-1),IntPtr.Zero,4,0,262144,mappingName);
   if(mapping==IntPtr.Zero){Console.WriteLine("fault_mapping_failed");return false;}
   string status=Path.Combine(root,"status-save.txt"); if(File.Exists(status))File.Delete(status);
   string title="Pulse isolated folder link fault "+kind+" "+token;
   string args="--child-block save \""+root+"\" \""+title+"\" "+desktopName+" "+kind+" \""+prefix+"\"";
   var fixture=Process.Start(new ProcessStartInfo(exe,args){UseShellExecute=false,CreateNoWindow=true,RedirectStandardOutput=true,RedirectStandardError=true});
   Process worker=null; IntPtr dialog=IntPtr.Zero; bool success=false;
   try {
    DateTime findUntil=DateTime.UtcNow.AddSeconds(15);
    while(DateTime.UtcNow<findUntil&&!fixture.HasExited) {
     int count=0;EnumDesktopWindows(desktop,delegate(IntPtr w,IntPtr x){uint id;GetWindowThreadProcessId(w,out id);if(id==fixture.Id&&IsWindowVisible(w)&&Class(w)=="#32770"&&Title(w)==title){dialog=w;count++;}return true;},IntPtr.Zero);
     if(count==1)break; if(count>1){dialog=IntPtr.Zero;break;}Thread.Sleep(50);
    }
    string initial=""; DateTime readyUntil=DateTime.UtcNow.AddSeconds(5);
    while(dialog!=IntPtr.Zero&&DateTime.UtcNow<readyUntil) {initial=Read(status);string[] fields=initial.Split('\n');if(fields.Length>=5&&fields[1]=="verification-only.txt"&&fields[3]=="0")break;Thread.Sleep(50);}
    string[] before=initial.Split('\n'); string target=Path.Combine(root,"target");
    if(dialog!=IntPtr.Zero&&before.Length>=5&&before[1]=="verification-only.txt"&&before[3]=="0") {
     string driver=Path.Combine(Path.GetDirectoryName(exe),"native-dialog-folder-link-driver.exe");
     string common="\""+desktopName+"\" "+dialog.ToInt64()+" "+fixture.Id+" \""+target+"\" ";
     string suffix=" \""+mappingName+"\" \""+marker+"\"";
     armed.Set(); worker=Process.Start(new ProcessStartInfo(driver,common+"fault"+suffix){UseShellExecute=false,CreateNoWindow=true,RedirectStandardOutput=true,RedirectStandardError=true});
     bool intercepted=entered.WaitOne(5000), killed=false;
     if(intercepted&&!worker.HasExited){worker.Kill();worker.WaitForExit();killed=true;}
     bool recovered=killed&&RunDriver(driver,common+"fault-recover"+suffix);
     release.Set();
     DateTime observation=DateTime.UtcNow.AddSeconds(3); bool observed=false,stable=true; string final="";
     int beforeCount=0;Int32.TryParse(before[4],out beforeCount);
     while(DateTime.UtcNow<observation){string text=Read(status);string[] fields=text.Split('\n');int count;
      if(fields.Length>=5&&Int32.TryParse(fields[4],out count)) {observed=true;final=text;
       stable &= fields[1]==before[1]&&fields[2]==before[2]&&fields[3]=="0"&&count<=beforeCount+1&&
        (String.Equals(fields[0],before[0],StringComparison.OrdinalIgnoreCase)||String.Equals(fields[0],target,StringComparison.OrdinalIgnoreCase));}
      Thread.Sleep(50);
     }
     uint pid;uint thread=GetWindowThreadProcessId(dialog,out pid);var gui=new GUIThreadInfo{cbSize=Marshal.SizeOf(typeof(GUIThreadInfo))};
     bool guiOk=GetGUIThreadInfo(thread,ref gui)&&gui.capture==IntPtr.Zero&&gui.focus!=IntPtr.Zero&&IsChild(dialog,gui.focus);
     IntPtr edit=IntPtr.Zero;EnumChildWindows(dialog,delegate(IntPtr w,IntPtr x){if(Class(w)=="Edit"&&GetDlgCtrlID(w)==41477&&IsWindowVisible(w))edit=w;return true;},IntPtr.Zero);
     var address=new StringBuilder(32768);IntPtr result;bool addressRead=edit!=IntPtr.Zero&&SendMessageTimeout(edit,13,(IntPtr)address.Capacity,address,3,500,out result)!=IntPtr.Zero;
     bool textSafe=kind!="settext"||(addressRead&&address.ToString()!=target);
     Console.WriteLine("fault="+kind+" entered="+intercepted+" killed="+killed+" recovered="+recovered+" observed="+observed+" stable="+stable+" gui="+guiOk+" address_read="+addressRead+" text_safe="+textSafe+" final="+final.Replace(root,"<root>").Replace("\n","|"));
     success=intercepted&&killed&&recovered&&observed&&stable&&guiOk&&textSafe;
    }
   } finally {
    release.Set(); if(worker!=null){if(!worker.HasExited){worker.Kill();worker.WaitForExit();}worker.Dispose();}
    if(dialog!=IntPtr.Zero&&IsWindow(dialog)){RemoveProp(dialog,marker);PostMessage(dialog,0x111,(IntPtr)2,IntPtr.Zero);}
    if(!fixture.WaitForExit(5000)){fixture.Kill();fixture.WaitForExit();}fixture.Dispose();CloseHandle(mapping);
   }
   return success;
  }
 }
 static bool Run(IntPtr desktop,string desktopName,string exe,string root,string mode){
  string title="Pulse isolated folder link "+mode+" "+Path.GetFileName(root);
  var args="--child "+mode+" \""+root+"\" \""+title+"\" "+desktopName;
  var proc=Process.Start(new ProcessStartInfo(exe,args){UseShellExecute=false,CreateNoWindow=true,RedirectStandardOutput=true,RedirectStandardError=true});
  IntPtr dialog=IntPtr.Zero;DateTime deadline=DateTime.UtcNow.AddSeconds(15);
  while(DateTime.UtcNow<deadline&&!proc.HasExited){
   int count=0;EnumDesktopWindows(desktop,delegate(IntPtr w,IntPtr x){uint id;GetWindowThreadProcessId(w,out id);if(id==proc.Id&&IsWindowVisible(w)&&Class(w)=="#32770"&&Title(w)==title){dialog=w;count++;}return true;},IntPtr.Zero);
   if(count==1)break;if(count>1){dialog=IntPtr.Zero;break;}Thread.Sleep(50);
  }
  bool ok=false;
  if(dialog!=IntPtr.Zero){
   string status=Path.Combine(root,"status-"+mode+".txt");
   string initial="", last=""; DateTime stableSince=DateTime.UtcNow; DateTime initialDeadline=DateTime.UtcNow.AddSeconds(5);
   while(DateTime.UtcNow<initialDeadline) {
    string candidate=Read(status); string[] ready=candidate.Split('\n');
    bool complete=ready.Length>=5 && !String.IsNullOrEmpty(ready[4]) && String.Equals(ready[0],Path.Combine(root,"start"),StringComparison.OrdinalIgnoreCase) && (mode=="folder"||ready[1]=="verification-only.txt");
    if(complete) {if(candidate!=last){last=candidate;stableSince=DateTime.UtcNow;} else if((DateTime.UtcNow-stableSince).TotalMilliseconds>=400){initial=candidate;break;}}
    Thread.Sleep(50);
   }
   string[] previous=initial.Split('\n'); string target=Path.Combine(root,"target");
   Console.WriteLine("initial="+initial.Replace(root,"<root>").Replace("\n","|"));
   // The driver links the production code and accepts only this fixture PID.
   string driver=Path.Combine(Path.GetDirectoryName(exe),"native-dialog-folder-link-driver.exe");
   if(previous.Length>=5 && (mode=="folder"||previous[1]=="verification-only.txt") &&
      (mode=="folder"||previous[2]=="2") && previous[3]=="0" && File.Exists(driver)) {
    bool rejected=RunDriver(driver,"\""+desktopName+"\" "+dialog.ToInt64()+" "+proc.Id+" \""+Path.Combine(root,"missing")+"\" reject");
    // Periodic writes can briefly expose a truncated file; compare complete
    // snapshots for a bounded interval, and track actual folder-change events
    // so a transient unwanted navigation cannot hide behind a later snapshot.
    bool observed=false; DateTime rejectUntil=DateTime.UtcNow.AddMilliseconds(750);
    while(DateTime.UtcNow<rejectUntil) {
     string[] unchanged=Read(status).Split('\n');
     if(unchanged.Length>=5&&!String.IsNullOrEmpty(unchanged[4])) {
      observed=true;
      rejected &= String.Equals(unchanged[0],previous[0],StringComparison.OrdinalIgnoreCase) &&
       (mode=="folder"||unchanged[1]==previous[1]) && unchanged[2]==previous[2] &&
       unchanged[3]=="0" && unchanged[4]==previous[4];
     }
     Thread.Sleep(50);
    }
    rejected &= observed;
    bool twice=RunDriver(driver,"\""+desktopName+"\" "+dialog.ToInt64()+" "+proc.Id+" \""+target+"\" twice \""+status+"\"");
    bool returned=false; int beforeChanges=0; string afterTwice=""; DateTime returnedStable=DateTime.UtcNow;
    Int32.TryParse(previous[4],out beforeChanges);
    DateTime twiceUntil=DateTime.UtcNow.AddSeconds(5);
    while(DateTime.UtcNow<twiceUntil) {
     string snapshot=Read(status); string[] state=snapshot.Split('\n'); int changes=0;
     if(state.Length>=5 && Int32.TryParse(state[4],out changes) && changes>=beforeChanges+2 &&
        String.Equals(state[0],previous[0],StringComparison.OrdinalIgnoreCase) &&
        (mode=="folder"||state[1]==previous[1]) && state[2]==previous[2] && state[3]=="0") {
      if(snapshot!=afterTwice){afterTwice=snapshot;returnedStable=DateTime.UtcNow;}
      else if((DateTime.UtcNow-returnedStable).TotalMilliseconds>=400){returned=true;break;}
     }
     Thread.Sleep(50);
    }
    bool issued=twice&&returned&&RunDriver(driver,"\""+desktopName+"\" "+dialog.ToInt64()+" "+proc.Id+" \""+target+"\" accept");
    DateTime until=DateTime.UtcNow.AddSeconds(5); string targetSnapshot=""; DateTime targetStable=DateTime.UtcNow;
    while(DateTime.UtcNow<until) { string snapshot=Read(status); string[] lines=snapshot.Split('\n');
      if(lines.Length>=5 && !String.IsNullOrEmpty(lines[4]) && String.Equals(lines[0],target,StringComparison.OrdinalIgnoreCase) &&
         (mode=="folder" || lines[1]==previous[1]) && lines[2]==previous[2] && lines[3]=="0") {
        if(snapshot!=targetSnapshot){targetSnapshot=snapshot;targetStable=DateTime.UtcNow;}
        else if((DateTime.UtcNow-targetStable).TotalMilliseconds>=400){ok=rejected&&twice&&returned&&issued;break;}
      }
      Thread.Sleep(50);
    }
    bool recovered=ok&&RunDriver(driver,"\""+desktopName+"\" "+dialog.ToInt64()+" "+proc.Id+" \""+Path.Combine(root,"start")+"\" recover");
    bool recoveryPreserved=true, recoveryObserved=false; string[] targetState=targetSnapshot.Split('\n'); DateTime recoveryUntil=DateTime.UtcNow.AddMilliseconds(750);
    while(DateTime.UtcNow<recoveryUntil) {
     string[] afterRecovery=Read(status).Split('\n');
     if(afterRecovery.Length>=5&&!String.IsNullOrEmpty(afterRecovery[4])) {
      recoveryObserved=true;
      recoveryPreserved &= String.Equals(afterRecovery[0],target,StringComparison.OrdinalIgnoreCase)&&
       (mode=="folder"||afterRecovery[1]==previous[1])&&afterRecovery[2]==previous[2]&&afterRecovery[3]=="0"&&
       targetState.Length>=5&&afterRecovery[4]==targetState[4];
     }
     Thread.Sleep(50);
    }
    recoveryPreserved &= recoveryObserved;
    ok &= recovered&&recoveryPreserved;
    bool unwritten=!File.Exists(Path.Combine(root,"start","verification-only.txt")) &&
      !File.Exists(Path.Combine(target,"verification-only.txt"));
    ok &= unwritten;
    Console.WriteLine("invalid_rejected="+rejected+" reject_snapshot_observed="+observed+" twice_issued="+twice+" twice_returned="+returned+" navigation_issued="+issued+" recovery="+recovered+" recovery_preserved="+recoveryPreserved+" fixture_file_absent="+unwritten);
   }
   Console.WriteLine("final="+Read(status).Replace(root,"<root>").Replace("\n","|")); if(IsWindow(dialog))PostMessage(dialog,0x111,(IntPtr)2,IntPtr.Zero);
  }
  bool normalExit=proc.WaitForExit(5000); if(!normalExit){proc.Kill();proc.WaitForExit(3000);}
  string finalState=Read(Path.Combine(root,"status-"+mode+".txt")); string[] finalLines=finalState.Split('\n');
  bool noAccept=finalLines.Length>=5 && finalLines[3]=="0";
  string staleDriver=Path.Combine(Path.GetDirectoryName(exe),"native-dialog-folder-link-driver.exe");
  bool stale=normalExit&&dialog!=IntPtr.Zero&&RunDriver(staleDriver,"\""+desktopName+"\" "+dialog.ToInt64()+" "+proc.Id+" \""+Path.Combine(root,"target")+"\" stale");
  ok &= normalExit&&noAccept&&stale;
  Console.WriteLine("post_recovery_on_file_ok_zero="+noAccept+" destroyed_dialog_rejected="+stale);
  Console.WriteLine("child_exit="+proc.ExitCode+" dialog_found="+(dialog!=IntPtr.Zero)+" child_out="+proc.StandardOutput.ReadToEnd()+" child_err="+proc.StandardError.ReadToEnd()); proc.Dispose();
  Console.WriteLine(mode+": "+(ok?"PASS":"FAIL or unsupported address navigation"));return ok;
 }
 [STAThread] static int Main(string[] args){
  if((args.Length==5&&args[0]=="--child")||(args.Length==7&&args[0]=="--child-block")) { int result=12; Thread t=new Thread(delegate(){result=Child(args[1],args[2],args[3],args[4],args.Length==7?args[5]:null,args.Length==7?args[6]:null);}); t.SetApartmentState(ApartmentState.STA); t.Start(); t.Join(); return result; }
  if(args.Length!=0)return 2;
  string root=Path.Combine(Path.GetTempPath(),"pulse-folder-probe-"+Guid.NewGuid().ToString("N"));Directory.CreateDirectory(root);Directory.CreateDirectory(Path.Combine(root,"start"));Directory.CreateDirectory(Path.Combine(root,"target"));
  string name="PulseFolderProbe_"+Guid.NewGuid().ToString("N");IntPtr desktop=CreateDesktop(name,IntPtr.Zero,IntPtr.Zero,0,READOBJECTS|CREATEWINDOW|WRITEOBJECTS|ENUMERATE|HOOKCONTROL,IntPtr.Zero);
  if(desktop==IntPtr.Zero){Console.WriteLine("private desktop creation failed: "+Marshal.GetLastWin32Error());return 3;}
  string exe=Process.GetCurrentProcess().MainModule.FileName;bool a=false,b=false,c=false;
  try{a=Run(desktop,name,exe,root,"save");b=Run(desktop,name,exe,root,"open");c=Run(desktop,name,exe,root,"folder");if(a&&b&&c){a=RunBlocked(desktop,name,exe,root,"settext");b=RunBlocked(desktop,name,exe,root,"enter");}}
  finally{CloseDesktop(desktop);foreach(string mode in new[]{"save","open","folder"}){string p=Path.Combine(root,"status-"+mode+".txt");if(File.Exists(p))File.Delete(p);}Directory.Delete(Path.Combine(root,"start"));Directory.Delete(Path.Combine(root,"target"));Directory.Delete(root);}
  return a&&b&&c?0:1;
 }
}












