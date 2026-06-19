; =============================================================================
; Victor 9000 RAM Test ROM
; =============================================================================
; Bare-metal 4K diagnostic ROM for the FF000 socket (MAME victor9k ".8j" chip).
; Replaces only the high (FF000) system ROM. No DOS, no BIOS, no interrupts.
;
; Continuously tests main DRAM and shows live results on the built-in CRT.
;
; Target: 8088/8086 real mode. Output: 4096-byte flat binary loaded at FF000,
; reset far-jump at offset 0xFF0 (CPU resets to FFFF0 = our offset 0xFF0).
;
; Status: M1-M3. Display bring-up, ASCII text UI, burn-in loop with error
; latching. Per-region tests: pattern fill/verify + address-in-address, then a
; March C- pass (transition/coupling/decoder faults). A gated DRAM data-
; retention test (write, hold, verify) catches refresh/leakage faults.
; 100% low-RAM coverage (font/stack relocation) is M4.
; =============================================================================

[BITS 16]
[CPU 8086]
org 0x0000

; Set to 0 to fall back to the proven high-RAM-only tester (skips the low-64K
; font/stack relocation phase). Default 1 = full 100% coverage.
%define FULL_COVERAGE 1

; Set to 1 for a fast smoke test: only the first FAST_WORDS words of each region
; are tested, so a full pass completes in well under a second and the PASS
; counter ticks rapidly. Default 0 = thorough (every word; ~10-30s per pass).
%define FAST 0
%if FAST
  %define REGION_WORDS 0200h          ; 512 words = first 1KB of each region
%else
  %define REGION_WORDS 8000h          ; full 64K segment
%endif

; March C- (transition/coupling coverage) runs every pass alongside the pattern
; engine. The DRAM data-retention test runs once every RETENTION_EVERY passes
; over high RAM. RETENTION_EVERY must be a power of two (used as a pass mask).
RETENTION_EVERY equ 4
; Retention hold time ~= RETENTION_OUTER * 65536 inner loops (~0.2s each on a
; 5 MHz 8088), so the default is roughly a 1-second hold - long enough that a
; refresh failure (cells decay in ms) or a leaky cell shows up on read-back.
RETENTION_OUTER equ 6

; -----------------------------------------------------------------------------
; Hardware constants
; -----------------------------------------------------------------------------
SCREEN_SEG   equ 0F000h          ; video RAM (NOT main DRAM) - 80x25 words
CRTC_SEG     equ 0E800h          ; 6500/6845 CRT controller (as6500)
CRT_RG       equ 0               ; CRTC index register offset
IO_TOP_SEG   equ 0E000h          ; first non-DRAM segment (I/O space)

; -----------------------------------------------------------------------------
; Font placement (downloadable character generator lives in DRAM, low 64K)
; -----------------------------------------------------------------------------
FONT_OFS     equ 0400h           ; dot_ram home (32-byte aligned)
CHAR_BASE    equ (FONT_OFS>>5)   ; glyph-index offset for font_data[0] (=0x20)

; Font homes for low-64K relocation (both in segment 0, 32-byte aligned).
; The font occupies FONT_GLYPHS*32 bytes; with 64 glyphs that is 0x800 bytes.
FONT_A       equ 0400h           ; primary home (= FONT_OFS)
FONT_B       equ 8000h           ; alternate home (used while testing FONT_A)
FONT_BYTES   equ FONT_GLYPHS*32  ; size of the loaded font in bytes
HISEG        equ 1000h           ; verified high segment hosting stack+vars in phase L

; Cell attribute bits (bits 11-15 of the cell word)
ATTR_NORMAL  equ 0000h
ATTR_REV     equ 8000h           ; reverse video

COLS         equ 80
ROWS         equ 25

; -----------------------------------------------------------------------------
; RAM variables (reserved low window, not part of the tested range in M1-M3)
; -----------------------------------------------------------------------------
VARS         equ 0C00h
v_memtop     equ VARS+0          ; first absent 64K segment (top of RAM)
v_pass_lo    equ VARS+2          ; pass counter (32-bit)
v_pass_hi    equ VARS+4
v_err        equ VARS+6          ; error counter
v_failseg    equ VARS+8          ; last failure: segment
v_failoff    equ VARS+10         ;               byte offset
v_failexp    equ VARS+12         ;               expected
v_failgot    equ VARS+14         ;               got
g_attr       equ VARS+16         ; current cell attribute word
g_cbase      equ VARS+18         ; current char base (font location >> 5)
v_spin       equ VARS+20         ; activity spinner frame index (0-3)
VARS_WORDS   equ 16              ; size of the VARS block to relocate (words)

