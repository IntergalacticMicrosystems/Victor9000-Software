; TEST.COM - DOSBox smoke test for MOUSE.COM (see run.sh). Redirects stdout to OUT.TXT,
; EXECs MOUSE.COM four times and logs the INT 33h/FBh/41h vectors, DOS clock and return codes:
;   run 1  no letter                           -> usage screen, rc 1, nothing changed
;   run 2  "a", nothing on the "port"          -> 2 s timeout, "No mouse detected", rc 2,
;                                                 not resident, INT 41h back as it was
;   run 3  "a", INT 08h hook sets RR0 bit 0 of E000:0042 after 9 ticks
;                                              -> replacement notice (DOSBox's INT 33h),
;                                                 detected in ~0.5 s, TSR (rc 0300)
;   run 4  "b" while installed                 -> "already installed", rc 1
        cpu 8086
        org 0x100
start:
        ; shrink our memory block so EXEC has room: keep 4 KB
        mov bx,0x100
        mov ah,0x4a
        int 0x21
        ; create OUT.TXT, dup onto handle 1
        mov dx,fname
        xor cx,cx
        mov ah,0x3c
        int 0x21
        mov bx,ax
        mov cx,1
        mov ah,0x46
        int 0x21                ; handle 1 := file
        ; hook INT 08h: when armed, after 9 ticks (~0.5 s) set bit 0 of E000:0042 (RR0 ch A)
        mov ax,0x3508
        int 0x21
        mov [old08],bx
        mov [old08+2],es
        mov dx,new08
        mov ax,0x2508
        int 0x21
        call log_vec
        mov dx,tail_none                ; run 1: no letter -> usage, rc 1
        call run_mouse
        call log_vec
        mov dx,tail_a                   ; run 2: "a", nothing injected -> no mouse, rc 2, not resident
        call run_mouse
        call log_vec
        mov byte [armed],1              ; run 3: "a" with simulated mouse -> TSR in ~0.5 s
        mov word [ticks],0
        mov dx,tail_a
        call run_mouse
        call log_vec
        mov dx,tail_b                   ; run 4: already installed -> refused, rc 1
        call run_mouse
        call log_vec
        lds dx,[old08]
        mov ax,0x2508
        int 0x21
        mov ax,0x4c00
        int 0x21

new08:  cmp byte [cs:armed],0
        jz .chain
        inc word [cs:ticks]
        cmp word [cs:ticks],9
        jnz .chain
        push ax
        push es
        mov ax,0xe000
        mov es,ax
        mov byte [es:0x42],1            ; RR0 bit 0 = Rx character available
        pop es
        pop ax
.chain: jmp far [cs:old08]
old08:  dd 0
armed:  db 0
ticks:  dw 0

run_mouse:                      ; DX -> command tail (len, text, CR)
        mov [pb_tail],dx
        call stamp
        mov [pb_tail+2],cs
        mov [pb_env],cs         ; dummy
        mov word [pb_fcb1],0x5c
        mov [pb_fcb1+2],cs
        mov word [pb_fcb2],0x6c
        mov [pb_fcb2+2],cs
        mov [save_ss],ss
        mov [save_sp],sp
        push cs
        pop es
        mov bx,pblock
        mov dx,prog
        mov ax,0x4b00
        int 0x21
        mov ss,[cs:save_ss]
        mov sp,[cs:save_sp]
        push cs
        pop ds
        jc .fail
        call stamp
        mov ah,0x4d
        int 0x21                ; AX = child return code (AH=3 if TSR)
        push ax
        mov dx,m_rc
        mov ah,9
        int 0x21
        pop ax
        call hex4
        jmp short .done
.fail:  push ax
        mov dx,m_execfail
        mov ah,9
        int 0x21
        pop ax
        call hex4
.done:  mov dx,crlf
        mov ah,9
        int 0x21
        ret

stamp:                          ; print "[ss.hh]" from the DOS clock
        push ax
        push dx
        mov ah,0x2c
        int 0x21
        push dx
        mov dl,'['
        call putc
        pop ax
        push ax
        mov al,ah
        call hex2
        mov dl,'.'
        call putc
        pop ax
        call hex2
        mov dl,']'
        call putc
        pop dx
        pop ax
        ret

log_vec:
        mov dx,m_vec
        mov ah,9
        int 0x21
        mov ax,0x3533
        int 0x21                ; ES:BX = INT 33h vector
        mov ax,es
        call hex4
        mov dl,':'
        call putc
        mov ax,bx
        call hex4
        mov dl,' '
        call putc
        mov cx,5
        sub bx,5
.sig:   mov dl,[es:bx]
        cmp dl,' '
        jae .ok
        mov dl,'.'
.ok:    call putc
        inc bx
        loop .sig
        mov dl,' '
        call putc
        mov ax,0x35fb
        int 0x21
        mov ax,es
        call hex4
        mov dl,':'
        call putc
        mov ax,bx
        call hex4
        mov dx,m_vec41
        mov ah,9
        int 0x21
        mov ax,0x3541
        int 0x21                ; ES:BX = INT 41h vector (the serial ISR hook)
        mov ax,es
        call hex4
        mov dl,':'
        call putc
        mov ax,bx
        call hex4
        mov dx,crlf
        mov ah,9
        int 0x21
        ret

putc:   mov ah,2
        int 0x21
        ret
hex4:   push ax
        mov al,ah
        call hex2
        pop ax
hex2:   push ax
        mov cl,4
        shr al,cl
        call hex1
        pop ax
hex1:   and al,0xf
        add al,'0'
        cmp al,'9'
        jbe .p
        add al,7
.p:     mov dl,al
        jmp putc

fname:  db 'OUT.TXT',0
prog:   db 'MOUSE.COM',0
tail_none: db 0,13
tail_a: db 2,' a',13
tail_b: db 2,' b',13
m_vec:  db 13,10,'INT33=$'
m_vec41: db ' INT41=$'
m_rc:   db 'exec ok, child rc=$'
m_execfail: db 'EXEC FAILED ax=$'
crlf:   db 13,10,'$'
save_ss: dw 0
save_sp: dw 0
pblock:
pb_env:  dw 0
pb_tail: dd 0
pb_fcb1: dd 0
pb_fcb2: dd 0
