import sys

sys.path.append("../../regress/")

from connection import *
from message import *
from timeout import *

import time

active = []

for _i in range(5):
	nc = Connection()
	nc.connect("eecs-digital-51.mit.edu", 443)	

	label = b"hello"
	file = b"world" * 50000

	testmsg = Message(MessageOp.SENDFILE, label=label, file=file)
	nc.write_bytes(testmsg.to_bytes())

	timeout = Timeout(1)
	message = Message.from_conn(nc, timeout=timeout)

	assert(message.opcode() == MessageOp.SENDFILE)
	assert(message.label() == "hello")
	assert(message.file() == file)

	active.append(nc)

print("all done, closing connections")
for conn in active: conn.close()
