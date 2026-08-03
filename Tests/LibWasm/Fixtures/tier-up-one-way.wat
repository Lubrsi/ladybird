(module
  (import "env" "compile" (func $compile))

  (global $iterations (mut i32) (i32.const 0))

  (func (export "run") (result i32)
    (local $padding i32)
    (local $iteration i32)

    (call $compile)

    (local.set $padding (i32.add (local.get $padding) (i32.const 1)))
    (local.set $padding (i32.add (local.get $padding) (i32.const 1)))
    (local.set $padding (i32.add (local.get $padding) (i32.const 1)))
    (local.set $padding (i32.add (local.get $padding) (i32.const 1)))
    (local.set $padding (i32.add (local.get $padding) (i32.const 1)))
    (local.set $padding (i32.add (local.get $padding) (i32.const 1)))
    (local.set $padding (i32.add (local.get $padding) (i32.const 1)))
    (local.set $padding (i32.add (local.get $padding) (i32.const 1)))
    (local.set $padding (i32.add (local.get $padding) (i32.const 1)))
    (local.set $padding (i32.add (local.get $padding) (i32.const 1)))
    (local.set $padding (i32.add (local.get $padding) (i32.const 1)))
    (local.set $padding (i32.add (local.get $padding) (i32.const 1)))

    (loop $loop
      (global.set $iterations (i32.add (global.get $iterations) (i32.const 1)))
      (local.set $iteration (i32.add (local.get $iteration) (i32.const 1)))
      (br_if $loop (i32.lt_u (local.get $iteration) (i32.const 3))))

    (global.get $iterations)))
