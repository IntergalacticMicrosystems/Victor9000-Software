; =============================================================================
;  MOUSE.COM - the Victor 9000 serial mouse driver as a run-anytime TSR
; =============================================================================
;
;  Same resident code as MOUSE.DEV, built with BUILD_COM defined:
;
;      nasm -f bin MOUSE_COM.asm -o MOUSE.COM        (or: make MOUSE.COM)
;      MOUSE A | MOUSE B                              (no letter: usage screen)
;
;  MOUSE.asm supplies everything that stays resident (INT 33h / INT FBh /
;  INT 41h handlers, cursor code, variables) with org 100h, so every offset
;  in MOUSE_DRIVER.md is valid here as "offset + 100h". The DOS device header,
;  strategy/interrupt routines and device_init are replaced by a jump to
;  com_install below, which does what device_init did with DOS's INIT request:
;      * port letter: first a/A/b/B of the PSP command tail (81h..) instead of
;        the CONFIG.SYS line. No letter shows the usage screen (the .DEV
;        silently defaults to B).
;      * reset_driver with BX = 0 (full hardware init, INT 41h hook)
;      * INT 33h and INT FBh vectors, sign-on message
;      * stay resident with INT 21h AH=31h instead of the INIT break address.
;  Checks the .DEV never made:
;      * no port letter -> usage screen, exit with errorlevel 1, nothing changed.
;      * already installed: an INT 33h handler with the 'MOUSE' signature in
;        front of it (that is how PICASSO detects this driver) -> message,
;        exit with errorlevel 1, nothing changed.
;      * no mouse: after reset_driver has power-cycled the mouse (RTS off,
;        delay, RTS on) a Microsoft mouse answers with 'M'. The installer
;        waits up to 2 s (DOS clock) for the ISR to see any byte (pkt_state
;        leaves 0) or for RR0 bit 0 to report one waiting. Otherwise it
;        resets the channel, restores INT 41h and the 8259 mask it had
;        changed, reports "no mouse detected" and exits with errorlevel 2
;        without staying resident. INT 33h / INT FBh are only hooked after
;        a mouse has answered.
;      * another INT 33h handler without the signature -> notice that it is
;        being replaced (printed only when the install goes ahead).
;
;  Install path tested in DOSBox (a PC: no 7201, so an unaided run reports
;  "no mouse"); not yet run on a Victor 9000 or in MAME victor9k.
; =============================================================================

%define BUILD_COM
%include "MOUSE.asm"

; ============================================================================
;  INSTALLER - not resident (everything from com_install on is released)
; ============================================================================

DETECT_TIMEOUT  equ 200         ; hundredths of a second to wait for the mouse

;-----------------------------------------------------------------------------
; com_install - .COM entry (jumped to from offset 100h)
;
; In : DS = ES = CS = PSP segment; [80h] = command tail length, [81h..] text
;      terminated by CR.
; Order matters: nothing is hooked until a mouse has answered, so a failed
; install leaves the system exactly as it found it (INT 41h, 8259 mask) except
; that the chosen 7201 channel is left reset.
;-----------------------------------------------------------------------------
com_install:
        ; 1. Port letter: first a/A/b/B of the command tail. None -> usage, exit 1.
        mov si,0x81                             ; PSP command tail
.scan:                                          ; same scan as device_init
        lodsb
        cmp al,0xa                              ; LF or CR: end of line
        jz .usage
        cmp al,0xd
        jz .usage
        cmp al,'a'
        jz .port_a
        cmp al,'A'
        jz .port_a
        cmp al,'b'
        jz .port_b
        cmp al,'B'
        jz .port_b
        jmp short .scan
.usage:
        mov dx,msg_usage
        mov ah,0x9
        int 0x21
        mov ax,0x4c01
        int 0x21                                ; exit, errorlevel 1
.port_a:
        mov word [cs:port_index],0x0            ; serial A
        jmp short .check_installed
.port_b:
        mov word [cs:port_index],0x1            ; serial B

        ; 2. Already installed? (the 'MOUSE' signature test PICASSO uses)
.check_installed:
        mov ax,0x3533
        int 0x21                                ; ES:BX = current INT 33h vector
        mov ax,es
        or ax,bx
        jz .not_installed                       ; null vector: nothing there
        cmp word [es:bx-5],'MO'                 ; 'MOUSE' signature before the entry?
        jnz .foreign_handler
        cmp word [es:bx-3],'US'
        jnz .foreign_handler
        cmp byte [es:bx-1],'E'
        jnz .foreign_handler
        mov dx,msg_already
        mov ah,0x9
        int 0x21                                ; "already installed"
        mov ax,0x4c01
        int 0x21                                ; exit, errorlevel 1
.foreign_handler:
        mov byte [cs:foreign],1                 ; some other INT 33h handler: mention it if we install

        ; 3. Remember what reset_driver changes outside this program, then
        ;    bring the hardware up and wait for the mouse.
