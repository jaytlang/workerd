from util import *

output = run_build("testscripts/t_helloworld.py").rstrip()
print(f"expected 'hello world' but got {output}")
