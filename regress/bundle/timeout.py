import signal

class Timeout:
	def _handle(self, signum, frame):
		print(f"timeout of {self._timeout} expired")
		exit(1)

	def __init__(self, timeout):
		self._timeout = timeout
		signal.signal(signal.SIGALRM, self._handle)
		signal.alarm(self._timeout)

	def cancel(self):
		signal.alarm(0)
