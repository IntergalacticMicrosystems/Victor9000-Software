; =============================================================================
;  MOUSE.DEV - serial mouse driver for the Victor 9000 / Sirius 1 (MS-DOS 3.1)
;  "Device MOUSE v1.0, (C) Issue 1990"
; =============================================================================
;
;  Byte-exact NASM disassembly of MOUSE.DEV (3,294 bytes), shipped on the RAT
;  mouse disk (../RAT/) together with MOUSE.DOC and the PICASSO.EXE paint program.
;
;      nasm -f bin MOUSE.asm -o MOUSE_rebuilt.bin && cmp MOUSE.DEV MOUSE_rebuilt.bin
;      (or: make verify)
;
;  WHAT IT IS
;  A raw DOS character-device driver (no EXE header, org 0), loaded by
;      device = mouse.dev a          (a = serial port A, b = serial port B)
;  It does nothing through the DOS device interface itself - every request
;  except INIT is answered "unknown command". INIT parses the port letter,
;  programs the hardware and hooks three interrupt vectors:
;      INT 33h  Microsoft-style mouse API, functions 0..16      (int33_entry)
;      INT FBh  small private API: presence / buttons / raw motion (int_fb_entry)
;      INT 41h  8259 IR1 = NEC uPD7201 serial interrupt          (serial_isr)
;
;  HOW IT WORKS
;  * The mouse is a Microsoft serial mouse: 1200 baud, 7 data bits, no parity,
;    1 stop bit, 3-byte packets (sync byte has bit 6 set). init_serial_hw sets
;    the 8253 baud counter to 65 (1.25 MHz / 65 / 16 = 1202 baud), programs the
;    7201 channel for 7N1 x16 with an interrupt per received character, and
;    raises RTS, which powers/wakes the mouse.
;  * serial_isr assembles packets, tracks button edges (press/release counts
;    and positions), converts mickeys to pixels (ratio, simple 2x acceleration),
;    clamps the position to the range, calls the user event handler (fn 12) and
;    redraws the cursor.
;  * The cursor is drawn by the driver itself:
;      text mode     - hardware 6845 cursor (default after reset), or a software
;                      cursor that AND/XORs the screen-RAM word of the cell;
;      graphics mode - a 16x16 AND/XOR bitmap cursor drawn straight into the
;                      font-cell RAM that the 800x400 graphics screen is made of
;                      (see draw_graphics_cursor for the layout it assumes).
;
;  MEMORY MAP (offsets in the driver segment)
;      0000-0015  DOS device header + saved request-header pointer
;      0016-0052  strategy / interrupt routines
;      0053-0084  INT FBh private API
;      0085-0089  'MOUSE' signature just before the INT 33h entry point
;      008A-0389  INT 33h entry, dispatcher and functions 0..16
;      038A-05C9  serial receive ISR (packet decoder), acceleration
;      05CA-0891  cursor draw/erase, text cell address, clamping, mode detect,
;                 hardware init
;      0892-08BF  7201 poll / EOI helpers
;      08C0-0ADA  graphics cursor draw and restore
;      0ADB-0C1F  variables (all resident)
;      0C20-0CDD  INIT code + sign-on text: discarded (break address = 0C20h)
;
;  All Victor I/O is memory mapped; the driver reaches every device through
;  ES = E000h (see the equates below), so E000:8000 = E800:0000 is the 6845.
;
;  BUILD_COM: MOUSE_COM.asm defines this and %includes this file to build
;  MOUSE.COM, a run-anytime TSR with the same resident code at org 100h. The
;  three %ifdef/%ifndef BUILD_COM blocks (org, device header + strategy/
;  interrupt, device_init) are the only differences; the .DEV build is
;  unchanged and still byte-exact.
;
;  Encoding note: the original was built with MASM, which encodes reg,reg ALU
;  and MOV instructions with the "reg <- r/m" opcode (8B, 03, 0B, 23, 2B, 33,
;  3B, 8A, 0A, 22), and some imm16 compares with the full-width form. NASM
;  picks the other opcode, so those lines use the `rr` macro below or
;  `strict word`. Everything else is plain NASM.
; =============================================================================

        cpu     8086
        bits    16
%ifdef BUILD_COM
        org     0x100           ; MOUSE_COM.asm: .COM build, see com_install
%else
        org     0
%endif

; ---------------------------------------------------------------------------
; MASM-compatible reg,reg encoding:  rr <op>, <dst>, <src>  ==  <op> dst,src
; ---------------------------------------------------------------------------
%define R16_ax 0
%define R16_cx 1
%define R16_dx 2
%define R16_bx 3
%define R16_sp 4
%define R16_bp 5
%define R16_si 6
%define R16_di 7
%define R8_al 0
%define R8_cl 1
%define R8_dl 2
%define R8_bl 3
%define R8_ah 4
%define R8_ch 5
%define R8_dh 6
%define R8_bh 7
%define OP16_mov 0x8B
%define OP16_add 0x03
%define OP16_or  0x0B
%define OP16_and 0x23
%define OP16_sub 0x2B
%define OP16_xor 0x33
%define OP16_cmp 0x3B
%define OP8_mov  0x8A
%define OP8_or   0x0A
%define OP8_and  0x22
%macro rr 3
  %ifdef R8_%2
        db OP8_%1, 0xC0 | (R8_%2 << 3) | R8_%3
  %else
        db OP16_%1, 0xC0 | (R16_%2 << 3) | R16_%3
  %endif
%endmacro

; ---------------------------------------------------------------------------
; Hardware (offsets relative to IO_SEG = E000h unless noted)
; ---------------------------------------------------------------------------
IO_SEG          equ 0xE000      ; Victor I/O block E0000h
PIC_OCW         equ 0x0000      ; 8259A A0=0: OCW2 (EOI)            E0000h
PIC_IMR         equ 0x0001      ; 8259A A0=1: OCW1 interrupt mask   E0001h
PIC_VECTOR_IR1  equ 0x41        ; 8259 vector base 40h + IR1 (COMM = 7201)
PIC_EOI_IR1     equ 0x61        ; OCW2: specific EOI, level 1
PIT_COUNTER0    equ 0x0020      ; 8253 counter 0 -> 7201 ch A Rx/Tx clock  E0020h
PIT_COUNTER1    equ 0x0021      ; 8253 counter 1 -> 7201 ch B Rx/Tx clock  E0021h
PIT_CONTROL     equ 0x0023      ; 8253 mode/control word                  E0023h
SIO_DATA_A      equ 0x0040      ; uPD7201 channel A data   (+1 = channel B data)
SIO_CTRL_A      equ 0x0042      ; uPD7201 channel A control (+1 = channel B control)
CRTC_ADDR       equ 0x8000      ; HD46505 address register  (E8000h = E800:0000)
CRTC_DATA       equ 0x8001      ; HD46505 data register     (E8001h)
SCREEN_SEG      equ 0xF000      ; screen RAM, 2048 words: bits 10-0 font cell, 15-11 attributes

SCREEN_W        equ 800         ; graphics screen width / height in pixels
SCREEN_H        equ 400
STRIP_BYTES     equ 0x320       ; one 16-pixel-wide column strip: 400 rows x 1 word
MASK_BYTES      equ 0x20        ; 16 words: cursor_image_mask follows cursor_screen_mask

; DOS request header fields
RQ_COMMAND      equ 0x02        ; command code (0 = INIT)
RQ_STATUS       equ 0x03        ; status word
RQ_BREAK_OFS    equ 0x0E        ; INIT: end of resident code (offset)
RQ_BREAK_SEG    equ 0x10        ; INIT: end of resident code (segment)
RQ_CMDLINE      equ 0x12        ; INIT: far pointer to the CONFIG.SYS line after "device="

; Caller registers as saved by int33_dispatch (BP = SP after the pushes)
REG_DX          equ 0x0A
REG_CX          equ 0x0C
REG_BX          equ 0x0E
REG_AX          equ 0x10

; =============================================================================
;  DOS DEVICE HEADER
; =============================================================================
%ifndef BUILD_COM
device_header:
        dd      -1                      ; next driver: FFFF:FFFF (filled in by DOS)
        dw      0x8000                  ; attribute: character device, nothing else
        dw      strategy                ; strategy entry
        dw      interrupt               ; interrupt entry
        db      'MOUSE',0,0,0           ; device name, padded with NULs (not blanks)
request_ptr:
        dd      0                       ; request header saved by the strategy routine

; ============================================================================
;  DOS DRIVER ENTRY POINTS
; ============================================================================

;-----------------------------------------------------------------------------
; strategy - DOS strategy entry (far call)
;
; In : ES:BX = request header
; Out: request header pointer saved in request_ptr
;-----------------------------------------------------------------------------
strategy:
        mov [cs:request_ptr],bx                 ; remember the request header
        mov [cs:request_ptr+2],es
        retf

;-----------------------------------------------------------------------------
; interrupt - DOS interrupt entry (far call)
;
; Command 0 (INIT) goes to device_init, which lives in the discarded part
; of the image. Every other command is rejected with 8103h (error, done,
; "unknown command"): the driver is only reachable through INT 33h / FBh.
; request_done is also the exit used by device_init (status 01xxh = done).
;-----------------------------------------------------------------------------
interrupt:
        push ax
        push bx
        push cx
        push dx
        push si
        push di
        push bp
        push ds
        push es
        lds bx,[cs:request_ptr]                 ; DS:BX = request header
        mov al,[bx+RQ_COMMAND]                  ; command code
        rr or, al,al
        jnz .unknown_command                    ; 0 = INIT?
        jmp near device_init                    ; yes: device_init (jumps back to request_done)
.unknown_command:
        mov ax,0x8103                           ; status: error + done + 03 unknown command
        jmp short request_done.store_status     ; MASM-padded short jump (EB xx 90)
        nop
