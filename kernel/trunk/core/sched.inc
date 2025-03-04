;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;                                                              ;;
;; Copyright (C) KolibriOS team 2004-2023. All rights reserved. ;;
;; Copyright (C) MenuetOS 2000-2004 Ville Mikael Turjanmaa      ;;
;; Distributed under terms of the GNU General Public License    ;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

$Revision$


;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;; IRQ0 HANDLER (TIMER INTERRUPT) ;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;


align 32
irq0:
        pushad
        mov     ax, app_data
        mov     ds, ax
        mov     es, ax
        inc     [timer_ticks]
        mov     eax, [timer_ticks]
        call    playNote       ; <<<--- Speaker driver
        sub     eax, [next_usage_update]
        cmp     eax, 100
        jb      .nocounter
        add     [next_usage_update], 100
        call    updatecputimes
  .nocounter:
        xor     ecx, ecx        ; send End Of Interrupt signal
        call    irq_eoi

        mov     bl, SCHEDULE_ANY_PRIORITY
        call    find_next_task
        jz      .return  ; if there is only one running process
        call    do_change_task
  .return:
        popad
        iretd

align 4
change_task:
        pushfd
        cli
        pushad
        mov     bl, SCHEDULE_ANY_PRIORITY
        call    find_next_task
        jz      .return  ; the same task -> skip switch

        call    do_change_task
  .return:
        popad
        popfd
        ret

uglobal
align 4
;  far_jump:
;   .offs dd ?
;   .sel  dw ?
   context_counter     dd 0 ;noname & halyavin
   next_usage_update   dd 0
   timer_ticks         dd 0
;  prev_slot           dd ?
;  event_sched         dd ?
endg

align 4
update_counters:
        mov     esi, [current_slot]
        rdtsc
        sub     eax, [esi + APPDATA.counter_add] ; time stamp counter add
        add     [esi + APPDATA.counter_sum], eax ; counter sum
        ret

align 4
updatecputimes:
        mov     ecx, [thread_count]
        mov     esi, SLOT_BASE
  .newupdate:
        xor     eax, eax
        add     esi, sizeof.APPDATA
        xchg    eax, [esi + APPDATA.counter_sum]
        mov     [esi + APPDATA.cpu_usage], eax
        loop    .newupdate
        ret

;TODO: Надо бы убрать использование do_change_task из V86...
; и после этого перенести обработку APPDATA.counter_add/sum в do_change_task

align 4
do_change_task:
;param:
;   ebx = address of the APPDATA for incoming task (new)
;warning:
;   [current_slot_idx] must be changed before (e.g. in find_next_task)
;   [current_slot] is the outcoming (old), and set here to a new value (ebx)
;scratched: eax,ecx,esi
        mov     esi, ebx
        xchg    esi, [current_slot]
; set new stack after saving old
        mov     [esi + APPDATA.saved_esp], esp
        mov     esp, [ebx + APPDATA.saved_esp]
; set new thread io-map
        mov     eax, [ebx + APPDATA.io_map]
        mov     dword [page_tabs + ((tss._io_map_0 and -4096) shr 10)], eax
        mov     eax, [ebx + APPDATA.io_map+4]
        mov     dword [page_tabs + ((tss._io_map_1 and -4096) shr 10)], eax
; set new thread memory-map
        mov     eax, [ebx + APPDATA.process]
        cmp     eax, [current_process]
        je      @f
        mov     [current_process], eax
        mov     eax, [eax + PROC.pdt_0_phys]
        mov     cr3, eax
@@:
; set tss.esp0

        mov     eax, [ebx + APPDATA.saved_esp0]
        mov     [tss._esp0], eax

        mov     edx, [ebx + APPDATA.tls_base]

        mov     [tls_data_l + 2], dx
        shr     edx, 16
        mov     [tls_data_l + 4], dl
        mov     [tls_data_l + 7], dh

        mov     dx, app_tls
        mov     fs, dx

