print("hiiiiiii")

with open("testfile.txt", 'rb+') as f:
	content = f.read()
	print(f"content length is {len(content)}")
	f.write(b", friend")

# commit is here
save("testfile.txt")
print("committed ok!")
