with open("testfile.txt", 'r+') as f:
	content = f.read()
	if content != "hi there":
		print(f"got erroneous content {content}")
		raise ValueError("file did not transmit correctly!")
	
	f.write(", friend")

# commit is here
save("testfile.txt")
print("committed ok!")
