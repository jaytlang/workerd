import time

print("never gonna give you up!")
print("never gonna let you down!")

with open("somefile.txt", 'r') as f:
	print(f.read())

print("enter your favorite number")
number = readline()

with open("output.txt", 'w+') as f:
	if number == 69: f.write("BASED")
	else: f.write("NOT BASED")

save("output.txt")
time.sleep(1)
