/*
 * Copyright (C) 2016 The Android Open Source Project
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

#define LOG_TAG "keystore"

#include "key_store_service.h"
#include "include/keystore/KeystoreArg.h"

#include <fcntl.h>
#include <sys/stat.h>

#include <algorithm>
#include <sstream>

#include <binder/IInterface.h>
#include <binder/IPCThreadState.h>
#include <binder/IPermissionController.h>
#include <binder/IServiceManager.h>

#include <private/android_filesystem_config.h>

#include <android/hardware/keymaster/3.0/IHwKeymasterDevice.h>

#include "defaults.h"
#include "keystore_attestation_id.h"
#include "keystore_keymaster_enforcement.h"
#include "keystore_utils.h"
#include <keystore/keystore_hidl_support.h>

#include <hardware/hw_auth_token.h>

namespace keystore {

using namespace android;

namespace {

using ::android::binder::Status;
using ::android::hardware::keymaster::V3_0::KeyFormat;
using android::security::KeystoreArg;
using android::security::keymaster::ExportResult;
using android::security::keymaster::KeymasterArguments;
using android::security::keymaster::KeymasterBlob;
using android::security::keymaster::KeymasterCertificateChain;
using android::security::keymaster::OperationResult;

constexpr size_t kMaxOperations = 15;
constexpr double kIdRotationPeriod = 30 * 24 * 60 * 60; /* Thirty days, in seconds */
const char* kTimestampFilePath = "timestamp";

struct BIGNUM_Delete {
    void operator()(BIGNUM* p) const { BN_free(p); }
};
typedef std::unique_ptr<BIGNUM, BIGNUM_Delete> Unique_BIGNUM;

bool containsTag(const hidl_vec<KeyParameter>& params, Tag tag) {
    return params.end() != std::find_if(params.begin(), params.end(),
                                        [&](auto& param) { return param.tag == tag; });
}

bool isAuthenticationBound(const hidl_vec<KeyParameter>& params) {
    return !containsTag(params, Tag::NO_AUTH_REQUIRED);
}

std::pair<KeyStoreServiceReturnCode, bool> hadFactoryResetSinceIdRotation() {
    struct stat sbuf;
    if (stat(kTimestampFilePath, &sbuf) == 0) {
        double diff_secs = difftime(time(NULL), sbuf.st_ctime);
        return {ResponseCode::NO_ERROR, diff_secs < kIdRotationPeriod};
    }

    if (errno != ENOENT) {
        ALOGE("Failed to stat \"timestamp\" file, with error %d", errno);
        return {ResponseCode::SYSTEM_ERROR, false /* don't care */};
    }

    int fd = creat(kTimestampFilePath, 0600);
    if (fd < 0) {
        ALOGE("Couldn't create \"timestamp\" file, with error %d", errno);
        return {ResponseCode::SYSTEM_ERROR, false /* don't care */};
    }

    if (close(fd)) {
        ALOGE("Couldn't close \"timestamp\" file, with error %d", errno);
        return {ResponseCode::SYSTEM_ERROR, false /* don't care */};
    }

    return {ResponseCode::NO_ERROR, true};
}

constexpr size_t KEY_ATTESTATION_APPLICATION_ID_MAX_SIZE = 1024;

KeyStoreServiceReturnCode updateParamsForAttestation(uid_t callingUid, AuthorizationSet* params) {
    KeyStoreServiceReturnCode responseCode;
    bool factoryResetSinceIdRotation;
    std::tie(responseCode, factoryResetSinceIdRotation) = hadFactoryResetSinceIdRotation();

    if (!responseCode.isOk()) return responseCode;
    if (factoryResetSinceIdRotation) params->push_back(TAG_RESET_SINCE_ID_ROTATION);

    auto asn1_attestation_id_result = security::gather_attestation_application_id(callingUid);
    if (!asn1_attestation_id_result.isOk()) {
        ALOGE("failed to gather attestation_id");
        return ErrorCode::ATTESTATION_APPLICATION_ID_MISSING;
    }
    std::vector<uint8_t>& asn1_attestation_id = asn1_attestation_id_result;

    /*
     * The attestation application ID cannot be longer than
     * KEY_ATTESTATION_APPLICATION_ID_MAX_SIZE, so we truncate if too long.
     */
    if (asn1_attestation_id.size() > KEY_ATTESTATION_APPLICATION_ID_MAX_SIZE) {
        asn1_attestation_id.resize(KEY_ATTESTATION_APPLICATION_ID_MAX_SIZE);
    }

    params->push_back(TAG_ATTESTATION_APPLICATION_ID, asn1_attestation_id);

    return ResponseCode::NO_ERROR;
}

}  // anonymous namespace

void KeyStoreService::binderDied(const wp<IBinder>& who) {
    auto operations = mOperationMap.getOperationsForToken(who.unsafe_get());
    for (const auto& token : operations) {
        int32_t unused_result;
        abort(token, &unused_result);
    }
}

Status KeyStoreService::getState(int32_t userId, int32_t* aidl_return) {
    if (!checkBinderPermission(P_GET_STATE)) {
        *aidl_return = static_cast<int32_t>(ResponseCode::PERMISSION_DENIED);
        return Status::ok();
    }
    *aidl_return = mKeyStore->getState(userId);
    return Status::ok();
}

Status KeyStoreService::get(const String16& name, int32_t uid, ::std::vector<uint8_t>* item) {
    uid_t targetUid = getEffectiveUid(uid);
    if (!checkBinderPermission(P_GET, targetUid)) {
        // see keystore/keystore.h
        return Status::fromServiceSpecificError(
            static_cast<int32_t>(ResponseCode::PERMISSION_DENIED));
    }

    String8 name8(name);
    Blob keyBlob;
    KeyStoreServiceReturnCode rc =
        mKeyStore->getKeyForName(&keyBlob, name8, targetUid, TYPE_GENERIC);
    if (!rc.isOk()) {
        *item = ::std::vector<uint8_t>();
        // Return empty array if key is not found
        // TODO: consider having returned value nullable or parse exception on the client.
        return Status::fromServiceSpecificError(static_cast<int32_t>(rc));
    }
    auto resultBlob = blob2hidlVec(keyBlob);
    // The static_cast here is needed to prevent a move, forcing a deep copy.
    if (item) *item = static_cast<const hidl_vec<uint8_t>&>(blob2hidlVec(keyBlob));
    return Status::ok();
}

Status KeyStoreService::insert(const String16& name, const ::std::vector<uint8_t>& item,
                               int targetUid, int32_t flags, int32_t* aidl_return) {
    targetUid = getEffectiveUid(targetUid);
    KeyStoreServiceReturnCode result =
        checkBinderPermissionAndKeystoreState(P_INSERT, targetUid, flags & KEYSTORE_FLAG_ENCRYPTED);
    if (!result.isOk()) {
        *aidl_return = static_cast<int32_t>(result);
        return Status::ok();
    }

    String8 name8(name);
    String8 filename(mKeyStore->getKeyNameForUidWithDir(name8, targetUid, ::TYPE_GENERIC));

    Blob keyBlob(&item[0], item.size(), NULL, 0, ::TYPE_GENERIC);
    keyBlob.setEncrypted(flags & KEYSTORE_FLAG_ENCRYPTED);

    *aidl_return =
        static_cast<int32_t>(mKeyStore->put(filename.string(), &keyBlob, get_user_id(targetUid)));
    return Status::ok();
}

Status KeyStoreService::del(const String16& name, int targetUid, int32_t* aidl_return) {
    targetUid = getEffectiveUid(targetUid);
    if (!checkBinderPermission(P_DELETE, targetUid)) {
        *aidl_return = static_cast<int32_t>(ResponseCode::PERMISSION_DENIED);
        return Status::ok();
    }
    String8 name8(name);
    ALOGI("del %s %d", name8.string(), targetUid);
    auto filename = mKeyStore->getBlobFileNameIfExists(name8, targetUid, ::TYPE_ANY);
    if (!filename.isOk()) {
        *aidl_return = static_cast<int32_t>(ResponseCode::KEY_NOT_FOUND);
        return Status::ok();
    }

    ResponseCode result =
        mKeyStore->del(filename.value().string(), ::TYPE_ANY, get_user_id(targetUid));
    if (result != ResponseCode::NO_ERROR) {
        *aidl_return = static_cast<int32_t>(result);
        return Status::ok();
    }

    filename = mKeyStore->getBlobFileNameIfExists(name8, targetUid, ::TYPE_KEY_CHARACTERISTICS);
    if (filename.isOk()) {
        *aidl_return = static_cast<int32_t>(mKeyStore->del(
            filename.value().string(), ::TYPE_KEY_CHARACTERISTICS, get_user_id(targetUid)));
        return Status::ok();
    }
    *aidl_return = static_cast<int32_t>(ResponseCode::NO_ERROR);
    return Status::ok();
}

Status KeyStoreService::exist(const String16& name, int targetUid, int32_t* aidl_return) {
    targetUid = getEffectiveUid(targetUid);
    if (!checkBinderPermission(P_EXIST, targetUid)) {
        *aidl_return = static_cast<int32_t>(ResponseCode::PERMISSION_DENIED);
        return Status::ok();
    }

    auto filename = mKeyStore->getBlobFileNameIfExists(String8(name), targetUid, ::TYPE_ANY);
    *aidl_return = static_cast<int32_t>(filename.isOk() ? ResponseCode::NO_ERROR
                                                        : ResponseCode::KEY_NOT_FOUND);
    return Status::ok();
}

Status KeyStoreService::list(const String16& prefix, int targetUid,
                             ::std::vector<::android::String16>* matches) {
    targetUid = getEffectiveUid(targetUid);
    if (!checkBinderPermission(P_LIST, targetUid)) {
        return Status::fromServiceSpecificError(
            static_cast<int32_t>(ResponseCode::PERMISSION_DENIED));
    }
    const String8 prefix8(prefix);
    String8 filename(mKeyStore->getKeyNameForUid(prefix8, targetUid, TYPE_ANY));
    android::Vector<android::String16> matches_internal;
    if (mKeyStore->list(filename, &matches_internal, get_user_id(targetUid)) !=
        ResponseCode::NO_ERROR) {
        return Status::fromServiceSpecificError(static_cast<int32_t>(ResponseCode::SYSTEM_ERROR));
    }
    matches->clear();
    for (size_t i = 0; i < matches_internal.size(); ++i) {
        matches->push_back(matches_internal[i]);
    }
    return Status::ok();
}

Status KeyStoreService::reset(int32_t* aidl_return) {
    if (!checkBinderPermission(P_RESET)) {
        *aidl_return = static_cast<int32_t>(ResponseCode::PERMISSION_DENIED);
        return Status::ok();
    }

    uid_t callingUid = IPCThreadState::self()->getCallingUid();
    mKeyStore->resetUser(get_user_id(callingUid), false);
    *aidl_return = static_cast<int32_t>(ResponseCode::NO_ERROR);
    return Status::ok();
}

