import re,sys
W,H=92,28
grid=[[' ']*W for _ in range(H)]
fg=[[None]*W for _ in range(H)]
s=open(sys.argv[1],'rb').read().decode('utf-8','replace')
i=0; cx=cy=0
while i<len(s):
    c=s[i]; i+=1
    if c=='\x1b':
        n=s[i]; i+=1
        if n=='[':
            m=''
            while i<len(s) and s[i] not in 'A-HJKfnPm':
                m+=s[i]; i+=1
            if i<len(s): f=s[i]; i+=1
            else: break
            if f=='H':
                p=m.split(';') if m else ['1','1']
                cy=(int(p[0])-1 if p[0] else cy)
                cx=(int(p[1])-1 if len(p)>1 and p[1] else cx)
        elif n in '()#': i+=1
        continue
    if c=='\n': next_=True; cy=min(cy+1,H-1); cx=0; continue
    if c=='\r': cx=0; continue
    if c=='\b': cx=max(0,cx-1); continue
    if 0<=cy<H and 0<=cx<W: grid[cy][cx]=c
    cx=min(cx+1,W)
print('+'+'-'*W+'+')
for r in grid: print('|'+''.join(r).rstrip()+'|')
print('+'+'-'*W+'+')
