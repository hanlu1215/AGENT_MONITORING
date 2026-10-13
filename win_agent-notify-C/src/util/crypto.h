// 哈希与编码：全部走系统自带的 CNG（bcrypt.dll），不自己实现算法。
#pragma once

#include <string>
#include <vector>

namespace cn {

std::vector<unsigned char> Sha1(const std::string& data);
std::vector<unsigned char> Sha256Bytes(const std::string& data);
std::vector<unsigned char> HmacSha256(const std::string& key, const std::string& message);

std::string Sha1Hex(const std::string& data);          // 40 位小写
std::string Sha256Hex(const std::string& data);        // 64 位小写
std::string Base64Encode(const std::vector<unsigned char>& data);

int RandomInt(int low, int high);                      // 含两端

}  // namespace cn
