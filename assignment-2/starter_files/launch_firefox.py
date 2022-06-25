import subprocess
import sys
import os

if len(sys.argv) != 2:
	print 'Error: Usage is python launch_firefox.py <profile_num>'
	exit(1)

profileNum = sys.argv[1]
devNull = open(os.devnull, 'w')
subprocess.call(["sudo", "-u", "csci4430", "firefox", "-P", "csci4430profile" + profileNum], stdout=devNull, stderr=devNull)
