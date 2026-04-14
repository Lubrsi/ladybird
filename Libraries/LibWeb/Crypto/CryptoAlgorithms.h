/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/EnumBits.h>
#include <AK/String.h>
#include <LibCrypto/BigInt/UnsignedBigInteger.h>
#include <LibGC/Cell.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/SubtleCrypto.h>
#include <LibWeb/Crypto/CryptoBindings.h>
#include <LibWeb/Crypto/CryptoKey.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::Crypto {

using AlgorithmIdentifier = Variant<GC::Ref<JS::Object>, String>;
using NamedCurve = String;
using KeyDataType = Variant<GC::Root<WebIDL::BufferSource>, JsonWebKey>;

// https://wicg.github.io/webcrypto-modern-algos/#encapsulation
struct EncapsulatedKey {
    Optional<GC::Root<CryptoKey>> shared_key;
    Optional<ByteBuffer> ciphertext;

    JS::ThrowCompletionOr<GC::Ref<JS::Object>> to_object(JS::Realm&);
};

// https://wicg.github.io/webcrypto-modern-algos/#encapsulation
struct EncapsulatedBits {
    Optional<ByteBuffer> shared_key;
    Optional<ByteBuffer> ciphertext;

    JS::ThrowCompletionOr<GC::Ref<JS::Object>> to_object(JS::Realm&) const;
};

struct HashAlgorithmIdentifier : public AlgorithmIdentifier {
    using AlgorithmIdentifier::AlgorithmIdentifier;

    JS::ThrowCompletionOr<String> name(JS::VM& vm) const
    {
        auto value = visit(
            [](String const& name) -> JS::ThrowCompletionOr<String> { return name; },
            [&](GC::Root<JS::Object> const& obj) -> JS::ThrowCompletionOr<String> {
                auto name_property = TRY(obj->get("name"_utf16_fly_string));
                return name_property.to_string(vm);
            });

        return value;
    }
};

// https://w3c.github.io/webcrypto/#algorithm-overview
class AlgorithmParams : public GC::Cell {
    GC_CELL(AlgorithmParams, GC::Cell);
    GC_DECLARE_ALLOCATOR(AlgorithmParams);

public:
    virtual ~AlgorithmParams() override = default;

    // NOTE: this is initialized when normalizing the algorithm name as the spec requests.
    //       It must not be set in `from_value`.
    String name;

    static JS::ThrowCompletionOr<GC::Ref<AlgorithmParams>> from_value(JS::VM&, JS::Value);

protected:
    AlgorithmParams() = default;
};

// https://w3c.github.io/webcrypto/#aes-cbc
struct AesCbcParams final : public AlgorithmParams {
    GC_CELL(AesCbcParams, AlgorithmParams);
    GC_DECLARE_ALLOCATOR(AesCbcParams);

    AesCbcParams(ByteBuffer iv)
        : iv(move(iv))
    {
    }

    ByteBuffer iv;

    static JS::ThrowCompletionOr<GC::Ref<AlgorithmParams>> from_value(JS::VM&, JS::Value);
};

// https://w3c.github.io/webcrypto/#dfn-AesCtrParams
struct AesCtrParams final : public AlgorithmParams {
    GC_CELL(AesCtrParams, AlgorithmParams);
    GC_DECLARE_ALLOCATOR(AesCtrParams);

    AesCtrParams(ByteBuffer counter, u8 length)
        : counter(move(counter))
        , length(length)
    {
    }

    ByteBuffer counter;
    u8 length;

    static JS::ThrowCompletionOr<GC::Ref<AlgorithmParams>> from_value(JS::VM&, JS::Value);
};

// https://w3c.github.io/webcrypto/#dfn-AesGcmParams
struct AesGcmParams final : public AlgorithmParams {
    GC_CELL(AesGcmParams, AlgorithmParams);
    GC_DECLARE_ALLOCATOR(AesGcmParams);

    AesGcmParams(ByteBuffer iv, Optional<ByteBuffer> additional_data, Optional<u8> tag_length)
        : iv(move(iv))
        , additional_data(move(additional_data))
        , tag_length(tag_length)
    {
    }

    ByteBuffer iv;
    Optional<ByteBuffer> additional_data;
    Optional<u8> tag_length;

    static JS::ThrowCompletionOr<GC::Ref<AlgorithmParams>> from_value(JS::VM&, JS::Value);
};

// https://w3c.github.io/webcrypto/#hkdf-params
struct HKDFParams final : public AlgorithmParams {
    GC_CELL(HKDFParams, AlgorithmParams);
    GC_DECLARE_ALLOCATOR(HKDFParams);

    HKDFParams(HashAlgorithmIdentifier hash, ByteBuffer salt, ByteBuffer info)
        : hash(move(hash))
        , salt(move(salt))
        , info(move(info))
    {
    }

    HashAlgorithmIdentifier hash;
    ByteBuffer salt;
    ByteBuffer info;

