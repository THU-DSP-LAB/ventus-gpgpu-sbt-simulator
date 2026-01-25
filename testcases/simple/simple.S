
.global _start
_start:
    li      t0, 0x90000000
    vid.v	v2
    vsll.vi v3, v2, 2
    vadd.vx v0, v3, t0
    vlw12.v	v1, 0(v0)
    vadd.vv	v1, v1, v2
    vsw12.v	v1, 0(v0)
    endprg	x0, x0, x0
