(module
  (global $value (mut v128) (v128.const i64x2 0 0))

  (func $produce (export "produce") (result v128)
    (local $temporary v128)
    v128.const i64x2 0x0123456789abcdef 0x76543210fedcba98
    local.set $temporary
    local.get $temporary
    global.set $value
    global.get $value)

  (func $consume (export "consume") (param $input v128) (result i64)
    (local $copy v128)
    local.get $input
    local.tee $copy
    global.set $value
    local.get $copy
    i64x2.extract_lane 1)

  (func (export "produce_low") (result i64)
    call $produce
    i64x2.extract_lane 0)

  (func (export "produce_high") (result i64)
    call $produce
    i64x2.extract_lane 1)

  (func (export "consume_high") (result i64)
    v128.const i64x2 0x1122334455667788 0x1020304050607080
    call $consume)

  (func (export "merge_high") (param $condition i32) (result i64)
    local.get $condition
    if (result v128)
      v128.const i64x2 0x1111111111111111 0x2222222222222222
    else
      v128.const i64x2 0x3333333333333333 0x4444444444444444
    end
    i64x2.extract_lane 1)

  ;; Keep the local count above Cranelift's promotion cap so $value remains in the frame.
  (func (export "frame_local_high") (result i64)
    (local
      i64 i64 i64 i64 i64 i64 i64 i64
      i64 i64 i64 i64 i64 i64 i64 i64
      i64 i64 i64 i64 i64 i64 i64 i64)
    (local $value v128)
    v128.const i64x2 0x7777777777777777 0x0123456789abcdef
    local.set $value
    local.get $value
    i64x2.extract_lane 1)

  (func (export "tiered_high") (param $iterations i32) (result i64)
    (local $value v128)
    v128.const i64x2 0x5555555555555555 0x6666666666666666
    local.set $value
    block $exit
      loop $loop
        local.get $iterations
        i32.eqz
        br_if $exit
        local.get $value
        drop
        local.get $value
        drop
        local.get $value
        drop
        local.get $value
        drop
        local.get $value
        drop
        local.get $value
        drop
        local.get $value
        drop
        local.get $value
        drop
        local.get $value
        drop
        local.get $value
        drop
        local.get $value
        drop
        local.get $value
        drop
        local.get $value
        drop
        local.get $value
        drop
        local.get $value
        drop
        local.get $value
        drop
        local.get $iterations
        i32.const 1
        i32.sub
        local.set $iterations
        br $loop
      end
    end
    local.get $value
    i64x2.extract_lane 1))
