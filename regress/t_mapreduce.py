from util import *

output = run_build("testscripts/t_mapreduce.py", files=["aux/bees.txt"]).rstrip()
print(output)
