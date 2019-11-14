/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "fscrypt/fscrypt.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <asm/ioctl.h>
#include <cutils/properties.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <logwrap/logwrap.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utils/misc.h>

#include <array>
#include <string>
#include <vector>

using namespace std::string_literals;

// TODO: switch to <linux/fscrypt.h> once it's in Bionic
#ifndef FSCRYPT_POLICY_V1

// Careful: due to an API quirk this is actually 0, not 1.  We use 1 everywhere
// else, so make sure to only use this constant in the ioctl itself.
#define FSCRYPT_POLICY_V1 0
#define FSCRYPT_KEY_DESCRIPTOR_SIZE 8
struct fscrypt_policy_v1 {
    __u8 version;
    __u8 contents_encryption_mode;
    __u8 filenames_encryption_mode;
    __u8 flags;
    __u8 master_key_descriptor[FSCRYPT_KEY_DESCRIPTOR_SIZE];
};

#define FSCRYPT_POLICY_V2 2
#define FSCRYPT_KEY_IDENTIFIER_SIZE 16
struct fscrypt_policy_v2 {
    __u8 version;
    __u8 contents_encryption_mode;
    __u8 filenames_encryption_mode;
    __u8 flags;
    __u8 __reserved[4];
    __u8 master_key_identifier[FSCRYPT_KEY_IDENTIFIER_SIZE];
};

#endif /* FSCRYPT_POLICY_V1 */

/* modes not supported by upstream kernel, so not in <linux/fs.h> */
#define FS_ENCRYPTION_MODE_AES_256_HEH      126
#define FS_ENCRYPTION_MODE_PRIVATE          127

#define HEX_LOOKUP "0123456789abcdef"

struct ModeLookupEntry {
    std::string name;
    int id;
};

static const auto contents_modes = std::vector<ModeLookupEntry>{
        {"aes-256-xts"s, FS_ENCRYPTION_MODE_AES_256_XTS},
        {"software"s, FS_ENCRYPTION_MODE_AES_256_XTS},
        {"adiantum"s, FS_ENCRYPTION_MODE_ADIANTUM},
        {"ice"s, FS_ENCRYPTION_MODE_PRIVATE},
};

static const auto filenames_modes = std::vector<ModeLookupEntry>{
        {"aes-256-cts"s, FS_ENCRYPTION_MODE_AES_256_CTS},
        {"aes-256-heh"s, FS_ENCRYPTION_MODE_AES_256_HEH},
        {"adiantum"s, FS_ENCRYPTION_MODE_ADIANTUM},
};

static bool LookupModeByName(const std::vector<struct ModeLookupEntry>& modes,
                             const std::string& name, int* result) {
    for (const auto& e : modes) {
        if (e.name == name) {
            *result = e.id;
            return true;
        }
    }
    return false;
}

static bool LookupModeById(const std::vector<struct ModeLookupEntry>& modes, int id,
                           std::string* result) {
    for (const auto& e : modes) {
        if (e.id == id) {
            *result = e.name;
            return true;
        }
    }
    return false;
}

bool fscrypt_is_native() {
    char value[PROPERTY_VALUE_MAX];
    property_get("ro.crypto.type", value, "none");
    return !strcmp(value, "file");
}

