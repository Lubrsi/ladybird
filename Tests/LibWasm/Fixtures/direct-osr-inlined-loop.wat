(module
  (import "env" "compile" (func $compile))

  (func $looping_callee (result i32)
    (local $iteration i32)

    (loop $loop
      (local.set $iteration (i32.add (local.get $iteration) (i32.const 1)))
      (br_if $loop (i32.lt_u (local.get $iteration) (i32.const 3))))

    (local.get $iteration))

  (func (export "run") (result i32)
    (local $callee_result i32)
    (local $iteration i32)
    (local $padding i32)

    ;; Keep the caller large enough for tier-up checkpoint insertion after bytecode inlining.
    (local.set $padding (i32.const 1))
    (local.set $padding (i32.const 2))
    (local.set $padding (i32.const 3))
    (local.set $padding (i32.const 4))
    (local.set $padding (i32.const 5))
    (local.set $padding (i32.const 6))
    (local.set $padding (i32.const 7))
    (local.set $padding (i32.const 8))
    (local.set $padding (i32.const 9))
    (local.set $padding (i32.const 10))
    (local.set $padding (i32.const 11))
    (local.set $padding (i32.const 12))
    (local.set $padding (i32.const 13))
    (local.set $padding (i32.const 14))
    (local.set $padding (i32.const 15))
    (local.set $padding (i32.const 16))
    (local.set $padding (i32.const 17))
    (local.set $padding (i32.const 18))
    (local.set $padding (i32.const 19))
    (local.set $padding (i32.const 20))

    (call $compile)
    (local.set $callee_result (call $looping_callee))

    ;; This parsed loop gives the direct OSR body one real resume checkpoint. The earlier inlined
    ;; loop must not be treated as another transferable checkpoint.
    (loop $loop
      (local.set $iteration (i32.add (local.get $iteration) (i32.const 1)))
      (br_if $loop (i32.lt_u (local.get $iteration) (i32.const 3))))

    (i32.add (local.get $callee_result) (local.get $iteration))))
