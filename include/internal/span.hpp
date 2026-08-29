/**
 * @file span.hpp
 * @brief 轻量级非拥有型数组视图（兼容 std::span 接口）
 *
 * 本模块实现了符合 C++20 std::span 行为特性的轻量级数组视图。
 * 支持从指针、数组、std::array、std::vector 等构造，并提供高效的子视图切分接口，适合在内存拷贝最小化场景中使用。
 */

#pragma once

#include <cstddef>
#include <array>
#include <vector>
#include <type_traits>

namespace internal {
template <typename T>
class span {
  public:
    typedef T element_type;
    typedef typename std::remove_cv<T>::type value_type;
    typedef std::size_t size_type;
    typedef std::ptrdiff_t difference_type;
    typedef T* pointer;
    typedef const T* const_pointer;
    typedef T& reference;
    typedef const T& const_reference;
    typedef pointer iterator;
    typedef const_pointer const_iterator;

  private:
    pointer data_;
    size_type size_;

  public:
    // default constructor
    span() noexcept : data_(nullptr), size_(0) {}

    // pointer + size constructor
    span(pointer ptr, size_type count) noexcept : data_(ptr), size_(count) {}

    // from C-style array
    template <std::size_t N>
    span(element_type (&arr)[N]) noexcept : data_(arr), size_(N) {}

    // from std::array
    template <std::size_t N>
    span(std::array<value_type, N>& arr) noexcept : data_(arr.data()), size_(N) {}

    template <std::size_t N>
    span(const std::array<value_type, N>& arr) noexcept : data_(arr.data()), size_(N) {}

    // from std::vector
    template <typename Allocator>
    span(std::vector<value_type, Allocator>& vec) noexcept : data_(vec.data()), size_(vec.size()) {}

    template <typename Allocator>
    span(const std::vector<value_type, Allocator>& vec) noexcept : data_(vec.data()), size_(vec.size()) {}

    // size
    size_type size() const noexcept {
        return size_;
    }

    bool empty() const noexcept {
        return size_ == 0;
    }

    // data pointer
    pointer data() const noexcept {
        return data_;
    }

    // iterator
    iterator begin() const noexcept {
        return data_;
    }

    iterator end() const noexcept {
        return data_ + size_;
    }

    const_iterator cbegin() const noexcept {
        return data_;
    }

    const_iterator cend() const noexcept {
        return data_ + size_;
    }

    // element access
    reference operator[](size_type idx) const {
        return data_[idx];
    }

    reference front() const {
        return data_[0];
    }

    reference back() const {
        return data_[size_ - 1];
    }

    // subviews
    span<element_type> first(size_type count) const {
        return span<element_type>(data_, count);
    }

    span<element_type> last(size_type count) const {
        return span<element_type>(data_ + (size_ - count), count);
    }

    span<element_type> subspan(size_type offset, size_type count = static_cast<size_type>(-1)) const {
        if (count == static_cast<size_type>(-1)) {
            count = size_ - offset;
        }
        return span<element_type>(data_ + offset, count);
    }
};
}; // namespace internal
