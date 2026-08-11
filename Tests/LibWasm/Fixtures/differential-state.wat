(module
  (memory (export "memory") 1)
  (global $state (export "state") (mut i64) (i64.const 1))

  (func (export "step") (param $iterations i32) (result i64)
    (local $index i32)
    (block $done
      (loop $loop
        (br_if $done
          (i32.ge_u
            (local.get $index)
            (local.get $iterations)))

        (global.set $state
          (i64.add
            (i64.mul
              (global.get $state)
              (i64.const 3))
            (i64.const 1)))

        (local.set $index
          (i32.add
            (local.get $index)
            (i32.const 1)))

        (i64.store
          (i32.shl
            (local.get $index)
            (i32.const 3))
          (global.get $state))

        (br $loop)))

    (global.get $state))

  (func (export "trap") (result i32)
    (i32.load (i32.const 65535)))

  ;; Keep a multi-value leaf in the differential fixture so native result marshalling is compared
  ;; directly with interpreter execution.
  (func (export "fallback") (result i32 i64)
    (drop (global.get $state))
    (i32.const -7)
    (i64.const 1234605616436508552)))
