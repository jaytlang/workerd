import sys

sys.path.append("../../regress")

from connection import *
from message import *
from timeout import *

import time

nc = Connection()
nc.connect("eecs-digital-51.mit.edu", 8123)

time.sleep(2)
