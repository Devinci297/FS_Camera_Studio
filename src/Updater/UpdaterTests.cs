using System;
using System.IO;
using System.Net;
using System.Net.Http;
using System.Threading;
using System.Threading.Tasks;
using System.Text;
using CameraStudio.Update;
class FakeHttp : HttpMessageHandler {
    public Func<HttpRequestMessage,HttpResponseMessage> Response;
    protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request,CancellationToken token){return Task.FromResult(Response(request));}
}
class Tests {
    static int checks;
    static void Check(bool value,string text){checks++;if(!value)throw new Exception(text);}
    static async Task Fails(Func<Task> action,string text){bool failed=false;try{await action();}catch{failed=true;}Check(failed,text);}
    static string Metadata(string version,string hash,long size) {
        return "{\"tag_name\":\"v"+version+"\",\"draft\":false,\"prerelease\":false,\"assets\":[{\"name\":\"FS-Camera-Studio-"+version+"-Setup.exe\",\"state\":\"uploaded\",\"size\":"+size+",\"digest\":\"sha256:"+hash+"\",\"browser_download_url\":\""+Core.Repository+"/releases/download/v"+version+"/FS-Camera-Studio-"+version+"-Setup.exe\"}]}";
    }
    static async Task Run(string root,bool live) {
        Directory.CreateDirectory(root);byte[] payload=Encoding.UTF8.GetBytes("fixture-installer-not-executable");string hash;
        using(var input=new MemoryStream(payload))hash=Core.Hash(input);
        string json=Metadata("0.5.2",hash,payload.Length);var release=Core.ReadRelease(json,"0.5.2");
        Check(Core.Newer("0.5.2","0.5.1"),"new version");Check(!Core.Newer("0.5.1","0.5.1"),"same version");Check(!Core.Newer("0.5.0","0.5.1"),"no downgrade");
        Check(Core.ParseRequest("fscamerastudio://update/0.5.2")=="0.5.2","protocol link");
        foreach(var bad in new[]{"https://attacker/update/0.5.2","fscamerastudio://update/0.5.2?url=evil","fscamerastudio://update/0.5.2\" /silent","fscamerastudio://update/../0.5.2","fscamerastudio://update/01.5.2"})
            await Fails(()=>Task.Run(()=>Core.ParseRequest(bad)),"reject malformed protocol");
        foreach(var bad in new[]{json.Replace("\"draft\":false","\"draft\":true"),json.Replace("\"prerelease\":false","\"prerelease\":true"),json.Replace(Core.Repository,"https://evil.example"),json.Replace("sha256:","md5:"),Metadata("0.5.2",hash,Core.MaxInstaller+1)})
            await Fails(()=>Task.Run(()=>Core.ReadRelease(bad,"0.5.2")),"reject invalid metadata");
        await Fails(()=>Task.Run(()=>Core.ReadRelease(json,"0.5.3")),"wrong release");
        foreach(var url in new[]{"http://github.com/a","https://github.com.evil.example/a","https://github.com:444/a","https://user@github.com/a","file:///c:/setup.exe"})Check(!Core.AllowedUrl(new Uri(url)),"reject untrusted transport");
        var mock=new FakeHttp {Response=request=>new HttpResponseMessage(HttpStatusCode.OK){Content=new ByteArrayContent(payload)}};
        string path=Path.Combine(root,"verified.exe");using(var network=new Network(mock))await network.Download(release,path,n=>{},CancellationToken.None);
        Check(File.Exists(path)&&!File.Exists(path+".partial"),"verified completed download");
        int running=3,installs=0;
        await Core.InstallWhenReady(path,release,()=>running>0,t=>{running--;return Task.FromResult(0);},p=>{Check(running==0,"defer installer until exit");installs++;bool locked=false;try{using(var f=File.Open(p,FileMode.Open,FileAccess.Write,FileShare.ReadWrite)){} }catch(IOException){locked=true;}Check(locked,"installer locked against replacement");return Task.FromResult(0);},s=>{},CancellationToken.None);
        Check(installs==1,"launch once");
        var cancel=new CancellationTokenSource();
        await Fails(()=>Core.InstallWhenReady(path,release,()=>true,t=>{cancel.Cancel();return Task.FromResult(0);},p=>{installs++;return Task.FromResult(0);},s=>{},cancel.Token),"cancel waiting");Check(installs==1,"cancel never launches");
        await Fails(()=>Core.InstallWhenReady(path,release,()=>false,t=>Task.FromResult(0),p=>Task.FromResult(7),s=>{},CancellationToken.None),"setup failure is not success");
        File.WriteAllText(path,"corrupted");
        await Fails(()=>Core.InstallWhenReady(path,release,()=>false,t=>Task.FromResult(0),p=>{installs++;return Task.FromResult(0);},s=>{},CancellationToken.None),"reverify before launch");Check(installs==1,"corruption never launches");
        int choices=0,closes=0;
        Check(!await Core.RequestRestart(()=>false,()=>{choices++;return Task.FromResult(true);},()=>closes++,CancellationToken.None),"no restart prompt when closed");
        Check(choices==0&&closes==0,"closed simulator is untouched");
        Check(!await Core.RequestRestart(()=>true,()=>{choices++;return Task.FromResult(false);},()=>closes++,CancellationToken.None),"defer restart");
        Check(closes==0,"later never closes simulator");
        Check(await Core.RequestRestart(()=>true,()=>Task.FromResult(true),()=>closes++,CancellationToken.None),"explicit restart consent");
        Check(closes==1,"one approved close request");
        bool alive=true;
        Check(!await Core.RequestRestart(()=>alive,()=>{alive=false;return Task.FromResult(true);},()=>closes++,CancellationToken.None),"natural exit while prompt open");
        var cancelled=new CancellationTokenSource();cancelled.Cancel();
        await Fails(()=>Core.RequestRestart(()=>true,()=>Task.FromResult(true),()=>closes++,cancelled.Token),"cancel prevents close");Check(closes==1,"no unapproved close");
        string manifest="Simulators=msfs,xplane\nCommunity=C:\\Sim Packages\\Community\nXPlane=D:\\X-Plane 12\n";
        string args=Core.SetupArguments(manifest);
        Check(args.Contains("/VERYSILENT")&&args.Contains("/COMPONENTS=msfs,xplane"),"automatic setup with remembered components");
        Check(args.Contains("\"/COMMUNITY=C:\\Sim Packages\\Community\"")&&args.Contains("\"/XPLANE=D:\\X-Plane 12\""),"remembered folders quoted");
        Check(Core.SetupArguments("Simulators=xplane\nXPlane=D:\\Vols é\\X-Plane 12\n").Contains("Vols é"),"Unicode folder preserved");
        foreach(string bad in new[]{manifest.Replace("msfs,xplane","all"),manifest.Replace("C:\\Sim Packages\\Community","relative"),manifest.Replace("C:\\Sim Packages\\Community","C:relative"),manifest.Replace("C:\\Sim Packages\\Community",@"\root-relative"),manifest.Replace("D:\\X-Plane 12","D:\\bad\" /SILENT"),manifest+"Simulators=msfs\n"})
            await Fails(()=>Task.Run(()=>Core.SetupArguments(bad)),"reject unsafe installation record");
        int scenario=0;
        foreach(var bytes in new[]{Encoding.UTF8.GetBytes("bad"),new byte[payload.Length],new byte[payload.Length+1]}) {
            string broken=Path.Combine(root,"bad"+(scenario++)+".exe");
            using(var network=new Network(new FakeHttp {Response=request=>new HttpResponseMessage(HttpStatusCode.OK){Content=new ByteArrayContent(bytes)}}))
                await Fails(()=>network.Download(release,broken,n=>{},CancellationToken.None),"partial or corrupt download");
            Check(!File.Exists(broken)&&!File.Exists(broken+".partial"),"failed downloads cleaned");
        }
        using(var network=new Network(new FakeHttp {Response=request=>{var r=new HttpResponseMessage(HttpStatusCode.Redirect);r.Headers.Location=new Uri("https://attacker.example/setup.exe");return r;}}))
            await Fails(()=>network.Download(release,Path.Combine(root,"redirect.exe"),n=>{},CancellationToken.None),"reject hostile redirect");
        using(var network=new Network(new FakeHttp {Response=request=>new HttpResponseMessage((HttpStatusCode)429)}))
            await Fails(()=>network.Release(null,CancellationToken.None),"rate limit");
        if(live)using(var network=new Network()){
            var current=await network.Release("0.5.1",CancellationToken.None);
            await network.Download(current,Path.Combine(root,"live-verified-0.5.1.exe"),n=>{},CancellationToken.None);
            Check(current.Version=="0.5.1","real official release metadata and verified download; NEVER execute");
        }
        Console.WriteLine("PASS: "+checks+" updater checks (metadata, URLs, hashes, download failures, cancellation, file lock, deferred launch, installer failure"+(live?", real GitHub download":"")+").");
    }
    static int Main(string[] args){try{if(args.Length==2&&args[0]=="--setup-args"){Console.WriteLine(Core.SetupArguments(File.ReadAllText(args[1])));return 0;}Run(args[0],args.Length>1&&args[1]=="--live-download").GetAwaiter().GetResult();return 0;}catch(Exception e){Console.Error.WriteLine(e);return 1;}}
}