Status KeyStoreService::onUserPasswordChanged(int32_t userId, const String16& password,
                                              int32_t* aidl_return) {
    if (!checkBinderPermission(P_PASSWORD)) {
        *aidl_return = static_cast<int32_t>(ResponseCode::PERMISSION_DENIED);
        return Status::ok();
    }

    const String8 password8(password);
    // Flush the auth token table to prevent stale tokens from sticking
    // around.
    mAuthTokenTable.Clear();

    if (password.size() == 0) {
        ALOGI("Secure lockscreen for user %d removed, deleting encrypted entries", userId);
        mKeyStore->resetUser(userId, true);
        *aidl_return = static_cast<int32_t>(ResponseCode::NO_ERROR);
        return Status::ok();
    } else {
        switch (mKeyStore->getState(userId)) {
        case ::STATE_UNINITIALIZED: {
            // generate master key, encrypt with password, write to file,
            // initialize mMasterKey*.
            *aidl_return = static_cast<int32_t>(mKeyStore->initializeUser(password8, userId));
            return Status::ok();
        }
        case ::STATE_NO_ERROR: {
            // rewrite master key with new password.
            *aidl_return = static_cast<int32_t>(mKeyStore->writeMasterKey(password8, userId));
            return Status::ok();
        }
        case ::STATE_LOCKED: {
            ALOGE("Changing user %d's password while locked, clearing old encryption", userId);
            mKeyStore->resetUser(userId, true);
            *aidl_return = static_cast<int32_t>(mKeyStore->initializeUser(password8, userId));
            return Status::ok();
        }
        }
        *aidl_return = static_cast<int32_t>(ResponseCode::SYSTEM_ERROR);
        return Status::ok();
    }
}

Status KeyStoreService::onUserAdded(int32_t userId, int32_t parentId, int32_t* aidl_return) {
    if (!checkBinderPermission(P_USER_CHANGED)) {
        *aidl_return = static_cast<int32_t>(ResponseCode::PERMISSION_DENIED);
        return Status::ok();
    }

    // Sanity check that the new user has an empty keystore.
    if (!mKeyStore->isEmpty(userId)) {
        ALOGW("New user %d's keystore not empty. Clearing old entries.", userId);
    }
    // Unconditionally clear the keystore, just to be safe.
    mKeyStore->resetUser(userId, false);
    if (parentId != -1) {
        // This profile must share the same master key password as the parent profile. Because the
        // password of the parent profile is not known here, the best we can do is copy the parent's
        // master key and master key file. This makes this profile use the same master key as the
        // parent profile, forever.
        *aidl_return = static_cast<int32_t>(mKeyStore->copyMasterKey(parentId, userId));
        return Status::ok();
    } else {
        *aidl_return = static_cast<int32_t>(ResponseCode::NO_ERROR);
        return Status::ok();
    }
}

Status KeyStoreService::onUserRemoved(int32_t userId, int32_t* aidl_return) {
    if (!checkBinderPermission(P_USER_CHANGED)) {
        *aidl_return = static_cast<int32_t>(ResponseCode::PERMISSION_DENIED);
        return Status::ok();
    }

    mKeyStore->resetUser(userId, false);
    *aidl_return = static_cast<int32_t>(ResponseCode::NO_ERROR);
    return Status::ok();
}

Status KeyStoreService::lock(int32_t userId, int32_t* aidl_return) {
    if (!checkBinderPermission(P_LOCK)) {
        *aidl_return = static_cast<int32_t>(ResponseCode::PERMISSION_DENIED);
        return Status::ok();
    }

    State state = mKeyStore->getState(userId);
    if (state != ::STATE_NO_ERROR) {
        ALOGD("calling lock in state: %d", state);
        *aidl_return = static_cast<int32_t>(ResponseCode(state));
        return Status::ok();
    }

    mKeyStore->lock(userId);
    *aidl_return = static_cast<int32_t>(ResponseCode::NO_ERROR);
    return Status::ok();
}

Status KeyStoreService::unlock(int32_t userId, const String16& pw, int32_t* aidl_return) {
    if (!checkBinderPermission(P_UNLOCK)) {
        *aidl_return = static_cast<int32_t>(ResponseCode::PERMISSION_DENIED);
        return Status::ok();
    }

    State state = mKeyStore->getState(userId);
    if (state != ::STATE_LOCKED) {
        switch (state) {
        case ::STATE_NO_ERROR:
            ALOGI("calling unlock when already unlocked, ignoring.");
            break;
        case ::STATE_UNINITIALIZED:
            ALOGE("unlock called on uninitialized keystore.");
            break;
        default:
            ALOGE("unlock called on keystore in unknown state: %d", state);
            break;
        }
        *aidl_return = static_cast<int32_t>(ResponseCode(state));
        return Status::ok();
    }

    const String8 password8(pw);
    // read master key, decrypt with password, initialize mMasterKey*.
    *aidl_return = static_cast<int32_t>(mKeyStore->readMasterKey(password8, userId));
    return Status::ok();
}

Status KeyStoreService::isEmpty(int32_t userId, int32_t* aidl_return) {
    if (!checkBinderPermission(P_IS_EMPTY)) {
        *aidl_return = static_cast<int32_t>(false);
        return Status::ok();
    }

    *aidl_return = static_cast<int32_t>(mKeyStore->isEmpty(userId));
    return Status::ok();
}

Status KeyStoreService::generate(const String16& name, int32_t targetUid, int32_t keyType,
                                 int32_t keySize, int32_t flags,
                                 const ::android::security::KeystoreArguments& keystoreArgs,
                                 int32_t* aidl_return) {
    const Vector<sp<KeystoreArg>>* args = &(keystoreArgs.getArguments());
    targetUid = getEffectiveUid(targetUid);
    KeyStoreServiceReturnCode result =
        checkBinderPermissionAndKeystoreState(P_INSERT, targetUid, flags & KEYSTORE_FLAG_ENCRYPTED);
    if (!result.isOk()) {
        *aidl_return = static_cast<int32_t>(result);
        return Status::ok();
    }

    keystore::AuthorizationSet params;
    add_legacy_key_authorizations(keyType, &params);

    switch (keyType) {
    case EVP_PKEY_EC: {
        params.push_back(TAG_ALGORITHM, Algorithm::EC);
        if (keySize == -1) {
            keySize = EC_DEFAULT_KEY_SIZE;
        } else if (keySize < EC_MIN_KEY_SIZE || keySize > EC_MAX_KEY_SIZE) {
            ALOGI("invalid key size %d", keySize);
            *aidl_return = static_cast<int32_t>(ResponseCode::SYSTEM_ERROR);
            return Status::ok();
        }
        params.push_back(TAG_KEY_SIZE, keySize);
        break;
    }
    case EVP_PKEY_RSA: {
        params.push_back(TAG_ALGORITHM, Algorithm::RSA);
        if (keySize == -1) {
            keySize = RSA_DEFAULT_KEY_SIZE;
        } else if (keySize < RSA_MIN_KEY_SIZE || keySize > RSA_MAX_KEY_SIZE) {
            ALOGI("invalid key size %d", keySize);
            *aidl_return = static_cast<int32_t>(ResponseCode::SYSTEM_ERROR);
            return Status::ok();
        }
        params.push_back(TAG_KEY_SIZE, keySize);
        unsigned long exponent = RSA_DEFAULT_EXPONENT;
        if (args->size() > 1) {
            ALOGI("invalid number of arguments: %zu", args->size());
            *aidl_return = static_cast<int32_t>(ResponseCode::SYSTEM_ERROR);
            return Status::ok();
        } else if (args->size() == 1) {
            const sp<KeystoreArg>& expArg = args->itemAt(0);
            if (expArg != NULL) {
                Unique_BIGNUM pubExpBn(BN_bin2bn(
                    reinterpret_cast<const unsigned char*>(expArg->data()), expArg->size(), NULL));
                if (pubExpBn.get() == NULL) {
                    ALOGI("Could not convert public exponent to BN");
                    *aidl_return = static_cast<int32_t>(ResponseCode::SYSTEM_ERROR);
                    return Status::ok();
                }
                exponent = BN_get_word(pubExpBn.get());
                if (exponent == 0xFFFFFFFFL) {
                    ALOGW("cannot represent public exponent as a long value");
                    *aidl_return = static_cast<int32_t>(ResponseCode::SYSTEM_ERROR);
                    return Status::ok();
                }
            } else {
                ALOGW("public exponent not read");
                *aidl_return = static_cast<int32_t>(ResponseCode::SYSTEM_ERROR);
                return Status::ok();
            }
        }
        params.push_back(TAG_RSA_PUBLIC_EXPONENT, exponent);
        break;
    }
    default: {
        ALOGW("Unsupported key type %d", keyType);
        *aidl_return = static_cast<int32_t>(ResponseCode::SYSTEM_ERROR);
        return Status::ok();
    }
    }

    int32_t aidl_result;
    android::security::keymaster::KeyCharacteristics unused_characteristics;
    auto rc = generateKey(name, KeymasterArguments(params.hidl_data()), ::std::vector<uint8_t>(),
                          targetUid, flags, &unused_characteristics, &aidl_result);
    if (!KeyStoreServiceReturnCode(aidl_result).isOk()) {
        ALOGW("generate failed: %d", int32_t(aidl_result));
    }
    *aidl_return = aidl_result;
    return Status::ok();
}

Status KeyStoreService::import_key(const String16& name, const ::std::vector<uint8_t>& data,
                                   int targetUid, int32_t flags, int32_t* aidl_return) {

    const uint8_t* ptr = &data[0];

    Unique_PKCS8_PRIV_KEY_INFO pkcs8(d2i_PKCS8_PRIV_KEY_INFO(NULL, &ptr, data.size()));
    if (!pkcs8.get()) {
        *aidl_return = static_cast<int32_t>(ResponseCode::SYSTEM_ERROR);
        return Status::ok();
    }
    Unique_EVP_PKEY pkey(EVP_PKCS82PKEY(pkcs8.get()));
    if (!pkey.get()) {
        *aidl_return = static_cast<int32_t>(ResponseCode::SYSTEM_ERROR);
        return Status::ok();
    }
    int type = EVP_PKEY_type(pkey->type);
    AuthorizationSet params;
    add_legacy_key_authorizations(type, &params);
    switch (type) {
    case EVP_PKEY_RSA:
        params.push_back(TAG_ALGORITHM, Algorithm::RSA);
        break;
    case EVP_PKEY_EC:
        params.push_back(TAG_ALGORITHM, Algorithm::EC);
        break;
    default:
        ALOGW("Unsupported key type %d", type);
        *aidl_return = static_cast<int32_t>(ResponseCode::SYSTEM_ERROR);
        return Status::ok();
    }

    int import_result;
    auto rc = importKey(name, KeymasterArguments(params.hidl_data()),
                        static_cast<int32_t>(KeyFormat::PKCS8), data, targetUid, flags,
                        /*outCharacteristics*/ NULL, &import_result);

    if (!KeyStoreServiceReturnCode(import_result).isOk()) {
        ALOGW("importKey failed: %d", int32_t(import_result));
    }
    *aidl_return = static_cast<int32_t>(ResponseCode::NO_ERROR);
    return Status::ok();
}

Status KeyStoreService::sign(const String16& name, const ::std::vector<uint8_t>& data,
                             ::std::vector<uint8_t>* out) {
    if (!checkBinderPermission(P_SIGN)) {
        return Status::fromServiceSpecificError(
            static_cast<int32_t>(ResponseCode::PERMISSION_DENIED));
    }
    hidl_vec<uint8_t> legacy_out;
    KeyStoreServiceReturnCode res =
        doLegacySignVerify(name, data, &legacy_out, hidl_vec<uint8_t>(), KeyPurpose::SIGN);
    *out = legacy_out;
    return Status::fromServiceSpecificError((res));
}

