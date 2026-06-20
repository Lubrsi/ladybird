;; Regression fixture for register clobbering across stack-argument calls in the
;; bytecode interpreter. $store_value holds a store address (a constant) live in a
;; register across two calls whose callees use the whole register file, then stores
;; through that address and reads it back. If the interpreter fails to preserve the
;; caller's registers across the calls, the address is clobbered and the readback is
;; wrong (or the store traps out of bounds). Memory is sized so the intended address
;; is in bounds.
(module
  (memory 140) ;; ~9.17 MiB, so 0x84A06C is a valid address

  ;; Six-parameter callee that keeps eight values live, forcing use of the full
  ;; register file (mirrors a real allocator/constructor call).
  (func $clobber_a (param i32 i32 i32 i32 i32 i32) (result i32)
    (local i32 i32 i32 i32 i32 i32 i32 i32)
    (local.set 6  (i32.add (local.get 0) (i32.const 0xfffff000)))
    (local.set 7  (i32.add (local.get 1) (local.get 6)))
    (local.set 8  (i32.xor (local.get 2) (local.get 7)))
    (local.set 9  (i32.add (local.get 3) (local.get 8)))
    (local.set 10 (i32.sub (local.get 4) (local.get 9)))
    (local.set 11 (i32.add (local.get 5) (local.get 10)))
    (local.set 12 (i32.add (local.get 6) (local.get 11)))
    (local.set 13 (i32.add (local.get 7) (local.get 12)))
    (i32.const 512))

  ;; Five-parameter callee, same idea.
  (func $clobber_b (param i32 i32 i32 i32 i32) (result i32)
    (local i32 i32 i32 i32 i32 i32 i32 i32)
    (local.set 5  (i32.add (local.get 0) (i32.const 0xfffff000)))
    (local.set 6  (i32.add (local.get 1) (local.get 5)))
    (local.set 7  (i32.xor (local.get 2) (local.get 6)))
    (local.set 8  (i32.add (local.get 3) (local.get 7)))
    (local.set 9  (i32.sub (local.get 4) (local.get 8)))
    (local.set 10 (i32.add (local.get 5) (local.get 9)))
    (local.set 11 (i32.add (local.get 6) (local.get 10)))
    (local.set 12 (i32.add (local.get 7) (local.get 11)))
    (i32.const 1230))

  (func $store_value (result i32)
    (i32.store
      (i32.const 8691820) ;; store address, must survive both calls
      (call $clobber_b
        (call $clobber_a (i32.const 1)(i32.const 2)(i32.const 3)(i32.const 4)(i32.const 5)(i32.const 6))
        (i32.const 7)(i32.const 8)(i32.const 9)(i32.const 10)))
    (i32.load (i32.const 8691820))) ;; returns 1230 iff the address was not clobbered

  (export "store_value" (func $store_value)))
