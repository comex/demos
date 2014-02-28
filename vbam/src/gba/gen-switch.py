import sys, re
# I thought just using
#  case 0: armInsnTable[0]();
#  ...etc...
# would optimize into something reasonable, but based on a quick test, I don't
# trust clang not to make copies of duplicate entries in the table.
code = open(sys.argv[1]).read()
size, table = re.search('\[(1024|4096)\] = {(.*?)}', code, re.S).groups()
size = int(size)
table = re.sub('\/\/.*', '', table)
table = re.sub('\s', '', table)
table = re.sub('REP([0-9]+)\((.*?)\)', lambda m: ','.join([m.group(2)] * int(m.group(1))), table)
table = table.strip(',').split(',')
assert len(table) == size
reverse = {}
for idx, func in enumerate(table):
    reverse.setdefault(func, []).append(idx)
for func, idxs in reverse.iteritems():
    for idx in idxs:
        print 'case 0x%x:' % idx
    print '  %s(opcode);' % func
    print '  break;'