; Activity spinner: a glyph at (row 9, col 19) advanced from the test inner loop
; so it spins at a few Hz regardless of RAM size, proving liveness within a pass.
SPIN_CELL    equ (9*COLS+19)*2   ; VRAM byte offset for the spinner cell

; High-RAM region tested every pass: segment FIRST_SEG .. (v_memtop-1)
FIRST_SEG    equ 1000h           ; low 64K is tested separately in phase L

; =============================================================================
; Font placed in low ROM (MAME-patch dodge)
; =============================================================================
; Stock MAME's victor9k driver patches two spots in the FF000 chip after load:
;   offset 0x1AB    -> 0xC3  ("patch out SCP self test")
;   offset 0xD51-54 -> 0x90  ("patch out ROM checksum error")
; Those patches do not exist on real hardware, but on stock MAME they would
; corrupt whatever lands at those offsets. We arrange the image so both land in
; harmless bytes: the font occupies low ROM so 0x1AB falls inside the unused ','
; glyph (index 12, never displayed), and 0xD51-54 falls in tail padding (below).
%define FONT_LEAD 0x20                  ; shifts 0x1AB into glyph 12 (the ',' cell)
    times (FONT_LEAD - ($-$$)) db 0
%include "font.inc"                      ; font_data = FONT_LEAD
%if ((0x1AB - FONT_LEAD) / 32) != 12
  %error "MAME 0x1AB patch no longer lands in the unused ',' glyph - fix FONT_LEAD"
%endif

; =============================================================================
; Entry point (reset far-jumps here -> FF00:cold_start)
; =============================================================================
cold_start:
    cli
    cld

    ; --- segments & stack (stack below the font in low RAM) ---
    xor ax, ax
    mov ds, ax
    mov ss, ax
    mov sp, 03FCh

    ; --- blank the CRT before touching VRAM (set displayed chars = 0) ---
    mov ax, CRTC_SEG
    mov es, ax
    mov byte [es:0], 1
    mov byte [es:1], 0

    ; --- copy font from ROM to dot_ram (DRAM) ---
    push cs
    pop ds
    mov si, font_data
    xor ax, ax
    mov es, ax                   ; ES = 0
    mov di, FONT_OFS
    mov cx, FONT_GLYPHS*16
    rep movsw

    ; --- re-establish DS = 0 for variable access ---
    xor ax, ax
    mov ds, ax

    ; --- point the NMI vector (int 2) at a ROM IRET. The IVT lives in tested
    ;     DRAM; a stray NMI through a trashed vector would otherwise crash us. ---
    mov word [0x08], nmi_iret
    mov word [0x0A], 0FF00h

    ; --- init variables ---
    mov word [g_attr], ATTR_NORMAL
    mov word [g_cbase], FONT_A>>5
    mov word [v_pass_lo], 0
    mov word [v_pass_hi], 0
    mov word [v_err], 0
    mov word [v_spin], 0

    ; --- clear screen, init CRTC, brightness ---
    call clear_screen
    call crt_reset
    mov ax, CRTC_SEG
    mov es, ax
    mov byte [es:040h], 054h      ; brightness/contrast
    mov byte [es:042h], 0FFh      ; data direction register

    ; --- static UI ---
    call draw_ui

    ; --- size memory ---
    call size_ram                 ; -> [v_memtop]

; =============================================================================
; Burn-in loop
; =============================================================================
main_loop:
    add word [v_pass_lo], 1
    adc word [v_pass_hi], 0
    call reprint_dyn

    ; ---- phase H: test high RAM (segment FIRST_SEG .. memtop-1) ----
    mov bx, FIRST_SEG
.seg_loop:
    cmp bx, [v_memtop]
    jae .pass_done

    mov ax, bx                    ; current segment (row 8, col 13)
    mov dx, 090Dh
    call print_hex16

    call test_seg                 ; in: BX=segment; CF=1 on failure
    jc .had_error

    add bx, 1000h
    jmp .seg_loop

.had_error:
    add word [v_err], 1
    call show_error
    call reprint_dyn              ; refresh error count
    add bx, 1000h                 ; keep going (accumulate intermittent faults)
    jmp .seg_loop

.pass_done:
    call maybe_retention          ; gated DRAM data-retention test (high RAM)
