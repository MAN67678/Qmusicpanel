import os,pty,time,select,fcntl,termios,struct,sys
pid,fd=pty.fork()
if pid==0:
    os.environ['TERM']='xterm-256color'
    os.execvp("bash",["bash","-c",sys.argv[1]])
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", int(os.environ.get('ROWS', 28)), int(os.environ.get('COLS', 92)), 0, 0))
buf=b''
interval=float(sys.argv[2]); end=float(sys.argv[3])
out=open(sys.argv[4],'wb')
def pump(t0,t1):
    global buf
    while time.time()<t1:
        r,_,_=select.select([fd],[],[],0.1)
        if r:
            try: d=os.read(fd,65536)
            except OSError: return
            if not d: return
            buf+=d
# WAIT_FOR: block until the app has actually painted. Audio device probing can
# take a few seconds before the first frame, and keystrokes sent before raw mode
# is engaged are echoed by the tty instead of reaching the app.
mark = os.environ.get('WAIT_FOR')
if mark:
    deadline = time.time() + float(os.environ.get('WAIT_TIMEOUT', 25))
    mb = mark.encode()
    while time.time() < deadline and mb not in buf:
        pump(time.time(), time.time() + 0.2)
pump(time.time(),time.time()+interval)
if os.environ.get('KEYS'):
    keys=os.environ['KEYS'].split('|')
    for k in keys:
        os.write(fd, bytes(k,'utf-8').decode('unicode_escape').encode('latin1') if '\\' in k else k.encode())
        pump(time.time(),time.time()+0.6)
pump(time.time(),time.time()+end)
out.write(buf)
# kill the child: a lingering instance keeps the audio device open, and the
# next run then blocks in device probing for ~25s before its first frame
try:
    os.kill(pid, 9)
    os.waitpid(pid, 0)
except OSError:
    pass