Status KeyStoreService::verify(const String16& name, const ::std::vector<uint8_t>& data,
                               const ::std::vector<uint8_t>& signature, int32_t* aidl_return) {
    if (!checkBinderPermission(P_VERIFY)) {
        return Status::fromServiceSpecificError(
            static_cast<int32_t>(ResponseCode::PERMISSION_DENIED));
    }
    *aidl_return = static_cast<int32_t>(
        doLegacySignVerify(name, data, nullptr, signature, KeyPurpose::VERIFY));
    return Status::ok();
}

/*
 * TODO: The abstraction between things stored in hardware and regular blobs
 * of data stored on the filesystem should be moved down to keystore itself.
 * Unfortunately the Java code that calls this has naming conventions that it
 * knows about. Ideally keystore shouldn't be used to store random blobs of
 * data.
 *
 * Until that happens, it's necessary to have a separate "get_pubkey" and
 * "del_key" since the Java code doesn't really communicate what it's
 * intentions are.
 */
Status KeyStoreService::get_pubkey(const String16& name, ::std::vector<uint8_t>* pubKey) {
    android::security::keymaster::ExportResult result;
    KeymasterBlob clientId;
    KeymasterBlob appId;
    exportKey(name, static_cast<int32_t>(KeyFormat::X509), clientId, appId, UID_SELF, &result);
    if (!result.resultCode.isOk()) {
        ALOGW("export failed: %d", int32_t(result.resultCode));
        return Status::fromServiceSpecificError(static_cast<int32_t>(result.resultCode));
    }

    if (pubKey) *pubKey = std::move(result.exportData);
    return Status::ok();
}

Status KeyStoreService::grant(const String16& name, int32_t granteeUid,
                              ::android::String16* aidl_return) {
    uid_t callingUid = IPCThreadState::self()->getCallingUid();
    auto result = checkBinderPermissionAndKeystoreState(P_GRANT);
    if (!result.isOk()) {
        *aidl_return = String16();
        return Status::ok();
    }

    String8 name8(name);
    String8 filename(mKeyStore->getKeyNameForUidWithDir(name8, callingUid, ::TYPE_ANY));

    if (access(filename.string(), R_OK) == -1) {
        *aidl_return = String16();
        return Status::ok();
    }

    *aidl_return =
        String16(mKeyStore->addGrant(String8(name).string(), callingUid, granteeUid).c_str());
    return Status::ok();
}

Status KeyStoreService::ungrant(const String16& name, int32_t granteeUid, int32_t* aidl_return) {
    uid_t callingUid = IPCThreadState::self()->getCallingUid();
    KeyStoreServiceReturnCode result = checkBinderPermissionAndKeystoreState(P_GRANT);
    if (!result.isOk()) {
        *aidl_return = static_cast<int32_t>(result);
        return Status::ok();
    }

    String8 name8(name);
    String8 filename(mKeyStore->getKeyNameForUidWithDir(name8, callingUid, ::TYPE_ANY));

    if (access(filename.string(), R_OK) == -1) {
        *aidl_return = static_cast<int32_t>((errno != ENOENT) ? ResponseCode::SYSTEM_ERROR
                                                              : ResponseCode::KEY_NOT_FOUND);
        return Status::ok();
    }

    *aidl_return = static_cast<int32_t>(mKeyStore->removeGrant(name8, callingUid, granteeUid)
                                            ? ResponseCode::NO_ERROR
                                            : ResponseCode::KEY_NOT_FOUND);
    return Status::ok();
}

Status KeyStoreService::getmtime(const String16& name, int32_t uid, int64_t* time) {
    uid_t targetUid = getEffectiveUid(uid);
    if (!checkBinderPermission(P_GET, targetUid)) {
        ALOGW("permission denied for %d: getmtime", targetUid);
        *time = -1L;
        return Status::ok();
    }

    auto filename = mKeyStore->getBlobFileNameIfExists(String8(name), targetUid, ::TYPE_ANY);

    if (!filename.isOk()) {
        ALOGW("could not access %s for getmtime", filename.value().string());
        *time = -1L;
        return Status::ok();
    }

    int fd = TEMP_FAILURE_RETRY(open(filename.value().string(), O_NOFOLLOW, O_RDONLY));
    if (fd < 0) {
        ALOGW("could not open %s for getmtime", filename.value().string());
        *time = -1L;
        return Status::ok();
    }

    struct stat s;
    int ret = fstat(fd, &s);
    close(fd);
    if (ret == -1) {
        ALOGW("could not stat %s for getmtime", filename.value().string());
        *time = -1L;
        return Status::ok();
    }

    *time = static_cast<int64_t>(s.st_mtime);
    return Status::ok();
}

// TODO(tuckeris): This is dead code, remove it.  Don't bother copying over key characteristics here
Status KeyStoreService::duplicate(const String16& srcKey, int32_t srcUid, const String16& destKey,
                                  int32_t destUid, int32_t* aidl_return) {
    uid_t callingUid = IPCThreadState::self()->getCallingUid();
    pid_t spid = IPCThreadState::self()->getCallingPid();
    if (!has_permission(callingUid, P_DUPLICATE, spid)) {
        ALOGW("permission denied for %d: duplicate", callingUid);
        *aidl_return = static_cast<int32_t>(ResponseCode::PERMISSION_DENIED);
        return Status::ok();
    }

    State state = mKeyStore->getState(get_user_id(callingUid));
    if (!isKeystoreUnlocked(state)) {
        ALOGD("calling duplicate in state: %d", state);
        *aidl_return = static_cast<int32_t>(ResponseCode(state));
        return Status::ok();
    }

    if (srcUid == -1 || static_cast<uid_t>(srcUid) == callingUid) {
        srcUid = callingUid;
    } else if (!is_granted_to(callingUid, srcUid)) {
        ALOGD("migrate not granted from source: %d -> %d", callingUid, srcUid);
        *aidl_return = static_cast<int32_t>(ResponseCode::PERMISSION_DENIED);
        return Status::ok();
    }

    if (destUid == -1) {
        destUid = callingUid;
    }

    if (srcUid != destUid) {
        if (static_cast<uid_t>(srcUid) != callingUid) {
            ALOGD("can only duplicate from caller to other or to same uid: "
                  "calling=%d, srcUid=%d, destUid=%d",
                  callingUid, srcUid, destUid);
            *aidl_return = static_cast<int32_t>(ResponseCode::PERMISSION_DENIED);
            return Status::ok();
        }

        if (!is_granted_to(callingUid, destUid)) {
            ALOGD("duplicate not granted to dest: %d -> %d", callingUid, destUid);
            *aidl_return = static_cast<int32_t>(ResponseCode::PERMISSION_DENIED);
            return Status::ok();
        }
    }

    String8 source8(srcKey);
    String8 sourceFile(mKeyStore->getKeyNameForUidWithDir(source8, srcUid, ::TYPE_ANY));

    String8 target8(destKey);
    String8 targetFile(mKeyStore->getKeyNameForUidWithDir(target8, destUid, ::TYPE_ANY));

    if (access(targetFile.string(), W_OK) != -1 || errno != ENOENT) {
        ALOGD("destination already exists: %s", targetFile.string());
        *aidl_return = static_cast<int32_t>(ResponseCode::SYSTEM_ERROR);
        return Status::ok();
    }

    Blob keyBlob;
    ResponseCode responseCode =
        mKeyStore->get(sourceFile.string(), &keyBlob, TYPE_ANY, get_user_id(srcUid));
    if (responseCode != ResponseCode::NO_ERROR) {
        *aidl_return = static_cast<int32_t>(responseCode);
        return Status::ok();
    }

    *aidl_return =
        static_cast<int32_t>(mKeyStore->put(targetFile.string(), &keyBlob, get_user_id(destUid)));
    return Status::ok();
}

Status KeyStoreService::is_hardware_backed(const String16& keyType, int32_t* aidl_return) {
    *aidl_return = static_cast<int32_t>(mKeyStore->isHardwareBacked(keyType) ? 1 : 0);
    return Status::ok();
}

Status KeyStoreService::clear_uid(int64_t targetUid64, int32_t* aidl_return) {
    uid_t targetUid = getEffectiveUid(targetUid64);
    if (!checkBinderPermissionSelfOrSystem(P_CLEAR_UID, targetUid)) {
        *aidl_return = static_cast<int32_t>(ResponseCode::PERMISSION_DENIED);
        return Status::ok();
    }
    ALOGI("clear_uid %" PRId64, targetUid64);

    mKeyStore->removeAllGrantsToUid(targetUid);

    String8 prefix = String8::format("%u_", targetUid);
    Vector<String16> aliases;
    if (mKeyStore->list(prefix, &aliases, get_user_id(targetUid)) != ResponseCode::NO_ERROR) {
        *aidl_return = static_cast<int32_t>(ResponseCode::SYSTEM_ERROR);
        return Status::ok();
    }

    for (uint32_t i = 0; i < aliases.size(); i++) {
        String8 name8(aliases[i]);
        String8 filename(mKeyStore->getKeyNameForUidWithDir(name8, targetUid, ::TYPE_ANY));

        if (get_app_id(targetUid) == AID_SYSTEM) {
            Blob keyBlob;
            ResponseCode responseCode =
                mKeyStore->get(filename.string(), &keyBlob, ::TYPE_ANY, get_user_id(targetUid));
            if (responseCode == ResponseCode::NO_ERROR && keyBlob.isCriticalToDeviceEncryption()) {
                // Do not clear keys critical to device encryption under system uid.
                continue;
            }
        }

        mKeyStore->del(filename.string(), ::TYPE_ANY, get_user_id(targetUid));

        // del() will fail silently if no cached characteristics are present for this alias.
        String8 chr_filename(
            mKeyStore->getKeyNameForUidWithDir(name8, targetUid, ::TYPE_KEY_CHARACTERISTICS));
        mKeyStore->del(chr_filename.string(), ::TYPE_KEY_CHARACTERISTICS, get_user_id(targetUid));
    }
    *aidl_return = static_cast<int32_t>(ResponseCode::NO_ERROR);
    return Status::ok();
}

Status KeyStoreService::addRngEntropy(const ::std::vector<uint8_t>& entropy, int32_t* aidl_return) {
    const auto& device = mKeyStore->getDevice();
    *aidl_return = static_cast<int32_t>(
        KeyStoreServiceReturnCode(KS_HANDLE_HIDL_ERROR(device->addRngEntropy(entropy))));
    return Status::ok();
}

