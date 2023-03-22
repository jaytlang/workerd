import sys

sys.path.append("../../regress")

from connection import *

import time

def open_and_close():
	newconn = Connection()
	newconn.connect("localhost", 8123)
	newconn.close()

for _i in range(5):
	open_and_close()