%if FULL_COVERAGE
    ; ---- phase L: test low 64K via font/stack relocation ----
    call test_low64k
%endif
    jmp main_loop

; =============================================================================
; reprint_dyn - repaint the dynamic counters (pass / memtop / errors)
; =============================================================================
reprint_dyn:
    mov ax, [v_pass_hi]
    mov dx, 0706h                 ; row 7 col 6
    call print_hex16
    mov ax, [v_pass_lo]
    mov dx, 070Ah                 ; row 7 col 10
    call print_hex16
    mov ax, [v_memtop]
    mov dx, 0310h                 ; row 3 col 16
    call print_hex16
    mov ax, [v_err]
    mov dx, 071Ch                 ; row 7 col 28
    call print_hex16
    ret

; =============================================================================
; test_words - test ES:[start..start+num) words with patterns + addr-in-addr
; In:  ES = segment, AX = start word index, BP = number of words
; Out: CF=1 on failure with DI=byte offset, DX=expected, AX=got; CF=0 on pass
; Clobbers: AX, CX, DX, SI, DI (BX, BP preserved)
; Note: touches no RAM variables -> safe to call with relocated globals.
; =============================================================================
test_words:
    push bx
    push bp
    mov bx, ax                    ; BX = start word index (held across loop)
    mov si, patterns
    mov cx, NUM_PATTERNS
.ploop:
    push cx
    push si
    mov ax, [cs:si]               ; pattern
    mov dx, ax                    ; keep expected
    mov di, bx
    add di, di                    ; DI = start byte offset
    mov cx, bp
    rep stosw                     ; fill (AX = pattern)
    mov di, bx
    add di, di
    mov cx, bp
    mov ax, dx
    repe scasw
    jnz .fail_scan                ; ZF=0 -> mismatch
    pop si
    pop cx
    add si, 2
    call spin                     ; advance the activity spinner (per pattern)
    loop .ploop

    ; address-in-address
    mov di, bx
    add di, di
    mov cx, bp
.afill:
    mov ax, di
    mov [es:di], ax
    inc di
    inc di
    loop .afill
    mov di, bx
    add di, di
    mov cx, bp
.acheck:
    mov ax, [es:di]
    cmp ax, di
    jne .fail_addr
    inc di
    inc di
    loop .acheck

    pop bp
    pop bx
    clc
    ret

.fail_scan:
    pop si                        ; balance push si
    pop cx                        ; balance push cx
    sub di, 2                     ; DI = failing byte offset
    mov ax, [es:di]               ; got (DX already = expected)
    pop bp
    pop bx
    stc
    ret

.fail_addr:
    mov dx, di                    ; expected = offset
    mov ax, [es:di]               ; got
    pop bp
    pop bx
    stc
    ret

; =============================================================================
; test_seg - test one full 64K segment; records failures into VARS
; In:  BX = segment ; Out: CF=1 on failure (with VARS set), CF=0 pass
; =============================================================================
test_seg:
    mov es, bx
    xor ax, ax                    ; start word 0
    mov bp, REGION_WORDS          ; full 64K (or FAST_WORDS in fast mode)
    call test_region
    jc .fail
    ret                           ; CF=0 from test_words
.fail:
    mov [v_failseg], bx           ; BX preserved by test_words
    mov [v_failoff], di
    mov [v_failexp], dx
    mov [v_failgot], ax
    stc
    ret

%if FULL_COVERAGE
; =============================================================================
; test_low64k - test all of segment 0 (the low 64K) at 100% coverage.
;
; The downloadable font, the stack, and the VARS block all normally live in
; segment 0. We relocate the stack+VARS to verified high RAM (HISEG), and use
; two font homes (FONT_A / FONT_B) so the bytes that host the font itself are
; also covered: test everything except the FONT_A window with the font at A,
; then move the font to B (redrawing) and test the FONT_A window.
;
; Requires memtop > FIRST_SEG (else there is no verified high RAM to host the
; stack/vars, and the phase is skipped).
; Entry: DS=SS=0, SP=03FCh (normal). Exit: same.
; =============================================================================
test_low64k:
    cmp word [v_memtop], FIRST_SEG
    ja .go
    ret                           ; only 64K present: nowhere to host stack/vars