Status
KeyStoreService::generateKey(const String16& name, const KeymasterArguments& params,
                             const ::std::vector<uint8_t>& entropy, int uid, int flags,
                             android::security::keymaster::KeyCharacteristics* outCharacteristics,
                             int32_t* aidl_return) {
    uid = getEffectiveUid(uid);
    KeyStoreServiceReturnCode rc =
        checkBinderPermissionAndKeystoreState(P_INSERT, uid, flags & KEYSTORE_FLAG_ENCRYPTED);
    if (!rc.isOk()) {
        *aidl_return = static_cast<int32_t>(rc);
        return Status::ok();
    }
    if ((flags & KEYSTORE_FLAG_CRITICAL_TO_DEVICE_ENCRYPTION) && get_app_id(uid) != AID_SYSTEM) {
        ALOGE("Non-system uid %d cannot set FLAG_CRITICAL_TO_DEVICE_ENCRYPTION", uid);
        *aidl_return = static_cast<int32_t>(ResponseCode::PERMISSION_DENIED);
        return Status::ok();
    }

    if (containsTag(params.getParameters(), Tag::INCLUDE_UNIQUE_ID)) {
        if (!checkBinderPermission(P_GEN_UNIQUE_ID)) {
            *aidl_return = static_cast<int32_t>(ResponseCode::PERMISSION_DENIED);
            return Status::ok();
        }
    }

    bool usingFallback = false;
    auto& dev = mKeyStore->getDevice();
    AuthorizationSet keyCharacteristics = params.getParameters();

    // TODO: Seed from Linux RNG before this.
    int32_t result;
    addRngEntropy(entropy, &result);  // binder error is not possible.
    if (!KeyStoreServiceReturnCode(result).isOk()) {
        *aidl_return = static_cast<int32_t>(result);
        return Status::ok();
    }

    KeyStoreServiceReturnCode error;
    auto hidl_cb = [&](ErrorCode ret, const ::std::vector<uint8_t>& hidlKeyBlob,
                       const KeyCharacteristics& keyCharacteristics) {
        error = ret;
        if (!error.isOk()) {
            return;
        }
        if (outCharacteristics)
            *outCharacteristics =
                ::android::security::keymaster::KeyCharacteristics(keyCharacteristics);

        // Write the key
        String8 name8(name);
        String8 filename(mKeyStore->getKeyNameForUidWithDir(name8, uid, ::TYPE_KEYMASTER_10));

        Blob keyBlob(&hidlKeyBlob[0], hidlKeyBlob.size(), NULL, 0, ::TYPE_KEYMASTER_10);
        keyBlob.setFallback(usingFallback);
        keyBlob.setCriticalToDeviceEncryption(flags & KEYSTORE_FLAG_CRITICAL_TO_DEVICE_ENCRYPTION);
        if (isAuthenticationBound(params.getParameters()) &&
            !keyBlob.isCriticalToDeviceEncryption()) {
            keyBlob.setSuperEncrypted(true);
        }
        keyBlob.setEncrypted(flags & KEYSTORE_FLAG_ENCRYPTED);

        error = mKeyStore->put(filename.string(), &keyBlob, get_user_id(uid));
    };

    rc = KS_HANDLE_HIDL_ERROR(dev->generateKey(params.getParameters(), hidl_cb));
    if (!rc.isOk()) {
        *aidl_return = static_cast<int32_t>(rc);
        return Status::ok();
    }
    if (!error.isOk()) {
        ALOGE("Failed to generate key -> falling back to software keymaster");
        usingFallback = true;
        auto fallback = mKeyStore->getFallbackDevice();
        if (!fallback.isOk()) {
            *aidl_return = static_cast<int32_t>(error);
            return Status::ok();
        }
        rc = KS_HANDLE_HIDL_ERROR(fallback.value()->generateKey(params.getParameters(), hidl_cb));
        if (!rc.isOk()) {
            *aidl_return = static_cast<int32_t>(rc);
            return Status::ok();
        }
        if (!error.isOk()) {
            *aidl_return = static_cast<int32_t>(error);
            return Status::ok();
        }
    }

    // Write the characteristics:
    String8 name8(name);
    String8 cFilename(mKeyStore->getKeyNameForUidWithDir(name8, uid, ::TYPE_KEY_CHARACTERISTICS));

    std::stringstream kc_stream;
    keyCharacteristics.Serialize(&kc_stream);
    if (kc_stream.bad()) {
        *aidl_return = static_cast<int32_t>(ResponseCode::SYSTEM_ERROR);
        return Status::ok();
    }
    auto kc_buf = kc_stream.str();
    Blob charBlob(reinterpret_cast<const uint8_t*>(kc_buf.data()), kc_buf.size(), NULL, 0,
                  ::TYPE_KEY_CHARACTERISTICS);
    charBlob.setFallback(usingFallback);
    charBlob.setEncrypted(flags & KEYSTORE_FLAG_ENCRYPTED);

    *aidl_return =
        static_cast<int32_t>(mKeyStore->put(cFilename.string(), &charBlob, get_user_id(uid)));
    return Status::ok();
}

Status KeyStoreService::getKeyCharacteristics(
    const String16& name, const ::android::security::keymaster::KeymasterBlob& clientId,
    const ::android::security::keymaster::KeymasterBlob& appId, int32_t uid,
    ::android::security::keymaster::KeyCharacteristics* outCharacteristics, int32_t* aidl_return) {
    if (!outCharacteristics) {
        *aidl_return =
            static_cast<int32_t>(KeyStoreServiceReturnCode(ErrorCode::UNEXPECTED_NULL_POINTER));
        return Status::ok();
    }

    uid_t targetUid = getEffectiveUid(uid);
    uid_t callingUid = IPCThreadState::self()->getCallingUid();
    if (!is_granted_to(callingUid, targetUid)) {
        ALOGW("uid %d not permitted to act for uid %d in getKeyCharacteristics", callingUid,
              targetUid);
        *aidl_return = static_cast<int32_t>(ResponseCode::PERMISSION_DENIED);
        return Status::ok();
    }

    Blob keyBlob;
    String8 name8(name);

    KeyStoreServiceReturnCode rc =
        mKeyStore->getKeyForName(&keyBlob, name8, targetUid, TYPE_KEYMASTER_10);
    if (rc == ResponseCode::UNINITIALIZED) {
        /*
         * If we fail reading the blob because the master key is missing we try to retrieve the
         * key characteristics from the characteristics file. This happens when auth-bound
         * keys are used after a screen lock has been removed by the user.
         */
        rc = mKeyStore->getKeyForName(&keyBlob, name8, targetUid, TYPE_KEY_CHARACTERISTICS);
        if (!rc.isOk()) {
            *aidl_return = static_cast<int32_t>(rc);
            return Status::ok();
        }
        AuthorizationSet keyCharacteristics;
        // TODO write one shot stream buffer to avoid copying (twice here)
        std::string charBuffer(reinterpret_cast<const char*>(keyBlob.getValue()),
                               keyBlob.getLength());
        std::stringstream charStream(charBuffer);
        keyCharacteristics.Deserialize(&charStream);

        outCharacteristics->softwareEnforced = KeymasterArguments(keyCharacteristics.hidl_data());
        *aidl_return = static_cast<int32_t>(rc);
        return Status::ok();
    } else if (!rc.isOk()) {
        *aidl_return = static_cast<int32_t>(rc);
        return Status::ok();
    }

    auto hidlKeyBlob = blob2hidlVec(keyBlob);
    auto& dev = mKeyStore->getDevice(keyBlob);

    KeyStoreServiceReturnCode error;

    auto hidlCb = [&](ErrorCode ret, const KeyCharacteristics& keyCharacteristics) {
        error = ret;
        if (!error.isOk()) {
            return;
        }
        *outCharacteristics =
            ::android::security::keymaster::KeyCharacteristics(keyCharacteristics);
    };

    rc = KS_HANDLE_HIDL_ERROR(
        dev->getKeyCharacteristics(hidlKeyBlob, clientId.getData(), appId.getData(), hidlCb));
    if (!rc.isOk()) {
        *aidl_return = static_cast<int32_t>(rc);
        return Status::ok();
    }

    if (error == ErrorCode::KEY_REQUIRES_UPGRADE) {
        AuthorizationSet upgradeParams;
        if (clientId.getData().size()) {
            upgradeParams.push_back(TAG_APPLICATION_ID, clientId.getData());
        }
        if (appId.getData().size()) {
            upgradeParams.push_back(TAG_APPLICATION_DATA, appId.getData());
        }
        rc = upgradeKeyBlob(name, targetUid, upgradeParams, &keyBlob);
        if (!rc.isOk()) {
            *aidl_return = static_cast<int32_t>(rc);
            return Status::ok();
        }

        auto upgradedHidlKeyBlob = blob2hidlVec(keyBlob);

        rc = KS_HANDLE_HIDL_ERROR(dev->getKeyCharacteristics(
            upgradedHidlKeyBlob, clientId.getData(), appId.getData(), hidlCb));
        if (!rc.isOk()) {
            *aidl_return = static_cast<int32_t>(rc);
            return Status::ok();
        }
        // Note that, on success, "error" will have been updated by the hidlCB callback.
        // So it is fine to return "error" below.
    }
    *aidl_return = static_cast<int32_t>(KeyStoreServiceReturnCode(error));
    return Status::ok();
}

Status
KeyStoreService::importKey(const String16& name, const KeymasterArguments& params, int32_t format,
                           const ::std::vector<uint8_t>& keyData, int uid, int flags,
                           ::android::security::keymaster::KeyCharacteristics* outCharacteristics,
                           int32_t* aidl_return) {

    uid = getEffectiveUid(uid);
    KeyStoreServiceReturnCode rc =
        checkBinderPermissionAndKeystoreState(P_INSERT, uid, flags & KEYSTORE_FLAG_ENCRYPTED);
    if (!rc.isOk()) {
        *aidl_return = static_cast<int32_t>(rc);
        return Status::ok();
    }
    if ((flags & KEYSTORE_FLAG_CRITICAL_TO_DEVICE_ENCRYPTION) && get_app_id(uid) != AID_SYSTEM) {
        ALOGE("Non-system uid %d cannot set FLAG_CRITICAL_TO_DEVICE_ENCRYPTION", uid);
        *aidl_return = static_cast<int32_t>(ResponseCode::PERMISSION_DENIED);
        return Status::ok();
    }

    bool usingFallback = false;
    auto& dev = mKeyStore->getDevice();

    String8 name8(name);

    KeyStoreServiceReturnCode error;

    auto hidlCb = [&](ErrorCode ret, const ::std::vector<uint8_t>& keyBlob,
                      const KeyCharacteristics& keyCharacteristics) {
        error = ret;
        if (!error.isOk()) {
            return;
        }
        if (outCharacteristics)
            *outCharacteristics =
                ::android::security::keymaster::KeyCharacteristics(keyCharacteristics);

        // Write the key:
        String8 filename(mKeyStore->getKeyNameForUidWithDir(name8, uid, ::TYPE_KEYMASTER_10));

        Blob ksBlob(&keyBlob[0], keyBlob.size(), NULL, 0, ::TYPE_KEYMASTER_10);
        ksBlob.setFallback(usingFallback);
        ksBlob.setCriticalToDeviceEncryption(flags & KEYSTORE_FLAG_CRITICAL_TO_DEVICE_ENCRYPTION);
        if (isAuthenticationBound(params.getParameters()) &&
            !ksBlob.isCriticalToDeviceEncryption()) {
            ksBlob.setSuperEncrypted(true);
        }
        ksBlob.setEncrypted(flags & KEYSTORE_FLAG_ENCRYPTED);

        error = mKeyStore->put(filename.string(), &ksBlob, get_user_id(uid));
    };

    rc = KS_HANDLE_HIDL_ERROR(
        dev->importKey(params.getParameters(), KeyFormat(format), keyData, hidlCb));
    // possible hidl error
    if (!rc.isOk()) {
        *aidl_return = static_cast<int32_t>(rc);
        return Status::ok();
    }
    // now check error from callback
    if (!error.isOk()) {
        ALOGE("Failed to import key -> falling back to software keymaster");
        usingFallback = true;
        auto fallback = mKeyStore->getFallbackDevice();
        if (!fallback.isOk()) {
            *aidl_return = static_cast<int32_t>(error);
            return Status::ok();
        }
        rc = KS_HANDLE_HIDL_ERROR(fallback.value()->importKey(params.getParameters(),
                                                              KeyFormat(format), keyData, hidlCb));
        // possible hidl error
        if (!rc.isOk()) {
            *aidl_return = static_cast<int32_t>(rc);
            return Status::ok();
        }
        // now check error from callback
        if (!error.isOk()) {
            *aidl_return = static_cast<int32_t>(error);
            return Status::ok();
        }
    }

    // Write the characteristics:
    String8 cFilename(mKeyStore->getKeyNameForUidWithDir(name8, uid, ::TYPE_KEY_CHARACTERISTICS));

    AuthorizationSet opParams = params.getParameters();
    std::stringstream kcStream;
    opParams.Serialize(&kcStream);
    if (kcStream.bad()) {
        *aidl_return = static_cast<int32_t>(ResponseCode::SYSTEM_ERROR);
        return Status::ok();
    }
    auto kcBuf = kcStream.str();

    Blob charBlob(reinterpret_cast<const uint8_t*>(kcBuf.data()), kcBuf.size(), NULL, 0,
                  ::TYPE_KEY_CHARACTERISTICS);
    charBlob.setFallback(usingFallback);
    charBlob.setEncrypted(flags & KEYSTORE_FLAG_ENCRYPTED);

    *aidl_return =
        static_cast<int32_t>(mKeyStore->put(cFilename.string(), &charBlob, get_user_id(uid)));

    return Status::ok();
}

