// FeatureBus: native menu-state bus (polled by hack_thread / ModMenu_Get*).
#pragma once

#include <cstring>
#include <map>
#include <mutex>
#include <string>

class FeatureBus {
public:
    static void SetBool(int feat, bool v) {
        std::lock_guard<std::mutex> l(Mutex());
        Bools()[feat] = v;
    }
    static void SetInt(int feat, int v) {
        std::lock_guard<std::mutex> l(Mutex());
        Ints()[feat] = v;
    }
    static void SetLong(int feat, long long v) {
        std::lock_guard<std::mutex> l(Mutex());
        Longs()[feat] = v;
    }
    static void SetString(int feat, const std::string& v) {
        std::lock_guard<std::mutex> l(Mutex());
        Strings()[feat] = v;
    }
    static void PushButton(int feat) {
        std::lock_guard<std::mutex> l(Mutex());
        Buttons()[feat]++;
    }

    static bool Has(int feat) {
        std::lock_guard<std::mutex> l(Mutex());
        return Bools().count(feat) || Ints().count(feat) ||
               Longs().count(feat) || Strings().count(feat);
    }
    static bool GetBool(int feat, bool def = false) {
        std::lock_guard<std::mutex> l(Mutex());
        auto it = Bools().find(feat);
        return it != Bools().end() ? it->second : def;
    }
    static int GetInt(int feat, int def = 0) {
        std::lock_guard<std::mutex> l(Mutex());
        auto it = Ints().find(feat);
        return it != Ints().end() ? it->second : def;
    }
    static long long GetLong(int feat, long long def = 0) {
        std::lock_guard<std::mutex> l(Mutex());
        auto it = Longs().find(feat);
        return it != Longs().end() ? it->second : def;
    }
    static int GetString(int feat, char* out, int outLen) {
        if (!out || outLen <= 0) return -1;
        std::lock_guard<std::mutex> l(Mutex());
        auto it = Strings().find(feat);
        if (it == Strings().end()) return -1;
        size_t n = it->second.size();
        if (n > (size_t)(outLen - 1)) n = (size_t)(outLen - 1);
        memcpy(out, it->second.data(), n);
        out[n] = '\0';
        return (int)n;
    }
    static int ConsumeButton(int feat) {
        std::lock_guard<std::mutex> l(Mutex());
        auto it = Buttons().find(feat);
        if (it == Buttons().end() || it->second <= 0) return 0;
        int n = it->second;
        it->second = 0;
        return n;
    }

private:
    static std::mutex& Mutex() {
        static std::mutex m;
        return m;
    }
    static std::map<int, bool>& Bools() {
        static std::map<int, bool> m;
        return m;
    }
    static std::map<int, int>& Ints() {
        static std::map<int, int> m;
        return m;
    }
    static std::map<int, long long>& Longs() {
        static std::map<int, long long> m;
        return m;
    }
    static std::map<int, std::string>& Strings() {
        static std::map<int, std::string> m;
        return m;
    }
    static std::map<int, int>& Buttons() {
        static std::map<int, int> m;
        return m;
    }
};
