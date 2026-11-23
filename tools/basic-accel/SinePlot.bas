10 rem sine wave on the hi-res screen, the way a type-in listing does it
20 poke 53265,peek(53265)or32:poke 53272,peek(53272)or8
30 for i=8192 to 16191:poke i,0:next
40 for i=1024 to 2023:poke i,16:next
50 poke 829,1:t=ti
60 for x=0 to 319
70 y=int(100-80*sin(x/50.93))
80 by=8192+int(y/8)*320+int(x/8)*8+(y and 7)
90 poke by,peek(by)or(2^(7-(x and 7)))
100 next
110 t=ti-t:poke 830,t and 255:poke 831,int(t/256):poke 828,1
120 goto 120