.go:
    ; show segment 0000 in the TESTING SEG field for the duration of phase L
    ; (DS=0 still, so g_attr/g_cbase are valid; print_hex16 saves/restores ES).
    ; AX/CX/DX/SI/DI are all reloaded below, so clobbering them here is fine.
    xor ax, ax
    mov dx, 090Dh                  ; row 9 col 13 (same field phase H uses)
    call print_hex16

    ; NOTE: this path does NOT return via RET. The caller's return address lives
    ; on the low stack (0:~03FAh), which is inside the RAM we are about to test
    ; and overwrite. Instead we finish by resetting the stack and jumping back
    ; into main_loop directly (see .ldone).
    ; relocate VARS (0:VARS) -> HISEG:VARS, then move stack+globals to HISEG
    xor ax, ax
    mov ds, ax
    mov ax, HISEG
    mov es, ax
    mov si, VARS
    mov di, VARS
    mov cx, VARS_WORDS
    rep movsw
    mov ax, HISEG
    mov ds, ax
    mov ss, ax
    mov sp, VARS                  ; stack grows down, below the vars copy

    ; ---- sub-pass 1: all of segment 0 except the FONT_A window ----
    xor ax, ax
    mov es, ax                    ; ES = 0 (low RAM under test)
    mov ax, 8                     ; skip first 16 bytes (keep NMI vector at 0:8)
    mov bp, (FONT_A/2)-8          ; words [8, FONT_A)
    call test_region
    jc .lfail
    mov ax, (FONT_A+FONT_BYTES)/2 ; words after the FONT_A window
%if FAST
    mov bp, REGION_WORDS
%else
    mov bp, 8000h
    sub bp, ax
%endif
    call test_region
    jc .lfail

    ; ---- move font A->B (redraw), then test the FONT_A window ----
%ifndef SKIP_FONTRELOC
    call font_A_to_B
    xor ax, ax
    mov es, ax
    mov ax, FONT_A/2
%if FAST
    mov bp, REGION_WORDS         ; partial font window in fast mode
%else
    mov bp, FONT_BYTES/2
%endif
    call test_region
    pushf
    call font_B_to_A              ; always restore the font to home A
    popf
    jc .lfail
%endif
    jmp .ldone

.lfail:
    ; failure: font is at home A here (sub-pass 1, or already restored). Record.
    mov word [v_failseg], 0
    mov [v_failoff], di
    mov [v_failexp], dx
    mov [v_failgot], ax
    add word [v_err], 1
    call show_error
    call reprint_dyn

.ldone:
    ; restore VARS HISEG->0 and stack/globals to low RAM
    mov ax, HISEG
    mov ds, ax
    xor ax, ax
    mov es, ax
    mov si, VARS
    mov di, VARS
    mov cx, VARS_WORDS
    rep movsw
    xor ax, ax
    mov ds, ax
    mov ss, ax
    mov sp, 03FCh                 ; fresh stack (the old return word was overwritten)
    jmp main_loop                 ; cannot RET: caller's return addr was in tested RAM

; font_A_to_B / font_B_to_A - copy the loaded font between homes, update the
; char base, and re-base on-screen cells to reference the new font location.
font_A_to_B:
    push ds
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov si, FONT_A
    mov di, FONT_B
    mov cx, FONT_BYTES/2
    rep movsw
    pop ds
    mov word [g_cbase], FONT_B>>5
    mov ax, (FONT_B>>5)-(FONT_A>>5)   ; re-base every cell to the moved font
    call rebase
    ret

font_B_to_A:
    push ds
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov si, FONT_B
    mov di, FONT_A
    mov cx, FONT_BYTES/2
    rep movsw
    pop ds
    mov word [g_cbase], FONT_A>>5
    mov ax, (FONT_A>>5)-(FONT_B>>5)
    call rebase
    ret

; rebase - add AX (mod 2^16) to every VRAM cell, retargeting all on-screen
; glyphs to a relocated font with no blank-flash. Preserves regs except AX.
rebase:
    push es
    push cx
    push dx
    push di
    mov dx, ax
    mov ax, SCREEN_SEG
    mov es, ax
    xor di, di
    mov cx, COLS*ROWS
.rb:
    mov ax, [es:di]
    add ax, dx
    mov [es:di], ax
    inc di
    inc di
    loop .rb
    pop di
    pop dx
    pop cx
    pop es
    ret
%endif

