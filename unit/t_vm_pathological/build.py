import socket
import subprocess

gethost = "route -n | grep '^0.0.0.0' | tr -s ' ' | cut -f 2 -d ' '"
host = str(subprocess.check_output(gethost, shell=True), encoding='ascii').rstrip()
port = 8123

try:
	print(f"attempting to connect to {host}:{port}")
	s = socket.create_connection((host, port), timeout=1)
	assert False, f"connect to {host}:{port} should have failed, but returned {s}"

except TimeoutError: print("connection was killed as expected")
