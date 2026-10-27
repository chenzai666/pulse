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
 readonly string status; int count=0;
 public Events(string s) { status=s; }
 public void Write(IFileDialog d) {
  IShellItem item; string folder="", file=""; uint index=0;
  if(d.GetFolder(out item)>=0 && item!=null) { IntPtr p; if(item.GetDisplayName(0x80058000,out p)>=0 && p!=IntPtr.Zero) {folder=Marshal.PtrToStringUni(p);Marshal.FreeCoTaskMem(p);} Marshal.ReleaseComObject(item); }
  d.GetFileName(out file); d.GetFileTypeIndex(out index);
  try {File.WriteAllText(status, folder+"\n"+(file??"")+"\n"+index+"\n"+count+"\n",Encoding.UTF8);} catch(IOException) {}
 }
 public int OnFileOk(IFileDialog d) {count++;Write(d);return 1;}
 public int OnFolderChanging(IFileDialog d,IShellItem s){return 0;}
 public int OnFolderChange(IFileDialog d){Write(d);return 0;}
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
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr w,StringBuilder b,int max);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr w,StringBuilder b,int max);
 [DllImport("user32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern IntPtr SendMessageTimeout(IntPtr w,uint m,IntPtr a,string b,uint flags,uint timeout,out IntPtr result);
 [DllImport("user32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern IntPtr SendMessageTimeout(IntPtr w,uint m,IntPtr a,StringBuilder b,uint flags,uint timeout,out IntPtr result);
 [DllImport("user32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern IntPtr SendMessageTimeout(IntPtr w,uint m,IntPtr a,IntPtr b,uint flags,uint timeout,out IntPtr result);
 [DllImport("user32.dll")] static extern bool PostMessage(IntPtr w,uint m,IntPtr a,IntPtr b);
 delegate void TimerProc(IntPtr window,uint message,UIntPtr id,uint time);
 [DllImport("user32.dll",SetLastError=true)] static extern UIntPtr SetTimer(IntPtr window,UIntPtr id,uint interval,TimerProc callback);
 [DllImport("user32.dll")] static extern bool KillTimer(IntPtr window,UIntPtr id);
 [DllImport("shell32.dll",CharSet=CharSet.Unicode,PreserveSig=true)] static extern int SHCreateItemFromParsingName(string path,IntPtr bind,ref Guid iid,[MarshalAs(UnmanagedType.Interface)] out IShellItem item);
 static string Class(IntPtr w){var b=new StringBuilder(128);GetClassName(w,b,b.Capacity);return b.ToString();}
 static string Title(IntPtr w){var b=new StringBuilder(1024);GetWindowText(w,b,b.Capacity);return b.ToString();}
 static bool RunDriver(string driver,string args) {
  var process=Process.Start(new ProcessStartInfo(driver,args) {UseShellExecute=false,CreateNoWindow=true,RedirectStandardOutput=true,RedirectStandardError=true});
  bool finished=process.WaitForExit(5000);
  if(!finished){process.Kill();process.WaitForExit();}
  string output=process.StandardOutput.ReadToEnd(); string error=process.StandardError.ReadToEnd();
  int code=process.ExitCode; process.Dispose();
  Console.WriteLine("driver_exit="+code+" timed_out="+(!finished)+" output="+output.Trim()+" error="+error.Trim());
  return finished&&code==0;
 }
 static int Child(string mode,string root,string title,string desktopName) {
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
  // Thread timer runs on the same STA as Show's modal message loop. Reading
  // only in early OnFolderChange captures initialization before the filename
  // edit is populated. Seed once after Show starts, then sample actual COM state.
  bool seeded=false; TimerProc timer=delegate(IntPtr w,uint msg,UIntPtr id,uint tick) {
   try { if(!seeded){seeded=true;if(mode!="folder")Console.WriteLine("set_filename_hr="+d.SetFileName("verification-only.txt"));} events.Write(d); }
   catch(Exception ex){Console.WriteLine("timer_error="+ex.GetType().Name+":"+ex.Message);}
  };
  UIntPtr timerId=SetTimer(IntPtr.Zero,UIntPtr.Zero,200,timer); if(timerId==UIntPtr.Zero){d.Unadvise(cookie);Marshal.ReleaseComObject(d);return 12;}
  try {d.Show(IntPtr.Zero);} finally {KillTimer(IntPtr.Zero,timerId);d.Unadvise(cookie);GC.KeepAlive(timer);GC.KeepAlive(events);Marshal.ReleaseComObject(d);CloseDesktop(desktop);} return 0;
 }
 static string Read(string p){try{return File.ReadAllText(p,Encoding.UTF8);}catch(IOException){return "";}catch(UnauthorizedAccessException){return "";}}
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
   string initial=""; DateTime initialDeadline=DateTime.UtcNow.AddSeconds(5);
   while(DateTime.UtcNow<initialDeadline) { initial=Read(status); string[] ready=initial.Split('\n'); if(ready.Length>=4&&String.Equals(ready[0],Path.Combine(root,"start"),StringComparison.OrdinalIgnoreCase)&&(mode=="folder"||ready[1]=="verification-only.txt")) break; Thread.Sleep(50); }
   string[] previous=initial.Split('\n'); string target=Path.Combine(root,"target");
   Console.WriteLine("initial="+initial.Replace(root,"<root>").Replace("\n","|"));
   // The driver links the production code and accepts only this fixture PID.
   string driver=Path.Combine(Path.GetDirectoryName(exe),"native-dialog-folder-link-driver.exe");
   if(previous.Length>=4 && (mode=="folder"||previous[1]=="verification-only.txt") &&
      (mode=="folder"||previous[2]=="2") && previous[3]=="0" && File.Exists(driver)) {
    bool rejected=RunDriver(driver,"\""+desktopName+"\" "+dialog.ToInt64()+" "+proc.Id+" \""+Path.Combine(root,"missing")+"\" reject");
    string unchanged=Read(status); rejected &= unchanged==initial;
    bool issued=RunDriver(driver,"\""+desktopName+"\" "+dialog.ToInt64()+" "+proc.Id+" \""+target+"\" accept");
    DateTime until=DateTime.UtcNow.AddSeconds(5);
    while(DateTime.UtcNow<until) { string[] lines=Read(status).Split('\n');
      if(lines.Length>=4 && String.Equals(lines[0],target,StringComparison.OrdinalIgnoreCase) &&
         (mode=="folder" || lines[1]==previous[1]) && lines[2]==previous[2] && lines[3]=="0") {ok=rejected&&issued;break;} Thread.Sleep(50); }
    bool unwritten=!File.Exists(Path.Combine(root,"start","verification-only.txt")) &&
      !File.Exists(Path.Combine(target,"verification-only.txt"));
    ok &= unwritten;
    Console.WriteLine("invalid_rejected="+rejected+" navigation_issued="+issued+" fixture_file_absent="+unwritten);
   }
   Console.WriteLine("final="+Read(status).Replace(root,"<root>").Replace("\n","|")); if(IsWindow(dialog))PostMessage(dialog,0x111,(IntPtr)2,IntPtr.Zero);
  }
  if(!proc.WaitForExit(5000)){proc.Kill();proc.WaitForExit(3000);} Console.WriteLine("child_exit="+proc.ExitCode+" dialog_found="+(dialog!=IntPtr.Zero)+" child_out="+proc.StandardOutput.ReadToEnd()+" child_err="+proc.StandardError.ReadToEnd()); proc.Dispose();
  Console.WriteLine(mode+": "+(ok?"PASS":"FAIL or unsupported address navigation"));return ok;
 }
 [STAThread] static int Main(string[] args){
  if(args.Length==5&&args[0]=="--child") { int result=12; Thread t=new Thread(delegate(){result=Child(args[1],args[2],args[3],args[4]);}); t.SetApartmentState(ApartmentState.STA); t.Start(); t.Join(); return result; }
  if(args.Length!=0)return 2;
  string root=Path.Combine(Path.GetTempPath(),"pulse-folder-probe-"+Guid.NewGuid().ToString("N"));Directory.CreateDirectory(root);Directory.CreateDirectory(Path.Combine(root,"start"));Directory.CreateDirectory(Path.Combine(root,"target"));
  string name="PulseFolderProbe_"+Guid.NewGuid().ToString("N");IntPtr desktop=CreateDesktop(name,IntPtr.Zero,IntPtr.Zero,0,READOBJECTS|CREATEWINDOW|WRITEOBJECTS|ENUMERATE|HOOKCONTROL,IntPtr.Zero);
  if(desktop==IntPtr.Zero){Console.WriteLine("private desktop creation failed: "+Marshal.GetLastWin32Error());return 3;}
  string exe=Process.GetCurrentProcess().MainModule.FileName;bool a=false,b=false,c=false;
  try{a=Run(desktop,name,exe,root,"save");b=Run(desktop,name,exe,root,"open");c=Run(desktop,name,exe,root,"folder");}
  finally{CloseDesktop(desktop);foreach(string mode in new[]{"save","open","folder"}){string p=Path.Combine(root,"status-"+mode+".txt");if(File.Exists(p))File.Delete(p);}Directory.Delete(Path.Combine(root,"start"));Directory.Delete(Path.Combine(root,"target"));Directory.Delete(root);}
  return a&&b&&c?0:1;
 }
}