; =============================================================================
; show_error - display latched failure info on rows 10-11 (reverse video)
; =============================================================================
show_error:
    mov word [g_attr], ATTR_REV

    mov ax, [v_failseg]
    mov dx, 0D04h                 ; row 13 col 4  (under SEG:)
    call print_hex16
    mov ax, [v_failoff]
    mov dx, 0D10h                 ; row 13 col 16 (under OFF:)
    call print_hex16

    mov ax, [v_failexp]
    mov dx, 0E04h                 ; row 14 col 4  (under EXP:)
    call print_hex16
    mov ax, [v_failgot]
    mov dx, 0E10h                 ; row 14 col 16 (under GOT:)
    call print_hex16
    mov ax, [v_failexp]           ; failing bits = exp XOR got
    xor ax, [v_failgot]
    mov dx, 0E1Ch                 ; row 14 col 28 (under BIT:)
    call print_hex16

    mov word [g_attr], ATTR_NORMAL
    ret

; =============================================================================
; size_ram - find first absent 64K segment -> [v_memtop]
; =============================================================================
size_ram:
    mov bx, FIRST_SEG
.probe:
    mov es, bx
    mov word [es:0], 0AA55h
    mov ax, [es:0]
    cmp ax, 0AA55h
    jne .done
    mov word [es:0], 055AAh
    mov ax, [es:0]
    cmp ax, 055AAh
    jne .done
    add bx, 1000h
    cmp bx, IO_TOP_SEG
    jb .probe
.done:
    mov [v_memtop], bx
    mov ax, bx                    ; show top segment (row 2, col 16)
    mov dx, 0310h
    call print_hex16
    mov ax, bx                    ; RAM detected in KB = (memtop paras) / 64
    mov cl, 6
    shr ax, cl
    mov dx, 050Eh                 ; row 5, col 14 (after "RAM DETECTED: ")
    call print_dec
    mov al, 'K'
    call print_char
    ret

; =============================================================================
; draw_ui - static labels
; =============================================================================
draw_ui:
    mov word [g_attr], ATTR_REV
    mov si, s_title
    mov dx, 0000h
    call print_str
    mov si, s_date                ; build date, just after title (col 23)
    mov dx, 0017h
    call print_str
    mov word [g_attr], ATTR_NORMAL

    mov si, s_brand               ; branding line, row 1 (below the title)
    mov dx, 0100h
    call print_str

    mov si, s_memtop
    mov dx, 0300h
    call print_str
    mov si, s_ramdet
    mov dx, 0500h
    call print_str
    mov si, s_pass
    mov dx, 0700h
    call print_str
    mov si, s_errors
    mov dx, 0714h
    call print_str
    mov si, s_testing
    mov dx, 0900h
    call print_str

    ; FAIL block: header row 12, then two aligned rows; label columns at 0/12/24
    mov si, s_fail
    mov dx, 0C00h
    call print_str
    mov si, s_fseg
    mov dx, 0D00h
    call print_str
    mov si, s_foff
    mov dx, 0D0Ch
    call print_str
    mov si, s_exp
    mov dx, 0E00h
    call print_str
    mov si, s_got
    mov dx, 0E0Ch
    call print_str
    mov si, s_bit
    mov dx, 0E18h
    call print_str
    ret

; =============================================================================
; print_str - print ASCIIZ string from CS:SI at (DH=row, DL=col); advances DL
; =============================================================================
print_str:
    push ax
.l:
    mov al, [cs:si]
    inc si
    or al, al
    jz .done
    call print_char
    jmp .l
.done:
    pop ax
    ret

; =============================================================================
; print_hex16 - print AX as 4 hex digits at (DH=row, DL=col); advances DL by 4
; =============================================================================
print_hex16:
    push ax
    push bx
    push cx
    mov bx, ax
    mov cx, 4
.nh:
    push cx
    mov cl, 4
    rol bx, cl
    pop cx
    mov al, bl
    and al, 0Fh
    cmp al, 10
    jb .dig
    add al, 'A'-10
    jmp .pc
.dig:
    add al, '0'
.pc:
    call print_char
    loop .nh
    pop cx
    pop bx
    pop ax
    ret

; =============================================================================
; print_dec - print AX as decimal at (DH=row, DL=col); advances DL; no leading 0s
; =============================================================================
print_dec:
    push ax
    push bx
    push cx
    push si
    mov si, dx                    ; save cursor (DIV clobbers DX)
    mov bx, 10
    xor cx, cx                    ; digit count
.gen:
    xor dx, dx
    div bx                        ; AX = AX/10, DX = AX%10
    push dx                       ; stash digit
    inc cx
    or ax, ax
    jnz .gen
    mov dx, si                    ; restore cursor