request_done:
        mov ah,0x1                              ; status high byte: done (AL is left as is)
.store_status:
        lds bx,[cs:request_ptr]
        mov [bx+RQ_STATUS],ax                   ; store the status word
        pop es
        pop ds
        pop bp
        pop di
        pop si
        pop dx
        pop cx
        pop bx
        pop ax
        retf                                    ; far return to DOS
%else
; BUILD_COM: no device header. The COM starts with a jump to the installer
; (com_install in MOUSE_COM.asm) and pads to 0053h so that every resident
; offset below is exactly the .DEV offset + 100h.
        jmp near com_install
        times 0x53-($-$$) db 0
%endif ; BUILD_COM

; ============================================================================
;  INT FBh - PRIVATE API
; ============================================================================

;-----------------------------------------------------------------------------
; int_fb_entry - INT FBh handler
;
; A tiny non-Microsoft interface (presumably for PICASSO.EXE) that reads
; the raw mouse instead of the cursor position.
; In : CX = 1  presence check  -> AX = 1
;      CX = 2  read            -> AX = button_state (bit 0 left, bit 1 right)
;                                 BX = raw X mickeys since the last read
;                                 CX = raw Y mickeys, NEGATED (+ = up)
;      other   AX unchanged
; Every call (any CX) clears raw_x/raw_y - the same counters INT 33h fn 11
; returns, so mixing the two APIs loses motion. No registers are saved and
; DS is not touched (all accesses use CS:).
;-----------------------------------------------------------------------------
int_fb_entry:
        cmp cx,byte 0x1
        jz .presence
        cmp cx,byte 0x2
        jz .read_motion
        jmp short .clear_counters
        nop
.presence:
        mov ax,0x1
        jmp short .clear_counters
        nop
.read_motion:
        mov ax,[cs:button_state]
        mov bx,[cs:raw_x]
        mov cx,[cs:raw_y]
        neg cx
.clear_counters:
        mov word [cs:raw_x],0x0                 ; all paths: clear the raw motion counters
        mov word [cs:raw_y],0x0
        iret

; ============================================================================
;  INT 33h - MICROSOFT MOUSE API
; ============================================================================
int33_signature:
        db 'MOUSE'                              ; signature right before the INT 33h entry

;-----------------------------------------------------------------------------
; int33_entry - INT 33h vector target
;
; Re-enables interrupts and calls the dispatcher, so the dispatcher can use
; a near RET and the saved-register frame below.
;-----------------------------------------------------------------------------
int33_entry:
        sti
        call int33_dispatch
        iret

;-----------------------------------------------------------------------------
; int33_dispatch - INT 33h function dispatcher
;
; In : AL = function (AH is ignored: CBW sign-extends AL, so 80h-FFh are
;      "negative" and fall out of the unsigned range check like 17-127)
; Out: handlers return values by overwriting the saved caller registers:
;      [bp+REG_AX]=AX  [bp+REG_BX]=BX  [bp+REG_CX]=CX  [bp+REG_DX]=DX
;      DS = CS inside every handler; ES, SI, DI, BP are restored on exit.
; Implemented: 0-12, 15, 16. 13/14 (light pen) are accepted no-ops,
; 17 and up are ignored.
;-----------------------------------------------------------------------------
int33_dispatch:
        push ax
        push bx
        push cx
        push dx
        push di                                 ; note: DI is pushed before SI
        push si
        push bp
        push ds
        push es
        rr mov, bp,sp                           ; BP = frame: +0 ES +2 DS +4 BP +6 SI +8 DI +A DX +C CX +E BX +10 AX
        cld
        cbw                                     ; function number = AL only
        push cs
        pop ds
        cmp ax,strict word 0x0                  ; if-chain, one compare per function
        jnz .try_1
        call fn00_reset                         ; AX=0  reset driver and read status
        jmp near .return
.try_1:
        cmp ax,strict word 0x1
        jnz .try_2
        call fn01_show_cursor                   ; AX=1  show cursor
        jmp near .return
.try_2:
        cmp ax,strict word 0x2
        jnz .try_3
        call fn02_hide_cursor                   ; AX=2  hide cursor
        jmp near .return
.try_3:
        cmp ax,strict word 0x3
        jnz .try_4
        call fn03_get_status                    ; AX=3  get position and button status
        jmp near .return
.try_4:
        cmp ax,strict word 0x4
        jnz .try_5
        call fn04_set_position                  ; AX=4  set cursor position
        jmp short .return
        nop
.try_5:
        cmp ax,strict word 0x5
        jnz .try_6
        call fn05_press_info                    ; AX=5  button press information
        jmp short .return
        nop
.try_6:
        cmp ax,strict word 0x6
        jnz .try_7
        call fn06_release_info                  ; AX=6  button release information
        jmp short .return
        nop
.try_7:
        cmp ax,strict word 0x7
        jnz .try_8
        call fn07_set_x_range                   ; AX=7  set horizontal range
        jmp short .return
        nop
.try_8:
        cmp ax,strict word 0x8
        jnz .try_9
        call fn08_set_y_range                   ; AX=8  set vertical range
        jmp short .return
        nop
.try_9:
        cmp ax,strict word 0x9
        jnz .try_10
        call fn09_set_graphics_cursor           ; AX=9  set graphics cursor block
        jmp short .return
        nop
.try_10:
        cmp ax,strict word 0xa
        jnz .try_11
        call fn10_set_text_cursor               ; AX=10 set text cursor
        jmp short .return
        nop
.try_11:
        cmp ax,strict word 0xb
        jnz .try_12
        call fn11_read_motion                   ; AX=11 read motion counters
        jmp short .return
        nop
.try_12:
        cmp ax,strict word 0xc
        jnz .try_13
        call fn12_set_user_handler              ; AX=12 set user event handler
        jmp short .return
        nop
.try_13:
        cmp ax,strict word 0xd
        jnz .try_14
        jmp short .return                       ; AX=13 light pen emulation on: no-op
        nop
.try_14:
        cmp ax,strict word 0xe
        jnz .try_15
        jmp short .return                       ; AX=14 light pen emulation off: no-op
        nop
.try_15:
        cmp ax,strict word 0xf
        jnz .try_16
        call fn15_set_mickey_ratio              ; AX=15 set mickey/pixel ratio
        jmp short .return
        nop
.try_16:
        cmp ax,strict word 0x10
        ja .return                              ; 17..FFFFh (AL >= 80h after CBW): ignored
        call fn16_conditional_off               ; AX=16 conditional off
.return:
        pop es
        pop ds
        pop bp
        pop si
        pop di
        pop dx
        pop cx
        pop bx
        pop ax
        ret                                     ; near return into int33_entry (which IRETs)

;-----------------------------------------------------------------------------
; fn00_reset - INT 33h AX=0: reset driver and read status
;
; Out: AX = FFFFh (reset_status), BX = 2 buttons; then falls into
;      reset_driver with the CALLER'S BX still in BX: if the program happens
;      to call with BX=0, the serial port is re-initialised too (channel
;      reset, INT 21h AH=25h from inside INT 33h, ~65535-read delay, mouse
;      power cycle via RTS). Not part of the Microsoft API.
;-----------------------------------------------------------------------------
fn00_reset:
        mov ax,[cs:reset_status]
        mov [bp+REG_AX],ax
        mov word [bp+REG_BX],2                  ; falls through into reset_driver

;-----------------------------------------------------------------------------
; reset_driver - common reset (fn 0 and device_init)
;
; In : BX = 0 -> also run init_serial_hw (device_init passes 0); DS = CS
; Does: re-detect text/graphics mode; hardware text cursor; ratios 8/8;
;       range 0..799 x 0..399; no user handler; clear counters and button
;       records; default arrow cursor, hot spot (-1,-1); exclusion area
;       disabled; cursor hidden (level -1); position = screen centre
;       (400,200), rounded to the cell grid. Ends in set_position_ax_bx (STI).
; Destroys AX BX CX SI DI ES.
;-----------------------------------------------------------------------------
reset_driver:
        cmp bx,byte 0x0
        jnz .no_hw_init
        call init_serial_hw                     ; BX=0: program SIO, timer, PIC, INT 41h
.no_hw_init:
        call detect_video_mode                  ; text/graphics: sets gran_x/y, bitmap_seg
        cli
        mov byte [cs:text_cursor_type],0x1      ; default text cursor = hardware 6845 cursor
        mov word [cs:mickey_ratio_x],0x8        ; 8 mickeys per 8 pixels = 1 mickey/pixel
        mov word [cs:mickey_ratio_y],0x8
        mov ax,[cs:screen_width]                ; x_max = 799
        dec ax
        mov [cs:x_max],ax
        mov ax,[cs:screen_height]               ; y_max = 399
        dec ax
        mov [cs:y_max],ax
        mov byte [cs:isr_not_busy],0xff         ; user handler may be called
        rr xor, ax,ax
        mov [cs:x_min],ax                       ; x_min = 0
        mov [cs:y_min],ax                       ; y_min = 0
        mov [cs:user_mask],ax                   ; user_mask = 0: no handler calls
        mov cx,0xf                              ; 15 words: raw_x, raw_y, button_state and
        push ds
        pop es
        mov di,raw_x                            ;   the 4 x 3-word press/release records
        rep stosw
        dec ax                                  ; AX = FFFFh
        mov [cs:hotspot_x],ax                   ; hot spot (-1,-1), as Microsoft's default
        mov [cs:hotspot_y],ax
        mov byte [cs:cursor_level],0x0          ; cursor_level = 0 ...
        mov si,default_screen_mask              ; install the default arrow (DS = CS)
        call load_cursor_masks
        call fn02_hide_cursor                   ; ... then hide it: level -1
        mov ax,[cs:screen_width]
        mov bx,[cs:screen_height]
        rr mov, cx,bx
        add cx,byte 0x1f                        ; excl_top = 400+31: below any cursor = off
        mov [cs:excl_top],cx
        shr ax,1                                ; AX = 400 = centre X
        shr bx,1                                ; BX = 200 = centre Y
        jmp short set_position_ax_bx            ; store position and return
        nop

