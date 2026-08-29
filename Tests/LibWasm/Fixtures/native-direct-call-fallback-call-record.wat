(module
  (type $unary-i32 (func (param i32) (result i32)))

  ;; The reference local keeps this target in the interpreter.
  (func $interpreter-target (type $unary-i32) (local externref)
    local.get 0)

  (table 1 funcref)
  (elem (i32.const 0) $interpreter-target)

  (func $direct-callee (result i32)
    i32.const 42
    i32.const 0
    call_indirect (type $unary-i32))

  ;; This function has no call record of its own. Its direct callee must create
  ;; temporary call-record storage before falling back to the interpreter.
  (func (export "run") (result i32)
    call $direct-callee))