Status KeyStoreService::exportKey(const String16& name, int32_t format,
                                  const ::android::security::keymaster::KeymasterBlob& clientId,
                                  const ::android::security::keymaster::KeymasterBlob& appId,
                                  int32_t uid, ExportResult* result) {

    uid_t targetUid = getEffectiveUid(uid);
    uid_t callingUid = IPCThreadState::self()->getCallingUid();
    if (!is_granted_to(callingUid, targetUid)) {
        ALOGW("uid %d not permitted to act for uid %d in exportKey", callingUid, targetUid);
        result->resultCode = ResponseCode::PERMISSION_DENIED;
        return Status::ok();
    }

    Blob keyBlob;
    String8 name8(name);

    result->resultCode = mKeyStore->getKeyForName(&keyBlob, name8, targetUid, TYPE_KEYMASTER_10);
    if (!result->resultCode.isOk()) {
        return Status::ok();
    }

    auto key = blob2hidlVec(keyBlob);
    auto& dev = mKeyStore->getDevice(keyBlob);

    auto hidlCb = [&](ErrorCode ret, const ::android::hardware::hidl_vec<uint8_t>& keyMaterial) {
        result->resultCode = ret;
        if (!result->resultCode.isOk()) {
            return;
        }
        result->exportData = keyMaterial;
    };
    KeyStoreServiceReturnCode rc = KS_HANDLE_HIDL_ERROR(
        dev->exportKey(KeyFormat(format), key, clientId.getData(), appId.getData(), hidlCb));
    // Overwrite result->resultCode only on HIDL error. Otherwise we want the result set in the
    // callback hidlCb.
    if (!rc.isOk()) {
        result->resultCode = rc;
    }

    if (result->resultCode == ErrorCode::KEY_REQUIRES_UPGRADE) {
        AuthorizationSet upgradeParams;
        if (clientId.getData().size()) {
            upgradeParams.push_back(TAG_APPLICATION_ID, clientId.getData());
        }
        if (appId.getData().size()) {
            upgradeParams.push_back(TAG_APPLICATION_DATA, appId.getData());
        }
        result->resultCode = upgradeKeyBlob(name, targetUid, upgradeParams, &keyBlob);
        if (!result->resultCode.isOk()) {
            return Status::ok();
        }

        auto upgradedHidlKeyBlob = blob2hidlVec(keyBlob);

        result->resultCode = KS_HANDLE_HIDL_ERROR(dev->exportKey(
            KeyFormat(format), upgradedHidlKeyBlob, clientId.getData(), appId.getData(), hidlCb));
        if (!result->resultCode.isOk()) {
            return Status::ok();
        }
    }
    return Status::ok();
}

static inline void addAuthTokenToParams(AuthorizationSet* params, const HardwareAuthToken* token) {
    if (token) {
        params->push_back(TAG_AUTH_TOKEN, authToken2HidlVec(*token));
    }
}

Status KeyStoreService::begin(const sp<IBinder>& appToken, const String16& name, int32_t purpose,
                              bool pruneable, const KeymasterArguments& params,
                              const ::std::vector<uint8_t>& entropy, int32_t uid,
                              OperationResult* result) {
    uid_t callingUid = IPCThreadState::self()->getCallingUid();
    uid_t targetUid = getEffectiveUid(uid);
    if (!is_granted_to(callingUid, targetUid)) {
        ALOGW("uid %d not permitted to act for uid %d in begin", callingUid, targetUid);
        result->resultCode = ResponseCode::PERMISSION_DENIED;
        return Status::ok();
    }
    if (!pruneable && get_app_id(callingUid) != AID_SYSTEM) {
        ALOGE("Non-system uid %d trying to start non-pruneable operation", callingUid);
        result->resultCode = ResponseCode::PERMISSION_DENIED;
        return Status::ok();
    }
    if (!checkAllowedOperationParams(params.getParameters())) {
        result->resultCode = ErrorCode::INVALID_ARGUMENT;
        return Status::ok();
    }
    Blob keyBlob;
    String8 name8(name);
    result->resultCode = mKeyStore->getKeyForName(&keyBlob, name8, targetUid, TYPE_KEYMASTER_10);
    if (result->resultCode == ResponseCode::LOCKED && keyBlob.isSuperEncrypted()) {
        result->resultCode = ErrorCode::KEY_USER_NOT_AUTHENTICATED;
    }
    if (!result->resultCode.isOk()) {
        return Status::ok();
    }

    auto key = blob2hidlVec(keyBlob);
    auto& dev = mKeyStore->getDevice(keyBlob);
    AuthorizationSet opParams = params.getParameters();
    KeyCharacteristics characteristics;
    result->resultCode = getOperationCharacteristics(key, &dev, opParams, &characteristics);

    if (result->resultCode == ErrorCode::KEY_REQUIRES_UPGRADE) {
        result->resultCode = upgradeKeyBlob(name, targetUid, opParams, &keyBlob);
        if (!result->resultCode.isOk()) {
            return Status::ok();
        }
        key = blob2hidlVec(keyBlob);
        result->resultCode = getOperationCharacteristics(key, &dev, opParams, &characteristics);
    }
    if (!result->resultCode.isOk()) {
        return Status::ok();
    }

    const HardwareAuthToken* authToken = NULL;

    // Merge these characteristics with the ones cached when the key was generated or imported
    Blob charBlob;
    AuthorizationSet persistedCharacteristics;
    result->resultCode =
        mKeyStore->getKeyForName(&charBlob, name8, targetUid, TYPE_KEY_CHARACTERISTICS);
    if (result->resultCode.isOk()) {
        // TODO write one shot stream buffer to avoid copying (twice here)
        std::string charBuffer(reinterpret_cast<const char*>(charBlob.getValue()),
                               charBlob.getLength());
        std::stringstream charStream(charBuffer);
        persistedCharacteristics.Deserialize(&charStream);
    } else {
        ALOGD("Unable to read cached characteristics for key");
    }

    // Replace the sw_enforced set with those persisted to disk, minus hw_enforced
    AuthorizationSet softwareEnforced = characteristics.softwareEnforced;
    AuthorizationSet teeEnforced = characteristics.teeEnforced;
    persistedCharacteristics.Union(softwareEnforced);
    persistedCharacteristics.Subtract(teeEnforced);
    characteristics.softwareEnforced = persistedCharacteristics.hidl_data();

    auto authResult = getAuthToken(characteristics, 0, KeyPurpose(purpose), &authToken,
                                   /*failOnTokenMissing*/ false);
    // If per-operation auth is needed we need to begin the operation and
    // the client will need to authorize that operation before calling
    // update. Any other auth issues stop here.
    if (!authResult.isOk() && authResult != ResponseCode::OP_AUTH_NEEDED) {
        result->resultCode = authResult;
        return Status::ok();
    }

    addAuthTokenToParams(&opParams, authToken);

    // Add entropy to the device first.
    if (entropy.size()) {
        int32_t resultCode;
        addRngEntropy(entropy, &resultCode);  // binder error is not possible
        result->resultCode = KeyStoreServiceReturnCode(resultCode);
        if (!result->resultCode.isOk()) {
            return Status::ok();
        }
    }

    // Create a keyid for this key.
    km_id_t keyid;
    if (!enforcement_policy.CreateKeyId(key, &keyid)) {
        ALOGE("Failed to create a key ID for authorization checking.");
        result->resultCode = ErrorCode::UNKNOWN_ERROR;
        return Status::ok();
    }

    // Check that all key authorization policy requirements are met.
    AuthorizationSet key_auths = characteristics.teeEnforced;
    key_auths.append(&characteristics.softwareEnforced[0],
                     &characteristics.softwareEnforced[characteristics.softwareEnforced.size()]);

    result->resultCode =
        enforcement_policy.AuthorizeOperation(KeyPurpose(purpose), keyid, key_auths, opParams,
                                              0 /* op_handle */, true /* is_begin_operation */);
    if (!result->resultCode.isOk()) {
        return Status::ok();
    }

    // If there are more than kMaxOperations, abort the oldest operation that was started as
    // pruneable.
    while (mOperationMap.getOperationCount() >= kMaxOperations) {
        ALOGD("Reached or exceeded concurrent operations limit");
        if (!pruneOperation()) {
            break;
        }
    }

    auto hidlCb = [&](ErrorCode ret, const hidl_vec<KeyParameter>& outParams,
                      uint64_t operationHandle) {
        result->resultCode = ret;
        if (!result->resultCode.isOk()) {
            return;
        }
        result->handle = operationHandle;
        result->outParams = outParams;
    };

    ErrorCode rc =
        KS_HANDLE_HIDL_ERROR(dev->begin(KeyPurpose(purpose), key, opParams.hidl_data(), hidlCb));
    if (rc != ErrorCode::OK) {
        ALOGW("Got error %d from begin()", rc);
    }

    // If there are too many operations abort the oldest operation that was
    // started as pruneable and try again.
    while (rc == ErrorCode::TOO_MANY_OPERATIONS && mOperationMap.hasPruneableOperation()) {
        ALOGW("Ran out of operation handles");
        if (!pruneOperation()) {
            break;
        }
        rc = KS_HANDLE_HIDL_ERROR(
            dev->begin(KeyPurpose(purpose), key, opParams.hidl_data(), hidlCb));
    }
    if (rc != ErrorCode::OK) {
        result->resultCode = rc;
        return Status::ok();
    }

    // Note: The operation map takes possession of the contents of "characteristics".
    // It is safe to use characteristics after the following line but it will be empty.
    sp<IBinder> operationToken =
        mOperationMap.addOperation(result->handle, keyid, KeyPurpose(purpose), dev, appToken,
                                   std::move(characteristics), pruneable);
    assert(characteristics.teeEnforced.size() == 0);
    assert(characteristics.softwareEnforced.size() == 0);
    result->token = operationToken;

    if (authToken) {
        mOperationMap.setOperationAuthToken(operationToken, authToken);
    }
    // Return the authentication lookup result. If this is a per operation
    // auth'd key then the resultCode will be ::OP_AUTH_NEEDED and the
    // application should get an auth token using the handle before the
    // first call to update, which will fail if keystore hasn't received the
    // auth token.
    if (result->resultCode == ErrorCode::OK) {
        result->resultCode = authResult;
    }

    // Other result fields were set in the begin operation's callback.
    return Status::ok();
}

