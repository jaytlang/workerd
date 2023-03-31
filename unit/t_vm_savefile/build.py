# file edits are transient
# commit the file (at most once) to preserve changes
# made to it (they will be archived by the engine)

with load("testfile.txt", 'r+') as f:
	content = f.read()
	if content != "hi there":
		print(f"got erroneous content {content}")
		raise ValueError("file did not transmit correctly!")
	
	f.write(", friend")

# commit is here
save("testfile.txt")
print("committed ok!")
