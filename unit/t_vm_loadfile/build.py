# use load() instead of open()
# the idea is that load() falls back to
# open if a file already exists

# remember, file edits are transient
# commit the file (at most once) to preserve changes
# made to it (they will be archived by the engine)

with load("testfile.txt", 'r') as f:
	content = f.read()
	if content != "hi there":
		raise ValueError("file did not transmit correctly!")
	
	print("hello to you too")