Status KeyStoreService::update(const sp<IBinder>& token, const KeymasterArguments& params,
                               const ::std::vector<uint8_t>& data, OperationResult* result) {
    if (!checkAllowedOperationParams(params.getParameters())) {
        result->resultCode = ErrorCode::INVALID_ARGUMENT;
        return Status::ok();
    }
    km_device_t dev;
    uint64_t handle;
    KeyPurpose purpose;
    km_id_t keyid;
    const KeyCharacteristics* characteristics;
    if (!mOperationMap.getOperation(token, &handle, &keyid, &purpose, &dev, &characteristics)) {
        result->resultCode = ErrorCode::INVALID_OPERATION_HANDLE;
        return Status::ok();
    }
    AuthorizationSet opParams = params.getParameters();
    result->resultCode = addOperationAuthTokenIfNeeded(token, &opParams);
    if (!result->resultCode.isOk()) {
        return Status::ok();
    }

    // Check that all key authorization policy requirements are met.
    AuthorizationSet key_auths(characteristics->teeEnforced);
    key_auths.append(&characteristics->softwareEnforced[0],
                     &characteristics->softwareEnforced[characteristics->softwareEnforced.size()]);
    result->resultCode = enforcement_policy.AuthorizeOperation(
        purpose, keyid, key_auths, opParams, handle, false /* is_begin_operation */);
    if (!result->resultCode.isOk()) {
        return Status::ok();
    }

    auto hidlCb = [&](ErrorCode ret, uint32_t inputConsumed,
                      const hidl_vec<KeyParameter>& outParams,
                      const ::std::vector<uint8_t>& output) {
        result->resultCode = ret;
        if (!result->resultCode.isOk()) {
            return;
        }
        result->inputConsumed = inputConsumed;
        result->outParams = outParams;
        result->data = output;
    };

    KeyStoreServiceReturnCode rc =
        KS_HANDLE_HIDL_ERROR(dev->update(handle, opParams.hidl_data(), data, hidlCb));
    // just a reminder: on success result->resultCode was set in the callback. So we only overwrite
    // it if there was a communication error indicated by the ErrorCode.
    if (!rc.isOk()) {
        result->resultCode = rc;
    }
    return Status::ok();
}

Status KeyStoreService::finish(const sp<IBinder>& token, const KeymasterArguments& params,
                               const ::std::vector<uint8_t>& signature,
                               const ::std::vector<uint8_t>& entropy, OperationResult* result) {
    if (!checkAllowedOperationParams(params.getParameters())) {
        result->resultCode = ErrorCode::INVALID_ARGUMENT;
        return Status::ok();
    }
    km_device_t dev;
    uint64_t handle;
    KeyPurpose purpose;
    km_id_t keyid;
    const KeyCharacteristics* characteristics;
    if (!mOperationMap.getOperation(token, &handle, &keyid, &purpose, &dev, &characteristics)) {
        result->resultCode = ErrorCode::INVALID_OPERATION_HANDLE;
        return Status::ok();
    }
    AuthorizationSet opParams = params.getParameters();
    result->resultCode = addOperationAuthTokenIfNeeded(token, &opParams);
    if (!result->resultCode.isOk()) {
        return Status::ok();
    }

    if (entropy.size()) {
        int resultCode;
        addRngEntropy(entropy, &resultCode);  // binder error is not possible
        result->resultCode = KeyStoreServiceReturnCode(resultCode);
        if (!result->resultCode.isOk()) {
            return Status::ok();
        }
    }

    // Check that all key authorization policy requirements are met.
    AuthorizationSet key_auths(characteristics->teeEnforced);
    key_auths.append(&characteristics->softwareEnforced[0],
                     &characteristics->softwareEnforced[characteristics->softwareEnforced.size()]);
    result->resultCode = enforcement_policy.AuthorizeOperation(
        purpose, keyid, key_auths, opParams, handle, false /* is_begin_operation */);
    if (!result->resultCode.isOk()) return Status::ok();

    auto hidlCb = [&](ErrorCode ret, const hidl_vec<KeyParameter>& outParams,
                      const ::std::vector<uint8_t>& output) {
        result->resultCode = ret;
        if (!result->resultCode.isOk()) {
        }
        result->outParams = outParams;
        result->data = output;
    };

    KeyStoreServiceReturnCode rc = KS_HANDLE_HIDL_ERROR(
        dev->finish(handle, opParams.hidl_data(),
                    ::std::vector<uint8_t>() /* TODO(swillden): wire up input to finish() */,
                    signature, hidlCb));
    // Remove the operation regardless of the result
    mOperationMap.removeOperation(token);
    mAuthTokenTable.MarkCompleted(handle);

    // just a reminder: on success result->resultCode was set in the callback. So we only overwrite
    // it if there was a communication error indicated by the ErrorCode.
    if (!rc.isOk()) {
        result->resultCode = rc;
    }
    return Status::ok();
}

Status KeyStoreService::abort(const sp<IBinder>& token, int32_t* aidl_return) {
    km_device_t dev;
    uint64_t handle;
    KeyPurpose purpose;
    km_id_t keyid;
    if (!mOperationMap.getOperation(token, &handle, &keyid, &purpose, &dev, NULL)) {
        *aidl_return =
            static_cast<int32_t>(KeyStoreServiceReturnCode(ErrorCode::INVALID_OPERATION_HANDLE));
        return Status::ok();
    }
    mOperationMap.removeOperation(token);

    ErrorCode error_code = KS_HANDLE_HIDL_ERROR(dev->abort(handle));
    mAuthTokenTable.MarkCompleted(handle);
    *aidl_return = static_cast<int32_t>(KeyStoreServiceReturnCode(error_code));
    return Status::ok();
}

Status KeyStoreService::isOperationAuthorized(const sp<IBinder>& token, bool* aidl_return) {
    km_device_t dev;
    uint64_t handle;
    const KeyCharacteristics* characteristics;
    KeyPurpose purpose;
    km_id_t keyid;
    if (!mOperationMap.getOperation(token, &handle, &keyid, &purpose, &dev, &characteristics)) {
        *aidl_return = false;
        return Status::ok();
    }
    const HardwareAuthToken* authToken = NULL;
    mOperationMap.getOperationAuthToken(token, &authToken);
    AuthorizationSet ignored;
    auto authResult = addOperationAuthTokenIfNeeded(token, &ignored);
    *aidl_return = authResult.isOk();
    return Status::ok();
}

Status KeyStoreService::addAuthToken(const ::std::vector<uint8_t>& authTokenAsVector,
                                     int32_t* aidl_return) {

    // TODO(swillden): When gatekeeper and fingerprint are ready, this should be updated to
    // receive a HardwareAuthToken, rather than an opaque byte array.

    if (!checkBinderPermission(P_ADD_AUTH)) {
        ALOGW("addAuthToken: permission denied for %d", IPCThreadState::self()->getCallingUid());
        *aidl_return = static_cast<int32_t>(ResponseCode::PERMISSION_DENIED);
        return Status::ok();
    }
    if (authTokenAsVector.size() != sizeof(hw_auth_token_t)) {
        *aidl_return = static_cast<int32_t>(KeyStoreServiceReturnCode(ErrorCode::INVALID_ARGUMENT));
        return Status::ok();
    }

    hw_auth_token_t authToken;
    memcpy(reinterpret_cast<void*>(&authToken), authTokenAsVector.data(), sizeof(hw_auth_token_t));
    if (authToken.version != 0) {
        *aidl_return = static_cast<int32_t>(KeyStoreServiceReturnCode(ErrorCode::INVALID_ARGUMENT));
        return Status::ok();
    }

    std::unique_ptr<HardwareAuthToken> hidlAuthToken(new HardwareAuthToken);
    hidlAuthToken->challenge = authToken.challenge;
    hidlAuthToken->userId = authToken.user_id;
    hidlAuthToken->authenticatorId = authToken.authenticator_id;
    hidlAuthToken->authenticatorType = authToken.authenticator_type;
    hidlAuthToken->timestamp = authToken.timestamp;
    static_assert(
        std::is_same<decltype(hidlAuthToken->hmac),
                     ::android::hardware::hidl_array<uint8_t, sizeof(authToken.hmac)>>::value,
        "This function assumes token HMAC is 32 bytes, but it might not be.");
    std::copy(authToken.hmac, authToken.hmac + sizeof(authToken.hmac), hidlAuthToken->hmac.data());

    mAuthTokenTable.AddAuthenticationToken(std::move(hidlAuthToken));
    *aidl_return = static_cast<int32_t>(ResponseCode::NO_ERROR);
    return Status::ok();
}

bool isDeviceIdAttestationRequested(const KeymasterArguments& params) {
    const hardware::hidl_vec<KeyParameter> paramsVec = params.getParameters();
    for (size_t i = 0; i < paramsVec.size(); ++i) {
        switch (paramsVec[i].tag) {
        case Tag::ATTESTATION_ID_BRAND:
        case Tag::ATTESTATION_ID_DEVICE:
        case Tag::ATTESTATION_ID_IMEI:
        case Tag::ATTESTATION_ID_MANUFACTURER:
        case Tag::ATTESTATION_ID_MEID:
        case Tag::ATTESTATION_ID_MODEL:
        case Tag::ATTESTATION_ID_PRODUCT:
        case Tag::ATTESTATION_ID_SERIAL:
            return true;
        default:
            break;
        }
    }
    return false;
}

