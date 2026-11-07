using System;
using System.Diagnostics;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;
using System.Drawing;
using System.Collections.Generic;
using System.Linq;
using CameraStudio.Update;

[assembly:System.Reflection.AssemblyTitle("FS Camera Studio Updater")]
[assembly:System.Reflection.AssemblyVersion("1.0.0.0")]
namespace CameraStudio.Update {
static class Program {
    [STAThread] static int Main(string[] args) {
        Application.EnableVisualStyles();Application.SetCompatibleTextRenderingDefault(false);
        try {
            if(args.Length>1)throw new InvalidDataException("Unexpected updater arguments.");
            string requested=args.Length==0?null:Core.ParseRequest(args[0]);
            bool created;
            // A local per-user lock prevents two panels from installing at the same time.
            string user=System.Security.Principal.WindowsIdentity.GetCurrent().User.Value;
            using(var mutex=new Mutex(true,"Local\\FSCSUpdater-"+user,out created)) {
                if(!created){MessageBox.Show("Camera Studio Updater is already running. Check its window on the taskbar.","Camera Studio updater");return 0;}
                try{Application.Run(new UpdateWindow(requested));}finally{mutex.ReleaseMutex();}
            }
            return 0;
        }catch(Exception e){MessageBox.Show(e.Message,"Camera Studio update",MessageBoxButtons.OK,MessageBoxIcon.Error);return 1;}
    }
}
sealed class UpdateWindow : Form {
    readonly string requested;
    readonly Label status=new Label();readonly ProgressBar progress=new ProgressBar();readonly Button cancel=new Button();
    readonly Button restart=new Button(), later=new Button();
    TaskCompletionSource<bool> restartChoice;
    readonly List<string> restartPaths=new List<string>();
    string setupArguments;
    readonly CancellationTokenSource cancellation=new CancellationTokenSource();
    bool installing,finished,started;
    readonly string support=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),"FS Camera Studio");
    public UpdateWindow(string version) {
        requested=version;Text="Camera Studio update";ClientSize=new Size(560,290);FormBorderStyle=FormBorderStyle.FixedDialog;
        MaximizeBox=false;StartPosition=FormStartPosition.CenterScreen;Font=new Font("Segoe UI",10);
        var title=new Label { Text="Update Camera Studio",Font=new Font("Segoe UI",15,FontStyle.Bold),AutoSize=true,Location=new Point(22,20) };
        status.SetBounds(24,61,512,85);status.Text="Checking the official release...";
        progress.SetBounds(24,153,512,19);progress.Style=ProgressBarStyle.Marquee;
        cancel.Text="Cancel";cancel.SetBounds(426,241,110,30);cancel.Click+=(s,e)=>{if(finished)Close();else {cancellation.Cancel();cancel.Enabled=false;}};
        restart.Text="Restart and install";restart.SetBounds(24,193,190,34);restart.Visible=false;
        later.Text="When I close the sims";later.SetBounds(224,193,210,34);later.Visible=false;
        restart.Click+=(s,e)=>{if(restartChoice!=null)restartChoice.TrySetResult(true);};
        later.Click+=(s,e)=>{if(restartChoice!=null)restartChoice.TrySetResult(false);};
        Controls.AddRange(new Control[]{title,status,progress,restart,later,cancel});
        FormClosing+=(s,e)=>{if(!finished){e.Cancel=true;if(!installing){cancellation.Cancel();cancel.Enabled=false;}}};
        Shown+=async(s,e)=>{if(started)return;started=true;await Run();};
    }
    void Report(string message) { if(IsDisposed)return;if(InvokeRequired){BeginInvoke(new Action<string>(Report),message);return;}status.Text=message; }
    void Progress(int value) { if(IsDisposed)return;if(InvokeRequired){BeginInvoke(new Action<int>(Progress),value);return;}progress.Style=ProgressBarStyle.Continuous;progress.Value=Math.Max(0,Math.Min(100,value)); }
    static bool Running() {
        foreach(string name in new[]{"FlightSimulator2024","X-Plane","XPCSKeyHelper"}) {
            var processes=Process.GetProcessesByName(name);try {if(processes.Length>0)return true;}finally{foreach(var p in processes)p.Dispose();}
        }
        return false;
    }
    async Task<bool> ChooseRestart() {
        restartChoice=new TaskCompletionSource<bool>();
        Report("Update downloaded and verified. Save your views and flight first. Restart and install closes the simulators normally, installs the update, then reopens them. Or update when you close them later.");
        restart.Visible=later.Visible=true;
        try {
            while(!restartChoice.Task.IsCompleted && Running())await Task.Delay(250,cancellation.Token);
            return restartChoice.Task.IsCompleted && await restartChoice.Task;
        } finally {restart.Visible=later.Visible=false;restartChoice=null;}
    }
    void RequestClose() {
        // Only reached after the explicit Restart and install choice. Never terminate a process forcibly.
        foreach(string name in new[]{"FlightSimulator2024","X-Plane"}) {
            foreach(var process in Process.GetProcessesByName(name))using(process) {
                try {
                    if(process.HasExited)continue;
                    string path=process.MainModule.FileName;
                    if(!File.Exists(path))throw new IOException("Simulator executable could not be located.");
                    if(process.CloseMainWindow() && !restartPaths.Contains(path))restartPaths.Add(path);
                }catch(InvalidOperationException){/* Process exited between enumeration and the close request. */}
                catch(System.ComponentModel.Win32Exception){Report("Please close the simulator normally; Windows did not allow an automatic close request.");}
            }
        }
    }
    void RestartSimulators() {
        foreach(string path in restartPaths) {
            string name=Path.GetFileNameWithoutExtension(path);
            var active=Process.GetProcessesByName(name);bool alreadyRunning=active.Length>0;foreach(var p in active)p.Dispose();
            if(alreadyRunning)continue;
            try {Process.Start(new ProcessStartInfo(path){UseShellExecute=true,WorkingDirectory=Path.GetDirectoryName(path)});}
            catch {Report("Update installed. Windows could not reopen "+name+"; start it normally when ready.");}
        }
    }
    async Task<int> Install(string path) {
        installing=true;BeginInvoke(new Action(()=>{cancel.Enabled=false;progress.Style=ProgressBarStyle.Marquee;}));
        var info=new ProcessStartInfo(path,setupArguments) { UseShellExecute=false,WorkingDirectory=Path.GetDirectoryName(path) };
        using(var process=Process.Start(info)){if(process==null)throw new IOException("Could not start setup.");await Task.Run(()=>process.WaitForExit());return process.ExitCode;}
    }
    async Task Run() {
        string folder=null;
        try {
            Core.PlainDirectory(support);
            string infoPath=Path.Combine(support,"BUILD-INFO.json");
            if(!File.Exists(infoPath))throw new IOException("Install Camera Studio using the Windows setup first. Then Update now can manage future upgrades.");
            var info=Core.Json(File.ReadAllText(infoPath));object currentValue;
            if(!info.TryGetValue("buildId",out currentValue)||!Core.ValidVersion(currentValue as string))throw new InvalidDataException("Installed version could not be verified. Run the Windows setup manually.");
            string current=(string)currentValue;
            string installedRecord=Path.Combine(support,"installation.ini");
            if(!File.Exists(installedRecord))throw new IOException("This older installation needs the new Camera Studio setup once to enable fully automatic upgrades.");
            setupArguments=Core.SetupArguments(File.ReadAllText(installedRecord));
            using(var network=new Network()) {
                var release=await network.Release(requested,cancellation.Token);
                if(!Core.Newer(release.Version,current)){Report("Camera Studio "+current+" is already up to date.");return;}
                folder=Path.Combine(support,"Updates",Guid.NewGuid().ToString("N"));Directory.CreateDirectory(folder);Core.PlainDirectory(folder);
                string path=Path.Combine(folder,"FS-Camera-Studio-"+release.Version+"-Setup.exe");
                Report("Downloading Camera Studio "+release.Version+". Your simulator files are unchanged until setup runs.");
                await network.Download(release,path,Progress,cancellation.Token);
                await Core.RequestRestart(Running,ChooseRestart,RequestClose,cancellation.Token);
                await Task.Run(()=>Core.InstallWhenReady(path,release,Running,t=>Task.Delay(1000,t),Install,Report,cancellation.Token));
                // Setup can return success only after verification; independently confirm its installed version metadata.
                var after=Core.Json(File.ReadAllText(infoPath));object installed;
                if(!after.TryGetValue("buildId",out installed)||(string)installed!=release.Version)throw new IOException("Setup returned without confirming version "+release.Version+". Please run setup manually.");
                Report("Camera Studio "+release.Version+" is installed."+(restartPaths.Count>0?" Reopening your simulator...":" You can start your simulator when ready."));Progress(100);
                RestartSimulators();
            }
        }catch(OperationCanceledException){Report("Update cancelled. No installer was started.");}
        catch(Exception e){Report("Update failed: "+e.Message);}
        finally {
            finished=true;installing=false;restart.Visible=later.Visible=false;cancel.Enabled=true;cancel.Text="Close";progress.Style=ProgressBarStyle.Continuous;
            if(folder!=null){try{Core.PlainDirectory(folder);foreach(var p in Directory.GetFiles(folder))File.Delete(p);Directory.Delete(folder);}catch{} }
        }
    }
}
}
