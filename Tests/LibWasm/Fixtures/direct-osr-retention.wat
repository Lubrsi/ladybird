(module
  (import "env" "compile" (func $compile))

  (global $iterations (mut i32) (i32.const 0))

  ;; A direct-compatible function large enough to receive an empty-stack loop checkpoint. The
  ;; stable local is set before compilation starts and remains live across the OSR edge.
  (func (export "run") (result i32)
    (local $stable i32)
    (local $counter i32)

    (local.set $stable (i32.const 40))
    (call $compile)

    (loop $loop
      (drop (i32.const 0))
      (drop (i32.const 0))
      (drop (i32.const 0))
      (drop (i32.const 0))
      (drop (i32.const 0))
      (drop (i32.const 0))
      (drop (i32.const 0))
      (drop (i32.const 0))
      (drop (i32.const 0))
      (drop (i32.const 0))
      (drop (i32.const 0))
      (drop (i32.const 0))
      (drop (i32.const 0))
      (drop (i32.const 0))
      (drop (i32.const 0))
      (drop (i32.const 0))
      (global.set $iterations (i32.add (global.get $iterations) (i32.const 1)))
      (local.set $counter (i32.add (local.get $counter) (i32.const 1)))
      (br_if $loop (i32.lt_u (local.get $counter) (i32.const 3))))
    (i32.add (global.get $iterations) (local.get $stable))))
