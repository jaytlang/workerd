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
	active.append(nc)

for conn in active:
	label = b"hello"
	file = b"world" * 50000

	testmsg = Message(MessageOp.SENDFILE, label=label, file=file)
	conn.write_bytes(testmsg.to_bytes())

for conn in active:
	timeout = Timeout(1)
	message = Message.from_conn(conn, timeout=timeout)

	assert(message.opcode() == MessageOp.SENDFILE)
	assert(message.label() == "hello")
	assert(message.file() == file)

print("all done, closing connections")
for conn in active: conn.close()
