"""Removes the pid namespace from the web render sandbox. This kernel has no CONFIG_PID_NS,
so clone() fails with EINVAL and no renderer can start.
Usage: patch_sandbox.py IN OUT"""
import hashlib
import json
import re
import sys

src = open(sys.argv[1], "rb").read()
pat = re.compile(
    rb'("__internal__\.com\.ohos\.render": \[\s*\{\s*"sandbox-root": "[^"]*",\s*"sandbox-ns-flags": \[\s*)"pid",\s*("net")'
)
out, n = pat.subn(rb"\1\2", src)
if n != 1:
    sys.exit(f"expected 1 match, got {n}")
a, b = json.loads(src), json.loads(out)
key = "__internal__.com.ohos.render"
def find(doc):
    found = [s[key] for top in doc.values() for s in top if key in s]
    if len(found) != 1:
        sys.exit(f"expected 1 render section, found {len(found)}")
    return found[0]
print("before:", find(a)[0]["sandbox-ns-flags"])
print("after: ", find(b)[0]["sandbox-ns-flags"])
find(a)[0]["sandbox-ns-flags"] = ["net"]
if a != b:
    sys.exit("something else changed")
open(sys.argv[2], "wb").write(out)
print(f"bytes {len(src)} -> {len(out)}, sha {hashlib.sha256(out).hexdigest()[:16]}")
