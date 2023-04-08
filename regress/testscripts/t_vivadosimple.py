import os
import subprocess

class VivadoBuildError(Exception): pass

if not os.access("aux/build.tcl", os.R_OK):
	raise VivadoBuildError("you should pass a build script for us to run!")

proc = subprocess.Popen("vivado -mode batch -source aux/build.tcl",
	shell=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

while True:
	line = proc.stdout.readline()
	if not line: break
	else: print(line.rstrip())

proc.wait()
if proc.returncode != 0:
	raise VivadoBuildError(f"vivado exited with code {proc.returncode}")

# look for out.bit, because we've hard coded that for now i guess
if not os.access("out.bit", os.R_OK):
	raise VivadoBuildError("vivado exited successfully, but no out.bit generated")

save("out.bit")
