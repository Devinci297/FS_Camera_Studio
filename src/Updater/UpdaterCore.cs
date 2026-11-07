using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Security.Cryptography;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using System.Web.Script.Serialization;

namespace CameraStudio.Update {
public sealed class Release {
    public string Version, Url, Sha256;
    public long Size;
}
public static class Core {
    public const string Repository="https://github.com/Devinci297/FS_Camera_Studio";
    public const long MaxInstaller=256L*1024*1024;
    public static bool ValidVersion(string value) {
        if(value==null || !Regex.IsMatch(value,@"^(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})$"))return false;
        return value.Split('.').All(p=>Int32.Parse(p)<=65535);
    }
    public static string ParseRequest(string value) {
        if(value==null)return null;
        const string prefix="fscamerastudio://update/";
        if(!value.StartsWith(prefix,StringComparison.Ordinal))throw new InvalidDataException("Invalid update link.");
        string version=value.Substring(prefix.Length).TrimEnd('/');
        if(!ValidVersion(version))throw new InvalidDataException("Invalid update version.");
        return version;
    }
    public static bool Newer(string version,string current) {
        if(!ValidVersion(version)||!ValidVersion(current))throw new InvalidDataException("Invalid installed or release version.");
        return new Version(version)>new Version(current);
    }
    public static Dictionary<string,object> Json(string text) {
        if(text==null || text.Length>2*1024*1024)throw new InvalidDataException("Update metadata is too large.");
        var result=new JavaScriptSerializer { MaxJsonLength=2*1024*1024 }.DeserializeObject(text) as Dictionary<string,object>;
        if(result==null)throw new InvalidDataException("Invalid update metadata.");return result;
    }
    static string Str(Dictionary<string,object> o,string key) { object v;return o.TryGetValue(key,out v)?v as string:null; }
    public static Release ReadRelease(string text,string requested) {
        var o=Json(text);object value;
        if(!o.TryGetValue("draft",out value)||!(value is bool)||(bool)value || !o.TryGetValue("prerelease",out value)||!(value is bool)||(bool)value)
            throw new InvalidDataException("Only published normal releases can be installed.");
        string tag=Str(o,"tag_name");if(tag==null || !tag.StartsWith("v",StringComparison.Ordinal))throw new InvalidDataException("Invalid release tag.");
        string version=tag.Substring(1);
        if(!ValidVersion(version)||(requested!=null && requested!=version))throw new InvalidDataException("Unexpected update version.");
        if(!o.TryGetValue("assets",out value)||!(value is object[]))throw new InvalidDataException("Release has no installer assets.");
        string name="FS-Camera-Studio-"+version+"-Setup.exe",url=Repository+"/releases/download/v"+version+"/"+name;
        foreach(var item in (object[])value) {
            var asset=item as Dictionary<string,object>;if(asset==null||Str(asset,"name")!=name)continue;
            string digest=Str(asset,"digest");long size;
            if(Str(asset,"state")!="uploaded" || Str(asset,"browser_download_url")!=url || digest==null || !Regex.IsMatch(digest,@"^sha256:[a-fA-F0-9]{64}$") ||
                !asset.TryGetValue("size",out value) || !Int64.TryParse(Convert.ToString(value,System.Globalization.CultureInfo.InvariantCulture),out size) || size<=0 || size>MaxInstaller)
                throw new InvalidDataException("The release installer has no valid size, official URL or SHA-256 digest.");
            return new Release { Version=version,Url=url,Sha256=digest.Substring(7).ToLowerInvariant(),Size=size };
        }
        throw new InvalidDataException("The release installer is not available yet. Try again later.");
    }
    public static bool AllowedUrl(Uri uri) {
        return uri!=null && uri.Scheme=="https" && uri.Port==443 && uri.UserInfo=="" && uri.Fragment=="" &&
            new[]{"api.github.com","github.com","release-assets.githubusercontent.com","objects.githubusercontent.com"}.Contains(uri.Host);
    }
    public static string Hash(Stream stream) { using(var sha=SHA256.Create())return BitConverter.ToString(sha.ComputeHash(stream)).Replace("-","").ToLowerInvariant(); }
    public static void Verify(Stream stream,Release release) {
        if(stream.Length!=release.Size)throw new InvalidDataException("Installer download size did not match the release.");
        stream.Position=0;if(Hash(stream)!=release.Sha256)throw new InvalidDataException("Installer SHA-256 verification failed. Nothing was installed.");stream.Position=0;
    }
    public static void PlainDirectory(string path) {
        var current=new DirectoryInfo(Path.GetFullPath(path));
        while(current!=null){if(current.Exists && (current.Attributes&FileAttributes.ReparsePoint)!=0)throw new IOException("Updater paths cannot be junctions or symbolic links.");current=current.Parent;}
    }
    public static string SetupArguments(string manifest) {
        var data=new Dictionary<string,string>(StringComparer.Ordinal);
        foreach(string raw in manifest.Split(new[]{'\r','\n'},StringSplitOptions.RemoveEmptyEntries)) {
            string line=raw.TrimStart('\uFEFF');int equals=line.IndexOf('=');
            if(equals<=0)throw new InvalidDataException("Invalid installation record.");
            string key=line.Substring(0,equals);if(data.ContainsKey(key))throw new InvalidDataException("Duplicate installation setting.");data.Add(key,line.Substring(equals+1));
        }
        string components,community,xplane;
        if(!data.TryGetValue("Simulators",out components)||!(components=="msfs"||components=="xplane"||components=="msfs,xplane"))throw new InvalidDataException("Installed simulators are unknown.");
        string arguments="/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /COMPONENTS="+components;
        if(components.Contains("msfs")) {
            if(!data.TryGetValue("Community",out community))throw new InvalidDataException("MSFS installation folder is missing.");
            arguments+=" "+QuoteArgument("/COMMUNITY="+InstallPath(community));
        }
        if(components.Contains("xplane")) {
            if(!data.TryGetValue("XPlane",out xplane))throw new InvalidDataException("X-Plane installation folder is missing.");
            arguments+=" "+QuoteArgument("/XPLANE="+InstallPath(xplane));
        }
        return arguments;
    }
    static string InstallPath(string path) {
        if(String.IsNullOrWhiteSpace(path)||path.IndexOfAny(new[]{'\"','\r','\n','\0'})>=0||!Regex.IsMatch(path,@"^[A-Za-z]:[\\/]"))
            throw new InvalidDataException("Invalid installation folder.");
        return Path.GetFullPath(path).TrimEnd(Path.DirectorySeparatorChar,Path.AltDirectorySeparatorChar);
    }
    public static string QuoteArgument(string arg) {
        // Windows CreateProcess argument quoting, including backslashes before quotes and at the end.
        return "\""+Regex.Replace(arg,@"(\\*)\""",m=>m.Groups[1].Value+m.Groups[1].Value+"\\\"")+
            new string('\\',arg.Length-arg.TrimEnd('\\').Length)+"\"";
    }
    public static async Task<bool> RequestRestart(Func<bool> running,Func<Task<bool>> choose,Action close,CancellationToken token) {
        token.ThrowIfCancellationRequested();if(!running())return false;
        bool approved=await choose();token.ThrowIfCancellationRequested();
        if(approved && running()){close();return true;}return false;
    }
    public static async Task WaitForExit(Func<bool> simulatorsRunning,Func<CancellationToken,Task> delay,Action<string> report,CancellationToken token) {
        bool announced=false;
        while(simulatorsRunning()) {
            token.ThrowIfCancellationRequested();
            if(!announced){report("Waiting for the simulators to close. Respond to any simulator save/exit prompt; setup will start automatically.");announced=true;}
            await delay(token);
        }
        token.ThrowIfCancellationRequested();
    }
    public static async Task<int> InstallWhenReady(string path,Release release,Func<bool> running,Func<CancellationToken,Task> delay,
        Func<string,Task<int>> install,Action<string> report,CancellationToken token) {
        // Keep the verified file locked against modification through launch and setup completion.
        using(var stream=new FileStream(path,FileMode.Open,FileAccess.Read,FileShare.Read)) {
            Verify(stream,release);await WaitForExit(running,delay,report,token);
            report("Installing Camera Studio "+release.Version+". Please keep both simulators closed.");
            int code=await install(path);
            if(code!=0)throw new IOException("Setup did not complete (exit code "+code+"). Your update is not marked as installed. Close both simulators and try again.");
            return code;
        }
    }
}
public sealed class Network : IDisposable {
    readonly HttpClient http;
    public Network(HttpMessageHandler handler=null) {
        ServicePointManager.SecurityProtocol=SecurityProtocolType.Tls12;
        http=new HttpClient(handler ?? new HttpClientHandler { AllowAutoRedirect=false });http.Timeout=TimeSpan.FromMinutes(10);
        http.DefaultRequestHeaders.UserAgent.ParseAdd("FS-Camera-Studio-Updater/1.0");
    }
    async Task<HttpResponseMessage> Get(string url,CancellationToken token) {
        Uri uri=new Uri(url);
        for(int n=0;n<6;n++) {
            if(!Core.AllowedUrl(uri))throw new InvalidDataException("The update redirected outside the official GitHub download hosts.");
            var response=await http.GetAsync(uri,HttpCompletionOption.ResponseHeadersRead,token);
            int code=(int)response.StatusCode;
            if(code>=300 && code<400) {
                var location=response.Headers.Location;response.Dispose();
                if(location==null)throw new IOException("Invalid download redirect.");uri=location.IsAbsoluteUri?location:new Uri(uri,location);continue;
            }
            if(!response.IsSuccessStatusCode){response.Dispose();throw new IOException("GitHub request failed (HTTP "+code+"). Please try again later.");}
            return response;
        }
        throw new IOException("Too many download redirects.");
    }
    public async Task<Release> Release(string requested,CancellationToken token) {
        string url="https://api.github.com/repos/Devinci297/FS_Camera_Studio/releases/"+(requested==null?"latest":"tags/v"+requested);
        using(var response=await Get(url,token))using(var input=await response.Content.ReadAsStreamAsync())using(var output=new MemoryStream()) {
            await Copy(input,output,2*1024*1024,null,token);
            return Core.ReadRelease(System.Text.Encoding.UTF8.GetString(output.ToArray()),requested);
        }
    }
    public static async Task Copy(Stream input,Stream output,long limit,Action<long> progress,CancellationToken token) {
        byte[] buffer=new byte[65536];long total=0;int read;
        while((read=await input.ReadAsync(buffer,0,buffer.Length,token))>0) {
            total+=read;if(total>limit)throw new InvalidDataException("Download exceeded its declared size.");
            await output.WriteAsync(buffer,0,read,token);if(progress!=null)progress(total);
        }
    }
    public async Task Download(Release release,string path,Action<int> progress,CancellationToken token) {
        string partial=path+".partial";
        try {
            using(var response=await Get(release.Url,token)) {
                if(response.Content.Headers.ContentLength.HasValue && response.Content.Headers.ContentLength.Value!=release.Size)throw new InvalidDataException("Download length differs from the release.");
                using(var input=await response.Content.ReadAsStreamAsync())using(var output=new FileStream(partial,FileMode.CreateNew,FileAccess.ReadWrite,FileShare.None,65536,true)) {
                    await Copy(input,output,release.Size,n=>progress((int)(n*100/release.Size)),token);
                    await output.FlushAsync(token);Core.Verify(output,release);
                }
            }
            token.ThrowIfCancellationRequested();File.Move(partial,path);
        } finally { if(File.Exists(partial))File.Delete(partial); }
    }
    public void Dispose(){http.Dispose();}
}
}