.emit:
    pop ax                        ; digit (AL)
    add al, '0'
    call print_char               ; advances DL, preserves AX/CX/DH
    loop .emit
    pop si
    pop cx
    pop bx
    pop ax
    ret

; =============================================================================
; print_char - print ASCII char AL at (DH=row, DL=col); advances DL
; Out-of-range chars render as space.
; =============================================================================
print_char:
    push ax
    cmp al, FONT_FIRST_ASCII
    jb .space
    cmp al, FONT_LAST_ASCII
    ja .space
    sub al, FONT_FIRST_ASCII
    jmp .put
.space:
    xor al, al                    ; index 0 = space
.put:
    call put_glyph
    pop ax
    ret

; =============================================================================
; put_glyph - write glyph cell to screen at (DH=row, DL=col); advances DL
; In: AL = glyph index, DH=row, DL=col. Uses [g_attr]. Preserves AX/BX.
; =============================================================================
put_glyph:
    push ax
    push bx
    push di
    push es
    mov ah, 0
    add ax, [g_cbase]
    or  ax, [g_attr]
    mov bx, ax                    ; cell value
    mov al, dh
    mov ah, COLS
    mul ah                        ; AX = DH*80
    mov di, ax
    mov al, dl
    mov ah, 0
    add di, ax
    shl di, 1
    mov ax, SCREEN_SEG
    mov es, ax
    mov [es:di], bx
    inc dl
    pop es
    pop di
    pop bx
    pop ax
    ret

; =============================================================================
; spin - advance the activity spinner one frame and draw it.
; Called from the test inner loop. Preserves ALL registers and flags.
; Reads vars via DS (valid in both phases: DS=0 in phase H, DS=HISEG in phase L).
; =============================================================================
spin:
    pushf
    push ax
    push bx
    push es
    mov bx, [v_spin]
    inc bx
    and bx, 3
    mov [v_spin], bx
    mov al, [cs:SPIN_FRAMES+bx]   ; glyph index for this frame
    mov ah, 0
    add ax, [g_cbase]             ; -> cell value (normal attribute)
    mov bx, ax
    mov ax, SCREEN_SEG
    mov es, ax
    mov [es:SPIN_CELL], bx
    pop es
    pop bx
    pop ax
    popf
    ret

; =============================================================================
; clear_screen - fill VRAM with blank cells
; =============================================================================
clear_screen:
    push es
    mov ax, SCREEN_SEG
    mov es, ax
    xor di, di
    mov ax, [g_cbase]             ; glyph index 0 (space) + base, normal attr
    mov cx, COLS*ROWS
    rep stosw
    pop es
    ret

; =============================================================================
; crt_reset - send the 16-register init table to the CRTC (ported from stock)
; =============================================================================
crt_reset:
    mov cx, reset_table_size
    xor si, si
.loop:
    mov bx, si
    xchg bh, bl                   ; BH = register index
    mov bl, [cs:reset_table+si]   ; BL = value
    call set_crt_reg
    inc si
    loop .loop
    ret

set_crt_reg:
    push ax
    push es
    mov ax, CRTC_SEG
    mov es, ax
    mov [es:CRT_RG], bh
    mov ax, 1
    call delay100us
    mov [es:CRT_RG+1], bl
    pop es
    pop ax
    ret

nmi_iret:
    iret

delay100us:
    push cx
    or ax, ax
    jz .up
.l:
    mov cl, 78h
    shr cl, cl
    dec ax
    jnz .l
.up:
    pop cx
    ret

; =============================================================================
; Data
; =============================================================================
reset_table:
    db 92, 80, 81, 0CFh, 25, 6, 25, 25, 3, 14, 20h, 0, 0, 0, 0, 0
reset_table_size equ $-reset_table

patterns:
    dw 00000h, 0FFFFh, 05555h, 0AAAAh, 055AAh, 0AA55h
NUM_PATTERNS equ ($-patterns)/2

s_title   db "VICTOR 9000 RAM TEST V2", 0
s_memtop  db "MEMORY TOP SEG: ", 0
s_ramdet  db "RAM DETECTED: ", 0
s_pass    db "PASS:", 0
s_errors  db "ERRORS:", 0
s_testing db "TESTING SEG:", 0

