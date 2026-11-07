"""Exercise the real Windows helper against disposable preferences. Never opens a simulator."""
import json, os, stat, subprocess, sys, tempfile, time
from pathlib import Path
driver, helper, output = map(lambda s: Path(s).resolve(), sys.argv[1:4])
output.mkdir(parents=True, exist_ok=True)
original = b"I\r\n1005 Version\r\n1 <NONE> sim/view/old\r\n1 CTRL sim/view/keep\r\n"
class Session:
    def __init__(self, prefs, worker=helper):
        self.p = subprocess.Popen([str(driver), str(prefs), str(worker)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True, encoding="utf-8", creationflags=subprocess.CREATE_NO_WINDOW)
    def request(self, **payload):
        self.p.stdin.write(json.dumps(payload)+"\n"); self.p.stdin.flush()
        line=self.p.stdout.readline()
        assert line, self.p.stderr.read()
        return json.loads(line)
    def close(self):
        self.p.stdin.close()
        assert self.p.wait(timeout=8)==0
def folder():
    p=Path(tempfile.mkdtemp(prefix="keys-é-",dir=output));(p/"X-Plane Keys.prf").write_bytes(original);return p
def preview(s, command="xpcs/view_1"):
    return s.request(op="preview",profile="X-Plane Keys.prf",command=command,input={"code":"Digit1"})
def assign(s, p, command="xpcs/view_1"):
    return s.request(op="assign",profile="X-Plane Keys.prf",command=command,input={"code":"Digit1"},review=p["review"])
def result(p, expected):
    for _ in range(300):
        try:
            r=json.loads((p/"XPCameraStudio/native-key-result.json").read_text(encoding="utf-8"))
            if r["status"]==expected:return r
        except (OSError,ValueError): pass
        time.sleep(.02)
    raise AssertionError(f"No {expected} result in {p}")
# Missing helper must never produce a successful queued assignment.
p=folder();s=Session(p,p/"missing.exe")
assert not assign(s,preview(s))["ok"];s.close()
assert (p/"X-Plane Keys.prf").read_bytes()==original
assert not (p/"XPCameraStudio/native-key-queue.json").exists()
# Two panels: a reviewed conflict becomes stale when the other panel reassigns that key.
p=folder();s=Session(p);old=preview(s)
assert assign(s,preview(s,"xpcs/view_2"),"xpcs/view_2")["ok"]
assert not assign(s,old)["ok"];s.close()
result(p,"applied");assert b"1 <NONE> xpcs/view_2" in (p/"X-Plane Keys.prf").read_bytes()
# A native save changes the reviewed key: refuse the write, retain the queue, permit cancel/review/retry.
p=folder();s=Session(p);assert assign(s,preview(s))["ok"]
changed=original.replace(b"sim/view/old",b"sim/view/changed")
(p/"X-Plane Keys.prf").write_bytes(changed);s.close();result(p,"error")
assert (p/"X-Plane Keys.prf").read_bytes()==changed
s=Session(p);status=s.request(op="profiles")
assert status["ok"] and status["pendingCount"]==1
assert not s.request(op="inspect",profile="X-Plane Keys.prf",command="xpcs/view_1")["ok"]
assert s.request(op="cancel",profile="X-Plane Keys.prf",revision=status["revision"])["ok"]
assert assign(s,preview(s))["ok"];s.close();result(p,"applied")
assert b"sim/view/changed" not in (p/"X-Plane Keys.prf").read_bytes()
# Another instance of the simulator executable is still alive: do not apply yet.
p=folder();s=Session(p);assert assign(s,preview(s))["ok"]
other=Session(folder());other.request(op="profiles") # ensure process has started
s.close();result(p,"error");assert (p/"X-Plane Keys.prf").read_bytes()==original
other.close()
s=Session(p);assert s.request(op="profiles")["pendingCount"]==1;s.close();result(p,"applied")
# Read-only native preferences cause a recoverable failure, never loss of the original.
p=folder();s=Session(p);assert assign(s,preview(s))["ok"]
file=p/"X-Plane Keys.prf";os.chmod(file,stat.S_IREAD)
try:
    s.close();result(p,"error");assert file.read_bytes()==original
finally:os.chmod(file,stat.S_IWRITE|stat.S_IREAD)
s=Session(p);assert s.request(op="profiles")["ok"];s.close();result(p,"applied")
assert b"1 <NONE> xpcs/view_1" in file.read_bytes()
print("PASS: missing helper, stale preview, changed native save, cancel/retry, another process running, read-only recovery, Unicode paths.")
