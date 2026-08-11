(module
  (import "env" "compile" (func $compile))

  (global $iterations (mut i32) (i32.const 0))

  ;; A direct-compatible function large enough to receive an empty-stack loop checkpoint. The
  ;; stable local is set before compilation starts and remains live across the OSR edge.
  (func (export "run") (param $should_trap i32) (param $limit i32) (result i32 f64)
    (local $stable i32)
    (local $counter i32)

    (local.set $stable (i32.const 40))

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
      (if (i32.eqz (local.get $counter))
        (then
          (call $compile)))
      (global.set $iterations (i32.add (global.get $iterations) (i32.const 1)))
      (local.set $counter (i32.add (local.get $counter) (i32.const 1)))
      (br_if $loop (i32.lt_u (local.get $counter) (local.get $limit))))

    (if (local.get $should_trap)
      (then
        (unreachable)))

    (i32.add (global.get $iterations) (local.get $stable))
    (f64.convert_i32_u (global.get $iterations))))