    static JS::ThrowCompletionOr<GC::Ref<AlgorithmParams>> from_value(JS::VM&, JS::Value);

private:
    virtual void visit_edges(Visitor&) override;
};

// https://w3c.github.io/webcrypto/#pbkdf2-params
struct PBKDF2Params final : public AlgorithmParams {
    GC_CELL(PBKDF2Params, AlgorithmParams);
    GC_DECLARE_ALLOCATOR(PBKDF2Params);

    PBKDF2Params(ByteBuffer salt, u32 iterations, HashAlgorithmIdentifier hash)
        : salt(move(salt))
        , iterations(iterations)
        , hash(move(hash))
    {
    }

    ByteBuffer salt;
    u32 iterations;
    HashAlgorithmIdentifier hash;

    static JS::ThrowCompletionOr<GC::Ref<AlgorithmParams>> from_value(JS::VM&, JS::Value);

private:
    virtual void visit_edges(Visitor&) override;
};

// https://w3c.github.io/webcrypto/#dfn-RsaKeyGenParams
struct RsaKeyGenParams : public AlgorithmParams {
    GC_CELL(RsaKeyGenParams, AlgorithmParams);
    GC_DECLARE_ALLOCATOR(RsaKeyGenParams);

    RsaKeyGenParams(u32 modulus_length, ::Crypto::UnsignedBigInteger public_exponent)
        : modulus_length(modulus_length)
        , public_exponent(move(public_exponent))
    {
    }

    u32 modulus_length;
    // NOTE: The raw data is going to be in Big Endian u8[] format
    ::Crypto::UnsignedBigInteger public_exponent;

    static JS::ThrowCompletionOr<GC::Ref<AlgorithmParams>> from_value(JS::VM&, JS::Value);
};

// https://w3c.github.io/webcrypto/#dfn-RsaHashedKeyGenParams
struct RsaHashedKeyGenParams final : public RsaKeyGenParams {
    GC_CELL(RsaHashedKeyGenParams, RsaKeyGenParams);
    GC_DECLARE_ALLOCATOR(RsaHashedKeyGenParams);

    RsaHashedKeyGenParams(u32 modulus_length, ::Crypto::UnsignedBigInteger public_exponent, HashAlgorithmIdentifier hash)
        : RsaKeyGenParams(modulus_length, move(public_exponent))
        , hash(move(hash))
    {
    }

    HashAlgorithmIdentifier hash;

    static JS::ThrowCompletionOr<GC::Ref<AlgorithmParams>> from_value(JS::VM&, JS::Value);

private:
    virtual void visit_edges(Visitor&) override;
};

// https://w3c.github.io/webcrypto/#dfn-RsaHashedImportParams
struct RsaHashedImportParams final : public AlgorithmParams {
    GC_CELL(RsaHashedImportParams, AlgorithmParams);
    GC_DECLARE_ALLOCATOR(RsaHashedImportParams);

    RsaHashedImportParams(HashAlgorithmIdentifier hash)
        : hash(move(hash))
    {
    }

    HashAlgorithmIdentifier hash;

    static JS::ThrowCompletionOr<GC::Ref<AlgorithmParams>> from_value(JS::VM&, JS::Value);

private:
    virtual void visit_edges(Visitor&) override;
};

// https://w3c.github.io/webcrypto/#dfn-RsaOaepParams
struct RsaOaepParams final : public AlgorithmParams {
    GC_CELL(RsaOaepParams, AlgorithmParams);
    GC_DECLARE_ALLOCATOR(RsaOaepParams);

    RsaOaepParams(ByteBuffer label)
        : label(move(label))
    {
    }

    ByteBuffer label;

    static JS::ThrowCompletionOr<GC::Ref<AlgorithmParams>> from_value(JS::VM&, JS::Value);
};

// https://w3c.github.io/webcrypto/#dfn-RsaPssParams
struct RsaPssParams final : public AlgorithmParams {
    GC_CELL(RsaPssParams, AlgorithmParams);
    GC_DECLARE_ALLOCATOR(RsaPssParams);

    RsaPssParams(WebIDL::UnsignedLong salt_length)
        : salt_length(salt_length)
    {
    }

    WebIDL::UnsignedLong salt_length;

    static JS::ThrowCompletionOr<GC::Ref<AlgorithmParams>> from_value(JS::VM&, JS::Value);
};

// https://w3c.github.io/webcrypto/#dfn-EcdsaParams
struct EcdsaParams final : public AlgorithmParams {
    GC_CELL(EcdsaParams, AlgorithmParams);
    GC_DECLARE_ALLOCATOR(EcdsaParams);

    EcdsaParams(HashAlgorithmIdentifier hash)
        : hash(move(hash))
    {
    }

    HashAlgorithmIdentifier hash;

    static JS::ThrowCompletionOr<GC::Ref<AlgorithmParams>> from_value(JS::VM&, JS::Value);

private:
    virtual void visit_edges(Visitor&) override;
};

