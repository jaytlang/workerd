import subprocess

class BundleException(Exception): pass

def bundle(filelist):
	fileargs = ""
	for file in filelist: fileargs += f" {file}"

	completed = subprocess.run(f"python3 bundle/bundle.py -csf build.bundle {fileargs}",
		shell=True)

	if completed.returncode == 0: return "build.bundle"
	raise BundleException("bundle exited with non-zero return status!")
