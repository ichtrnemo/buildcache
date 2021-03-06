//--------------------------------------------------------------------------------------------------
// Copyright (c) 2019 Marcus Geelnard
//
// This software is provided 'as-is', without any express or implied warranty. In no event will the
// authors be held liable for any damages arising from the use of this software.
//
// Permission is granted to anyone to use this software for any purpose, including commercial
// applications, and to alter it and redistribute it freely, subject to the following restrictions:
//
//  1. The origin of this software must not be misrepresented; you must not claim that you wrote
//     the original software. If you use this software in a product, an acknowledgment in the
//     product documentation would be appreciated but is not required.
//
//  2. Altered source versions must be plainly marked as such, and must not be misrepresented as
//     being the original software.
//
//  3. This notice may not be removed or altered from any source distribution.
//--------------------------------------------------------------------------------------------------

#ifndef BUILDCACHE_HMAC_HPP_
#define BUILDCACHE_HMAC_HPP_

#include <string>

namespace bcache {

/// @brief Generate the SHA1 HMAC hash for a string.
/// @param key The secret key.
/// @param data The data to hash.
/// @returns the digest as a binary string (20 bytes long).
std::string sha1_hmac(const std::string& key, const std::string& data);

}  // namespace bcache

#endif  // BUILDCACHE_HMAC_HPP_
