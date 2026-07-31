(module
  (import "env" "sum4" (func $sum4 (param i32 i32 i32 i32) (result i32)))

  (func (export "run") (result i32)
    i32.const 1
    i32.const 2
    i32.const 3
    i32.const 4
    call $sum4))
