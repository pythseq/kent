import sys
import radict
import read

if len(sys.argv) != 3:
    print 'Usage Error. use python test.py file key'
    sys.exit(2)

file = sys.argv[1]
key = sys.argv[2]
d = read.readRaFile(file, key)
print d