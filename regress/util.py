import os
import shutil
import subprocess
import sys
import time

def run_build(path, files=[], input=None):
	filestr = ""
	for file in files: filestr += f"{file} "

	shutil.copy(path, "build.py")
	while True:
		try:
			output = subprocess.check_output(f"./remote/r.py build.py {filestr}",
				shell=True, input=input) 
			break
		except subprocess.CalledProcessError as e:
			output = str(e.output, encoding='ascii')
			if "try again later" in output:
				print("waiting for available vms...")
				time.sleep(1)
			else:
				print(f"builder process exited with non-zero: {output}")
				sys.exit(1)
				

	os.remove("build.py")
	return str(output, encoding='ascii')