Status KeyStoreService::attestKey(const String16& name, const KeymasterArguments& params,
                                  ::android::security::keymaster::KeymasterCertificateChain* chain,
                                  int32_t* aidl_return) {
    // check null output if method signature is updated and return ErrorCode::OUTPUT_PARAMETER_NULL
    if (!checkAllowedOperationParams(params.getParameters())) {
        *aidl_return = static_cast<int32_t>(KeyStoreServiceReturnCode(ErrorCode::INVALID_ARGUMENT));
        return Status::ok();
    }

    if (isDeviceIdAttestationRequested(params)) {
        // There is a dedicated attestDeviceIds() method for device ID attestation.
        *aidl_return = static_cast<int32_t>(KeyStoreServiceReturnCode(ErrorCode::INVALID_ARGUMENT));
        return Status::ok();
    }

    uid_t callingUid = IPCThreadState::self()->getCallingUid();

    AuthorizationSet mutableParams = params.getParameters();
    KeyStoreServiceReturnCode rc = updateParamsForAttestation(callingUid, &mutableParams);
    if (!rc.isOk()) {
        *aidl_return = static_cast<int32_t>(rc);
        return Status::ok();
    }

    Blob keyBlob;
    String8 name8(name);
    rc = mKeyStore->getKeyForName(&keyBlob, name8, callingUid, TYPE_KEYMASTER_10);
    if (!rc.isOk()) {
        *aidl_return = static_cast<int32_t>(rc);
        return Status::ok();
    }

    KeyStoreServiceReturnCode error;
    auto hidlCb = [&](ErrorCode ret, const hidl_vec<hidl_vec<uint8_t>>& certChain) {
        error = ret;
        if (!error.isOk()) {
            return;
        }
        if (chain) {
            *chain = KeymasterCertificateChain(certChain);
        }
    };

    auto hidlKey = blob2hidlVec(keyBlob);
    auto& dev = mKeyStore->getDevice(keyBlob);
    rc = KS_HANDLE_HIDL_ERROR(dev->attestKey(hidlKey, mutableParams.hidl_data(), hidlCb));
    if (!rc.isOk()) {
        *aidl_return = static_cast<int32_t>(rc);
        return Status::ok();
    }
    *aidl_return = static_cast<int32_t>(error);
    return Status::ok();
}

Status
KeyStoreService::attestDeviceIds(const KeymasterArguments& params,
                                 ::android::security::keymaster::KeymasterCertificateChain* chain,
                                 int32_t* aidl_return) {
    // check null output if method signature is updated and return ErrorCode::OUTPUT_PARAMETER_NULL

    if (!checkAllowedOperationParams(params.getParameters())) {
        *aidl_return = static_cast<int32_t>(KeyStoreServiceReturnCode(ErrorCode::INVALID_ARGUMENT));
        return Status::ok();
    }

    if (!isDeviceIdAttestationRequested(params)) {
        // There is an attestKey() method for attesting keys without device ID attestation.
        *aidl_return = static_cast<int32_t>(KeyStoreServiceReturnCode(ErrorCode::INVALID_ARGUMENT));
        return Status::ok();
    }

    uid_t callingUid = IPCThreadState::self()->getCallingUid();
    sp<IBinder> binder = defaultServiceManager()->getService(String16("permission"));
    if (binder == 0) {
        *aidl_return =
            static_cast<int32_t>(KeyStoreServiceReturnCode(ErrorCode::CANNOT_ATTEST_IDS));
        return Status::ok();
    }
    if (!interface_cast<IPermissionController>(binder)->checkPermission(
            String16("android.permission.READ_PRIVILEGED_PHONE_STATE"),
            IPCThreadState::self()->getCallingPid(), callingUid)) {
        *aidl_return =
            static_cast<int32_t>(KeyStoreServiceReturnCode(ErrorCode::CANNOT_ATTEST_IDS));
        return Status::ok();
    }

    AuthorizationSet mutableParams = params.getParameters();
    KeyStoreServiceReturnCode rc = updateParamsForAttestation(callingUid, &mutableParams);
    if (!rc.isOk()) {
        *aidl_return = static_cast<int32_t>(rc);
        return Status::ok();
    }

    // Generate temporary key.
    auto& dev = mKeyStore->getDevice();
    KeyStoreServiceReturnCode error;
    ::std::vector<uint8_t> hidlKey;

    AuthorizationSet keyCharacteristics;
    keyCharacteristics.push_back(TAG_PURPOSE, KeyPurpose::VERIFY);
    keyCharacteristics.push_back(TAG_ALGORITHM, Algorithm::EC);
    keyCharacteristics.push_back(TAG_DIGEST, Digest::SHA_2_256);
    keyCharacteristics.push_back(TAG_NO_AUTH_REQUIRED);
    keyCharacteristics.push_back(TAG_EC_CURVE, EcCurve::P_256);
    auto generateHidlCb = [&](ErrorCode ret, const ::std::vector<uint8_t>& hidlKeyBlob,
                              const KeyCharacteristics&) {
        error = ret;
        if (!error.isOk()) {
            return;
        }
        hidlKey = hidlKeyBlob;
    };

    rc = KS_HANDLE_HIDL_ERROR(dev->generateKey(keyCharacteristics.hidl_data(), generateHidlCb));
    if (!rc.isOk()) {
        *aidl_return = static_cast<int32_t>(rc);
        return Status::ok();
    }
    if (!error.isOk()) {
        *aidl_return = static_cast<int32_t>(error);
        return Status::ok();
    }

    // Attest key and device IDs.
    auto attestHidlCb = [&](ErrorCode ret, const hidl_vec<hidl_vec<uint8_t>>& certChain) {
        error = ret;
        if (!error.isOk()) {
            return;
        }
        *chain = ::android::security::keymaster::KeymasterCertificateChain(certChain);
    };
    KeyStoreServiceReturnCode attestationRc =
        KS_HANDLE_HIDL_ERROR(dev->attestKey(hidlKey, mutableParams.hidl_data(), attestHidlCb));

    // Delete temporary key.
    KeyStoreServiceReturnCode deletionRc = KS_HANDLE_HIDL_ERROR(dev->deleteKey(hidlKey));

    if (!attestationRc.isOk()) {
        *aidl_return = static_cast<int32_t>(attestationRc);
        return Status::ok();
    }
    if (!error.isOk()) {
        *aidl_return = static_cast<int32_t>(error);
        return Status::ok();
    }
    *aidl_return = static_cast<int32_t>(deletionRc);
    return Status::ok();
}

Status KeyStoreService::onDeviceOffBody(int32_t* aidl_return) {
    // TODO(tuckeris): add permission check.  This should be callable from ClockworkHome only.
    mAuthTokenTable.onDeviceOffBody();
    *aidl_return = static_cast<int32_t>(ResponseCode::NO_ERROR);
    return Status::ok();
}

/**
 * Prune the oldest pruneable operation.
 */
bool KeyStoreService::pruneOperation() {
    sp<IBinder> oldest = mOperationMap.getOldestPruneableOperation();
    ALOGD("Trying to prune operation %p", oldest.get());
    size_t op_count_before_abort = mOperationMap.getOperationCount();
    // We mostly ignore errors from abort() because all we care about is whether at least
    // one operation has been removed.
    int32_t abort_error;
    abort(oldest, &abort_error);
    if (mOperationMap.getOperationCount() >= op_count_before_abort) {
        ALOGE("Failed to abort pruneable operation %p, error: %d", oldest.get(), abort_error);
        return false;
    }
    return true;
}

/**
 * Get the effective target uid for a binder operation that takes an
 * optional uid as the target.
 */
uid_t KeyStoreService::getEffectiveUid(int32_t targetUid) {
    if (targetUid == UID_SELF) {
        return IPCThreadState::self()->getCallingUid();
    }
    return static_cast<uid_t>(targetUid);
}

/**
 * Check if the caller of the current binder method has the required
 * permission and if acting on other uids the grants to do so.
 */
bool KeyStoreService::checkBinderPermission(perm_t permission, int32_t targetUid) {
    uid_t callingUid = IPCThreadState::self()->getCallingUid();
    pid_t spid = IPCThreadState::self()->getCallingPid();
    if (!has_permission(callingUid, permission, spid)) {
        ALOGW("permission %s denied for %d", get_perm_label(permission), callingUid);
        return false;
    }
    if (!is_granted_to(callingUid, getEffectiveUid(targetUid))) {
        ALOGW("uid %d not granted to act for %d", callingUid, targetUid);
        return false;
    }
    return true;
}

/**
 * Check if the caller of the current binder method has the required
 * permission and the target uid is the caller or the caller is system.
 */
bool KeyStoreService::checkBinderPermissionSelfOrSystem(perm_t permission, int32_t targetUid) {
    uid_t callingUid = IPCThreadState::self()->getCallingUid();
    pid_t spid = IPCThreadState::self()->getCallingPid();
    if (!has_permission(callingUid, permission, spid)) {
        ALOGW("permission %s denied for %d", get_perm_label(permission), callingUid);
        return false;
    }
    return getEffectiveUid(targetUid) == callingUid || callingUid == AID_SYSTEM;
}

/**
 * Check if the caller of the current binder method has the required
 * permission or the target of the operation is the caller's uid. This is
 * for operation where the permission is only for cross-uid activity and all
 * uids are allowed to act on their own (ie: clearing all entries for a
 * given uid).
 */
bool KeyStoreService::checkBinderPermissionOrSelfTarget(perm_t permission, int32_t targetUid) {
    uid_t callingUid = IPCThreadState::self()->getCallingUid();
    if (getEffectiveUid(targetUid) == callingUid) {
        return true;
    } else {
        return checkBinderPermission(permission, targetUid);
    }
}

/**
 * Helper method to check that the caller has the required permission as
 * well as the keystore is in the unlocked state if checkUnlocked is true.
 *
 * Returns NO_ERROR on success, PERMISSION_DENIED on a permission error and
 * otherwise the state of keystore when not unlocked and checkUnlocked is
 * true.
 */
KeyStoreServiceReturnCode
KeyStoreService::checkBinderPermissionAndKeystoreState(perm_t permission, int32_t targetUid,
                                                       bool checkUnlocked) {
    if (!checkBinderPermission(permission, targetUid)) {
        return ResponseCode::PERMISSION_DENIED;
    }
    State state = mKeyStore->getState(get_user_id(getEffectiveUid(targetUid)));
    if (checkUnlocked && !isKeystoreUnlocked(state)) {
        // All State values coincide with ResponseCodes
        return static_cast<ResponseCode>(state);
    }

    return ResponseCode::NO_ERROR;
}

bool KeyStoreService::isKeystoreUnlocked(State state) {
    switch (state) {
    case ::STATE_NO_ERROR:
        return true;
    case ::STATE_UNINITIALIZED:
    case ::STATE_LOCKED:
        return false;
    }
    return false;
}

/**
 * Check that all KeyParameter's provided by the application are
 * allowed. Any parameter that keystore adds itself should be disallowed here.
 */
bool KeyStoreService::checkAllowedOperationParams(const hidl_vec<KeyParameter>& params) {
    for (size_t i = 0; i < params.size(); ++i) {
        switch (params[i].tag) {
        case Tag::ATTESTATION_APPLICATION_ID:
        case Tag::AUTH_TOKEN:
        case Tag::RESET_SINCE_ID_ROTATION:
            return false;
        default:
            break;
        }
    }
    return true;
}

ErrorCode KeyStoreService::getOperationCharacteristics(const hidl_vec<uint8_t>& key,
                                                       km_device_t* dev,
                                                       const AuthorizationSet& params,
                                                       KeyCharacteristics* out) {
    ::std::vector<uint8_t> appId;
    ::std::vector<uint8_t> appData;
    for (auto param : params) {
        if (param.tag == Tag::APPLICATION_ID) {
            appId = authorizationValue(TAG_APPLICATION_ID, param).value();
        } else if (param.tag == Tag::APPLICATION_DATA) {
            appId = authorizationValue(TAG_APPLICATION_DATA, param).value();
        }
    }
    ErrorCode error = ErrorCode::OK;

    auto hidlCb = [&](ErrorCode ret, const KeyCharacteristics& keyCharacteristics) {
        error = ret;
        if (error != ErrorCode::OK) {
            return;
        }
        if (out) *out = keyCharacteristics;
    };

    ErrorCode rc = KS_HANDLE_HIDL_ERROR((*dev)->getKeyCharacteristics(key, appId, appId, hidlCb));
    if (rc != ErrorCode::OK) {
        return rc;
    }
    return error;
}