// https://w3c.github.io/webcrypto/#dfn-EcKeyGenParams
struct EcKeyGenParams final : public AlgorithmParams {
    GC_CELL(EcKeyGenParams, AlgorithmParams);
    GC_DECLARE_ALLOCATOR(EcKeyGenParams);

    EcKeyGenParams(NamedCurve named_curve)
        : named_curve(move(named_curve))
    {
    }

    NamedCurve named_curve;

    static JS::ThrowCompletionOr<GC::Ref<AlgorithmParams>> from_value(JS::VM&, JS::Value);
};

// https://w3c.github.io/webcrypto/#dfn-AesKeyGenParams
struct AesKeyGenParams final : public AlgorithmParams {
    GC_CELL(AesKeyGenParams, AlgorithmParams);
    GC_DECLARE_ALLOCATOR(AesKeyGenParams);

    AesKeyGenParams(u16 length)
        : length(length)
    {
    }

    u16 length;

    static JS::ThrowCompletionOr<GC::Ref<AlgorithmParams>> from_value(JS::VM&, JS::Value);
};

// https://w3c.github.io/webcrypto/#dfn-AesDerivedKeyParams
struct AesDerivedKeyParams final : public AlgorithmParams {
    GC_CELL(AesDerivedKeyParams, AlgorithmParams);
    GC_DECLARE_ALLOCATOR(AesDerivedKeyParams);

    AesDerivedKeyParams(u16 length)
        : length(length)
    {
    }

    u16 length;

    static JS::ThrowCompletionOr<GC::Ref<AlgorithmParams>> from_value(JS::VM&, JS::Value);
};

// https://w3c.github.io/webcrypto/#hmac-importparams
struct HmacImportParams final : public AlgorithmParams {
    GC_CELL(HmacImportParams, AlgorithmParams);
    GC_DECLARE_ALLOCATOR(HmacImportParams);

    HmacImportParams(HashAlgorithmIdentifier hash, Optional<WebIDL::UnsignedLong> length)
        : hash(move(hash))
        , length(length)
    {
    }

    HashAlgorithmIdentifier hash;
    Optional<WebIDL::UnsignedLong> length;

    static JS::ThrowCompletionOr<GC::Ref<AlgorithmParams>> from_value(JS::VM&, JS::Value);

private:
    virtual void visit_edges(Visitor&) override;
};

// https://w3c.github.io/webcrypto/#hmac-keygen-params
struct HmacKeyGenParams final : public AlgorithmParams {
    GC_CELL(HmacKeyGenParams, AlgorithmParams);
    GC_DECLARE_ALLOCATOR(HmacKeyGenParams);

    HmacKeyGenParams(HashAlgorithmIdentifier hash, Optional<WebIDL::UnsignedLong> length)
        : hash(move(hash))
        , length(length)
    {
    }

    HashAlgorithmIdentifier hash;
    Optional<WebIDL::UnsignedLong> length;

    static JS::ThrowCompletionOr<GC::Ref<AlgorithmParams>> from_value(JS::VM&, JS::Value);

private:
    virtual void visit_edges(Visitor&) override;
};

class AlgorithmMethods : public GC::Cell {
    GC_CELL(AlgorithmMethods, GC::Cell);
    GC_DECLARE_ALLOCATOR(AlgorithmMethods);

public:
    virtual ~AlgorithmMethods() override = default;

    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> encrypt(AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&)
    {
        return WebIDL::NotSupportedError::create(m_realm, "encrypt is not supported"_utf16);
    }

    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> decrypt(AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&)
    {
        return WebIDL::NotSupportedError::create(m_realm, "decrypt is not supported"_utf16);
    }

    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> sign(AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&)
    {
        return WebIDL::NotSupportedError::create(m_realm, "sign is not supported"_utf16);
    }

