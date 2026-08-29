(module
  (type $binary_i32 (func (param i32 i32) (result i32)))
  (type $binary_i64 (func (param i64 i64) (result i64)))
  (type $binary_f32 (func (param f32 f32) (result f32)))
  (type $binary_f64 (func (param f64 f64) (result f64)))
  (type $sum9_i32 (func (param i32 i32 i32 i32 i32 i32 i32 i32 i32) (result i32)))
  (type $unary_i32 (func (param i32) (result i32)))
  (type $void (func))
  (type $mixed_i32_f32 (func (param i32 f32) (result i32)))
  (type $mixed_i32_i64_i32 (func (param i32 i64 i32) (result i32)))
  (type $mixed_six (func (param i32 i32 i32 f32 i32 i32) (result i32)))
  (type $mixed_f32_result (func (param i32 i32 i32 f32 f32) (result f32)))
  (type $mixed_f64 (func (param i32 f64 i32 i32 i32 i32 i32) (result i32)))
  (type $return_typed_funcref (func (result (ref null $binary_i32))))

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

  (func $fallback_binary_i32 (type $binary_i32) (local externref)
    local.get 0
    local.get 1
    i32.sub)

  (func $trap (type $void)
    unreachable)

  (func $nop (type $void))

  (func $mixed_i32_f32 (type $mixed_i32_f32)
    local.get 0
    local.get 1
    i32.trunc_f32_s
    i32.add)

  (func $mixed_i32_i64_i32 (type $mixed_i32_i64_i32)
    local.get 0
    local.get 1
    i32.wrap_i64
    i32.add
    local.get 2
    i32.add)

  (func $mixed_six (type $mixed_six)
    local.get 0
    local.get 1
    i32.add
    local.get 2
    i32.add
    local.get 3
    i32.trunc_f32_s
    i32.add
    local.get 4
    i32.add
    local.get 5
    i32.add)

  (func $mixed_f32_result (type $mixed_f32_result)
    local.get 0
    f32.convert_i32_s
    local.get 1
    f32.convert_i32_s
    f32.add
    local.get 2
    f32.convert_i32_s
    f32.add
    local.get 3
    f32.add
    local.get 4
    f32.add)

  (func $mixed_f64 (type $mixed_f64)
    local.get 0
    local.get 1
    i32.trunc_f64_s
    i32.add
    local.get 2
    i32.add
    local.get 3
    i32.add
    local.get 4
    i32.add
    local.get 5
    i32.add
    local.get 6
    i32.add)

  ;; Keep these targets in the interpreter so their typed reference results
  ;; exercise the interpreter-to-direct representation conversion.
  (func $return_null_typed_funcref (type $return_typed_funcref)
    ref.null $binary_i32)

  (func $return_non_null_typed_funcref (type $return_typed_funcref)
    ref.func $sub_i32)

  (table 17 funcref)
  (memory 1)
  (elem (i32.const 0) $sub_i32 $sub_i64 $sub_f32 $sub_f64 $sum9_i32 $fallback_i32 $trap $nop $mixed_i32_f32 $mixed_i32_i64_i32 $mixed_six $mixed_f32_result $mixed_f64 $fallback_binary_i32 $return_null_typed_funcref $return_non_null_typed_funcref)
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

  (func (export "run_mixed_i32_f32") (result i32)
    i32.const 20
    f32.const 3
    i32.const 8
    call_indirect (type $mixed_i32_f32))

  (func (export "run_mixed_i32_i64_i32") (result i32)
    i32.const 10
    i64.const 20
    i32.const 12
    i32.const 9
    call_indirect (type $mixed_i32_i64_i32))

  (func (export "run_mixed_six") (result i32)
    i32.const 1
    i32.const 2
    i32.const 3
    f32.const 4
    i32.const 5
    i32.const 6
    i32.const 10
    call_indirect (type $mixed_six))

  (func (export "run_mixed_f32_result") (result f32)
    i32.const 1
    i32.const 2
    i32.const 3
    f32.const 4.25
    f32.const 5.5
    i32.const 11
    call_indirect (type $mixed_f32_result))

  (func (export "run_mixed_f64") (result i32)
    i32.const 1
    f64.const 2
    i32.const 3
    i32.const 4
    i32.const 5
    i32.const 6
    i32.const 7
    i32.const 12
    call_indirect (type $mixed_f64))

  (func (export "run_br_table_then_indirect") (result i32)
    (block $done
      (block $case8
        (block $case7
          (block $case6
            (block $case5
              (block $case4
                (block $case3
                  (block $case2
                    (block $case1
                      (block $case0
                        i32.const 0
                        br_table $case0 $case1 $case2 $case3 $case4 $case5 $case6 $case7 $case8 $done))))))))))
    i32.const 20
    i32.const 3
    i32.const 0
    i32.load
    call_indirect (type $binary_i32))

  ;; The inner and outer call lifetimes overlap, so only the inner call can use
  ;; the shared call record. This leaves the outer call as a raw call_indirect.
  (func (export "run_nested_raw_i32") (result i32)
    i32.const 100
    i32.const 20
    i32.const 3
    i32.const 0
    call_indirect (type $binary_i32)
    i32.const 0
    call_indirect (type $binary_i32))

  (func (export "run_nested_raw_fallback_i32") (result i32)
    i32.const 7
    i32.const 100
    i32.const 20
    i32.const 3
    i32.const 0
    call_indirect (type $binary_i32)
    i32.const 13
    call_indirect (type $binary_i32)
    i32.add)

  (func (export "run_nested_raw_i64") (result i64)
    i64.const 10000000000
    i64.const 20
    i64.const 3
    i32.const 1
    call_indirect (type $binary_i64)
    i32.const 1
    call_indirect (type $binary_i64))

  (func (export "run_nested_raw_f32") (result f32)
    f32.const 100
    f32.const 7.5
    f32.const 2.25
    i32.const 2
    call_indirect (type $binary_f32)
    i32.const 2
    call_indirect (type $binary_f32))

  (func (export "run_nested_raw_f64") (result f64)
    f64.const 100
    f64.const 9.5
    f64.const 1.25
    i32.const 3
    call_indirect (type $binary_f64)
    i32.const 3
    call_indirect (type $binary_f64))

  (func (export "run_nested_raw_void") (result i32)
    i32.const 20
    i32.const 3
    i32.const 7
    call_indirect (type $void)
    i32.const 0
    call_indirect (type $binary_i32))

  (func (export "run_trap")
    i32.const 6
    call_indirect (type $void))

  (func (export "run_type_mismatch") (result i64)
    i64.const 1
    i64.const 2
    i32.const 0
    call_indirect (type $binary_i64))

  (func (export "run_null")
    i32.const 16
    call_indirect (type $void))

  (func (export "run_out_of_bounds")
    i32.const 17
    call_indirect (type $void))

  (func (export "run_null_typed_funcref") (result i32)
    i32.const 14
    call_indirect (type $return_typed_funcref)
    ref.is_null)

  (func (export "run_non_null_typed_funcref") (result i32)
    i32.const 15
    call_indirect (type $return_typed_funcref)
    ref.is_null))