;-----------------------------------------------------------------------------
; fn01_show_cursor - INT 33h AX=1: show cursor
;
; Cancels any conditional-off area (excl_top = 431) and increments the
; visibility counter, drawing the cursor when it goes from -1 to 0. At 0
; the call does nothing (the counter never goes positive).
; show_cursor_tail is shared by fn 9, 10, 16: they DEC the counter first,
; so the tail redraws exactly when the cursor was visible before.
;-----------------------------------------------------------------------------
fn01_show_cursor:
        mov ax,[cs:screen_height]               ; disable the fn 16 exclusion rectangle
        add ax,strict word 0x1f
        mov [cs:excl_top],ax
        cmp byte [cs:cursor_level],0x0          ; already visible: nothing to do
        jz show_cursor_tail.done
show_cursor_tail:
        cmp byte [cs:cursor_level],0xff         ; becoming visible?
        jnz .count_up
        call draw_cursor                        ; yes: draw (may hide again via fn 16 area)
.count_up:
        inc byte [cs:cursor_level]              ; level++
.done:
        ret

;-----------------------------------------------------------------------------
; fn02_hide_cursor - INT 33h AX=2: hide cursor
;
; Decrements the visibility counter and erases the cursor image if one is
; on screen. Note: with the hardware text cursor erase_cursor does nothing,
; so the 6845 cursor stays visible.
;-----------------------------------------------------------------------------
fn02_hide_cursor:
        dec byte [cs:cursor_level]
        call erase_cursor
        ret

;-----------------------------------------------------------------------------
; fn03_get_status - INT 33h AX=3: get position and buttons
;
; Out: BX = button_state (bit 0 left, bit 1 right), CX = X, DX = Y (pixels;
;      in text mode multiples of 10 / 16)
;-----------------------------------------------------------------------------
fn03_get_status:
        mov ax,[cs:button_state]
        mov [bp+REG_BX],ax
        mov ax,[cs:cursor_x]
        mov [bp+REG_CX],ax
        mov ax,[cs:cursor_y]
        mov [bp+REG_DX],ax
        ret

;-----------------------------------------------------------------------------
; fn04_set_position - INT 33h AX=4: set cursor position
;
; In : CX = X, DX = Y (clamped to the current range)
; set_position_ax_bx: AX = X, BX = Y already clamped; stores the virtual
; position and the cursor position rounded down to the cell grid.
; Note: the cursor is NOT redrawn - it jumps on the next mouse motion.
; The rounding uses CWD + unsigned DIV, so a negative coordinate (only
; possible with a negative range) would raise a divide overflow.
;-----------------------------------------------------------------------------
fn04_set_position:
        rr mov, ax,dx                           ; Y
        call clamp_y                            ; clamp to y_min..y_max
        rr mov, bx,ax
        rr mov, ax,cx                           ; X
        call clamp_x                            ; clamp to x_min..x_max
set_position_ax_bx:
        cli
        mov [cs:virt_x],ax
        mov [cs:virt_y],bx
        cwd                                     ; cursor_x = X rounded down to gran_x
        div word [cs:gran_x]
        mul word [cs:gran_x]
        mov [cs:cursor_x],ax
        rr mov, ax,bx                           ; cursor_y = Y rounded down to gran_y
        cwd
        div word [cs:gran_y]
        mul word [cs:gran_y]
        mov [cs:cursor_y],ax
        sti
        ret

;-----------------------------------------------------------------------------
; fn05_press_info - INT 33h AX=5: button press information
;
; In : BX bit 0 = 0 left, 1 right
; Out: AX = button_state, BX = presses since last call (then cleared),
;      CX/DX = position at the last press
; report_button_block is shared with fn 6: SI -> {count, x, y}.
;-----------------------------------------------------------------------------
fn05_press_info:
        mov si,left_press
        test bl,0x1
        jz report_button_block
        mov si,right_press
report_button_block:
        rr xor, ax,ax
        xchg ax,[si]                            ; fetch count and clear it
        mov [bp+REG_BX],ax
        mov ax,[si+0x2]
        mov [bp+REG_CX],ax
        mov ax,[si+0x4]
        mov [bp+REG_DX],ax
        mov ax,[cs:button_state]
        mov [bp+REG_AX],ax
        ret

;-----------------------------------------------------------------------------
; fn06_release_info - INT 33h AX=6: button release information
;
; In : BX = 0 left, any other value right (fn 5 tests only bit 0)
; Out: as fn 5, for releases
;-----------------------------------------------------------------------------
fn06_release_info:
        mov si,left_release
        rr or, bx,bx
        jz report_button_block
        mov si,right_release
        jmp short report_button_block

;-----------------------------------------------------------------------------
; fn07_set_x_range / fn08_set_y_range - INT 33h AX=7 / AX=8
;
; In : CX, DX = the two limits (either order; swapped if CX >= DX)
; Then the current virtual position is re-clamped through fn04_set_position
; (no redraw).
;-----------------------------------------------------------------------------
fn07_set_x_range:
        call order_cx_dx
        cli
        mov [cs:x_min],cx
        mov [cs:x_max],dx
        jmp short reapply_position
        nop
fn08_set_y_range:
        call order_cx_dx
        cli
        mov [cs:y_min],cx
        mov [cs:y_max],dx
reapply_position:
        mov cx,[cs:virt_x]                      ; re-apply the current virtual position
        mov dx,[cs:virt_y]
        jmp near fn04_set_position

;-----------------------------------------------------------------------------
; fn09_set_graphics_cursor - INT 33h AX=9: define graphics cursor
;
; In : BX = hot spot X, CX = hot spot Y,
;      ES:DX -> 16 words screen (AND) mask + 16 words cursor (XOR) mask
; The cursor level is lowered for the update and restored through
; show_cursor_tail, which redraws it if it was visible.
;-----------------------------------------------------------------------------
fn09_set_graphics_cursor:
        dec byte [cs:cursor_level]              ; temporarily hidden (no erase: draw erases)
        cli
        mov [cs:hotspot_y],cx
        mov [cs:hotspot_x],bx
        rr mov, si,dx
        sti
        push ds                                 ; DS = caller ES
        push es
        pop ds
        call load_cursor_masks
        pop ds
        jmp near show_cursor_tail

;-----------------------------------------------------------------------------
; load_cursor_masks - copy 32 mask words, bit-reversed
;
; In : DS:SI -> screen mask (16 words) + cursor mask (16 words), Microsoft
;      order (bit 15 = leftmost pixel)
; Out: CS:cursor_screen_mask / cursor_image_mask, each word mirrored so that
;      bit 0 = leftmost pixel, the order the Victor video hardware shifts
;      font-cell words out. ES = CS. Destroys AX BX CX SI DI.
; Callers: reset_driver, fn09_set_graphics_cursor
;-----------------------------------------------------------------------------
load_cursor_masks:
        push cs
        pop es
        mov di,cursor_screen_mask
        mov cx,0x20
.next_word:
        lodsw
        push cx
        mov cx,0x10
.reverse_bit:
        rcr ax,1                                ; shift AX out to the right ...
        rcl bx,1                                ; ... and into BX from the right: mirror
        loop .reverse_bit
        pop cx
        rr mov, ax,bx
        stosw
        loop .next_word
        ret

;-----------------------------------------------------------------------------
; fn10_set_text_cursor - INT 33h AX=10: define text cursor
;
; In : BX = 0  software cursor: CX = screen (AND) mask, DX = cursor (XOR)
;              mask, applied to the cell's screen-RAM word
;      BX <> 0 hardware cursor: CL/DL low nibble = 6845 R10/R11 cursor
;              start/end scan line (bits 4-7 dropped, so blink mode = 0)
; The masks are stored in either case. Switching from a visible software
; cursor to the hardware one leaves the software image on screen
; (erase_cursor skips the restore once text_cursor_type <> 0).
;-----------------------------------------------------------------------------
fn10_set_text_cursor:
        dec byte [cs:cursor_level]
        mov [cs:text_cursor_type],bl            ; type = BL, but the branch below tests BX
        mov [cs:text_screen_mask],cx
        mov [cs:text_cursor_mask],dx
        rr or, bx,bx
        jnz .hardware_cursor
        jmp near show_cursor_tail
.hardware_cursor:
        inc byte [cs:cursor_level]              ; undo the DEC: no redraw for hardware cursor
        mov ax,IO_SEG
        mov es,ax
        mov ah,0xf
        rr and, cl,ah
        rr and, dl,ah
        cli
        mov byte [es:CRTC_ADDR],0xa             ; R10 = cursor start raster
        mov [es:CRTC_DATA],cl
        mov byte [es:CRTC_ADDR],0xb             ; R11 = cursor end raster
        mov [es:CRTC_DATA],dl
        sti
        ret

;-----------------------------------------------------------------------------
; fn11_read_motion - INT 33h AX=11: read motion counters
;
; Out: CX = raw X mickeys, DX = raw Y mickeys since the last call; both
;      cleared. Counts are before acceleration. Shared with INT FBh.
;-----------------------------------------------------------------------------
fn11_read_motion:
        rr xor, ax,ax
        xchg ax,[cs:raw_x]
        mov [bp+REG_CX],ax
        rr xor, ax,ax
        xchg ax,[cs:raw_y]
        mov [bp+REG_DX],ax
        ret

