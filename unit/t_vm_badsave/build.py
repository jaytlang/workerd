print("hiiiiiii")

with open("testfile.txt", 'rb+') as f:
	content = f.read()
	print(f"content length is {len(content)}")
	f.write(b", friend")

# now let's try to save a nonexistent file
try: save("nottestfile.txt")
except: print("got error okay!")

# now commit the right thing...
print("we should get here!!!")
save("testfile.txt")