    virtual WebIDL::ExceptionOr<JS::Value> verify(AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&, ByteBuffer const&)
    {
        return WebIDL::NotSupportedError::create(m_realm, "verify is not supported"_utf16);
    }

    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> digest(AlgorithmParams const&, ByteBuffer const&)
    {
        return WebIDL::NotSupportedError::create(m_realm, "digest is not supported"_utf16);
    }

    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> derive_bits(AlgorithmParams const&, GC::Ref<CryptoKey>, Optional<u32>)
    {
        return WebIDL::NotSupportedError::create(m_realm, "deriveBits is not supported"_utf16);
    }

    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&)
    {
        return WebIDL::NotSupportedError::create(m_realm, "importKey is not supported"_utf16);
    }

    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> generate_key(AlgorithmParams const&, bool, Vector<Bindings::KeyUsage> const&)
    {
        return WebIDL::NotSupportedError::create(m_realm, "generateKey is not supported"_utf16);
    }

    virtual WebIDL::ExceptionOr<GC::Ref<JS::Object>> export_key(Bindings::KeyFormat, GC::Ref<CryptoKey>)
    {
        return WebIDL::NotSupportedError::create(m_realm, "exportKey is not supported"_utf16);
    }

    virtual WebIDL::ExceptionOr<JS::Value> get_key_length(AlgorithmParams const&)
    {
        return WebIDL::NotSupportedError::create(m_realm, "getKeyLength is not supported"_utf16);
    }

    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> wrap_key(AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&)
    {
        return WebIDL::NotSupportedError::create(m_realm, "wrapKey is not supported"_utf16);
    }

    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> unwrap_key(AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&)
    {
        return WebIDL::NotSupportedError::create(m_realm, "unwwrapKey is not supported"_utf16);
    }

    virtual WebIDL::ExceptionOr<EncapsulatedBits> encapsulate(AlgorithmParams const&, GC::Ref<CryptoKey>)
    {
        return WebIDL::NotSupportedError::create(m_realm, "encapsulate is not supported"_utf16);
    }

    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> decapsulate(AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&)
    {
        return WebIDL::NotSupportedError::create(m_realm, "decalpsulate is not supported"_utf16);
    }

    static GC::Ref<AlgorithmMethods> create(JS::Realm& realm) { return realm.heap().allocate<AlgorithmMethods>(realm); }

protected:
    explicit AlgorithmMethods(JS::Realm& realm)
        : m_realm(realm)
    {
    }

    virtual void visit_edges(Visitor&) override;

    GC::Ref<JS::Realm> m_realm;
};

