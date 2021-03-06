#include "andOTP.hpp"

#include <iostream>

#include <TokenDatabase.hpp>

#include <cereal/external/rapidjson/document.h>
#include <cereal/external/rapidjson/memorystream.h>
#include <cereal/external/rapidjson/stringbuffer.h>
#include <cereal/external/rapidjson/writer.h>

#include <cryptopp/sha.h>
#include <cryptopp/filters.h>
#include <cryptopp/randpool.h>
#include <cryptopp/osrng.h>

#include <cryptopp/aes.h>
#include <cryptopp/gcm.h>

// andOTP token entry schema
// JSON array
//
// see also: https://github.com/andOTP/andOTP/wiki/Special-features
//
// {
//     "secret": "",
//     "label": "",
//     "period": 30,
//     "digits": 6,
//     "type": "TOTP/HOTP/STEAM",
//     "algorithm": "SHA1",
//     "thumbnail": "Default",
//     "last_used": 0,
//     "tags": []
// }

namespace AppSupport {

const uint8_t andOTP::ANDOTP_IV_SIZE = 12U;
const uint8_t andOTP::ANDOTP_TAG_SIZE = 16U;

bool andOTP::importTokens(const std::string &file, std::vector<OTPToken*> &target, const Type &type, const std::string &password)
{
    // read file contents into memory
    std::string out;
    auto status = TokenDatabase::readFile(file, out);
    if (status != TokenDatabase::Success)
    {
        out.clear();
        return false;
    }

    if (out.empty())
    {
        return false;
    }

    // decrypt contents first if they are encrypted
    if (type == Encrypted)
    {
        std::string decrypted;
        auto status = decrypt(password, out, decrypted);
        if (status)
        {
            out = decrypted;
            decrypted.clear();
        }
        else
        {
            out.clear();
            decrypted.clear();
            return false;
        }
    }

    // parse json
    try {
        rapidjson::StringStream s(out.c_str());
        rapidjson::Document json;
        json.ParseStream(s);

        // root element must be an array
        if (!json.IsArray())
        {
            return false;
        }

        auto array = json.GetArray();
        for (auto&& elem : array)
        {
            // check if object has all andOTP members
            if (!(elem.HasMember("type") &&
                elem.HasMember("secret") &&
                elem.HasMember("label")))
            {
                continue;
            }

            try {

            const auto typeStr = std::string(elem["type"].GetString());

            if (typeStr == "TOTP")
            {
                auto token = new OTPToken(OTPToken::TOTP);
                token->setSecret(elem["secret"].GetString());
                token->setLabel(elem["label"].GetString());
                token->setPeriod(elem["period"].GetUint());
                token->setDigitLength(static_cast<OTPToken::DigitType>(elem["digits"].GetUint()));
                token->setAlgorithm(elem["algorithm"].GetString());
                target.push_back(token);
            }
            else if (typeStr == "HOTP")
            {
                auto token = new OTPToken(OTPToken::HOTP);
                token->setSecret(elem["secret"].GetString());
                token->setLabel(elem["label"].GetString());
                token->setCounter(elem["counter"].GetUint());
                token->setDigitLength(static_cast<OTPToken::DigitType>(elem["digits"].GetUint()));
                token->setAlgorithm(elem["algorithm"].GetString());
                target.push_back(token);
            }
            else if (typeStr == "STEAM")
            {
                auto token = new OTPToken(OTPToken::Steam);
                token->setSecret(elem["secret"].GetString());
                token->setLabel(elem["label"].GetString());
                target.push_back(token);
            }
            else
            {
                continue;
            }

            } catch (...) {
                continue;
            }
        }
    } catch (...) {
        // catch all rapidjson exceptions
        return false;
    }

    return true;
}

bool andOTP::exportTokens(const std::string &target, const std::vector<OTPToken*> &tokens, const Type &type, const std::string &password)
{
    try {
        rapidjson::Document json(rapidjson::kArrayType);

        for (auto&& token : tokens)
        {
            rapidjson::Value value(rapidjson::kObjectType);
            value.AddMember("secret", rapidjson::Value(token->secret().c_str(), json.GetAllocator()), json.GetAllocator());
            value.AddMember("label", rapidjson::Value(token->label().c_str(), json.GetAllocator()), json.GetAllocator());
            value.AddMember("period", token->period(), json.GetAllocator());
            value.AddMember("digits", token->digitLength(), json.GetAllocator());

            if (token->type() == OTPToken::HOTP)
            {
                value.AddMember("type", "HOTP", json.GetAllocator());
            }
            else if (token->type() == OTPToken::Steam)
            {
                value.AddMember("type", "STEAM", json.GetAllocator());
            }
            else
            {
                value.AddMember("type", "TOTP", json.GetAllocator());
            }

            if (token->type() == OTPToken::Steam)
            {
                value.AddMember("algorithm", "SHA1", json.GetAllocator());
                value["digits"].SetUint(5);
            }
            else
            {
                value.AddMember("algorithm", rapidjson::Value(token->algorithmName().c_str(), json.GetAllocator()), json.GetAllocator());
            }

            value.AddMember("thumbnail", "Default", json.GetAllocator());
            value.AddMember("last_used", 0, json.GetAllocator());
            value.AddMember("tags", rapidjson::Value(rapidjson::kArrayType), json.GetAllocator());
            json.PushBack(value, json.GetAllocator());
        }

        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        json.Accept(writer);

        if (type == PlainText)
        {
            const auto res = TokenDatabase::writeFile(target, buffer.GetString());
            if (res != TokenDatabase::Success)
            {
                return false;
            }
        }
        else
        {
            std::string encrypted;
            auto res = encrypt(password, buffer.GetString(), encrypted);
            if (!res)
            {
                return false;
            }
            res = TokenDatabase::writeFile(target, encrypted);
            if (res != TokenDatabase::Success)
            {
                return false;
            }
        }
    } catch (...) {
        return false;
    }

    return true;
}

const std::string andOTP::sha256_password(const std::string &password)
{
    if (password.empty())
        return password;

    std::string hashed_password;

    // don't use smart pointers here, already managed/deleted by crypto++ itself
    CryptoPP::SHA256 hash;
    CryptoPP::StringSource src(password, true,
        new CryptoPP::HashFilter(hash,
            new CryptoPP::StringSink(hashed_password)));

    return hashed_password;
}

bool andOTP::decrypt(const std::string &password, const std::string &buffer, std::string &decrypted)
{
    // stream too small
    if (buffer.size() <= (ANDOTP_IV_SIZE + ANDOTP_TAG_SIZE))
    {
        return false;
    }

    try {
        // extract IV and encrypted message from the andOTP database
        const auto iv = buffer.substr(0, ANDOTP_IV_SIZE);
        const auto enc_buf = buffer.substr(ANDOTP_IV_SIZE);

        CryptoPP::GCM<CryptoPP::AES>::Decryption d;
        const auto pwd = sha256_password(password);
        d.SetKeyWithIV(reinterpret_cast<const unsigned char*>(pwd.c_str()), pwd.size(),
                       reinterpret_cast<const unsigned char*>(iv.c_str()), ANDOTP_IV_SIZE);
        CryptoPP::AuthenticatedDecryptionFilter df(d, new CryptoPP::StringSink(decrypted),
                                                   CryptoPP::AuthenticatedDecryptionFilter::MAC_AT_END,
                                                   ANDOTP_TAG_SIZE);
        CryptoPP::StringSource(enc_buf, true, new CryptoPP::Redirector(df));
    } catch (...) {
        decrypted.clear();
        return false;
    }

    return true;
}

bool andOTP::encrypt(const std::string &password, const std::string &buffer, std::string &encrypted)
{
    // stream is empty
    if (buffer.empty())
    {
        return false;
    }

    // generate random IV
    CryptoPP::AutoSeededRandomPool prng;
    CryptoPP::byte *iv = static_cast<CryptoPP::byte*>(std::malloc(ANDOTP_IV_SIZE));
    prng.GenerateBlock(iv, ANDOTP_IV_SIZE);

    try {
        std::string enc_buf;

        CryptoPP::GCM<CryptoPP::AES>::Encryption e;
        const auto pwd = sha256_password(password);
        e.SetKeyWithIV(reinterpret_cast<const unsigned char*>(pwd.c_str()), pwd.size(),
                       iv, ANDOTP_IV_SIZE);
        CryptoPP::StringSource ss(buffer, true,
                                    new CryptoPP::AuthenticatedEncryptionFilter(e,
                                      new CryptoPP::StringSink(enc_buf), false, ANDOTP_TAG_SIZE));

        // andOTP requires the IV to be stored before the message
        encrypted.clear();
        encrypted.append(std::string(iv, iv + ANDOTP_IV_SIZE));
        encrypted.append(enc_buf);
    } catch (...) {
        std::free(iv);
        encrypted.clear();
        return false;
    }

    std::free(iv);
    return true;
}

}