;-----------------------------------------------------------------------------
; fn12_set_user_handler - INT 33h AX=12: set event handler
;
; In : CX = event mask, ES:DX = far handler
; The address is patched straight into the CALL FAR in serial_isr
; (self-modifying code: user_handler_ofs / user_handler_seg).
;-----------------------------------------------------------------------------
fn12_set_user_handler:
        cli
        mov [cs:user_handler_ofs],dx
        mov [cs:user_handler_seg],es
        mov [cs:user_mask],cx
        sti
        ret

;-----------------------------------------------------------------------------
; fn15_set_mickey_ratio - INT 33h AX=15: mickeys per 8 pixels
;
; In : CX = horizontal, DX = vertical. No validation: 0 makes the IDIV in
;      serial_isr fault (divide error inside the interrupt handler).
;-----------------------------------------------------------------------------
fn15_set_mickey_ratio:
        mov [cs:mickey_ratio_x],cx
        mov [cs:mickey_ratio_y],dx
        ret

;-----------------------------------------------------------------------------
; fn16_conditional_off - INT 33h AX=16: conditional off area
;
; In : CX = left, DX = top, SI = right, DI = bottom
; Lowers the level and lets show_cursor_tail redraw: draw_cursor calls
; check_exclusion, which hides the cursor (level -1) if it overlaps the
; area. fn 1 cancels the area.
;-----------------------------------------------------------------------------
fn16_conditional_off:
        dec byte [cs:cursor_level]
        mov [cs:excl_left],cx
        mov [cs:excl_top],dx
        mov [cs:excl_right],si
        mov [cs:excl_bottom],di
        jmp near show_cursor_tail

; ============================================================================
;  SERIAL RECEIVE INTERRUPT (INT 41h = 8259 IR1)
; ============================================================================

;-----------------------------------------------------------------------------
; serial_isr - uPD7201 receive interrupt, Microsoft serial mouse decoder
;
; Packet (7 data bits):  byte 1  1 L R Y7 Y6 X7 X6   (bit 6 = sync)
;                        byte 2  0 X5 X4 X3 X2 X1 X0
;                        byte 3  0 Y5 Y4 Y3 Y2 Y1 Y0
; Byte 1: button edges -> press/release records + event bits, high bits
;         of X/Y saved, pkt_state = 1.  Byte 2: low X bits, pkt_state = 2.
; Byte 3: deltas sign-extended, motion processed (see below), then the
;         user handler and the cursor redraw run with interrupts enabled
;         and a busy flag (isr_not_busy) against re-entry.
; Event bits (event_flags, Microsoft layout): 01h moved, 02h left pressed,
; 04h left released, 08h right pressed, 10h right released.
; Bytes without the sync bit while pkt_state = 0 are dropped.
; The original INT 41h vector is not chained, and the ISR does not check
; which 7201 channel interrupted.
;-----------------------------------------------------------------------------
serial_isr:
        push ax
        push ds
        push es
        push bx
        push cx
        push dx
        push di
        push si
        push bp
        mov ax,cs                               ; DS = CS
        mov ds,ax
        call serial_poll_rx                     ; ZF=0 and AL = byte if one was received
        jnz .got_byte
        jmp near .eoi_exit                      ; nothing received: just EOI
.got_byte:
        cld
        test al,0x40                            ; bit 6 set = first byte of a packet
        jnz .sync_byte
        jmp near .data_byte