; set gs selector unconditionally
        Mov     ax, graph_data
        Mov     gs, ax
        ; TS flag is not triggered by AVX* instructions, therefore
        ; we have to xsave/xrstor SIMD registers each task change
        bt      [cpu_caps + (CAPS_OSXSAVE/32)*4], CAPS_OSXSAVE mod 32
        jnc     .no_xsave
        mov     ecx, [esi + APPDATA.fpu_state]
        mov     eax, [xsave_eax]
        mov     edx, [xsave_edx]
        xsave   [ecx]
        mov     ecx, [current_slot_idx]
        mov     [fpu_owner], ecx
        mov     ecx, [current_slot]
        mov     ecx, [ecx + APPDATA.fpu_state]
        xrstor  [ecx]
  .no_xsave:
      ; set CR0.TS
        cmp     bh, byte[fpu_owner] ;bh == incoming task (new)
        clts                        ;clear a task switch flag
        je      @f
        mov     eax, cr0            ;and set it again if the owner
        or      eax, CR0_TS         ;of a fpu has changed
        mov     cr0, eax
  @@: ; set context_counter (only for user pleasure ???)
        inc     [context_counter]   ;noname & halyavin
      ; set debug-registers, if it's necessary
        test    byte[ebx + APPDATA.dbg_state], 1
        jz      @f
        xor     eax, eax
        mov     dr6, eax
        lea     esi, [ebx + APPDATA.dbg_regs]
        cld
  macro lodsReg [reg] {
        lodsd
        mov     reg, eax
  }     lodsReg dr0, dr1, dr2, dr3, dr7
  purge lodsReg
  @@:
        ret
;end.




MAX_PRIORITY      = 0   ; highest, used for kernel tasks
USER_PRIORITY     = 1   ; default
IDLE_PRIORITY     = 2   ; lowest, only IDLE thread goes here
NR_SCHED_QUEUES   = 3   ; MUST equal IDLE_PRIORYTY + 1

uglobal
; [scheduler_current + i*4] = zero if there are no threads with priority i,
;  pointer to APPDATA of the current thread with priority i otherwise.
align 4
scheduler_current       rd      NR_SCHED_QUEUES
endg

; Add the given thread to the given priority list for the scheduler.
; in: edx -> APPDATA, ecx = priority
proc scheduler_add_thread
; 1. Acquire the lock.
        spin_lock_irqsave SchedulerLock
; 2. Store the priority in APPDATA structure.
        mov     [edx + APPDATA.priority], ecx
; 3. There are two different cases: the given list is empty or not empty.
; In first case, go to 6. Otherwise, advance to 4.
        mov     eax, [scheduler_current+ecx*4]
        test    eax, eax
        jz      .new_list
; 4. Insert the new item immediately before the current item.
        mov     ecx, [eax + APPDATA.in_schedule.prev]
        mov     [edx + APPDATA.in_schedule.next], eax
        mov     [edx + APPDATA.in_schedule.prev], ecx
        mov     [eax + APPDATA.in_schedule.prev], edx
        mov     [ecx + APPDATA.in_schedule.next], edx
; 5. Release the lock and return.
        spin_unlock_irqrestore SchedulerLock
        ret
.new_list:
; 6. Initialize the list with one item and make it the current item.
        mov     [edx + APPDATA.in_schedule.next], edx
        mov     [edx + APPDATA.in_schedule.prev], edx
        mov     [scheduler_current + ecx*4], edx
; 7. Release the lock and return.
        spin_unlock_irqrestore SchedulerLock
        ret
endp

; Remove the given thread from the corresponding priority list for the scheduler.
; in: edx -> APPDATA
proc scheduler_remove_thread
; 1. Acquire the lock.
        spin_lock_irqsave SchedulerLock
; 2. Remove the item from the corresponding list.
        mov     eax, [edx + APPDATA.in_schedule.next]
        mov     ecx, [edx + APPDATA.in_schedule.prev]
        mov     [eax + APPDATA.in_schedule.prev], ecx
        mov     [ecx + APPDATA.in_schedule.next], eax
; 3. If the given thread is the current item in the list,
; advance the current item.
; 3a. Check whether the given thread is the current item;
; if no, skip the rest of this step.
        mov     ecx, [edx + APPDATA.priority]
        cmp     [scheduler_current + ecx*4], edx
        jnz     .return
; 3b. Set the current item to eax; step 2 has set eax = next item.
        mov     [scheduler_current + ecx*4], eax
; 3c. If there were only one item in the list, zero the current item.
        cmp     eax, edx
        jnz     .return
        mov     [scheduler_current + ecx*4], 0
.return:
; 4. Release the lock and return.
        spin_unlock_irqrestore SchedulerLock
        ret
endp

SCHEDULE_ANY_PRIORITY = 0
SCHEDULE_HIGHER_PRIORITY = 1
;info:
;   Find next task to execute
;in:
;   bl = SCHEDULE_ANY_PRIORITY:
;        consider threads with any priority
;   bl = SCHEDULE_HIGHER_PRIORITY:
;        consider only threads with strictly higher priority than the current one,
;        keep running the current thread if other ready threads have the same or lower priority
;retval:
;   ebx = address of the APPDATA for the selected task (slot-base)
;   ZF  = 1  if the task is the same
;warning:
;   [current_slot_idx] = bh -- as result
;   [current_slot] is not set to new value (ebx)!!!
;scratched: eax,ecx
proc find_next_task
        call    update_counters
        spin_lock_irqsave SchedulerLock
        push    NR_SCHED_QUEUES