.not_installed:
        mov ax,0x3541
        int 0x21                                ; ES:BX = current INT 41h vector
        mov [cs:old41],bx
        mov [cs:old41+2],es
        mov ax,IO_SEG
        mov es,ax
        mov al,[es:PIC_IMR]
        mov [cs:old_imr],al                     ; 8259 mask before we unmask IR1
        push cs
        pop ds
        xor bx,bx                               ; BX = 0: include the hardware init
        call reset_driver                       ; reset everything (hooks INT 41h, power-cycles the mouse)
        call detect_mouse
        jnc .install

        ; 4. No mouse: quiet the channel, give INT 41h and the 8259 mask back,
        ;    say so and exit without staying resident.
        mov ax,IO_SEG
        mov es,ax
        mov bx,SIO_CTRL_A
        add bx,[cs:port_index]                  ; ES:BX = our channel's control register
        cli
        mov byte [es:bx],0x1                    ; WR1:
        mov byte [es:bx],0x0                    ;   no interrupts
        mov byte [es:bx],0x18                   ; WR0: channel reset (also drops RTS: mouse unpowered)
        mov al,[cs:old_imr]
        mov [es:PIC_IMR],al                     ; 8259 mask as before
        sti
        lds dx,[cs:old41]
        mov ax,0x2541
        int 0x21                                ; INT 41h back to its previous owner
        push cs
        pop ds
        mov dx,msg_nomouse1
        mov ah,0x9
        int 0x21                                ; "No mouse detected on serial port "
        mov dx,[cs:port_index]
        add dl,'A'
        mov ah,0x2
        int 0x21                                ; port letter
        mov dx,msg_nomouse2
        mov ah,0x9
        int 0x21                                ; "...MOUSE is not installed."
        mov ax,0x4c02
        int 0x21                                ; exit, errorlevel 2

        ; 5. Mouse answered: hook INT 33h / INT FBh, sign on, stay resident.
.install:
        push cs
        pop ds
        cmp byte [foreign],0
        jz .vectors
        mov dx,msg_replacing
        mov ah,0x9
        int 0x21                                ; another INT 33h handler is being replaced
.vectors:
        mov dx,int33_entry
        mov ax,0x2533
        int 0x21                                ; DOS set vector INT 33h
        mov dx,int_fb_entry
        mov ax,0x25fb
        int 0x21                                ; DOS set vector INT FBh (private API)
        mov dx,msg_signon
        mov ah,0x9
        int 0x21                                ; print sign-on
        mov dx,[cs:port_index]
        add dl,'A'                              ; 'A' + port index
        mov ah,0x2
        int 0x21                                ; print the port letter
        mov dx,msg_tail
        mov ah,0x9
        int 0x21                                ; print '.' CR LF
        mov dx,RESIDENT_PARAS                   ; paragraphs to keep: PSP + resident code
        mov ax,0x3100
        int 0x21                                ; terminate and stay resident

old41:          dd 0                            ; INT 41h vector before reset_driver
old_imr:        db 0                            ; 8259 IMR before reset_driver
foreign:        db 0                            ; 1 = a non-MOUSE INT 33h handler was present

;-----------------------------------------------------------------------------
; detect_mouse - wait up to DETECT_TIMEOUT for any byte from the mouse
;
; reset_driver has just dropped and raised RTS, which power-cycles a mouse
; wired per MOUSE.DOC; a Microsoft mouse then sends 'M'. Interrupts are on,
; so the ISR takes the byte: it is a sync byte and leaves pkt_state = 1
; (any later packet bytes cycle it through 1, 2, 0 - at 8 ms per byte a
; polling loop cannot miss that). As a fallback also accept RR0 bit 0 (a
; byte waiting that has not raised an interrupt). Time comes from the DOS
; clock (INT 21h AH=2Ch), so no Victor-specific timer is touched.
; Out: CF = 0 mouse seen, CF = 1 nothing received within the timeout.
; Destroys AX BX CX DX SI ES.
;-----------------------------------------------------------------------------
detect_mouse:
        mov ax,IO_SEG
        mov es,ax
        mov bx,SIO_DATA_A
        add bx,[cs:port_index]                  ; ES:BX = our channel's data, +2 = RR0
        mov ah,0x2c
        int 0x21                                ; DH = seconds, DL = hundredths
        call .time_dx                           ; SI = start time in hundredths (mod 6000)
        mov si,ax
.poll:
        cmp byte [cs:pkt_state],0x0             ; the ISR has seen a sync byte?
        jnz .seen
        test byte [es:bx+0x2],0x1               ; RR0 bit 0: Rx character available?
        jnz .seen
        mov ah,0x2c
        int 0x21
        call .time_dx
        sub ax,si                               ; elapsed, wrapping at the minute
        jnc .no_wrap
        add ax,6000
.no_wrap:
        cmp ax,DETECT_TIMEOUT
        jb .poll
        stc                                     ; timed out
        ret
.seen:
        clc
        ret
.time_dx:                                       ; AX = DH*100 + DL
        mov al,dh
        mov ah,100
        mul ah
        xor dh,dh
        add ax,dx
        ret

; com_install - $$ is the file offset; + 100h for the PSP, rounded up to paragraphs.
RESIDENT_PARAS  equ (com_install - $$ + 0x100 + 15) >> 4

msg_signon:                             ; INT 21h AH=09h, '$'-terminated (as in MOUSE.DEV)
        db      13,10,'Device MOUSE v1.0, (C) Issue 1990 installed for serial port $'
msg_tail:                               ; printed after the port letter
        db      '.',13,10,'$'
msg_already:
        db      13,10,'MOUSE is already installed.',13,10,'$'
msg_replacing:
        db      13,10,'Note: another INT 33h mouse handler is installed and will be replaced.',13,10,'$'
msg_nomouse1:
        db      13,10,'No mouse detected on serial port $'
msg_nomouse2:                           ; printed after the port letter
        db      ': nothing was received after a power cycle.',13,10
        db      'Check the cable and the port letter (MOUSE A or MOUSE B). MOUSE is not installed.',13,10,'$'
msg_usage:
        db      13,10,'MOUSE - Victor 9000 serial mouse driver (Microsoft protocol, INT 33h)',13,10
        db      13,10,'Usage:  MOUSE A      mouse on serial port A',13,10
        db            '        MOUSE B      mouse on serial port B',13,10
        db      13,10,'Stays resident only if a mouse answers on that port.',13,10,'$'
