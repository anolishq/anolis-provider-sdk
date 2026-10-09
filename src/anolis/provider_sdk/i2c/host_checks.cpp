#include "anolis/provider_sdk/i2c/host_checks.hpp"

#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <utility>

#if defined(__linux__)
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace anolis::provider_sdk::i2c {
namespace {

using host_check::Requirement;
using host_check::Status;

constexpr const char* kPresent = "i2c.bus_present";
constexpr const char* kAccess = "i2c.bus_access";
constexpr const char* kClock = "i2c.bus_clock";

Requirement requirement(const char* id, Status status, std::string detail, std::string remedy = {}) {
    return Requirement{id, status, std::move(detail), std::move(remedy)};
}

#if defined(__linux__)
// The i2c-dev adapter name for a node path: "i2c-1" for "/dev/i2c-1"; empty when
// the last path component is not of that form.
std::string adapter_name(const std::string& bus_path) {
    const std::size_t slash = bus_path.find_last_of('/');
    std::string name = slash == std::string::npos ? bus_path : bus_path.substr(slash + 1);
    if (name.size() <= 4 || name.compare(0, 4, "i2c-") != 0) {
        return {};
    }
    for (std::size_t i = 4; i < name.size(); ++i) {
        if (std::isdigit(static_cast<unsigned char>(name[i])) == 0) {
            return {};
        }
    }
    return name;
}

std::string group_name(gid_t gid) {
    std::array<char, 4096> buf{};
    struct group grp {};
    struct group* found = nullptr;
    if (::getgrgid_r(gid, &grp, buf.data(), buf.size(), &found) == 0 && found != nullptr) {
        return found->gr_name;
    }
    return "gid " + std::to_string(gid);
}

std::string user_name(uid_t uid) {
    std::array<char, 4096> buf{};
    struct passwd pwd {};
    struct passwd* found = nullptr;
    if (::getpwuid_r(uid, &pwd, buf.data(), buf.size(), &found) == 0 && found != nullptr) {
        return found->pw_name;
    }
    return "uid " + std::to_string(uid);
}

Requirement check_access(const std::string& bus_path, const struct stat& st) {
    if (::geteuid() == 0) {
        return requirement(kAccess, Status::Unknown,
                           "running as root, which can open " + bus_path +
                               " whatever its permissions; run the check as the user the runtime runs as");
    }
    const std::string user = user_name(::geteuid());
    const int fd = ::open(bus_path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd >= 0) {
        (void)::close(fd);
        return requirement(kAccess, Status::Met, user + " can open " + bus_path + " read-write");
    }
    const int saved = errno;
    if (saved == EACCES || saved == EPERM) {
        char mode[8];
        std::snprintf(mode, sizeof(mode), "%04o", static_cast<unsigned int>(st.st_mode & 07777U));
        const std::string group = group_name(st.st_gid);
        return requirement(kAccess, Status::Unmet,
                           user + " cannot open " + bus_path + " read-write (group '" + group + "', mode " + mode + ")",
                           "add " + user + " to group '" + group +
                               "', the group that owns the node on this host, and start a new login or restart the "
                               "service");
    }
    return requirement(kAccess, Status::Unmet,
                       user + " cannot open " + bus_path + " read-write: " + std::strerror(saved),
                       "check that the I2C adapter behind " + bus_path + " has its driver loaded");
}

Requirement check_clock(const std::string& adapter, uint32_t max_hz, const std::string& sysfs_root) {
    const std::string path = sysfs_root + "/class/i2c-dev/" + adapter + "/device/of_node/clock-frequency";
    std::ifstream file(path, std::ios::binary);
    std::array<unsigned char, 4> raw{};
    if (!file.read(reinterpret_cast<char*>(raw.data()), raw.size())) {
        return requirement(kClock, Status::Unknown,
                           "the platform does not expose " + adapter + "'s bus clock (no " + path + ")");
    }
    // Device-tree cells are big-endian.
    const uint32_t hz = (uint32_t{raw[0]} << 24U) | (uint32_t{raw[1]} << 16U) | (uint32_t{raw[2]} << 8U) | raw[3];
    if (hz > max_hz) {
        return requirement(
            kClock, Status::Unmet,
            adapter + " is configured for " + std::to_string(hz) + " Hz, above this config's maximum of " +
                std::to_string(max_hz) + " Hz",
            "set " + adapter + "'s clock to at most " + std::to_string(max_hz) + " Hz in the platform's configuration");
    }
    return requirement(kClock, Status::Met,
                       adapter + " is configured for " + std::to_string(hz) + " Hz, within this config's maximum of " +
                           std::to_string(max_hz) + " Hz (it may run slower)");
}
#endif

}  // namespace

std::vector<Requirement> check_host(const std::string& bus_path, const HostCheckOptions& options) {
    std::vector<Requirement> out;
#if defined(__linux__)
    struct stat st {};
    if (::stat(bus_path.c_str(), &st) != 0) {
        const int saved = errno;
        if (saved == ENOENT || saved == ENOTDIR) {
            out.push_back(requirement(kPresent, Status::Unmet, bus_path + " does not exist",
                                      "enable this I2C bus on the host and load the i2c-dev module"));
        } else {
            out.push_back(
                requirement(kPresent, Status::Unknown, "cannot inspect " + bus_path + ": " + std::strerror(saved)));
        }
        const std::string skipped = "not checked: " + out.back().detail;
        out.push_back(requirement(kAccess, Status::Unknown, skipped));
        if (options.max_bus_hz) {
            out.push_back(requirement(kClock, Status::Unknown, skipped));
        }
        return out;
    }
    out.push_back(requirement(kPresent, Status::Met, bus_path + " exists"));
    out.push_back(check_access(bus_path, st));
    if (options.max_bus_hz) {
        const std::string adapter = adapter_name(bus_path);
        if (adapter.empty()) {
            out.push_back(
                requirement(kClock, Status::Unknown,
                            "cannot tell which I2C adapter " + bus_path + " is (expected a name like i2c-1)"));
        } else {
            out.push_back(check_clock(adapter, *options.max_bus_hz, options.sysfs_root));
        }
    }
#else
    (void)bus_path;
    const std::string why = "host checks are implemented for Linux only";
    out.push_back(requirement(kPresent, Status::Unknown, why));
    out.push_back(requirement(kAccess, Status::Unknown, why));
    if (options.max_bus_hz) {
        out.push_back(requirement(kClock, Status::Unknown, why));
    }
#endif
    return out;
}

}  // namespace anolis::provider_sdk::i2c