; Spinner frame -> font glyph index (| / - \). Indices are ASCII-0x20:
; '^'(0x5E)->0x3E used as '|', '/'(0x2F)->0x0F, '-'(0x2D)->0x0D, '\'(0x5C)->0x3C
SPIN_FRAMES db 03Eh, 00Fh, 00Dh, 03Ch
; s_fail..s_bit live in the ROM tail (below) to keep this string table clear of
; the 0xD51 MAME-patch guard.

; =============================================================================
; Reset vector and ROM tail
; =============================================================================
; Guard + pad up to the 0xD51-0xD54 MAME patch region (NASM errors here with a
; negative TIMES if code/data ever grows past it). Those four bytes are then
; harmless tail padding that MAME may overwrite with NOPs.
times (0D51h - ($-$$)) db 0

; 0xD51-0xD54: the four bytes MAME may overwrite with NOPs. Keep real data past
; them. The build date lives here (in tail space) so the string table stays
; clear of the 0xD51 patch region.
times (0D55h - ($-$$)) db 0
s_date    db ' ', __DATE__, 0           ; " YYYY-MM-DD", follows the title
s_retn    db "RETENTION:", 0
s_brand   db "INTERGALACTICMICRO.COM", 0
s_fail    db "FAIL", 0
s_fseg    db "SEG:", 0
s_foff    db "OFF:", 0
s_exp     db "EXP:", 0
s_got     db "GOT:", 0
s_bit     db "BIT:", 0

; =============================================================================
; test_region - pattern/addr engine (test_words) THEN March C- over the same
; window. Same in/out contract as test_words: ES=segment, AX=start word index,
; BP=word count; CF=1 on failure with DI=byte offset, DX=expected, AX=got;
; BX/BP preserved. Used by both phase H (test_seg) and phase L (test_low64k).
; =============================================================================
test_region:
    push ax                        ; save start-word index (test_words clobbers AX)
    call test_words
    jc .fail
    pop ax                         ; restore start word for march_test
    call march_test                ; returns CF (and DI/DX/AX on its own failure)
    ret
.fail:
    pop cx                         ; discard saved start word; keep AX=got and CF=1
    ret                            ; (pop does not affect flags)

; =============================================================================
; march_test - March C- over ES:[start..start+num) words (backgrounds 0/FFFF):
;   { up(w0); up(r0,w1); up(r1,w0); dn(r0,w1); dn(r1,w0); dn(r0) }
; Reads-then-writes each cell in address order, so it catches transition faults,
; address-decoder faults and coupling faults that the fill-then-verify pattern
; pass cannot. In/out contract matches test_words; touches no RAM variables.
; =============================================================================
%macro M_RW 3                     ; %1=dir(0 up / 1 dn)  %2=read-expect  %3=write
    mov dx, %2
%if %1 == 0
    mov di, bx                    ; up: DI = start byte offset
    mov cx, bp
%%lp:
    mov ax, [es:di]
    cmp ax, dx
    jne march_fail
    mov word [es:di], %3
    add di, 2
    loop %%lp
%else
    mov di, bx                    ; dn: DI = last word in the window
    add di, bp
    add di, bp
    sub di, 2
    mov cx, bp
%%lp:
    mov ax, [es:di]
    cmp ax, dx
    jne march_fail
    mov word [es:di], %3
    sub di, 2
    loop %%lp
%endif
    call spin
%endmacro

march_test:
    push bx
    push bp                        ; BP = word count, the loop count for all elems
    add ax, ax                     ; AX = start word * 2 = start byte offset
    mov bx, ax                     ; BX = start byte offset (constant)
    ; M1: up (w0)
    mov di, bx
    mov cx, bp
    xor ax, ax
    rep stosw
    call spin
    M_RW 0, 0,      0FFFFh         ; M2: up (r0,w1)
    M_RW 0, 0FFFFh, 0              ; M3: up (r1,w0)
    M_RW 1, 0,      0FFFFh         ; M4: dn (r0,w1)
    M_RW 1, 0FFFFh, 0              ; M5: dn (r1,w0)
    ; M6: dn (r0)
    mov dx, 0
    mov di, bx
    add di, bp
    add di, bp
    sub di, 2
    mov cx, bp
.m6:
    mov ax, [es:di]
    cmp ax, dx
    jne march_fail
    sub di, 2
    loop .m6
    call spin
    pop bp
    pop bx
    clc
    ret
march_fail:                        ; DX=expected, AX=got, DI=byte offset already set
    pop bp
    pop bx
    stc
    ret