class RSAOAEP final : public AlgorithmMethods {
    GC_CELL(RSAOAEP, AlgorithmMethods);
    GC_DECLARE_ALLOCATOR(RSAOAEP);

public:
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> encrypt(AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> decrypt(AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;

    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> generate_key(AlgorithmParams const&, bool, Vector<Bindings::KeyUsage> const&) override;

    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::Object>> export_key(Bindings::KeyFormat, GC::Ref<CryptoKey>) override;

    static GC::Ref<AlgorithmMethods> create(JS::Realm& realm) { return realm.heap().allocate<RSAOAEP>(realm); }

private:
    explicit RSAOAEP(JS::Realm& realm)
        : AlgorithmMethods(realm)
    {
    }
};

class RSAPSS final : public AlgorithmMethods {
    GC_CELL(RSAPSS, AlgorithmMethods);
    GC_DECLARE_ALLOCATOR(RSAPSS);

public:
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> sign(AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<JS::Value> verify(AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&, ByteBuffer const&) override;

    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> generate_key(AlgorithmParams const&, bool, Vector<Bindings::KeyUsage> const&) override;

    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::Object>> export_key(Bindings::KeyFormat, GC::Ref<CryptoKey>) override;

    static GC::Ref<AlgorithmMethods> create(JS::Realm& realm) { return realm.heap().allocate<RSAPSS>(realm); }

private:
    explicit RSAPSS(JS::Realm& realm)
        : AlgorithmMethods(realm)
    {
    }
};

class RSASSAPKCS1 final : public AlgorithmMethods {
    GC_CELL(RSASSAPKCS1, AlgorithmMethods);
    GC_DECLARE_ALLOCATOR(RSASSAPKCS1);

public:
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> sign(AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<JS::Value> verify(AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&, ByteBuffer const&) override;

    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> generate_key(AlgorithmParams const&, bool, Vector<Bindings::KeyUsage> const&) override;

    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::Object>> export_key(Bindings::KeyFormat, GC::Ref<CryptoKey>) override;

    static GC::Ref<AlgorithmMethods> create(JS::Realm& realm) { return realm.heap().allocate<RSASSAPKCS1>(realm); }

private:
    explicit RSASSAPKCS1(JS::Realm& realm)
        : AlgorithmMethods(realm)
    {
    }
};

class AesCbc final : public AlgorithmMethods {
    GC_CELL(AesCbc, AlgorithmMethods);
    GC_DECLARE_ALLOCATOR(AesCbc);

public:
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> encrypt(AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> decrypt(AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> generate_key(AlgorithmParams const&, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::Object>> export_key(Bindings::KeyFormat, GC::Ref<CryptoKey>) override;
    virtual WebIDL::ExceptionOr<JS::Value> get_key_length(AlgorithmParams const&) override;

    static GC::Ref<AlgorithmMethods> create(JS::Realm& realm) { return realm.heap().allocate<AesCbc>(realm); }

private:
    explicit AesCbc(JS::Realm& realm)
        : AlgorithmMethods(realm)
    {
    }
};

class AesCtr final : public AlgorithmMethods {
    GC_CELL(AesCtr, AlgorithmMethods);
    GC_DECLARE_ALLOCATOR(AesCtr);

public:
    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::Object>> export_key(Bindings::KeyFormat, GC::Ref<CryptoKey>) override;
    virtual WebIDL::ExceptionOr<JS::Value> get_key_length(AlgorithmParams const&) override;
    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> generate_key(AlgorithmParams const&, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> encrypt(AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> decrypt(AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;

    static GC::Ref<AlgorithmMethods> create(JS::Realm& realm) { return realm.heap().allocate<AesCtr>(realm); }

private:
    explicit AesCtr(JS::Realm& realm)
        : AlgorithmMethods(realm)
    {
    }
};

class AesGcm final : public AlgorithmMethods {
    GC_CELL(AesGcm, AlgorithmMethods);
    GC_DECLARE_ALLOCATOR(AesGcm);

public:
    virtual WebIDL::ExceptionOr<JS::Value> get_key_length(AlgorithmParams const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::Object>> export_key(Bindings::KeyFormat, GC::Ref<CryptoKey>) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> encrypt(AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> decrypt(AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> generate_key(AlgorithmParams const&, bool, Vector<Bindings::KeyUsage> const&) override;

    static GC::Ref<AlgorithmMethods> create(JS::Realm& realm) { return realm.heap().allocate<AesGcm>(realm); }

private:
    explicit AesGcm(JS::Realm& realm)
        : AlgorithmMethods(realm)
    {
    }
};

class AesKw final : public AlgorithmMethods {
    GC_CELL(AesKw, AlgorithmMethods);
    GC_DECLARE_ALLOCATOR(AesKw);

public:
    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::Object>> export_key(Bindings::KeyFormat, GC::Ref<CryptoKey>) override;
    virtual WebIDL::ExceptionOr<JS::Value> get_key_length(AlgorithmParams const&) override;
    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> generate_key(AlgorithmParams const&, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> wrap_key(AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> unwrap_key(AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;

    static GC::Ref<AlgorithmMethods> create(JS::Realm& realm) { return realm.heap().allocate<AesKw>(realm); }

private:
    explicit AesKw(JS::Realm& realm)
        : AlgorithmMethods(realm)
    {
    }
};

class HKDF final : public AlgorithmMethods {
    GC_CELL(HKDF, AlgorithmMethods);
    GC_DECLARE_ALLOCATOR(HKDF);

public:
    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> derive_bits(AlgorithmParams const&, GC::Ref<CryptoKey>, Optional<u32>) override;
    virtual WebIDL::ExceptionOr<JS::Value> get_key_length(AlgorithmParams const&) override;

    static GC::Ref<AlgorithmMethods> create(JS::Realm& realm) { return realm.heap().allocate<HKDF>(realm); }

private:
    explicit HKDF(JS::Realm& realm)
        : AlgorithmMethods(realm)
    {
    }
};

class PBKDF2 final : public AlgorithmMethods {
    GC_CELL(PBKDF2, AlgorithmMethods);
    GC_DECLARE_ALLOCATOR(PBKDF2);

public:
    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> derive_bits(AlgorithmParams const&, GC::Ref<CryptoKey>, Optional<u32>) override;
    virtual WebIDL::ExceptionOr<JS::Value> get_key_length(AlgorithmParams const&) override;

    static GC::Ref<AlgorithmMethods> create(JS::Realm& realm) { return realm.heap().allocate<PBKDF2>(realm); }

private:
    explicit PBKDF2(JS::Realm& realm)
        : AlgorithmMethods(realm)
    {
    }
};

class SHA final : public AlgorithmMethods {
    GC_CELL(SHA, AlgorithmMethods);
    GC_DECLARE_ALLOCATOR(SHA);

public:
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> digest(AlgorithmParams const&, ByteBuffer const&) override;

    static GC::Ref<AlgorithmMethods> create(JS::Realm& realm) { return realm.heap().allocate<SHA>(realm); }

private:
    explicit SHA(JS::Realm& realm)
        : AlgorithmMethods(realm)
    {
    }
};

class ECDSA final : public AlgorithmMethods {
    GC_CELL(ECDSA, AlgorithmMethods);
    GC_DECLARE_ALLOCATOR(ECDSA);

public:
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> sign(AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<JS::Value> verify(AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&, ByteBuffer const&) override;

    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> generate_key(AlgorithmParams const&, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::Object>> export_key(Bindings::KeyFormat, GC::Ref<CryptoKey>) override;

    static GC::Ref<AlgorithmMethods> create(JS::Realm& realm) { return realm.heap().allocate<ECDSA>(realm); }

private:
    explicit ECDSA(JS::Realm& realm)
        : AlgorithmMethods(realm)
    {
    }
};

class ECDH final : public AlgorithmMethods {
    GC_CELL(ECDH, AlgorithmMethods);
    GC_DECLARE_ALLOCATOR(ECDH);

public:
    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> generate_key(AlgorithmParams const&, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::Object>> export_key(Bindings::KeyFormat, GC::Ref<CryptoKey>) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> derive_bits(AlgorithmParams const&, GC::Ref<CryptoKey>, Optional<u32>) override;

    static GC::Ref<AlgorithmMethods> create(JS::Realm& realm) { return realm.heap().allocate<ECDH>(realm); }

private:
    explicit ECDH(JS::Realm& realm)
        : AlgorithmMethods(realm)
    {
    }
};

class ED25519 final : public AlgorithmMethods {
    GC_CELL(ED25519, AlgorithmMethods);
    GC_DECLARE_ALLOCATOR(ED25519);

public:
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> sign(AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<JS::Value> verify(AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&, ByteBuffer const&) override;

    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> generate_key(AlgorithmParams const&, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::Object>> export_key(Bindings::KeyFormat, GC::Ref<CryptoKey>) override;

    static GC::Ref<AlgorithmMethods> create(JS::Realm& realm) { return realm.heap().allocate<ED25519>(realm); }

private:
    explicit ED25519(JS::Realm& realm)
        : AlgorithmMethods(realm)
    {
    }
};

class ED448 final : public AlgorithmMethods {
    GC_CELL(ED448, AlgorithmMethods);
    GC_DECLARE_ALLOCATOR(ED448);

public:
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> sign(AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<JS::Value> verify(AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&, ByteBuffer const&) override;

    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> generate_key(AlgorithmParams const&, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::Object>> export_key(Bindings::KeyFormat, GC::Ref<CryptoKey>) override;

    static GC::Ref<AlgorithmMethods> create(JS::Realm& realm) { return realm.heap().allocate<ED448>(realm); }

private:
    explicit ED448(JS::Realm& realm)
        : AlgorithmMethods(realm)
    {
    }
};

class X25519 final : public AlgorithmMethods {
    GC_CELL(X25519, AlgorithmMethods);
    GC_DECLARE_ALLOCATOR(X25519);

public:
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> derive_bits(AlgorithmParams const&, GC::Ref<CryptoKey>, Optional<u32>) override;
    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> generate_key(AlgorithmParams const&, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::Object>> export_key(Bindings::KeyFormat, GC::Ref<CryptoKey>) override;

    static GC::Ref<AlgorithmMethods> create(JS::Realm& realm) { return realm.heap().allocate<X25519>(realm); }

private:
    explicit X25519(JS::Realm& realm)
        : AlgorithmMethods(realm)
    {
    }
};

class X448 final : public AlgorithmMethods {
    GC_CELL(X448, AlgorithmMethods);
    GC_DECLARE_ALLOCATOR(X448);

public:
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> derive_bits(AlgorithmParams const&, GC::Ref<CryptoKey>, Optional<u32>) override;
    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> generate_key(AlgorithmParams const&, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::Object>> export_key(Bindings::KeyFormat, GC::Ref<CryptoKey>) override;

    static GC::Ref<AlgorithmMethods> create(JS::Realm& realm) { return realm.heap().allocate<X448>(realm); }

private:
    explicit X448(JS::Realm& realm)
        : AlgorithmMethods(realm)
    {
    }
};

class HMAC final : public AlgorithmMethods {
    GC_CELL(HMAC, AlgorithmMethods);
    GC_DECLARE_ALLOCATOR(HMAC);

public:
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> sign(AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<JS::Value> verify(AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> generate_key(AlgorithmParams const&, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::Object>> export_key(Bindings::KeyFormat, GC::Ref<CryptoKey>) override;
    virtual WebIDL::ExceptionOr<JS::Value> get_key_length(AlgorithmParams const&) override;

    static GC::Ref<AlgorithmMethods> create(JS::Realm& realm) { return realm.heap().allocate<HMAC>(realm); }

private:
    explicit HMAC(JS::Realm& realm)
        : AlgorithmMethods(realm)
    {
    }
};

class MLDSA final : public AlgorithmMethods {
    GC_CELL(MLDSA, AlgorithmMethods);
    GC_DECLARE_ALLOCATOR(MLDSA);

public:
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> sign(AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<JS::Value> verify(AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> generate_key(AlgorithmParams const&, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::Object>> export_key(Bindings::KeyFormat, GC::Ref<CryptoKey>) override;

    static GC::Ref<AlgorithmMethods> create(JS::Realm& realm) { return realm.heap().allocate<MLDSA>(realm); }

private:
    explicit MLDSA(JS::Realm& realm)
        : AlgorithmMethods(realm)
    {
    }
};

class MLKEM final : public AlgorithmMethods {
    GC_CELL(MLKEM, AlgorithmMethods);
    GC_DECLARE_ALLOCATOR(MLKEM);

public:
    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> generate_key(AlgorithmParams const&, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::Object>> export_key(Bindings::KeyFormat, GC::Ref<CryptoKey>) override;
    virtual WebIDL::ExceptionOr<EncapsulatedBits> encapsulate(AlgorithmParams const&, GC::Ref<CryptoKey>) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> decapsulate(AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;

    static GC::Ref<AlgorithmMethods> create(JS::Realm& realm) { return realm.heap().allocate<MLKEM>(realm); }

private:
    explicit MLKEM(JS::Realm& realm)
        : AlgorithmMethods(realm)
    {
    }
};

class Argon2 final : public AlgorithmMethods {
    GC_CELL(Argon2, AlgorithmMethods);
    GC_DECLARE_ALLOCATOR(Argon2);

public:
    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> derive_bits(AlgorithmParams const&, GC::Ref<CryptoKey>, Optional<u32>) override;
    virtual WebIDL::ExceptionOr<JS::Value> get_key_length(AlgorithmParams const&) override;

    static GC::Ref<AlgorithmMethods> create(JS::Realm& realm) { return realm.heap().allocate<Argon2>(realm); }

private:
    explicit Argon2(JS::Realm& realm)
        : AlgorithmMethods(realm)
    {
    }
};

class CShake final : public AlgorithmMethods {
    GC_CELL(CShake, AlgorithmMethods);
    GC_DECLARE_ALLOCATOR(CShake);

public:
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> digest(AlgorithmParams const&, ByteBuffer const&) override;
    static GC::Ref<AlgorithmMethods> create(JS::Realm& realm) { return realm.heap().allocate<CShake>(realm); }

private:
    explicit CShake(JS::Realm& realm)
        : AlgorithmMethods(realm)
    {
    }
};

struct EcdhKeyDeriveParams final : public AlgorithmParams {
    GC_CELL(EcdhKeyDeriveParams, AlgorithmParams);
    GC_DECLARE_ALLOCATOR(EcdhKeyDeriveParams);

    EcdhKeyDeriveParams(CryptoKey& public_key)
        : public_key(public_key)
    {
    }

    virtual void visit_edges(Visitor&) override;

    GC::Ref<CryptoKey> public_key;

    static JS::ThrowCompletionOr<GC::Ref<AlgorithmParams>> from_value(JS::VM&, JS::Value);
};

struct EcKeyImportParams final : public AlgorithmParams {
    GC_CELL(EcKeyImportParams, AlgorithmParams);
    GC_DECLARE_ALLOCATOR(EcKeyImportParams);

    EcKeyImportParams(String named_curve)
        : named_curve(move(named_curve))
    {
    }

    String named_curve;

    static JS::ThrowCompletionOr<GC::Ref<AlgorithmParams>> from_value(JS::VM&, JS::Value);
};

// https://wicg.github.io/webcrypto-secure-curves/#dfn-Ed448Params
struct Ed448Params final : public AlgorithmParams {
    GC_CELL(Ed448Params, AlgorithmParams);
    GC_DECLARE_ALLOCATOR(Ed448Params);

    Ed448Params(Optional<ByteBuffer>& context)
        : context(context)
    {
    }

    Optional<ByteBuffer> context;

    static JS::ThrowCompletionOr<GC::Ref<AlgorithmParams>> from_value(JS::VM&, JS::Value);
};

// https://wicg.github.io/webcrypto-modern-algos/#dfn-ContextParams
using ContextParams = Ed448Params;

// https://wicg.github.io/webcrypto-modern-algos/#argon2-params
struct Argon2Params final : public AlgorithmParams {
    GC_CELL(Argon2Params, AlgorithmParams);
    GC_DECLARE_ALLOCATOR(Argon2Params);

    Argon2Params(ByteBuffer nonce, u32 parallelism, u32 memory, u32 passes, Optional<u8> version, Optional<ByteBuffer> secret_value, Optional<ByteBuffer> associated_data)
        : nonce(move(nonce))
        , parallelism(parallelism)
        , memory(memory)
        , passes(passes)
        , version(version)
        , secret_value(secret_value)
        , associated_data(associated_data)
    {
    }

    ByteBuffer nonce;
    u32 parallelism;
    u32 memory;
    u32 passes;
    Optional<u8> version;
    Optional<ByteBuffer> secret_value;
    Optional<ByteBuffer> associated_data;

    static JS::ThrowCompletionOr<GC::Ref<AlgorithmParams>> from_value(JS::VM&, JS::Value);
};

// https://wicg.github.io/webcrypto-modern-algos/#cshake-params
struct CShakeParams final : public AlgorithmParams {
    GC_CELL(CShakeParams, AlgorithmParams);
    GC_DECLARE_ALLOCATOR(CShakeParams);

    CShakeParams(u32 output_length, Optional<ByteBuffer> function_name, Optional<ByteBuffer> customization)
        : output_length(output_length)
        , function_name(move(function_name))
        , customization(move(customization))

    {
    }

    u32 output_length;
    Optional<ByteBuffer> function_name;
    Optional<ByteBuffer> customization;

    static JS::ThrowCompletionOr<GC::Ref<AlgorithmParams>> from_value(JS::VM&, JS::Value);
};

// https://wicg.github.io/webcrypto-modern-algos/#kmac-params
struct KmacParams final : public AlgorithmParams {
    GC_CELL(KmacParams, AlgorithmParams);
    GC_DECLARE_ALLOCATOR(KmacParams);

    KmacParams(u32 output_length, Optional<ByteBuffer> customization)
        : output_length(output_length)
        , customization(move(customization))
    {
    }

    u32 output_length;
    Optional<ByteBuffer> customization;

    static JS::ThrowCompletionOr<GC::Ref<AlgorithmParams>> from_value(JS::VM&, JS::Value);
};

// https://wicg.github.io/webcrypto-modern-algos/#kmac-keygen-params
struct KmacKeyGenParams final : public AlgorithmParams {
    GC_CELL(KmacKeyGenParams, AlgorithmParams);
    GC_DECLARE_ALLOCATOR(KmacKeyGenParams);

    KmacKeyGenParams(Optional<WebIDL::UnsignedLong> length)
        : length(length)
    {
    }

    Optional<WebIDL::UnsignedLong> length;

    static JS::ThrowCompletionOr<GC::Ref<AlgorithmParams>> from_value(JS::VM&, JS::Value);
};

// https://wicg.github.io/webcrypto-modern-algos/#kmac-import-params
struct KmacImportParams final : public AlgorithmParams {
    GC_CELL(KmacImportParams, AlgorithmParams);
    GC_DECLARE_ALLOCATOR(KmacImportParams);

    KmacImportParams(Optional<WebIDL::UnsignedLong> length)
        : length(length)
    {
    }

    Optional<WebIDL::UnsignedLong> length;

    static JS::ThrowCompletionOr<GC::Ref<AlgorithmParams>> from_value(JS::VM&, JS::Value);
};

class KMAC final : public AlgorithmMethods {
    GC_CELL(KMAC, AlgorithmMethods);
    GC_DECLARE_ALLOCATOR(KMAC);

public:
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> sign(AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<JS::Value> verify(AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> generate_key(AlgorithmParams const&, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::Object>> export_key(Bindings::KeyFormat, GC::Ref<CryptoKey>) override;
    virtual WebIDL::ExceptionOr<JS::Value> get_key_length(AlgorithmParams const&) override;

    static GC::Ref<AlgorithmMethods> create(JS::Realm& realm) { return realm.heap().allocate<KMAC>(realm); }

private:
    explicit KMAC(JS::Realm& realm)
        : AlgorithmMethods(realm)
    {
    }
};

// https://wicg.github.io/webcrypto-modern-algos/#dfn-AeadParams
// NOTE: The AeadParams dictionary is identical to the AesGcmParams
struct AeadParams final : public AlgorithmParams {
    GC_CELL(AeadParams, AlgorithmParams);
    GC_DECLARE_ALLOCATOR(AeadParams);

    AeadParams(ByteBuffer iv, Optional<ByteBuffer> additional_data, Optional<u8> tag_length)
        : iv(move(iv))
        , additional_data(move(additional_data))
        , tag_length(tag_length)
    {
    }

    ByteBuffer iv;
    Optional<ByteBuffer> additional_data;
    Optional<u8> tag_length;

    static JS::ThrowCompletionOr<GC::Ref<AlgorithmParams>> from_value(JS::VM&, JS::Value);
};

class ChaCha20Poly1305 final : public AlgorithmMethods {
    GC_CELL(ChaCha20Poly1305, AlgorithmMethods);
    GC_DECLARE_ALLOCATOR(ChaCha20Poly1305);

public:
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> encrypt(AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> decrypt(AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> generate_key(AlgorithmParams const&, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::Object>> export_key(Bindings::KeyFormat, GC::Ref<CryptoKey>) override;
    virtual WebIDL::ExceptionOr<JS::Value> get_key_length(AlgorithmParams const&) override;

    static GC::Ref<AlgorithmMethods> create(JS::Realm& realm) { return realm.heap().allocate<ChaCha20Poly1305>(realm); }

private:
    explicit ChaCha20Poly1305(JS::Realm& realm)
        : AlgorithmMethods(realm)
    {
    }
};

class AesOcb final : public AlgorithmMethods {
    GC_CELL(AesOcb, AlgorithmMethods);
    GC_DECLARE_ALLOCATOR(AesOcb);

public:
    virtual WebIDL::ExceptionOr<JS::Value> get_key_length(AlgorithmParams const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::Object>> export_key(Bindings::KeyFormat, GC::Ref<CryptoKey>) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> encrypt(AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> decrypt(AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> generate_key(AlgorithmParams const&, bool, Vector<Bindings::KeyUsage> const&) override;

    static GC::Ref<AlgorithmMethods> create(JS::Realm& realm) { return realm.heap().allocate<AesOcb>(realm); }

private:
    explicit AesOcb(JS::Realm& realm)
        : AlgorithmMethods(realm)
    {
    }
};

ErrorOr<String> base64_url_uint_encode(::Crypto::UnsignedBigInteger);
WebIDL::ExceptionOr<ByteBuffer> base64_url_bytes_decode(JS::Realm&, String const& base64_url_string);
WebIDL::ExceptionOr<::Crypto::UnsignedBigInteger> base64_url_uint_decode(JS::Realm&, String const& base64_url_string);

}
