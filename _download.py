import urllib.request, threading, os

url = "https://curl.se/windows/dl-8.21.0_7/curl-8.21.0_7-win64-mingw.zip"
total = 8629814
threads = 8
chunk = (total + threads - 1) // threads
out = "curl_win64.zip"

parts = [f"_part_{i}" for i in range(threads)]
results = [None] * threads

def download(i):
    start = i * chunk
    end = min(start + chunk, total) - 1
    req = urllib.request.Request(url, headers={"Range": f"bytes={start}-{end}"})
    try:
        with urllib.request.urlopen(req, timeout=120) as r:
            data = r.read()
        with open(parts[i], "wb") as f:
            f.write(data)
        results[i] = len(data)
        print(f"part {i}: {len(data)} bytes", flush=True)
    except Exception as e:
        print(f"part {i} failed: {e}", flush=True)
        results[i] = -1

ts = [threading.Thread(target=download, args=(i,)) for i in range(threads)]
for t in ts:
    t.start()
for t in ts:
    t.join()

if all(r is not None and r >= 0 for r in results):
    with open(out, "wb") as f:
        for p in parts:
            with open(p, "rb") as g:
                f.write(g.read())
    print("OK merged size:", os.path.getsize(out), flush=True)
    for p in parts:
        os.remove(p)
else:
    print("SOME PARTS FAILED", flush=True)