; If bl == SCHEDULE_ANY_PRIORITY = 0, loop over all NR_SCHED lists.
; Otherwise, loop over first [APPDATA.priority] lists.
        test    bl, bl
        jz      .start
        mov     ebx, [current_slot]
        mov     eax, [ebx + APPDATA.priority]
        test    eax, eax
        jz      .unlock_found
        mov     [esp], eax
.start:
        xor     ecx, ecx
.priority_loop:
        mov     ebx, [scheduler_current+ecx*4]
        test    ebx, ebx
        jz      .priority_next
.task_loop:
        mov     ebx, [ebx + APPDATA.in_schedule.next]
        mov     al, [ebx + APPDATA.state]
        test    al, al
        jz      .task_found     ; state == 0
        cmp     al, TSTATE_WAITING
        jne     .task_next      ; state == 1,2,3,4,9
      ; state == 5
        pushad  ; more freedom for [APPDATA.wait_test]
        call    [ebx + APPDATA.wait_test]
        mov     [esp + 28], eax
        popad
        or      eax, eax
        jnz     @f
      ; testing for timeout
        mov     eax, [timer_ticks]
        sub     eax, [ebx + APPDATA.wait_begin]
        cmp     eax, [ebx + APPDATA.wait_timeout]
        jb      .task_next
        xor     eax, eax
@@:
        mov     [ebx + APPDATA.wait_param], eax  ; retval for wait
        mov     [ebx + APPDATA.state], TSTATE_RUNNING
.task_found:
        mov     [scheduler_current+ecx*4], ebx
; If we have selected a thread with higher priority
; AND rescheduling is due to IRQ,
; turn the current scheduler list one entry back,
; so the current thread will be next after high-priority thread is done.
        mov     ecx, [esp]
        cmp     ecx, NR_SCHED_QUEUES
        jz      .unlock_found
        mov     eax, [current_slot]
        mov     eax, [eax + APPDATA.in_schedule.prev]
        mov     [scheduler_current + ecx*4], eax
.unlock_found:
        pop     ecx
        spin_unlock_irqrestore SchedulerLock
.found:
        ; the line below assumes APPDATA is 256 bytes long and SLOT_BASE is
        ; aligned on 0x10000
        mov     byte [current_slot_idx], bh

        rdtsc   ;call  _rdtsc
        mov     [ebx + APPDATA.counter_add], eax; for next using update_counters
        cmp     ebx, [current_slot]
        ret
.task_next:
        cmp     ebx, [scheduler_current+ecx*4]
        jnz     .task_loop
.priority_next:
        inc     ecx
        cmp     ecx, [esp]
        jb      .priority_loop
        mov     ebx, [current_slot]
        jmp     .unlock_found
endp

if 0

struc TIMER
{
  .next      dd ?
  .exp_time  dd ?
  .func      dd ?
  .arg       dd ?
}


uglobal
rdy_head   rd 16
endg

align 4
pick_task:

        xor     eax, eax
  .pick:
        mov     ebx, [rdy_head+eax*4]
        test    ebx, ebx
        jz      .next

        mov     [next_task], ebx
        test    [ebx+flags.billable]
        jz      @F
        mov     [bill_task], ebx
  @@:
        ret
  .next:
        inc     eax
        jmp     .pick

; param
;  eax= task
;
; retval
;  eax= task
;  ebx= queue
;  ecx= front if 1 or back if 0
align 4
shed:
        cmp     [eax+.tics_left], 0;signed compare
        mov     ebx, [eax+.priority]
        setg    ecx
        jg      @F

        mov     edx, [eax+.tics_quantum]
        mov     [eax+.ticks_left], edx
        cmp     ebx, (IDLE_PRIORITY-1)
        je      @F
        inc     ebx
  @@:
        ret

; param
;  eax= task
align 4
enqueue:
        call    shed;eax
        cmp     [rdy_head+ebx*4], 0
        jnz     @F

        mov     [rdy_head+ebx*4], eax
        mov     [rdy_tail+ebx*4], eax
        mov     [eax+.next_ready], 0
        jmp     .pick
  @@:
        test    ecx, ecx
        jz      .back

        mov     ecx, [rdy_head+ebx*4]
        mov     [eax+.next_ready], ecx
        mov     [rdy_head+ebx*4], eax
        jmp     .pick
  .back:
        mov     ecx, [rdy_tail+ebx*4]
        mov     [ecx+.next_ready], eax
        mov     [rdy_tail+ebx*4], eax
        mov     [eax+.next_ready], 0
  .pick:
        call    pick_proc;select next task
        ret

end if