.sync_byte:
        mov si,[cs:cursor_x]                    ; positions recorded with button edges
        mov di,[cs:cursor_y]                    ; (position BEFORE this packet's motion)
        rr xor, dx,dx
        mov [cs:event_flags],dx                 ; new packet: clear the event bits
        rr mov, dl,al
        mov cl,0x5                              ; byte bit 5 = left button
        shr dl,cl
        mov cx,0x1                              ; DX = new left bit (bit 0)
        rr and, dx,cx
        not cx
        and cx,[cs:button_state]                ; CX = other button bits of the old state
        rr or, dx,cx                            ; DX = new state
        xchg dx,[cs:button_state]               ; store new state, DX = old state
        cmp [cs:button_state],dx                ; new vs old (only bit 0 can differ)
        jz .right_button
        ja .left_pressed                        ; new > old: left went down
        inc word [cs:left_release]              ; left released: count, X, Y
        mov [cs:left_release+2],si
        mov [cs:left_release+4],di
        or byte [cs:event_flags],0x4            ; event 04h left released
        jmp short .right_button
        nop
.left_pressed:
        inc word [cs:left_press]                ; left pressed: count, X, Y
        mov [cs:left_press+2],si
        mov [cs:left_press+4],di
        or byte [cs:event_flags],0x2            ; event 02h left pressed
.right_button:
        rr mov, dl,al
        mov cl,0x3                              ; byte bit 4 = right button
        shr dl,cl
        mov cx,0x2                              ; -> bit 1
        rr and, dx,cx
        not cx
        and cx,[cs:button_state]
        rr or, dx,cx
        xchg dx,[cs:button_state]
        cmp [cs:button_state],dx
        jz .high_bits
        ja .right_pressed                       ; new > old: right went down
        inc word [cs:right_release]             ; right released
        mov [cs:right_release+2],si
        mov [cs:right_release+4],di
        or byte [cs:event_flags],0x10           ; event 10h right released
        jmp short .high_bits
        nop
.right_pressed:
        inc word [cs:right_press]               ; right pressed
        mov [cs:right_press+2],si
        mov [cs:right_press+4],di
        or byte [cs:event_flags],0x8            ; event 08h right pressed
.high_bits:
        rr mov, bx,ax                           ; BX = AX = byte 1 (AH is junk)
        mov cl,0x4
        shl ax,cl                               ; Y7:Y6 (bits 3:2) -> bits 7:6
        and al,0xc0
        mov [cs:pkt_y],al
        mov cl,0x6
        shl bx,cl                               ; X7:X6 (bits 1:0) -> bits 7:6
        and bl,0xc0
        mov [cs:pkt_x],bl
        mov byte [cs:pkt_state],0x1             ; expect byte 2
        jmp short .eoi_exit
        nop
.data_byte:
        and al,0x3f                             ; 6 data bits
        cmp byte [cs:pkt_state],0x1
        jc .eoi_exit                            ; state 0: stray byte, ignore
        ja .third_byte                          ; state 2: byte 3
        or [cs:pkt_x],al                        ; state 1: byte 2 = X5..X0
        inc byte [cs:pkt_state]                 ; expect byte 3
.eoi_exit:
        call serial_eoi                         ; end of interrupt (7201 + 8259)
        jmp near .exit
.third_byte:
        or al,[cs:pkt_y]                        ; byte 3 = Y5..Y0, complete Y delta
        cbw                                     ; sign-extend
        rr mov, cx,ax                           ; CX = dY (mickeys, + = down)
        mov al,[cs:pkt_x]
        cbw                                     ; sign-extend
        rr mov, bx,ax                           ; BX = dX (mickeys, + = right)
        mov byte [cs:pkt_state],0x0             ; next byte must be a sync byte
        rr mov, ax,bx
        rr or, ax,cx                            ; dX = dY = 0: button events only
        jnz .have_motion
        jmp near .packet_done
.have_motion:
        add [cs:raw_x],bx                       ; raw counters: unaccelerated mickeys
        add [cs:raw_y],cx
        rr mov, ax,bx
        call accelerate                         ; |dX| > 2: double dX and dY, CF=0
        jnc .x_motion                           ; CF=0 (|dX| >= 2): skip the dY test (BUG: =2)
        rr mov, ax,cx
        call accelerate                         ; |dY| > 2: double dX and dY
.x_motion:
        rr or, bx,bx
        jz .y_motion                            ; no X motion: BX = 0 = "X unchanged"
        shl bx,1                                ; dX * 8
        shl bx,1
        shl bx,1
        mov ax,[cs:frac_x]                      ; + remainder of the last packet
        rr add, ax,bx
        cwd
        idiv word [cs:mickey_ratio_x]           ; / mickeys per 8 pixels (signed)
        mov [cs:frac_x],dx                      ; keep the remainder
        add ax,[cs:virt_x]                      ; new virtual X
        call clamp_virtual_x                    ; clamp to x_min-16 .. x_max+16
        mov [cs:virt_x],ax
        call clamp_x                            ; cursor X: clamp to x_min..x_max
        mov bx,[cs:gran_x]
        cmp bl,0x1                              ; graphics (granularity 1): no rounding
        jz .x_store
        cwd                                     ; round down to the 10-pixel cell
        div bx
        mul bx
.x_store:
        mov [cs:cursor_x],ax
        sub ax,[cs:drawn_x]                     ; BX = change against the drawn position
        rr mov, bx,ax
.y_motion:
        jcxz .motion_flag                       ; no Y motion: CX = 0
        shl cx,1                                ; dY * 8
        shl cx,1
        shl cx,1
        mov ax,[cs:frac_y]
        rr add, ax,cx
        cwd
        idiv word [cs:mickey_ratio_y]           ; / mickeys per 8 pixels
        mov [cs:frac_y],dx
        add ax,[cs:virt_y]                      ; new virtual Y
        call clamp_virtual_y                    ; clamp to y_min-16 .. y_max+16
        mov [cs:virt_y],ax
        call clamp_y                            ; cursor Y: clamp to y_min..y_max
        mov cx,[cs:gran_y]
        cmp cl,0x1
        jz .y_store
        cwd                                     ; round down to the 16-pixel cell
        div cx
        mul cx
.y_store:
        mov [cs:cursor_y],ax
        sub ax,[cs:drawn_y]                     ; CX = change against the drawn position
        rr mov, cx,ax
.motion_flag:
        rr or, cx,bx                            ; moved (relative to the drawn cursor)?
        jz .packet_done
        or byte [cs:event_flags],0x1            ; event 01h cursor moved
.packet_done:
        call serial_eoi                         ; EOI before the slow part
        mov ax,[cs:event_flags]
        cmp byte [cs:isr_not_busy],0x0          ; re-entered while a handler/redraw runs?
        jz .exit                                ; yes: position is updated, skip the rest
        mov byte [cs:isr_not_busy],0x0          ; mark busy
        push ax
        and ax,[cs:user_mask]                   ; events the user asked for?
        jz .after_user
        mov bx,[cs:button_state]                ; Microsoft handler interface:
        mov cx,[cs:cursor_x]                    ;   AX events, BX buttons, CX/DX position
        mov dx,[cs:cursor_y]
        mov si,[cs:raw_x]                       ;   SI/DI raw mickey counters (not the
        mov di,[cs:raw_y]                       ;   per-packet delta)
        sti
.call_user:
        db 0x9A                                 ; CALL FAR user handler (patched by fn 12)
        dw 0                                    ; offset  - patched by fn 12 (user_handler_ofs)
        dw 0                                    ; segment - patched by fn 12 (user_handler_seg)
user_handler_ofs equ serial_isr.call_user + 1
user_handler_seg equ serial_isr.call_user + 3
        mov ax,cs                               ; handler may have changed DS
        mov ds,ax
.after_user:
        sti
        pop ax                                  ; AX = event bits again
        test al,0x1                             ; moved?
        jz .release_busy
        cmp byte [cs:cursor_level],0x0          ; and cursor visible?
        jnz .release_busy
        call draw_cursor                        ; redraw at the new position
.release_busy:
        cli
        mov byte [cs:isr_not_busy],0xff         ; not busy any more
.exit:
        pop bp
        pop si
        pop di
        pop dx
        pop cx
        pop bx
        pop es
        pop ds
        pop ax
        iret

;-----------------------------------------------------------------------------
; accelerate - double both deltas on fast motion
;
; In : AX = delta to test, BX = dX, CX = dY
; Out: |AX| > 2: BX, CX doubled, CF = 0
;      |AX| < 2: CF = 1         |AX| = 2: CF = 0 (and nothing doubled)
; serial_isr tests dX first and dY only when CF = 1, so a packet with
; |dX| = 2 never gets its dY tested - a fast vertical move with exactly
; 2 mickeys of sideways motion is not accelerated.
;-----------------------------------------------------------------------------
accelerate:
        rr or, ax,ax
        jnl .positive
        neg ax
.positive:
        cmp ax,strict word 0x2
        jna .done                               ; CF/ZF from the compare
        shl bx,1
        shl cx,1
        clc
.done:
        ret

; ============================================================================
;  CURSOR DRAWING
; ============================================================================

;-----------------------------------------------------------------------------
; draw_cursor - erase the old cursor and draw it at cursor_x/cursor_y
;
; Callers: show_cursor_tail (fn 1/9/10/16), serial_isr (after motion)
; Three cases, chosen by gran_y (1 = graphics) and text_cursor_type:
;  hardware text: 6845 R14/R15 = word address of the cell (no erase, no
;                 exclusion check)
;  software text: save the cell word, AND text_screen_mask, XOR
;                 text_cursor_mask (one 10x16 cell)
;  graphics:      16x16 bitmap at (cursor - hot spot), draw_graphics_cursor
; Both software cases first erase the old image and test the fn 16 area
; (check_exclusion hides the cursor and skips the draw on overlap).
; Records the drawn position in drawn_x/drawn_y. Exits with IF = 1.
;-----------------------------------------------------------------------------
draw_cursor:
        cld
        cmp byte [cs:gran_y],0x1                ; granularity 1 = graphics mode
        jnz .text_mode
        jmp short .graphics
        nop
.text_mode:
        cmp byte [cs:text_cursor_type],0x0      ; hardware text cursor?
        jz .software_text
        mov ax,[cs:cursor_y]
        mov bx,[cs:cursor_x]
        call text_cell_address                  ; ES:DI = cell, DI = byte offset
        shr di,1                                ; DI = word address = 6845 memory address
        rr mov, cx,di
        mov ax,IO_SEG
        mov es,ax
        cli
        mov byte [es:CRTC_ADDR],0xe             ; R14 = cursor address high
        mov [es:CRTC_DATA],ch
        mov byte [es:CRTC_ADDR],0xf             ; R15 = cursor address low
        mov [es:CRTC_DATA],cl
        mov byte [cs:cursor_drawn],0xff
        jmp short .remember_position
        nop
.software_text:
        call erase_cursor                       ; restore the old cell first
        mov ax,[cs:cursor_y]
        mov bx,[cs:cursor_x]
        rr mov, cx,ax                           ; CX = bottom = Y + 15
        rr mov, dx,bx                           ; DX = right  = X + 9
        add dx,[cs:cell_width]
        dec dx
        add cx,[cs:cell_height]
        dec cx
        call check_exclusion                    ; inside the fn 16 area: now hidden
        jc .done
        cli
        call text_cell_address                  ; ES:DI = screen word of the cell
        mov ax,[es:di]
        mov [cs:save_buffer],ax                 ; save it
        and ax,[cs:text_screen_mask]            ; default FFFFh
        xor ax,[cs:text_cursor_mask]            ; default 7700h (see MOUSE_DRIVER.md)
        stosw                                   ; write the cursor cell
        mov byte [cs:cursor_drawn],0xff
        jmp short .remember_position
        nop
.graphics:
        call erase_cursor                       ; restore the old background first
        cli
        mov bx,[cs:cursor_x]                    ; gc_x = X - hot spot X
        sub bx,[cs:hotspot_x]
        mov [cs:gc_x],bx
        mov ax,[cs:cursor_y]                    ; gc_y = Y - hot spot Y
        sub ax,[cs:hotspot_y]
        mov [cs:gc_y],ax
        sti
        and bl,0xf0                             ; exclusion test on the whole strip pair:
        rr mov, dx,bx                           ;   left = strip start, right = +31
        add dx,byte 0x1f
        rr mov, cx,ax                           ;   bottom = top + 15
        add cx,byte 0xf
        call check_exclusion
        jc .done
        cli
        call draw_graphics_cursor               ; draw into font-cell RAM
.remember_position:
        mov ax,[cs:cursor_x]
        mov [cs:drawn_x],ax
        mov ax,[cs:cursor_y]
        mov [cs:drawn_y],ax
        sti
.done:
        ret

;-----------------------------------------------------------------------------
; erase_cursor - remove the cursor image, if one is drawn
;
; graphics      -> restore_graphics_background
; hardware text -> nothing (the 6845 cursor stays where it is)
; software text -> write the saved word back at drawn_x/drawn_y
; Callers: fn02_hide_cursor, draw_cursor. Ends with STI on the software path.
;-----------------------------------------------------------------------------
erase_cursor:
        cld
        cmp byte [cs:cursor_drawn],0xff         ; anything on screen?
        jnz .done
        mov byte [cs:cursor_drawn],0x0
        cmp byte [cs:gran_y],0x1                ; graphics mode?
        jz .graphics
        cmp byte [cs:text_cursor_type],0x0      ; hardware cursor: nothing to undo
        jnz .done
        mov ax,[cs:drawn_y]
        mov bx,[cs:drawn_x]
        call text_cell_address
        mov ax,[cs:save_buffer]
        stosw
        sti
.done:
        ret
.graphics:
        call restore_graphics_background
        ret

;-----------------------------------------------------------------------------
; text_cell_address - screen-RAM address of a text cell
;
; In : AX = Y, BX = X in pixels (multiples of 16 / 10)
; Out: ES = F000h, DI = byte offset of the cell word
;      = 2 * (6845 start address R12:R13) + Y/16 * 160 + X/10 * 2
; The start address is not masked (R12 bits 4/5 are the font-bank and
; hi-res bits) and the sum does not wrap at the end of the 4 KB screen
; RAM, so the software cursor goes astray once the console has scrolled
; the start address far enough. AX, CL destroyed; no CLI here.
;-----------------------------------------------------------------------------
text_cell_address:
        push ax
        mov ax,IO_SEG
        mov es,ax
        mov byte [es:CRTC_ADDR],0xc
        mov ah,[es:CRTC_DATA]                   ; R12 start address high
        mov byte [es:CRTC_ADDR],0xd
        mov al,[es:CRTC_DATA]                   ; R13 start address low
        shl ax,1                                ; words -> bytes
        rr mov, di,ax
        pop ax
        div byte [cs:cell_height]               ; AL = text row (AH = remainder)
        mov cl,[cs:text_columns]                ; 80 columns
        shl cl,1                                ; * 2 bytes per cell word
        mul cl                                  ; row * 160
        rr add, di,ax
        rr mov, ax,bx
        div byte [cs:cell_width]                ; AL = column, AH = remainder (0 here)
        shl ax,1                                ; column * 2
        rr add, di,ax
        push ax
        mov ax,SCREEN_SEG
        mov es,ax
        pop ax
        ret

;-----------------------------------------------------------------------------
; clamp_x / clamp_y / clamp_virtual_x / clamp_virtual_y
;
; In : AX = coordinate.  Out: AX clamped (signed) to
;      clamp_x: x_min..x_max            clamp_virtual_x: x_min-16..x_max+16
;      clamp_y: y_min..y_max            clamp_virtual_y: y_min-16..y_max+16
; SI, DI destroyed. clamp_ax_si_di is the common tail (AX into SI..DI).
;-----------------------------------------------------------------------------
clamp_x:
        mov si,[cs:x_min]
        mov di,[cs:x_max]
        jmp short clamp_ax_si_di
        nop
clamp_y:
        mov si,[cs:y_min]
        mov di,[cs:y_max]
        jmp short clamp_ax_si_di
        nop
clamp_virtual_x:
        mov si,[cs:x_min]
        mov di,[cs:x_max]
        sub si,byte 0x10
        add di,byte 0x10
        jmp short clamp_ax_si_di
        nop
clamp_virtual_y:
        mov si,[cs:y_min]
        mov di,[cs:y_max]
        sub si,byte 0x10
        add di,byte 0x10
clamp_ax_si_di:
        rr cmp, ax,si
        jl .use_si
        rr mov, si,di
        rr cmp, ax,si
        jng .done
.use_si:
        rr mov, ax,si
.done:
        ret

;-----------------------------------------------------------------------------
; order_cx_dx - make CX < DX (swap when CX >= DX)
;
; Callers: fn07_set_x_range, fn08_set_y_range
;-----------------------------------------------------------------------------
order_cx_dx:
        rr cmp, cx,dx
        jl .done
        xchg cx,dx
.done:
        ret

;-----------------------------------------------------------------------------
; check_exclusion - test the cursor rectangle against the fn 16 area
;
; In : AX = top, BX = left, CX = bottom, DX = right
; Out: CF = 1 overlap: cursor_level decremented (cursor now hidden)
;      CF = 0 no overlap
;-----------------------------------------------------------------------------
check_exclusion:
        cmp cx,[cs:excl_top]                    ; bottom above the area top?
        jl .outside
        cmp ax,[cs:excl_bottom]                 ; top below the area bottom?
        jg .outside
        cmp dx,[cs:excl_left]                   ; right left of the area?
        jl .outside
        cmp bx,[cs:excl_right]                  ; left right of the area?
        jg .outside
        dec byte [cs:cursor_level]              ; overlap: hide
        stc
        ret
.outside:
        clc
        ret

;-----------------------------------------------------------------------------
; detect_video_mode - text or 800x400 graphics?
;
; Reads the 6845 start address R12:R13. R12 bit 5 (MA13) selects the
; hi-res 16-pixel character cell = the graphics screen; R12 bit 4 (MA12)
; selects the second 64 KB bank of font-cell RAM.
; Graphics: gran_x = gran_y = 1, bitmap_seg = bank*1000h + 2 * font-cell
;           number of screen word 0 (each cell = 32 bytes = 2 paragraphs).
;           The screen word at F000:0000 is used, not the one at the start
;           address.
; Text:     gran_x = 10, gran_y = 16 (one character cell)
; Called only from reset_driver: a program must call fn 0 after switching
; modes. AX, BX preserved; ES destroyed.
;-----------------------------------------------------------------------------
detect_video_mode:
        push ax
        push bx
        cli
        mov ax,IO_SEG
        mov es,ax
        mov byte [es:CRTC_ADDR],0xc
        mov ah,[es:CRTC_DATA]                   ; AH = R12
        mov byte [es:CRTC_ADDR],0xd
        mov al,[es:CRTC_DATA]                   ; AL = R13 (not used)
        sti
        test ah,0x20                            ; MA13: hi-res (graphics) mode?
        jz .text_mode
        mov word [cs:gran_x],0x1
        mov word [cs:gran_y],0x1
        test ah,0x10                            ; MA12: font bank 1 (second 64 KB)?
        mov ax,0x0
        jz .bank_known
        mov ax,0x1000
.bank_known:
        push ax
        mov ax,SCREEN_SEG
        mov es,ax
        pop ax
        mov bx,[es:0]                           ; screen word 0
        and bx,strict word 0x7ff                ; its font-cell number
        rr add, bx,bx                           ; * 2 paragraphs (32 bytes per cell)
        rr add, ax,bx
        mov [cs:bitmap_seg],ax                  ; segment of the top-left cell bitmap
        jmp short .done
        nop
.text_mode:
        mov word [cs:gran_x],0xa                ; text: one 10 x 16 character cell
        mov word [cs:gran_y],0x10
.done:
        pop bx
        pop ax
        ret

; ============================================================================
;  SERIAL HARDWARE
; ============================================================================

;-----------------------------------------------------------------------------
; init_serial_hw - program PIC, 8253 and the 7201 channel, hook INT 41h
;
; In : port_index (0 = A, 1 = B), DS = CS
; Sequence (7201 registers are written as pointer byte, value byte):
;   WR0 = 18h   channel reset
;   INT 21h AH=25h: INT 41h -> serial_isr (old vector not saved)
;   8259: specific EOI for IR1, unmask IR1
;   8253 counter 0 (A) / 1 (B): mode 3, count 65 -> 1.25 MHz/65 = 19.23 kHz
;        = 16 x 1202 baud
;   WR2A = 14h  both channels interrupt driven (bits 1:0 = 00), bit 2 =
;               priority select, bit 4 as DOS (DOS writes 10h)
;   WR4 = 44h   x16 clock, 1 stop bit, no parity
;   WR3 = 41h   Rx 7 bits, Rx enable, no auto-enables (DCD not needed)
;   WR5 = E8h   DTR on, Tx 8 bits, Tx enable, RTS OFF (mouse unpowered)
;   WR0 = 10h   reset external/status interrupts
;   read the data register 65535 times: flush + power-off delay
;   WR5 = EAh   RTS ON: the mouse powers up (and sends "M")
;   WR1 = 10h   interrupt on every received character
; Callers: reset_driver (BX = 0). Out: ES = CS; AX BX CX DX SI destroyed.
;-----------------------------------------------------------------------------
init_serial_hw:
        mov si,[cs:port_index]
        mov ax,IO_SEG
        mov es,ax
        mov bx,SIO_CTRL_A                       ; BX = control register of our channel
        rr add, bx,si
        mov byte [es:bx],0x18                   ; WR0: channel reset
        mov ah,0x25                             ; DOS set vector: INT 41h = 8259 IR1
        mov al,PIC_VECTOR_IR1
        mov dx,serial_isr                       ; DS:DX = CS:serial_isr
        int 0x21
        mov byte [es:PIC_OCW],PIC_EOI_IR1       ; clear any pending IR1
        and byte [es:PIC_IMR],~(1 << 1) & 0xFF  ; unmask IR1 (7201)
        cmp si,byte 0x0                         ; port A?
        jnz .timer_b
        mov byte [es:PIT_CONTROL],0x36          ; counter 0, LSB+MSB, mode 3, binary
        mov byte [es:PIT_COUNTER0],0x41         ; count = 0041h = 65 -> 1200 baud
        mov byte [es:PIT_COUNTER0],0x0
        jmp short .program_sio
        nop
.timer_b:
        mov byte [es:PIT_CONTROL],0x76          ; counter 1, LSB+MSB, mode 3, binary
        mov byte [es:PIT_COUNTER1],0x41         ; count = 65 -> 1200 baud
        mov byte [es:PIT_COUNTER1],0x0
.program_sio:
        mov byte [es:SIO_CTRL_A],0x2            ; WR2 (channel A only):
        mov byte [es:SIO_CTRL_A],0x14           ;   interrupt mode, priority
        mov byte [es:bx],0x4                    ; WR4:
        mov byte [es:bx],0x44                   ;   x16 clock, 1 stop bit, no parity
        mov byte [es:bx],0x3                    ; WR3:
        mov byte [es:bx],0x41                   ;   Rx 7 bits, Rx enable
        mov byte [es:bx],0x5                    ; WR5:
        mov byte [es:bx],0xe8                   ;   DTR, Tx 8 bits, Tx enable, RTS off
        mov byte [es:bx],0x10                   ; WR0: reset ext/status interrupts
        mov bx,SIO_DATA_A                       ; BX = data register of our channel
        rr add, bx,si
        mov cx,0xffff
.flush:
        mov al,[es:bx]                          ; drain the receiver / delay
        loop .flush
        add bx,byte 0x2                         ; back to the control register
        mov byte [es:bx],0x5                    ; WR5:
        mov byte [es:bx],0xea                   ;   same + RTS on: mouse powers up
        mov byte [es:bx],0x1                    ; WR1:
        mov byte [es:bx],0x10                   ;   Rx interrupt on all characters
        push cs
        pop es
        ret

;-----------------------------------------------------------------------------
; serial_poll_rx - read a received byte if there is one
;
; Out: ZF = 0, AL = byte   if RR0 bit 0 (Rx character available) is set
;      ZF = 1               otherwise.  ES = E000h, BX = data register.
;-----------------------------------------------------------------------------
serial_poll_rx:
        mov ax,IO_SEG
        mov es,ax
        mov bx,SIO_DATA_A
        add bx,[cs:port_index]
        mov al,[es:bx+0x2]                      ; RR0 of our channel
        and al,0x1                              ; bit 0: Rx character available
        jnz .read_data
        jmp short .done
        nop
.read_data:
        mov al,[es:bx]                          ; read the byte (flags still ZF = 0)
.done:
        ret

;-----------------------------------------------------------------------------
; serial_eoi - end of interrupt for 7201 and 8259
;
; WR0A = 38h "end of interrupt" (channel A register, valid for both
; channels), then 8259 specific EOI for IR1.  ES = E000h.
;-----------------------------------------------------------------------------
serial_eoi:
        mov ax,IO_SEG
        mov es,ax
        mov byte [es:SIO_CTRL_A],0x38           ; WR0A: return from interrupt
        mov byte [es:PIC_OCW],PIC_EOI_IR1       ; 8259: specific EOI IR1
        ret

; ============================================================================
;  GRAPHICS CURSOR
; ============================================================================

;-----------------------------------------------------------------------------
; draw_graphics_cursor - AND/XOR the 16x16 cursor into the graphics screen
;
; In : gc_x, gc_y = top-left corner; bitmap_seg; the active masks.
; Screen layout assumed: the 800x400 bitmap is made of 50 x 25 hi-res
; character cells (16x16 pixels, 32 bytes each) whose font-cell numbers
; run COLUMN-major from the cell of screen word 0. A 16-pixel-wide column
; strip is then one contiguous run of 400 words (800 = STRIP_BYTES bytes):
;     byte offset of pixel (x, y) = (x / 16) * 800 + y * 2, bit x mod 16
;     (bit 0 = leftmost pixel).
; An unaligned cursor covers two strips: the masks are shifted left by
; s = x mod 16 into the left strip word and right by 16-s into the word
; 800 bytes further on. Each touched word is saved in save_buffer (two
; words per row) for restore_graphics_background.
; Clipping: right/bottom are clamped to 799/399 and rows above the top
; are skipped. The left edge is not handled correctly: for gc_x < 0 the
; shift becomes gc_x mod 16 and the mask is still shifted LEFT inside
; strip 0, so the wrong columns appear (only possible with a hot spot X
; larger than the cursor X). The "right edge < 0" tests are dead code
; (CMP AX,0 never sets CF).
; Callers: draw_cursor (with CLI). Sets cursor_drawn unless off screen.
;-----------------------------------------------------------------------------
draw_graphics_cursor:
        push bx
        push cx
        push dx
        push bp
        push si
        push es
        mov ax,[cs:gc_x]
        cmp ax,strict word SCREEN_W             ; left edge beyond the screen: nothing
        jl .x_on_screen
        jmp near .exit
.x_on_screen:
        mov [cs:gc_strip_left],ax
        mov bx,0x0
        mov [cs:gc_skip_cols],bx
        cmp ax,strict word 0x0                  ; off the left edge?
        jnl .clip_top
        mov word [cs:gc_strip_left],0x0         ; start in strip 0 ...
        neg ax                                  ; ... with -gc_x columns clipped
        rr add, ax,bx
        mov [cs:gc_skip_cols],ax
.clip_top:
        mov ax,[cs:gc_y]
        cmp ax,strict word SCREEN_H             ; top below the screen: nothing
        jl .y_on_screen
        jmp near .exit
.y_on_screen:
        mov [cs:gc_row_first],ax
        rr xor, bx,bx
        mov [cs:gc_skip_rows],bx
        cmp ax,strict word 0x0                  ; off the top?
        jnl .right_edge
        mov word [cs:gc_row_first],0x0          ; start at row 0 ...
        neg ax                                  ; ... skipping -gc_y mask rows
        rr add, ax,bx
        mov [cs:gc_skip_rows],ax
.right_edge:
        mov ax,[cs:gc_x]
        add ax,strict word 0xf                  ; right edge
        cmp ax,strict word 0x0                  ; BUG: always CF = 0, never exits
        jnc .right_ok
        jmp near .exit
.right_ok:
        mov [cs:gc_strip_right],ax
        cmp ax,strict word SCREEN_W             ; clamp to the last column
        jl .bottom_edge
        mov word [cs:gc_strip_right],SCREEN_W-1
.bottom_edge:
        mov ax,[cs:gc_y]
        add ax,strict word 0xf                  ; bottom edge
        cmp ax,strict word 0x0                  ; BUG: always CF = 0, never exits
        jnc .bottom_ok
        jmp near .exit
.bottom_ok:
        mov [cs:gc_row_last],ax
        cmp ax,strict word SCREEN_H             ; clamp to the last row
        jl .shift_count
        mov word [cs:gc_row_last],SCREEN_H-1
.shift_count:
        mov ax,[cs:gc_skip_cols]                ; shift = (left - skipped) mod 16
        and ax,strict word 0xf
        mov bx,[cs:gc_strip_left]
        and bx,strict word 0xf
        rr sub, bx,ax
        mov [cs:gc_shift],bl
        cmp bx,byte 0x0
        jnl .strip_offsets                      ; = gc_x mod 16, made positive
        add bx,byte 0x10
        mov [cs:gc_shift],bl
.strip_offsets:
        mov ax,[cs:gc_strip_left]               ; left X -> strip byte offset
        and ax,strict word 0xfff0               ;   = (x / 16) * 16 * 50 = (x / 16) * 800
        mov cx,STRIP_BYTES/16
        mul cx
        mov [cs:gc_strip_left],ax
        mov ax,[cs:gc_strip_right]              ; right X -> strip byte offset
        and ax,strict word 0xfff0
        mul cx
        mov [cs:gc_strip_right],ax
        shl word [cs:gc_skip_rows],1            ; rows -> word offsets
        shl word [cs:gc_row_first],1
        shl word [cs:gc_row_last],1
        mov ax,[cs:bitmap_seg]                  ; ES = font-cell RAM of screen cell 0
        mov es,ax
        mov cl,[cs:gc_shift]                    ; CL = s
        rr mov, ch,cl                           ; CH = 16 - s
        xor ch,0xf
        inc ch
        mov dx,0xffff                           ; DX = FFFF << s
        shl dx,cl
        mov bx,[cs:gc_skip_rows]                ; first visible mask row
        rr mov, di,bx
        add bx,cursor_screen_mask               ; BX -> screen mask row (image at +20h)
        add di,save_buffer                      ; DI -> save buffer (same start offset)
        mov bp,[cs:gc_row_first]                ; BP = first row * 2
        mov ax,[cs:gc_strip_left]
        cmp ax,[cs:gc_strip_right]              ; one strip only (aligned or clipped)?
        jnz .two_strip_row
        jmp short .one_strip
        nop
.two_strip_row:
        push bp                                 ; two strips: left word
        add bp,[cs:gc_strip_left]
        mov ax,[es:bp+0]                        ; background, left strip
        mov [di],ax                             ; save
        add di,byte 0x2
        mov si,[bx]                             ; screen mask row
        shl si,cl                               ; shift into place
        not dx                                  ; DX = low s bits: pixels left of cursor
        rr or, si,dx                            ; keep those
        rr and, ax,si                           ; AND
        mov si,[bx+MASK_BYTES]                  ; cursor mask row
        shl si,cl
        rr xor, ax,si                           ; XOR
        mov [es:bp+0],ax
        mov ax,[es:bp+STRIP_BYTES]              ; same row in the next strip
        mov [di],ax                             ; save
        add di,byte 0x2
        xchg ch,cl                              ; CL = 16 - s
        mov si,[bx]
        shr si,cl                               ; the part that spilled over
        not dx                                  ; DX = FFFF << s: pixels right of it
        rr or, si,dx
        rr and, ax,si
        mov si,[bx+MASK_BYTES]
        shr si,cl
        rr xor, ax,si
        mov [es:bp+STRIP_BYTES],ax
        xchg ch,cl                              ; CL = s again
        add bx,byte 0x2                         ; next mask row
        pop bp
        add bp,byte 0x2                         ; next screen row
        cmp bp,[cs:gc_row_last]
        jng .two_strip_row
        jmp short .drawn
        nop
.one_strip:
        not dx                                  ; single strip: DX = low s bits
.one_strip_row:
        push bp
        add bp,[cs:gc_strip_left]
        mov ax,[es:bp+0]
        mov [di],ax
        add di,byte 0x4                         ; keep the two-words-per-row layout
        mov si,[bx]
        shl si,cl
        rr or, si,dx
        rr and, ax,si
        mov si,[bx+MASK_BYTES]
        shl si,cl
        rr xor, ax,si
        mov [es:bp+0],ax
        add bx,byte 0x2
        pop bp
        add bp,byte 0x2
        cmp bp,[cs:gc_row_last]
        jng .one_strip_row
.drawn:
        mov byte [cs:cursor_drawn],0xff         ; image is on screen
.exit:
        pop es
        pop si
        pop bp
        pop dx
        pop cx
        pop bx
        ret

;-----------------------------------------------------------------------------
; restore_graphics_background - undo draw_graphics_cursor
;
; Writes save_buffer back using the gc_* geometry of the last draw.
; Caller: erase_cursor (only when cursor_drawn was set).
;-----------------------------------------------------------------------------
restore_graphics_background:
        push di
        push si
        push es
        mov ax,[cs:bitmap_seg]
        mov es,ax
        mov si,[cs:gc_skip_rows]
        add si,save_buffer
        mov di,[cs:gc_row_first]
        mov ax,[cs:gc_strip_left]
        cmp ax,[cs:gc_strip_right]
        jnz .two_strip_row
        jmp short .one_strip_row
        nop
.two_strip_row:
        push di
        add di,[cs:gc_strip_left]
        lodsw                                   ; left strip word
        mov [es:di],ax
        lodsw                                   ; right strip word
        mov [es:di+STRIP_BYTES],ax
        pop di
        add di,byte 0x2
        cmp di,[cs:gc_row_last]
        jng .two_strip_row
        jmp short .done
        nop
.one_strip_row:
        push di
        add di,[cs:gc_strip_left]
        lodsw
        add si,byte 0x2                         ; skip the unused second word
        mov [es:di],ax
        pop di
        add di,byte 0x2
        cmp di,[cs:gc_row_last]
        jng .one_strip_row
.done:
        pop es
        pop si
        pop di
        ret

; ============================================================================
;  RESIDENT VARIABLES (0ADBh-0C1Fh)
;  Every variable is accessed with a CS: override. reset_driver clears
;  raw_x .. right_release+4 with one REP STOSW (15 words).
; ============================================================================
pkt_x:              db 0x00                     ; packet X delta being assembled: bits 7:6 from byte 1, bits 5:0 from byte 2
pkt_y:              db 0x00                     ; packet Y delta being assembled: bits 7:6 from byte 1, bits 5:0 from byte 3
raw_x:              dw 0x0000                   ; raw X mickeys since last read (fn 11 / INT FBh); reset clears 15 words from here
raw_y:              dw 0x0000                   ; raw Y mickeys since last read (+ = down)
button_state:       dw 0x0000                   ; bit 0 = left down, bit 1 = right down
left_press:         dw 0,-1,-1                  ; left press   : count, X, Y of last press   (fn 5, BX=0)
left_release:       dw 0,-1,-1                  ; left release : count, X, Y of last release (fn 6, BX=0)
right_press:        dw 0,-1,-1                  ; right press  : count, X, Y                (fn 5, BX bit 0 set)
right_release:      dw 0,-1,-1                  ; right release: count, X, Y                (fn 6, BX<>0)
x_min:              dw 0x0000                   ; horizontal range (fn 7), pixels
x_max:              dw 0x0000
y_min:              dw 0x0000                   ; vertical range (fn 8), pixels
y_max:              dw 0x0000
excl_left:          dw 0x0000                   ; conditional-off rectangle (fn 16): CX
excl_top:           dw 0x0000                   ; DX  (fn 0/1 park it at 400+31 = below the screen = disabled)
excl_right:         dw 0x0000                   ; SI
excl_bottom:        dw 0x0000                   ; DI
mickey_ratio_x:     dw 8                        ; mickeys per 8 pixels, X (fn 15, default 8)
mickey_ratio_y:     dw 8                        ; mickeys per 8 pixels, Y (fn 15, default 8)
user_mask:          dw 0x0000                   ; event mask for the user handler (fn 12 CX)
event_flags:        dw 0x0000                   ; events of the current packet (only the low byte is written)
isr_not_busy:       db 0xFF                     ; FFh = user handler may be called, 00h = handler running
screen_width:       dw 800                      ; 800 pixels
screen_height:      dw 400                      ; 400 pixels
text_columns:       db 80                       ; 80 character columns
cell_width:         dw 10                       ; 10 pixels per text cell (read as byte by div, as word by add)
cell_height:        dw 16                       ; 16 pixels per text cell
gran_x:             dw 10                       ; X granularity of the cursor position: 10 text / 1 graphics
gran_y:             dw 16                       ; Y granularity: 16 text / 1 graphics (=1 is the "graphics mode" test)
bitmap_seg:         dw 0x0000                   ; graphics mode: segment of the font cell of screen cell 0
reset_status:       dw -1                       ; returned in AX by fn 0 (FFFFh = mouse installed)
pkt_state:          db 0x00                     ; 0 = waiting for sync byte, 1 = expect byte 2 (X), 2 = expect byte 3 (Y)
port_index:         dw 0x0001                   ; 0 = serial A, 1 = serial B (init parameter; default B)
cursor_screen_mask: times 16 dw 0x0000          ; active graphics screen (AND) mask, bit-reversed
cursor_image_mask:  times 16 dw 0x0000          ; active graphics cursor (XOR) mask, bit-reversed
hotspot_x:          dw -1                       ; graphics cursor hot spot (fn 9 BX), default -1
hotspot_y:          dw -1                       ; graphics cursor hot spot (fn 9 CX), default -1
default_screen_mask:                    ; standard Microsoft arrow, screen mask (MSB = leftmost pixel) (# = 1 bit)
        dw 0011111111111111b   ; row  0  3FFFh  ..##############
        dw 0001111111111111b   ; row  1  1FFFh  ...#############
        dw 0000111111111111b   ; row  2  0FFFh  ....############
        dw 0000011111111111b   ; row  3  07FFh  .....###########
        dw 0000001111111111b   ; row  4  03FFh  ......##########
        dw 0000000111111111b   ; row  5  01FFh  .......#########
        dw 0000000011111111b   ; row  6  00FFh  ........########
        dw 0000000001111111b   ; row  7  007Fh  .........#######
        dw 0000000000111111b   ; row  8  003Fh  ..........######
        dw 0000000000011111b   ; row  9  001Fh  ...........#####
        dw 0000000111111111b   ; row 10  01FFh  .......#########
        dw 0001000011111111b   ; row 11  10FFh  ...#....########
        dw 0011000011111111b   ; row 12  30FFh  ..##....########
        dw 1111100001111111b   ; row 13  F87Fh  #####....#######
        dw 1111100001111111b   ; row 14  F87Fh  #####....#######
        dw 1111110000111111b   ; row 15  FC3Fh  ######....######
default_cursor_mask:                    ; standard Microsoft arrow, cursor mask (# = 1 bit)
        dw 0000000000000000b   ; row  0  0000h  ................
        dw 0100000000000000b   ; row  1  4000h  .#..............
        dw 0110000000000000b   ; row  2  6000h  .##.............
        dw 0111000000000000b   ; row  3  7000h  .###............
        dw 0111100000000000b   ; row  4  7800h  .####...........
        dw 0111110000000000b   ; row  5  7C00h  .#####..........
        dw 0111111000000000b   ; row  6  7E00h  .######.........
        dw 0111111100000000b   ; row  7  7F00h  .#######........
        dw 0111111110000000b   ; row  8  7F80h  .########.......
        dw 0111111111000000b   ; row  9  7FC0h  .#########......
        dw 0111110000000000b   ; row 10  7C00h  .#####..........
        dw 0100011000000000b   ; row 11  4600h  .#...##.........
        dw 0000011000000000b   ; row 12  0600h  .....##.........
        dw 0000001100000000b   ; row 13  0300h  ......##........
        dw 0000001100000000b   ; row 14  0300h  ......##........
        dw 0000000000000000b   ; row 15  0000h  ................
text_screen_mask:   dw 0xFFFF                   ; software text cursor AND mask (fn 10 CX)
text_cursor_mask:   dw 0x7700                   ; software text cursor XOR mask (fn 10 DX)
text_cursor_type:   db 0x00                     ; 0 = software text cursor, else hardware (6845) cursor; reset sets 1
frac_x:             dw 0x0000                   ; remainder of (mickeys*8)/ratio carried to the next packet, X
frac_y:             dw 0x0000                   ; same, Y
virt_x:             dw 0x0000                   ; virtual position, clamped to range +-16
virt_y:             dw 0x0000
cursor_x:           dw 0x0000                   ; reported/displayed position: range-clamped, rounded to granularity
cursor_y:           dw 0x0000
drawn_x:            dw 0x0000                   ; position at which the cursor was last drawn
drawn_y:            dw 0x0000
cursor_level:       db 0xFF                     ; visibility counter: 0 = shown, negative = hidden (signed byte)
cursor_drawn:       db 0x00                     ; FFh = cursor image is on screen and must be erased
save_buffer:        times 64 db 0x00            ; screen contents under the cursor (1 word text, 16 x 2 words graphics)
gc_x:               dw 0x0000                   ; graphics cursor left edge = cursor_x - hotspot_x
gc_y:               dw 0x0000                   ; graphics cursor top edge  = cursor_y - hotspot_y
gc_skip_rows:       dw 0x0000                   ; rows clipped off the top (later *2 = mask/buffer byte offset)
gc_skip_cols:       dw 0x0000                   ; pixels clipped off the left edge
gc_row_first:       dw 0x0000                   ; first screen row drawn (later *2 = byte offset in a strip)
gc_row_last:        dw 0x0000                   ; last screen row drawn (later *2)
gc_strip_left:      dw 0x0000                   ; left X, then byte offset of its 16-pixel strip (x/16*800)
gc_strip_right:     dw 0x0000                   ; right X, then byte offset of its strip
gc_shift:           db 0x00                     ; cursor X mod 16 = bit shift inside the strip word
unused_0C14:        times 12 db 0x00            ; never referenced

; ============================================================================
;  INITIALISATION - discarded after INIT (break address = device_init)
; ============================================================================

%ifndef BUILD_COM

;-----------------------------------------------------------------------------
; device_init - DOS INIT request (command 0)
;
; In : request_ptr -> request header; +12h -> the CONFIG.SYS text after
;      "device=", i.e. "mouse.dev a" + CR/LF
; Scans the WHOLE line, file name included, for the first a/A/b/B and
; selects port A (0) or B (1); no letter keeps the built-in default B.
; (A path like "b:mouse.dev a" would therefore select port B.)
; Then: reset_driver with BX = 0 (hardware init), INT 33h and INT FBh
; vectors, sign-on message, break address = CS:device_init.
; Exits through request_done; AL still holds 24h ("$") from the last
; INT 21h AH=09h, so the status word is 0124h - harmless, the error bit
; is clear.
;-----------------------------------------------------------------------------
device_init:
        push cs                                 ; DS = CS
        pop ds
        lds bx,[cs:request_ptr]                 ; request header
        lds si,[bx+RQ_CMDLINE]                  ; DS:SI -> CONFIG.SYS line
.scan:
        lodsb
        cmp al,0xa                              ; LF or CR: end of line
        jz .parsed
        cmp al,0xd
        jz .parsed
        cmp al,'a'
        jz .port_a
        cmp al,'A'
        jz .port_a
        cmp al,'b'
        jz .port_b
        cmp al,'B'
        jz .port_b
        jmp short .scan
.port_a:
        mov word [cs:port_index],0x0            ; serial A
        jmp short .parsed
        nop
.port_b:
        mov word [cs:port_index],0x1            ; serial B
.parsed:
        push cs
        pop ds
        rr xor, bx,bx                           ; BX = 0: include the hardware init
        call reset_driver                       ; reset everything
        push cs
        pop ds
        mov dx,int33_entry
        mov al,0x33
        mov ah,0x25
        int 0x21                                ; DOS set vector INT 33h
        mov dx,int_fb_entry
        mov al,0xfb
        mov ah,0x25
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
        lds bx,[cs:request_ptr]
        mov word [bx+RQ_BREAK_OFS],device_init  ; keep everything below device_init
        mov [bx+RQ_BREAK_SEG],cs
        jmp near request_done                   ; status = done
msg_signon:                             ; INT 21h AH=09h, '$'-terminated
        db      13,10,'Device MOUSE v1.0, (C) Issue 1990 installed for serial port $'
msg_tail:                               ; printed after the port letter
        db      '.',13,10,'$'
%endif ; BUILD_COM
; end of file: 0CDEh = 3294 bytes
