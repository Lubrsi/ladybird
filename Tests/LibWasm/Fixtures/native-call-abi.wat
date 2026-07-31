(module
  (func $sub_i32 (param i32 i32) (result i32)
    local.get 0
    local.get 1
    i32.sub)

  (func $sub_i64 (param i64 i64) (result i64)
    local.get 0
    local.get 1
    i64.sub)

  (func $sub_f32 (param f32 f32) (result f32)
    local.get 0
    local.get 1
    f32.sub)

  (func $sub_f64 (param f64 f64) (result f64)
    local.get 0
    local.get 1
    f64.sub)

  ;; Keep this target in the interpreter so its compiled caller exercises the
  ;; signature-matched fallback stub.
  (func $fallback_f64 (param f64 f64) (result f64) (local externref)
    local.get 0
    local.get 1
    f64.sub)

  (func $fallback_trap (local externref)
    unreachable)

  (func $fallback_sum9_i32 (param i32 i32 i32 i32 i32 i32 i32 i32 i32) (result i32) (local externref)
    local.get 0
    local.get 8
    i32.add)

  (func (export "run_i32") (result i32)
    i32.const 20
    i32.const 3
    call $sub_i32)

  (func (export "run_i64") (result i64)
    i64.const 10000000000
    i64.const 3
    call $sub_i64)

  (func (export "run_f32") (result f32)
    f32.const 7.5
    f32.const 2.25
    call $sub_f32)

  (func (export "run_f64") (result f64)
    f64.const 9.5
    f64.const 1.25
    call $sub_f64)

  (func (export "run_fallback_f64") (result f64)
    f64.const 12.5
    f64.const 3.25
    call $fallback_f64)

  (func (export "run_fallback_trap")
    call $fallback_trap)

  ;; The inner four-argument calls win call-record selection. Their ranges
  ;; overlap the outer call, leaving the outer call in the raw encoding. Keep
  ;; their target as a forward reference so these calls are not inlined.
  (func (export "run_raw_i32") (result i32)
    i32.const 1
    i32.const 2
    i32.const 3
    i32.const 4
    call $sum4_i32
    i32.const 5
    i32.const 6
    i32.const 7
    i32.const 8
    call $sum4_i32
    i32.const 9
    i32.const 10
    i32.const 11
    i32.const 12
    call $sum4_i32
    i32.const 13
    i32.const 14
    i32.const 15
    i32.const 16
    call $sum4_i32
    call $sum4_i32)

  (func (export "run_memory_i32") (result i32)
    i32.const 1
    i32.const 2
    i32.const 3
    i32.const 4
    i32.const 5
    i32.const 6
    i32.const 7
    i32.const 8
    i32.const 9
    call $sum9_i32)

  (func (export "run_memory_fallback_i32") (result i32)
    i32.const 1
    i32.const 2
    i32.const 3
    i32.const 4
    i32.const 5
    i32.const 6
    i32.const 7
    i32.const 8
    i32.const 9
    call $fallback_sum9_i32)

  (func $sum4_i32 (param i32 i32 i32 i32) (result i32)
    local.get 0
    local.get 1
    i32.add
    local.get 2
    i32.add
    local.get 3
    i32.add)

  (export "sum4_i32" (func $sum4_i32))

  (func $sum9_i32 (param i32 i32 i32 i32 i32 i32 i32 i32 i32) (result i32)
    local.get 0
    local.get 8
    i32.add))
