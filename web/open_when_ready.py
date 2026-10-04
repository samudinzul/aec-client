"""Poll the local aec-web server and open the GUI once
it answers. Runs minimized from start.bat; failure just
means no browser opens (the server window stays up)."""

import sys
import time
import webbrowser
import urllib.request

for _ in range(30):  # ~30 s startup grace
    try:
        urllib.request.urlopen("http://127.0.0.1:8000/", timeout=1)
        break
    except Exception:
        time.sleep(1.0)
else:
    sys.exit(1)
webbrowser.open("http://localhost:8000")
