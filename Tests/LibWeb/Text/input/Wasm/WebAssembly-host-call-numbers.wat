(module
  (import "env" "echo_i32" (func $echo_i32 (param i32) (result i32)))
  (import "env" "echo_f32" (func $echo_f32 (param f32) (result f32)))
  (import "env" "echo_f64" (func $echo_f64 (param f64) (result f64)))
  (import "env" "observe" (func $observe (param i32 f32 f64)))
  (import "env" "eight" (func $eight (param i32 f32 f64 i32 f32 f64 i32 f64) (result f64)))

  (func (export "call_echo_i32") (param i32) (result i32)
    (call $echo_i32 (local.get 0)))

  (func (export "call_echo_f32") (param f32) (result f32)
    (call $echo_f32 (local.get 0)))

  (func (export "call_echo_f64") (param f64) (result f64)
    (call $echo_f64 (local.get 0)))

  (func (export "call_observe") (param i32 f32 f64)
    (call $observe (local.get 0) (local.get 1) (local.get 2)))

  (func (export "call_eight") (result f64)
    (call $eight
      (i32.const -1) (f32.const 0.1) (f64.const -0.0) (i32.const -2147483648)
      (f32.const nan) (f64.const inf) (i32.const 7) (f64.const 2.5)))

  (func (export "add_one") (param i32) (result i32)
    (i32.add (local.get 0) (i32.const 1))))
