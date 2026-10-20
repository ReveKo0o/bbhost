#pragma once

#include <cstdint>
#include <cstdio>

namespace bb::libc {

inline const char* error_string(int error) {
    static constexpr const char* messages[] = {
        "No error",
        "Operation not permitted",
        "No such file or directory",
        "No such process",
        "Interrupted system call",
        "Input/output error",
        "Device not configured",
        "Argument list too long",
        "Exec format error",
        "Bad file descriptor",
        "No child processes",
        "Resource deadlock avoided",
        "Cannot allocate memory",
        "Permission denied",
        "Bad address",
        "Block device required",
        "Device busy",
        "File exists",
        "Cross-device link",
        "Operation not supported by device",
        "Not a directory",
        "Is a directory",
        "Invalid argument",
        "Too many open files in system",
        "Too many open files",
        "Inappropriate ioctl for device",
        "Text file busy",
        "File too large",
        "No space left on device",
        "Illegal seek",
        "Read-only filesystem",
        "Too many links",
        "Broken pipe",
        "Numerical argument out of domain",
        "Result too large",
        "Resource temporarily unavailable",
        "Operation now in progress",
        "Operation already in progress",
        "Socket operation on non-socket",
        "Destination address required",
        "Message too long",
        "Protocol wrong type for socket",
        "Protocol not available",
        "Protocol not supported",
        "Socket type not supported",
        "Operation not supported",
        "Protocol family not supported",
        "Address family not supported by protocol family",
        "Address already in use",
        "Can't assign requested address",
        "Network is down",
        "Network is unreachable",
        "Network dropped connection on reset",
        "Software caused connection abort",
        "Connection reset by peer",
        "No buffer space available",
        "Socket is already connected",
        "Socket is not connected",
        "Can't send after socket shutdown",
        "Too many references: can't splice",
        "Operation timed out",
        "Connection refused",
        "Too many levels of symbolic links",
        "File name too long",
        "Host is down",
        "No route to host",
        "Directory not empty",
        "Too many processes",
        "Too many users",
        "Disc quota exceeded",
        "Stale NFS file handle",
        "Too many levels of remote in path",
        "RPC struct is bad",
        "RPC version wrong",
        "RPC prog. not avail",
        "Program version wrong",
        "Bad procedure for program",
        "No locks available",
        "Function not implemented",
        "Inappropriate file type or format",
        "Authentication error",
        "Need authenticator",
        "Identifier removed",
        "No message of desired type",
        "Value too large to be stored in data type",
        "Operation canceled",
        "Illegal byte sequence",
        "Attribute not found",
        "Programming error",
        "Bad message",
        "Multihop attempted",
        "Link has been severed",
        "Protocol error",
        "Capabilities insufficient",
        "Not permitted in capability mode",
    };
    static_assert(sizeof(messages) / sizeof(messages[0]) == 95);
    if (error >= 0 && error < 95) return messages[error];
    switch (error) {
        case 152: return "File positioning error";
        case 1040: return "No message available";
        case 1050: return "No stream resources";
        case 1051: return "Not a stream";
        case 1056: return "State not recoverable";
        case 1062: return "Other";
        case 1064: return "Owner dead";
        case 1074: return "Stream timeout";
    }
    thread_local char unknown[22];
    std::snprintf(unknown, sizeof(unknown), "error_code=0x%08x", static_cast<unsigned>(error));
    return unknown;
}

}  // namespace bb::libc
