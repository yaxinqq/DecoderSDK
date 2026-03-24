#ifndef DECODER_SDK_ENUM_FLAGS_WRAPPER_H
#define DECODER_SDK_ENUM_FLAGS_WRAPPER_H

#include <cstdint>
#include <type_traits>

namespace decoder_sdk {

// Trait，控制哪些 enum 允许位运算
template <typename Enum>
struct EnableBitMaskOperators {
    static constexpr bool enable = false;
};

template <typename Enum>
using Underlying = std::underlying_type_t<Enum>;

template <typename Enum>
constexpr Underlying<Enum> toUnderlying(Enum e) noexcept
{
    return static_cast<Underlying<Enum>>(e);
}

// 位运算符（仅对启用的 enum 生效）
template <typename Enum>
constexpr std::enable_if_t<EnableBitMaskOperators<Enum>::enable, Enum> operator|(Enum a,
                                                                                 Enum b) noexcept
{
    return static_cast<Enum>(toUnderlying(a) | toUnderlying(b));
}

template <typename Enum>
constexpr std::enable_if_t<EnableBitMaskOperators<Enum>::enable, Enum> operator&(Enum a,
                                                                                 Enum b) noexcept
{
    return static_cast<Enum>(toUnderlying(a) & toUnderlying(b));
}

template <typename Enum>
constexpr std::enable_if_t<EnableBitMaskOperators<Enum>::enable, Enum> operator^(Enum a,
                                                                                 Enum b) noexcept
{
    return static_cast<Enum>(toUnderlying(a) ^ toUnderlying(b));
}

template <typename Enum>
constexpr std::enable_if_t<EnableBitMaskOperators<Enum>::enable, Enum> operator~(Enum a) noexcept
{
    return static_cast<Enum>(~toUnderlying(a));
}

// 复合运算符
template <typename Enum>
constexpr std::enable_if_t<EnableBitMaskOperators<Enum>::enable, Enum &> operator|=(Enum &a,
                                                                                    Enum b) noexcept
{
    a = a | b;
    return a;
}

template <typename Enum>
constexpr std::enable_if_t<EnableBitMaskOperators<Enum>::enable, Enum &> operator&=(Enum &a,
                                                                                    Enum b) noexcept
{
    a = a & b;
    return a;
}

// Flags 包装类
template <typename Enum>
class Flags {
    static_assert(std::is_enum_v<Enum>, "Flags requires enum type");

public:
    using UnderlyingType = Underlying<Enum>;

    // 构造
    constexpr Flags() noexcept = default;
    constexpr Flags(Enum flag) noexcept : value_(toUnderlying(flag))
    {
    }
    constexpr Flags(UnderlyingType value) noexcept : value_(value)
    {
    }

    // 查询
    constexpr bool has(Enum flag) const noexcept
    {
        return (value_ & toUnderlying(flag)) != 0;
    }
    constexpr bool containsAll(Flags other) const noexcept
    {
        return (value_ & other.value_) == other.value_;
    }
    constexpr bool containsAny(Flags other) const noexcept
    {
        return (value_ & other.value_) != 0;
    }
    constexpr bool empty() const noexcept
    {
        return value_ == 0;
    }

    // 修改
    constexpr void set(Enum flag, bool on = true) noexcept
    {
        if (on) {
            value_ |= toUnderlying(flag);
        } else {
            value_ &= ~toUnderlying(flag);
        }
    }
    constexpr void toggle(Enum flag) noexcept
    {
        value_ ^= toUnderlying(flag);
    }
    constexpr void reset() noexcept
    {
        value_ = 0;
    }

    // 访问
    constexpr UnderlyingType value() const noexcept
    {
        return value_;
    }

    static constexpr Flags fromRaw(UnderlyingType v) noexcept
    {
        return Flags(v);
    }

    // 运算符
    constexpr Flags operator|(Enum flag) const noexcept
    {
        return Flags(value_ | toUnderlying(flag));
    }

    constexpr Flags operator&(Enum flag) const noexcept
    {
        return Flags(value_ & toUnderlying(flag));
    }

    constexpr Flags operator|(Flags other) const noexcept
    {
        return Flags(value_ | other.value_);
    }

    constexpr Flags operator&(Flags other) const noexcept
    {
        return Flags(value_ & other.value_);
    }

    constexpr Flags &operator|=(Enum flag) noexcept
    {
        value_ |= toUnderlying(flag);
        return *this;
    }

    constexpr Flags &operator&=(Enum flag) noexcept
    {
        value_ &= toUnderlying(flag);
        return *this;
    }

    constexpr Flags &operator|=(Flags other) noexcept
    {
        value_ |= other.value_;
        return *this;
    }

    constexpr Flags &operator&=(Flags other) noexcept
    {
        value_ &= other.value_;
        return *this;
    }

    // 转换
    explicit constexpr operator UnderlyingType() const noexcept
    {
        return value_;
    }

private:
    UnderlyingType value_ = 0;
};

#define ENABLE_FLAGS(EnumType, FlagsType)                  \
    template <>                                            \
    struct decoder_sdk::EnableBitMaskOperators<EnumType> { \
        static constexpr bool enable = true;               \
    };                                                     \
    using FlagsType = decoder_sdk::Flags<EnumType>;

} // namespace decoder_sdk

#endif // DECODER_SDK_ENUM_FLAGS_WRAPPER_H