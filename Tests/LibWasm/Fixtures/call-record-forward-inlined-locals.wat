(module
  (func $leaf (param i32) (result i32)
    (local i32 i32 i32)
    local.get 0)

  (func $caller (result i32)
    i32.const 1
    i32.const 2
    i32.const 3
    i32.const 4
    call $callee)

  (func $callee (param i32 i32 i32 i32) (result i32)
    local.get 0
    call $leaf)

  (export "run" (func $caller)))
