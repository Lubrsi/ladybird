(module
  (type $nullary-i32 (func (result i32)))
  (type $unary-i32 (func (param i32) (result i32)))

  (import "provider" "value" (func $provider-value (type $nullary-i32)))

  (table 2 funcref)
  (elem (i32.const 0) $provider-value $increment)

  (func $increment (type $unary-i32) (param i32) (result i32)
    local.get 0
    i32.const 1
    i32.add)

  (func $invoke-indirect (result i32)
    i32.const 41
    i32.const 1
    call_indirect (type $unary-i32))

  (func (export "run") (result i32)
    i32.const 0
    call_indirect (type $nullary-i32)
    drop
    call $invoke-indirect))
