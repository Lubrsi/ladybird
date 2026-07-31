(module
  (type $binary_i32 (func (param i32 i32) (result i32)))
  (type $binary_i64 (func (param i64 i64) (result i64)))
  (type $binary_f32 (func (param f32 f32) (result f32)))
  (type $binary_f64 (func (param f64 f64) (result f64)))
  (type $sum9_i32 (func (param i32 i32 i32 i32 i32 i32 i32 i32 i32) (result i32)))
  (type $unary_i32 (func (param i32) (result i32)))
  (type $void (func))

  (func $sub_i32 (type $binary_i32)
    local.get 0
    local.get 1
    i32.sub)

  (func $add_i32 (type $binary_i32)
    local.get 0
    local.get 1
    i32.add)

  (func $sub_i64 (type $binary_i64)
    local.get 0
    local.get 1
    i64.sub)

  (func $sub_f32 (type $binary_f32)
    local.get 0
    local.get 1
    f32.sub)

  (func $sub_f64 (type $binary_f64)
    local.get 0
    local.get 1
    f64.sub)

  (func $sum9_i32 (type $sum9_i32)
    local.get 0
    local.get 8
    i32.add)

  ;; Keep this target in the interpreter so a compiled caller exercises the
  ;; signature-matched fallback path.
  (func $fallback_i32 (type $unary_i32) (local externref)
    local.get 0)

  (func $trap (type $void)
    unreachable)

  (func $nop (type $void))

  (table 9 funcref)
  (elem (i32.const 0) $sub_i32 $sub_i64 $sub_f32 $sub_f64 $sum9_i32 $fallback_i32 $trap $nop)
  (export "table" (table 0))
  (export "target_add_i32" (func $add_i32))
  (export "target_fallback_i32" (func $fallback_i32))

  (func (export "run_i32") (result i32)
    i32.const 20
    i32.const 3
    i32.const 0
    call_indirect (type $binary_i32))

  (func (export "run_i64") (result i64)
    i64.const 10000000000
    i64.const 3
    i32.const 1
    call_indirect (type $binary_i64))

  (func (export "run_f32") (result f32)
    f32.const 7.5
    f32.const 2.25
    i32.const 2
    call_indirect (type $binary_f32))

  (func (export "run_f64") (result f64)
    f64.const 9.5
    f64.const 1.25
    i32.const 3
    call_indirect (type $binary_f64))

  (func (export "run_sum9_i32") (result i32)
    i32.const 1
    i32.const 2
    i32.const 3
    i32.const 4
    i32.const 5
    i32.const 6
    i32.const 7
    i32.const 8
    i32.const 9
    i32.const 4
    call_indirect (type $sum9_i32))

  (func (export "run_fallback_i32") (result i32)
    i32.const 42
    i32.const 5
    call_indirect (type $unary_i32))

  (func (export "run_void")
    i32.const 7
    call_indirect (type $void))

  (func (export "run_trap")
    i32.const 6
    call_indirect (type $void))

  (func (export "run_type_mismatch") (result i64)
    i64.const 1
    i64.const 2
    i32.const 0
    call_indirect (type $binary_i64))

  (func (export "run_null")
    i32.const 8
    call_indirect (type $void))

  (func (export "run_out_of_bounds")
    i32.const 9
    call_indirect (type $void)))