; =============================================================================
; maybe_retention - DRAM data-retention test, run every RETENTION_EVERY'th pass.
; Phase-H context (DS=SS=0). High RAM only (segment 0 holds font/stack/vars).
; =============================================================================
maybe_retention:    
    mov ax, [v_pass_lo]
    and ax, (RETENTION_EVERY-1)
    jnz .skip                      ; only when pass % RETENTION_EVERY == 0
    call retention_phase
.skip:
    ret

; retention_phase - fill all high RAM with a pattern, hold (no DRAM access so
; hardware refresh has to keep it alive), then verify. Catches refresh failures
; and leaky/marginal cells that decay between refresh cycles.
retention_phase:
    cmp word [v_memtop], FIRST_SEG
    ja .have
    ret                            ; no high RAM to hold a pattern
.have:
    ; status (row 9): "RETENTION:" + the held pattern
    mov word [g_attr], ATTR_NORMAL
    mov si, s_retn
    mov dx, 0A00h
    call print_str
    call retn_pattern              ; DX = pattern
    mov ax, dx
    mov dx, 0A0Bh                  ; row 10, col 11 (after "RETENTION: ")
    call print_hex16
    ; --- write: fill all high RAM with the pattern ---
    call retn_pattern              ; DX = pattern (print clobbered DX)
    mov bx, FIRST_SEG
.wseg:
    cmp bx, [v_memtop]
    jae .wdone
    mov es, bx
    xor di, di
    mov ax, dx
    mov cx, REGION_WORDS
    rep stosw
    add bx, 1000h
    jmp .wseg
.wdone:
    ; --- hold: do not touch DRAM; refresh alone must preserve it ---
    call ret_delay
    ; --- verify: read it all back ---
    mov bx, FIRST_SEG
.vseg:
    cmp bx, [v_memtop]
    jae .vdone
    call retn_pattern              ; DX = pattern (recompute; show_error clobbers it)
    mov es, bx
    call ret_verify_seg            ; ES,DX -> CF, DI=byte offset, AX=got
    jc .vfail
.vnext:
    add bx, 1000h
    jmp .vseg
.vfail:
    mov [v_failseg], bx
    mov [v_failoff], di
    mov [v_failexp], dx
    mov [v_failgot], ax
    add word [v_err], 1
    call show_error
    call reprint_dyn
    jmp .vnext
.vdone:
    call blank_retn                ; clear the row-9 status line
    ret

; retn_pattern - DX = retention background, alternating AAAA/5555 across runs.
retn_pattern:
    mov dx, 0AAAAh
    test byte [v_pass_lo], RETENTION_EVERY
    jz .done
    mov dx, 05555h
.done:
    ret

; ret_delay - busy-wait the hold time, keeping the spinner alive. Touches no
; DRAM (only CX and VRAM via spin) so the pattern just sits and ages.
ret_delay:
    push bx
    mov bx, RETENTION_OUTER
.o:
    call spin
    xor cx, cx                     ; CX=0 -> 65536 inner iterations
.i:
    loop .i
    dec bx
    jnz .o
    pop bx
    ret

; ret_verify_seg - check ES:[0..REGION_WORDS) all equal DX.
; Out: CF=1 with DI=byte offset, AX=got on mismatch. Preserves BX, DX, BP.
ret_verify_seg:
    push cx
    xor di, di
    mov ax, dx
    mov cx, REGION_WORDS
    repe scasw
    jne .bad
    pop cx
    clc
    ret
.bad:
    sub di, 2                      ; DI -> failing word
    mov ax, [es:di]                ; got
    pop cx
    stc
    ret

; blank_retn - erase the row-9 retention status line.
blank_retn:
    push es
    push ax
    push cx
    push di
    mov ax, SCREEN_SEG
    mov es, ax
    mov di, (10*COLS)*2
    mov ax, [g_cbase]              ; blank cell (glyph 0 + base)
    mov cx, 20
    rep stosw
    pop di
    pop cx
    pop ax
    pop es
    ret

times (0FF0h - ($-$$)) db 0
    jmp 0FF00h:cold_start         ; offset 0xFF0: reset entry -> cold_start

times (0FFAh - ($-$$)) db 0
version:
    dw 0F3F7h
                                  ; 0xFFC-0xFFD: unused
times (0FFEh - ($-$$)) db 0
pcflag:
    dw 0FFFFh

times (1000h - ($-$$)) db 0      ; pad to exactly 4096 bytes
