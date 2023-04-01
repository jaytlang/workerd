import socket
import subprocess
import time

gethost = "route -n | grep '^0.0.0.0' | tr -s ' ' | cut -f 2 -d ' '"
host = str(subprocess.check_output(gethost, shell=True), encoding='ascii').rstrip()
port = 8123

try:
	print(f"attempting to connect to {host}:{port}")
	s = socket.create_connection((host, port), timeout=1)
	s.sendall(b"lololololololol")

	print(f"connect to {host}:{port} returned ok - hopefully the host doesn't see us")

except Exception as e:
	error_class = e.__class__.__name__
	print(f"connection was killed, which is kinda cool: {error_class}")

time.sleep(2)
