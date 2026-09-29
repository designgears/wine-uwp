#!/usr/bin/env python3
"""Download the Windows 11 Enterprise evaluation ISO from Microsoft, resumably.

    download_iso.py OUT.iso [--url URL]

The default URL is Microsoft's own Evaluation Center link (go.microsoft.com/fwlink/?linkid=2334167, which redirects
to aka.ms and on to Microsoft's download server). An interrupted download continues from OUT.iso.part. Progress
goes to stdout as "PROGRESS <bytes done> <bytes total>" and "RATE <bytes per second>" lines, and the result is
checked against the SHA-256 of the images this project has been tested with (KNOWN). An image that is not in that
list is kept, reported as "UNVERIFIED", and still has to pass the harvest, which only accepts a Windows image.
"""

import argparse
import hashlib
import os
import sys
import time
import urllib.request

URL = "https://go.microsoft.com/fwlink/?linkid=2334167"
# size -> (sha256, what it is); the evaluation ISO this project was tested with
KNOWN = {
    7092807680: ("a61adeab895ef5a4db436e0a7011c92a2ff17bb0357f58b13bbc4062e535e7b9",
                 "Windows 11 Enterprise evaluation 25H2, build 26200.6584, en-US"),
}
UA = "wine-uwp-setup (+https://www.microsoft.com/evalcenter)"


def say(*args):
    print(*args, flush=True)


def resolve(url):
    """Follow the redirects to the final download URL, and return it with the file's size."""
    req = urllib.request.Request(url, method="HEAD", headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=60) as resp:
        return resp.geturl(), int(resp.headers.get("Content-Length", 0))


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(1 << 22):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("--url", default=URL)
    args = ap.parse_args()

    say("STAGE Contacting Microsoft")
    final, total = resolve(args.url)
    say(f"SOURCE {final}")
    part = args.out + ".part"
    done = os.path.getsize(part) if os.path.exists(part) else 0
    if total and done > total:
        os.remove(part)
        done = 0
    if not os.path.exists(args.out) or os.path.getsize(args.out) != total:
        say("STAGE Downloading Windows 11" + (" (resuming)" if done else ""))
        headers = {"User-Agent": UA}
        if done:
            headers["Range"] = f"bytes={done}-"
        req = urllib.request.Request(final, headers=headers)
        with urllib.request.urlopen(req, timeout=60) as resp, open(part, "ab" if done else "wb") as f:
            if done and resp.status != 206:   # the server ignored the range: start over
                f.seek(0)
                f.truncate()
                done = 0
            t0, d0, last = time.monotonic(), done, 0.0
            while chunk := resp.read(1 << 20):
                f.write(chunk)
                done += len(chunk)
                now = time.monotonic()
                if now - last >= 0.5:
                    say(f"PROGRESS {done} {total}")
                    say(f"RATE {int((done - d0) / max(now - t0, 1e-3))}")
                    last = now
        if total and done != total:
            raise SystemExit(f"download incomplete: {done} of {total} bytes; run again to resume")
        os.replace(part, args.out)
        say(f"PROGRESS {total} {total}")

    say("STAGE Verifying the download")
    digest = sha256(args.out)
    known = KNOWN.get(os.path.getsize(args.out))
    if known and known[0] == digest:
        say(f"VERIFIED {known[1]}")
    elif known:
        os.remove(args.out)
        raise SystemExit("the download is corrupt (SHA-256 mismatch) and was deleted; please try again")
    else:
        say(f"UNVERIFIED sha256 {digest}: not an image this setup has been tested with")
    return 0


if __name__ == "__main__":
    sys.exit(main())
