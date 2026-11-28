#pragma once

#ifndef MEMORY_HH
#define MEMORY_HH
#include "napi.h"
#include "manager.hh"
#include "skyline/config.h"
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <string>

// 平台特定的头文件
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <semaphore.h>
#endif

namespace SharedMemory {
    // 全局变量来保存共享内存资源
    extern std::map<std::string, std::shared_ptr<SharedMemoryManager>> managerMap;
    extern std::map<std::string, std::shared_ptr<SharedMemoryManager>> copyCache;
#ifdef _WIN32
    extern std::map<uintptr_t, std::shared_ptr<SharedMemoryManager>> invalidatedManagers;
    void invalidate_manager_handle(
        const std::shared_ptr<SharedMemoryManager> &manager);
#endif
    /**
     * 设置共享内存 
     * @param info 回调信息
     * @return 共享内存的视图
     */
    Napi::Value set_memory(const Napi::CallbackInfo &info);

    /**
     * 获取共享内存
     * @param info 回调信息
     * @return 共享内存的视图
     */
    Napi::Value get_memory(const Napi::CallbackInfo &info);

    /**
     * 删除共享内存
     * @param info 回调信息
     * @return 是否成功
     */
    Napi::Value remove_memory(const Napi::CallbackInfo &info);

    // Copy into V8-owned storage; compatible with Electron's memory cage.
    Napi::Value copy_memory_to_buffer(const Napi::CallbackInfo &info);
    Napi::Value delete_copy_cache(const Napi::CallbackInfo &info);

    // SkylineShell receives a process-local pointer and byte length as a
    // little-endian 16-byte Uint8Array.  The pointer is only valid in the
    // process that created the mapping; cross-process access goes through the
    // shared file and getMemory/copyMemoryToBuffer.
    constexpr size_t kMemoryHandleSize = sizeof(std::uint64_t) * 2;

#ifdef _WIN32
    Napi::Value get_memory_by_address(const Napi::CallbackInfo &info);
    Napi::Value set_memory_by_address(const Napi::CallbackInfo &info);
    Napi::Value delete_set_cache(const Napi::CallbackInfo &info);
    Napi::Value delete_get_cache(const Napi::CallbackInfo &info);

    // Decode a handle without depending on host endianness.  Keeping this
    // check in one place prevents arbitrary pointer writes from malformed
    // JavaScript values.
    inline bool decode_memory_handle(const Napi::Value &value,
                                     void **address,
                                     size_t *size) {
        if (!value.IsTypedArray()) {
            return false;
        }

        auto typed = value.As<Napi::TypedArray>();
        if (typed.TypedArrayType() != napi_uint8_array ||
            typed.ByteLength() != kMemoryHandleSize) {
            return false;
        }

        auto bytes = value.As<Napi::Uint8Array>();
        const uint8_t *data = bytes.Data();
        std::uint64_t raw_address = 0;
        std::uint64_t raw_size = 0;
        for (size_t index = 0; index < sizeof(raw_address); ++index) {
            raw_address |= static_cast<std::uint64_t>(data[index])
                           << (index * 8);
            raw_size |= static_cast<std::uint64_t>(data[index + sizeof(raw_address)])
                        << (index * 8);
        }

        if (raw_size > static_cast<std::uint64_t>(
                           std::numeric_limits<size_t>::max())) {
            return false;
        }
        *address = reinterpret_cast<void *>(static_cast<uintptr_t>(raw_address));
        *size = static_cast<size_t>(raw_size);
        return true;
    }
#endif

    // Encode a process-local pointer/size descriptor.  Electron refuses
    // external ArrayBuffers (its V8 memory cage), so both the Windows backend
    // and the Linux backend running inside Electron hand SkylineShell this
    // 16-byte handle instead of a live view over the mapping.
    inline Napi::Uint8Array encode_memory_handle(Napi::Env env,
                                                  void *address,
                                                  size_t size) {
        auto backing = Napi::ArrayBuffer::New(env, kMemoryHandleSize);
        auto handle = Napi::Uint8Array::New(env, kMemoryHandleSize, backing, 0);
        const auto raw_address = static_cast<std::uint64_t>(
            reinterpret_cast<uintptr_t>(address));
        const auto raw_size = static_cast<std::uint64_t>(size);
        for (size_t index = 0; index < sizeof(raw_address); ++index) {
            handle[index] = static_cast<uint8_t>((raw_address >> (index * 8)) & 0xff);
            handle[index + sizeof(raw_address)] =
                static_cast<uint8_t>((raw_size >> (index * 8)) & 0xff);
        }
        return handle;
    }

    // Electron renderers forbid external ArrayBuffers, so the module returns a
    // 16-byte pointer/size handle there instead.  Besides `process.versions`,
    // the user agent identifies Electron ("Electron/..." in stock apps, and the
    // ` electron` token in the WeChat DevTools UA).  The DevTools appservice
    // deletes `process` from the page world after bootstrapping node
    // integration, which used to make this detection fail.
    inline bool user_agent_is_electron(Napi::Env env) {
        auto global = env.Global();
        if (!global.Has("navigator")) return false;
        auto navigator = global.Get("navigator");
        if (!navigator.IsObject()) return false;
        auto user_agent = navigator.As<Napi::Object>().Get("userAgent");
        if (!user_agent.IsString()) return false;
        const std::string text = user_agent.As<Napi::String>().Utf8Value();
        return text.find("Electron/") != std::string::npos ||
               text.find(" electron") != std::string::npos;
    }

    // True when the addon is running inside Electron rather than plain Node.
    // Electron's preload context does not always expose `process` on the
    // isolate global, so the DevTools launch env can force the handle mode.
    inline bool is_electron(Napi::Env env) {
        if (skyline::config::Flag(skyline::config::kSharedMemoryHandle)) {
            return true;
        }
        auto global = env.Global();
        if (global.Has("process")) {
            auto process = global.Get("process");
            if (process.IsObject()) {
                auto object = process.As<Napi::Object>();
                auto versions = object.Get("versions");
                if (versions.IsObject() &&
                    versions.As<Napi::Object>().Has("electron")) {
                    return true;
                }
                // Electron preload scripts also expose process.type without
                // versions.
                if (object.Has("type")) return true;
            }
        }
        return user_agent_is_electron(env);
    }

    struct BinaryView {
        void *data = nullptr;
        size_t size = 0;
    };

    inline bool get_binary_view(const Napi::Value &value, BinaryView *view) {
        if (value.IsBuffer()) {
            auto buffer = value.As<Napi::Buffer<uint8_t>>();
            view->data = buffer.Data();
            view->size = buffer.ByteLength();
            return true;
        }
        if (value.IsArrayBuffer()) {
            auto buffer = value.As<Napi::ArrayBuffer>();
            view->data = buffer.Data();
            view->size = buffer.ByteLength();
            return true;
        }
        if (value.IsTypedArray()) {
            auto typed = value.As<Napi::TypedArray>();
            auto backing = typed.ArrayBuffer();
            view->data = static_cast<uint8_t *>(backing.Data()) + typed.ByteOffset();
            view->size = typed.ByteLength();
            return true;
        }
        return false;
    }
}
#endif