namespace android {
namespace fscrypt {

static void log_ls(const char* dirname) {
    std::array<const char*, 3> argv = {"ls", "-laZ", dirname};
    int status = 0;
    auto res =
        logwrap_fork_execvp(argv.size(), argv.data(), &status, false, LOG_ALOG, false, nullptr);
    if (res != 0) {
        PLOG(ERROR) << argv[0] << " " << argv[1] << " " << argv[2] << "failed";
        return;
    }
    if (!WIFEXITED(status)) {
        LOG(ERROR) << argv[0] << " " << argv[1] << " " << argv[2]
                   << " did not exit normally, status: " << status;
        return;
    }
    if (WEXITSTATUS(status) != 0) {
        LOG(ERROR) << argv[0] << " " << argv[1] << " " << argv[2]
                   << " returned failure: " << WEXITSTATUS(status);
        return;
    }
}

void BytesToHex(const std::string& bytes, std::string* hex) {
    hex->clear();
    for (char c : bytes) {
        *hex += HEX_LOOKUP[(c & 0xF0) >> 4];
        *hex += HEX_LOOKUP[c & 0x0F];
    }
}

static bool fscrypt_is_encrypted(int fd) {
    fscrypt_policy_v1 policy;

    // success => encrypted with v1 policy
    // EINVAL => encrypted with v2 policy
    // ENODATA => not encrypted
    return ioctl(fd, FS_IOC_GET_ENCRYPTION_POLICY, &policy) == 0 || errno == EINVAL;
}

bool OptionsToString(const EncryptionOptions& options, std::string* options_string) {
    std::string contents_mode, filenames_mode;
    if (!LookupModeById(contents_modes, options.contents_mode, &contents_mode)) {
        return false;
    }
    if (!LookupModeById(filenames_modes, options.filenames_mode, &filenames_mode)) {
        return false;
    }
    *options_string = contents_mode + ":" + filenames_mode + ":v" + std::to_string(options.version);
    if ((options.flags & FSCRYPT_POLICY_FLAG_IV_INO_LBLK_64)) {
        *options_string += "+inlinecrypt_optimized";
    }
    EncryptionOptions options_check;
    if (!ParseOptions(*options_string, &options_check)) {
        LOG(ERROR) << "Internal error serializing options as string: " << *options_string;
        return false;
    }
    if (memcmp(&options, &options_check, sizeof(options_check)) != 0) {
        LOG(ERROR) << "Internal error serializing options as string, round trip failed: "
                   << *options_string;
        return false;
    }
    return true;
}

bool ParseOptions(const std::string& options_string, EncryptionOptions* options) {
    memset(options, '\0', sizeof(*options));
    auto parts = android::base::Split(options_string, ":");
    if (parts.size() < 1 || parts.size() > 3) {
        return false;
    }
    if (!LookupModeByName(contents_modes, parts[0], &options->contents_mode)) {
        LOG(ERROR) << "Invalid file contents encryption mode: " << parts[0];
        return false;
    }
    if (parts.size() >= 2) {
        if (!LookupModeByName(filenames_modes, parts[1], &options->filenames_mode)) {
            LOG(ERROR) << "Invalid file names encryption mode: " << parts[1];
            return false;
        }
    } else if (options->contents_mode == FS_ENCRYPTION_MODE_ADIANTUM) {
        options->filenames_mode = FS_ENCRYPTION_MODE_ADIANTUM;
    } else {
        options->filenames_mode = FS_ENCRYPTION_MODE_AES_256_CTS;
    }
    options->version = 1;
    options->flags = 0;
    if (parts.size() >= 3) {
        auto flags = android::base::Split(parts[2], "+");
        for (const auto& flag : flags) {
            if (flag == "v1") {
                options->version = 1;
            } else if (flag == "v2") {
                options->version = 2;
            } else if (flag == "inlinecrypt_optimized") {
                options->flags |= FSCRYPT_POLICY_FLAG_IV_INO_LBLK_64;
            } else {
                LOG(ERROR) << "Unknown flag: " << flag;
                return false;
            }
        }
    }

    // In the original setting of v1 policies and AES-256-CTS we used 4-byte
    // padding of filenames, so we have to retain that for compatibility.
    //
    // For everything else, use 16-byte padding.  This is more secure (it helps
    // hide the length of filenames), and it makes the inputs evenly divisible
    // into cipher blocks which is more efficient for encryption and decryption.
    if (options->version == 1 && options->filenames_mode == FS_ENCRYPTION_MODE_AES_256_CTS) {
        options->flags |= FS_POLICY_FLAGS_PAD_4;
    } else {
        options->flags |= FS_POLICY_FLAGS_PAD_16;
    }

    // Use DIRECT_KEY for Adiantum, since it's much more efficient but just as
    // secure since Android doesn't reuse the same master key for multiple
    // encryption modes.
    if (options->filenames_mode == FS_ENCRYPTION_MODE_ADIANTUM) {
        options->flags |= FS_POLICY_FLAG_DIRECT_KEY;
    }
    return true;
}

static std::string PolicyDebugString(const EncryptionPolicy& policy) {
    std::stringstream ss;
    std::string ref_hex;
    BytesToHex(policy.key_raw_ref, &ref_hex);
    ss << ref_hex;
    ss << " v" << policy.options.version;
    ss << " modes " << policy.options.contents_mode << "/" << policy.options.filenames_mode;
    ss << std::hex << " flags 0x" << policy.options.flags;
    return ss.str();
}

bool EnsurePolicy(const EncryptionPolicy& policy, const std::string& directory) {
    union {
        fscrypt_policy_v1 v1;
        fscrypt_policy_v2 v2;
    } kern_policy;
    memset(&kern_policy, 0, sizeof(kern_policy));

    switch (policy.options.version) {
        case 1:
            if (policy.key_raw_ref.size() != FSCRYPT_KEY_DESCRIPTOR_SIZE) {
                LOG(ERROR) << "Invalid key descriptor length for v1 policy: "
                           << policy.key_raw_ref.size();
                return false;
            }
            // Careful: FSCRYPT_POLICY_V1 is actually 0 in the API, so make sure
            // to use it here instead of a literal 1.
            kern_policy.v1.version = FSCRYPT_POLICY_V1;
            kern_policy.v1.contents_encryption_mode = policy.options.contents_mode;
            kern_policy.v1.filenames_encryption_mode = policy.options.filenames_mode;
            kern_policy.v1.flags = policy.options.flags;
            policy.key_raw_ref.copy(reinterpret_cast<char*>(kern_policy.v1.master_key_descriptor),
                                    FSCRYPT_KEY_DESCRIPTOR_SIZE);
            break;
        case 2:
            if (policy.key_raw_ref.size() != FSCRYPT_KEY_IDENTIFIER_SIZE) {
                LOG(ERROR) << "Invalid key identifier length for v2 policy: "
                           << policy.key_raw_ref.size();
                return false;
            }
            kern_policy.v2.version = FSCRYPT_POLICY_V2;
            kern_policy.v2.contents_encryption_mode = policy.options.contents_mode;
            kern_policy.v2.filenames_encryption_mode = policy.options.filenames_mode;
            kern_policy.v2.flags = policy.options.flags;
            policy.key_raw_ref.copy(reinterpret_cast<char*>(kern_policy.v2.master_key_identifier),
                                    FSCRYPT_KEY_IDENTIFIER_SIZE);
            break;
        default:
            LOG(ERROR) << "Invalid encryption policy version: " << policy.options.version;
            return false;
    }

    android::base::unique_fd fd(open(directory.c_str(), O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (fd == -1) {
        PLOG(ERROR) << "Failed to open directory " << directory;
        return false;
    }

    bool already_encrypted = fscrypt_is_encrypted(fd);

    // FS_IOC_SET_ENCRYPTION_POLICY will set the policy if the directory is
    // unencrypted; otherwise it will verify that the existing policy matches.
    // Setting the policy will fail if the directory is already nonempty.
    if (ioctl(fd, FS_IOC_SET_ENCRYPTION_POLICY, &kern_policy) != 0) {
        std::string reason;
        switch (errno) {
            case EEXIST:
                reason = "The directory already has a different encryption policy.";
                break;
            default:
                reason = strerror(errno);
                break;
        }
        LOG(ERROR) << "Failed to set encryption policy of " << directory << " to "
                   << PolicyDebugString(policy) << ": " << reason;
        if (errno == ENOTEMPTY) {
            log_ls(directory.c_str());
        }
        return false;
    }

    if (already_encrypted) {
        LOG(INFO) << "Verified that " << directory << " has the encryption policy "
                  << PolicyDebugString(policy);
    } else {
        LOG(INFO) << "Encryption policy of " << directory << " set to "
                  << PolicyDebugString(policy);
    }
    return true;
}

}  // namespace fscrypt
}  // namespace android
