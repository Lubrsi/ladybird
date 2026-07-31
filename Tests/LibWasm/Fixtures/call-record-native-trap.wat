(module
  (func $divide (param i32 i32 i32 i32) (result i32)
    local.get 0
    local.get 1
    i32.div_s)

  (func (export "trap") (result i32)
    i32.const 1
    i32.const 0
    i32.const 0
    i32.const 0
    call $divide)

  (func (export "recover") (result i32)
    i32.const 8
    i32.const 2
    i32.const 0
    i32.const 0
    call $divide))
