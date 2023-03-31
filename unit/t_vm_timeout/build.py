import os
import signal
import subprocess

# objective: kill our parent (mux) and make sure we get
# properly timed out by the engine - so just spin after

print("asking systemd for parent pid")
pidstr = subprocess.check_output("systemctl show --property MainPID --value worker", shell=True)
pid = int(pidstr)

print(f"parent pid is {pid}, stopping it")
os.kill(pid, signal.SIGSTOP)

dead = 0
while True: dead += 1
