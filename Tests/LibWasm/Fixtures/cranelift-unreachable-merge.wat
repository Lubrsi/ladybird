(module
  (func (export "unreachable_then") (param $condition i32) (result i32)
    (if (result i32) (local.get $condition)
      (then
        (unreachable))
      (else
        (i32.const 42))))

  (func (export "unreachable_else") (param $condition i32) (result f64)
    (if (result f64) (local.get $condition)
      (then
        (f64.const 13.5))
      (else
        (unreachable))))

  (func (export "branched_result") (result f32)
    (block $result (result f32)
      (f32.const 7.25)
      (br $result)
      (unreachable)))
)