/**
 * Get the auth token for this operation from the auth token table.
 *
 * Returns ResponseCode::NO_ERROR if the auth token was set or none was required.
 *         ::OP_AUTH_NEEDED if it is a per op authorization, no
 *         authorization token exists for that operation and
 *         failOnTokenMissing is false.
 *         KM_ERROR_KEY_USER_NOT_AUTHENTICATED if there is no valid auth
 *         token for the operation
 */
KeyStoreServiceReturnCode KeyStoreService::getAuthToken(const KeyCharacteristics& characteristics,
                                                        uint64_t handle, KeyPurpose purpose,
                                                        const HardwareAuthToken** authToken,
                                                        bool failOnTokenMissing) {

    AuthorizationSet allCharacteristics;
    for (size_t i = 0; i < characteristics.softwareEnforced.size(); i++) {
        allCharacteristics.push_back(characteristics.softwareEnforced[i]);
    }
    for (size_t i = 0; i < characteristics.teeEnforced.size(); i++) {
        allCharacteristics.push_back(characteristics.teeEnforced[i]);
    }
    AuthTokenTable::Error err = mAuthTokenTable.FindAuthorization(
        allCharacteristics, KeyPurpose(purpose), handle, authToken);
    switch (err) {
    case AuthTokenTable::OK:
    case AuthTokenTable::AUTH_NOT_REQUIRED:
        return ResponseCode::NO_ERROR;
    case AuthTokenTable::AUTH_TOKEN_NOT_FOUND:
    case AuthTokenTable::AUTH_TOKEN_EXPIRED:
    case AuthTokenTable::AUTH_TOKEN_WRONG_SID:
        ALOGE("getAuthToken failed: %d", err);  // STOPSHIP: debug only, to be removed
        return ErrorCode::KEY_USER_NOT_AUTHENTICATED;
    case AuthTokenTable::OP_HANDLE_REQUIRED:
        return failOnTokenMissing ? KeyStoreServiceReturnCode(ErrorCode::KEY_USER_NOT_AUTHENTICATED)
                                  : KeyStoreServiceReturnCode(ResponseCode::OP_AUTH_NEEDED);
    default:
        ALOGE("Unexpected FindAuthorization return value %d", err);
        return ErrorCode::INVALID_ARGUMENT;
    }
}

/**
 * Add the auth token for the operation to the param list if the operation
 * requires authorization. Uses the cached result in the OperationMap if available
 * otherwise gets the token from the AuthTokenTable and caches the result.
 *
 * Returns ResponseCode::NO_ERROR if the auth token was added or not needed.
 *         KM_ERROR_KEY_USER_NOT_AUTHENTICATED if the operation is not
 *         authenticated.
 *         KM_ERROR_INVALID_OPERATION_HANDLE if token is not a valid
 *         operation token.
 */
KeyStoreServiceReturnCode KeyStoreService::addOperationAuthTokenIfNeeded(const sp<IBinder>& token,
                                                                         AuthorizationSet* params) {
    const HardwareAuthToken* authToken = nullptr;
    mOperationMap.getOperationAuthToken(token, &authToken);
    if (!authToken) {
        km_device_t dev;
        uint64_t handle;
        const KeyCharacteristics* characteristics = nullptr;
        KeyPurpose purpose;
        km_id_t keyid;
        if (!mOperationMap.getOperation(token, &handle, &keyid, &purpose, &dev, &characteristics)) {
            return ErrorCode::INVALID_OPERATION_HANDLE;
        }
        auto result = getAuthToken(*characteristics, handle, purpose, &authToken);
        if (!result.isOk()) {
            return result;
        }
        if (authToken) {
            mOperationMap.setOperationAuthToken(token, authToken);
        }
    }
    addAuthTokenToParams(params, authToken);
    return ResponseCode::NO_ERROR;
}

/**
 * Translate a result value to a legacy return value. All keystore errors are
 * preserved and keymaster errors become SYSTEM_ERRORs
 */
KeyStoreServiceReturnCode KeyStoreService::translateResultToLegacyResult(int32_t result) {
    if (result > 0) {
        return static_cast<ResponseCode>(result);
    }
    return ResponseCode::SYSTEM_ERROR;
}

static NullOr<const Algorithm&> getKeyAlgoritmFromKeyCharacteristics(
    const ::android::security::keymaster::KeyCharacteristics& characteristics) {
    for (size_t i = 0; i < characteristics.teeEnforced.getParameters().size(); ++i) {
        auto algo =
            authorizationValue(TAG_ALGORITHM, characteristics.teeEnforced.getParameters()[i]);
        if (algo.isOk()) return algo.value();
    }
    for (size_t i = 0; i < characteristics.softwareEnforced.getParameters().size(); ++i) {
        auto algo =
            authorizationValue(TAG_ALGORITHM, characteristics.softwareEnforced.getParameters()[i]);
        if (algo.isOk()) return algo.value();
    }
    return {};
}

void KeyStoreService::addLegacyBeginParams(const String16& name, AuthorizationSet* params) {
    // All legacy keys are DIGEST_NONE/PAD_NONE.
    params->push_back(TAG_DIGEST, Digest::NONE);
    params->push_back(TAG_PADDING, PaddingMode::NONE);

    // Look up the algorithm of the key.
    ::android::security::keymaster::KeyCharacteristics characteristics;
    int32_t result;
    auto rc = getKeyCharacteristics(name, ::android::security::keymaster::KeymasterBlob(),
                                    ::android::security::keymaster::KeymasterBlob(), UID_SELF,
                                    &characteristics, &result);
    if (!rc.isOk()) {
        ALOGE("Failed to get key characteristics");
        return;
    }
    auto algorithm = getKeyAlgoritmFromKeyCharacteristics(characteristics);
    if (!algorithm.isOk()) {
        ALOGE("getKeyCharacteristics did not include KM_TAG_ALGORITHM");
        return;
    }
    params->push_back(TAG_ALGORITHM, algorithm.value());
}

KeyStoreServiceReturnCode KeyStoreService::doLegacySignVerify(const String16& name,
                                                              const hidl_vec<uint8_t>& data,
                                                              hidl_vec<uint8_t>* out,
                                                              const hidl_vec<uint8_t>& signature,
                                                              KeyPurpose purpose) {

    std::basic_stringstream<uint8_t> outBuffer;
    OperationResult result;
    AuthorizationSet inArgs;
    addLegacyBeginParams(name, &inArgs);
    sp<IBinder> appToken(new BBinder);
    sp<IBinder> token;

    begin(appToken, name, static_cast<int32_t>(purpose), true,
          KeymasterArguments(inArgs.hidl_data()), ::std::vector<uint8_t>(), UID_SELF, &result);
    if (!result.resultCode.isOk()) {
        if (result.resultCode == ResponseCode::KEY_NOT_FOUND) {
            ALOGW("Key not found");
        } else {
            ALOGW("Error in begin: %d", int32_t(result.resultCode));
        }
        return translateResultToLegacyResult(result.resultCode);
    }
    inArgs.Clear();
    token = result.token;
    size_t consumed = 0;
    size_t lastConsumed = 0;
    hidl_vec<uint8_t> data_view;
    do {
        data_view.setToExternal(const_cast<uint8_t*>(&data[consumed]), data.size() - consumed);
        update(token, KeymasterArguments(inArgs.hidl_data()), data_view, &result);
        if (result.resultCode != ResponseCode::NO_ERROR) {
            ALOGW("Error in update: %d", int32_t(result.resultCode));
            return translateResultToLegacyResult(result.resultCode);
        }
        if (out) {
            outBuffer.write(&result.data[0], result.data.size());
        }
        lastConsumed = result.inputConsumed;
        consumed += lastConsumed;
    } while (consumed < data.size() && lastConsumed > 0);

    if (consumed != data.size()) {
        ALOGW("Not all data consumed. Consumed %zu of %zu", consumed, data.size());
        return ResponseCode::SYSTEM_ERROR;
    }

    finish(token, KeymasterArguments(inArgs.hidl_data()), signature, ::std::vector<uint8_t>(),
           &result);
    if (result.resultCode != ResponseCode::NO_ERROR) {
        ALOGW("Error in finish: %d", int32_t(result.resultCode));
        return translateResultToLegacyResult(result.resultCode);
    }
    if (out) {
        outBuffer.write(&result.data[0], result.data.size());
    }

    if (out) {
        auto buf = outBuffer.str();
        out->resize(buf.size());
        memcpy(&(*out)[0], buf.data(), out->size());
    }

    return ResponseCode::NO_ERROR;
}

KeyStoreServiceReturnCode KeyStoreService::upgradeKeyBlob(const String16& name, uid_t uid,
                                                          const AuthorizationSet& params,
                                                          Blob* blob) {
    // Read the blob rather than assuming the caller provided the right name/uid/blob triplet.
    String8 name8(name);
    KeyStoreServiceReturnCode responseCode =
        mKeyStore->getKeyForName(blob, name8, uid, TYPE_KEYMASTER_10);
    if (responseCode != ResponseCode::NO_ERROR) {
        return responseCode;
    }
    ALOGI("upgradeKeyBlob %s %d", name8.string(), uid);

    auto hidlKey = blob2hidlVec(*blob);
    auto& dev = mKeyStore->getDevice(*blob);

    KeyStoreServiceReturnCode error;
    auto hidlCb = [&](ErrorCode ret, const ::std::vector<uint8_t>& upgradedKeyBlob) {
        error = ret;
        if (!error.isOk()) {
            return;
        }

        auto filename = mKeyStore->getBlobFileNameIfExists(name8, uid, ::TYPE_KEYMASTER_10);
        if (!filename.isOk()) {
            ALOGI("trying to upgrade a non existing blob");
            return;
        }
        error = mKeyStore->del(filename.value().string(), ::TYPE_ANY, get_user_id(uid));
        if (!error.isOk()) {
            ALOGI("upgradeKeyBlob keystore->del failed %d", (int)error);
            return;
        }

        Blob newBlob(&upgradedKeyBlob[0], upgradedKeyBlob.size(), nullptr /* info */,
                     0 /* infoLength */, ::TYPE_KEYMASTER_10);
        newBlob.setFallback(blob->isFallback());
        newBlob.setEncrypted(blob->isEncrypted());
        newBlob.setSuperEncrypted(blob->isSuperEncrypted());
        newBlob.setCriticalToDeviceEncryption(blob->isCriticalToDeviceEncryption());

        error = mKeyStore->put(filename.value().string(), &newBlob, get_user_id(uid));
        if (!error.isOk()) {
            ALOGI("upgradeKeyBlob keystore->put failed %d", (int)error);
            return;
        }

        // Re-read blob for caller.  We can't use newBlob because writing it modified it.
        error = mKeyStore->getKeyForName(blob, name8, uid, TYPE_KEYMASTER_10);
    };

    KeyStoreServiceReturnCode rc =
        KS_HANDLE_HIDL_ERROR(dev->upgradeKey(hidlKey, params.hidl_data(), hidlCb));
    if (!rc.isOk()) {
        return rc;
    }

    return error;
}

}  // namespace keystore
