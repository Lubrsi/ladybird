(module
  (type $unary (func (param i32) (result i32)))
  (type $pair (func (param i32) (result i32 i32)))
  (type $sink (func (param i32)))

  (global $last (mut i32) (i32.const 0))

  (func $sum16
    (param i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32)
    (result i32)
    (local v128)
    local.get 0
    local.get 1
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
    i32.add
    local.get 7
    i32.add
    local.get 8
    i32.add
    local.get 9
    i32.add
    local.get 10
    i32.add
    local.get 11
    i32.add
    local.get 12
    i32.add
    local.get 13
    i32.add
    local.get 14
    i32.add
    local.get 15
    i32.add
  )

  (func $sink16
    (param i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32)
    (local v128)
    local.get 0
    global.set $last
  )

  (func $double (type $unary) (local v128)
    local.get 0
    i32.const 2
    i32.mul
  )

  (func $make_pair (type $pair) (local v128)
    local.get 0
    local.get 0
    i32.const 1
    i32.add
  )

  (func $remember (type $sink) (local v128)
    local.get 0
    global.set $last
  )

  (table 4 funcref)
  (elem (i32.const 0) $double $make_pair)
  (elem (i32.const 3) $remember)

  (func (export "direct") (param i32 i32) (result i32)
    i32.const 1000
    local.get 0
    if (result i32)
      local.get 1
      i32.const 1
      i32.const 2
      i32.const 3
      i32.const 4
      i32.const 5
      i32.const 6
      i32.const 7
      i32.const 8
      i32.const 9
      i32.const 10
      i32.const 11
      i32.const 12
      i32.const 13
      i32.const 14
      i32.const 15
      call $sum16
    else
      i32.const 7
    end
    i32.add
  )

  (func (export "void_call") (param i32) (result i32)
    i32.const 77
    local.get 0
    i32.const 1
    i32.const 2
    i32.const 3
    i32.const 4
    i32.const 5
    i32.const 6
    i32.const 7
    i32.const 8
    i32.const 9
    i32.const 10
    i32.const 11
    i32.const 12
    i32.const 13
    i32.const 14
    i32.const 15
    call $sink16
  )

  (func (export "read_last") (result i32)
    global.get $last
  )

  (func (export "indirect") (param i32) (result i32)
    i32.const 1000
    local.get 0
    i32.const 0
    call_indirect (type $unary)
    i32.add
  )

  (func (export "indirect_pair") (param i32) (result i32)
    i32.const 1000
    local.get 0
    i32.const 1
    call_indirect (type $pair)
    i32.add
    i32.add
  )

  (func (export "indirect_index") (param i32 i32) (result i32)
    local.get 0
    local.get 1
    call_indirect (type $unary)
  )

  (func (export "indirect_void") (param i32) (result i32)
    local.get 0
    i32.const 3
    call_indirect (type $sink)
    global.get $last
  )

  (func (export "drive") (param i32) (result i32)
    (local i32)
    block $done
      loop $loop
        local.get 0
        i32.eqz
        br_if $done

        local.get 1
        i32.const 1
        i32.const 2
        i32.const 3
        i32.const 4
        i32.const 5
        i32.const 6
        i32.const 7
        i32.const 8
        i32.const 9
        i32.const 10
        i32.const 11
        i32.const 12
        i32.const 13
        i32.const 14
        i32.const 15
        call $sum16
        local.set 1

        local.get 0
        i32.const 1
        i32.sub
        local.set 0
        br $loop
      end
    end
    local.get 1
  )
)
