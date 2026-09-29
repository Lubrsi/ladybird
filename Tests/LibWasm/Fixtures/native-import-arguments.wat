(module
  (import "env" "none" (func $none (result i32)))
  (import "env" "three" (func $three (param i32 i64 f32) (result i64)))
  (import "env" "four" (func $four (param i32 i64 f32 f64) (result f64)))
  (import "env" "eight" (func $eight (param i32 i64 f32 f64 i32 i64 f32 f64) (result f32)))

  (func (export "call_none") (result i32)
    (call $none))

  (func (export "call_three") (result i64)
    (call $three (i32.const -7) (i64.const 0x123456789) (f32.const -1.5)))

  (func (export "call_four") (result f64)
    (call $four (i32.const -7) (i64.const -2) (f32.const 0.25) (f64.const 1e300)))

  (func (export "call_eight") (result f32)
    (call $eight
      (i32.const 1) (i64.const -1) (f32.const 2.5) (f64.const -0.5)
      (i32.const -2147483648) (i64.const 9007199254740993) (f32.const -0.0) (f64.const 3.25))))
