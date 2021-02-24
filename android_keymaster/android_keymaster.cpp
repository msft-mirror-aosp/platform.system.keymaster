/*
 * Copyright 2014 The Android Open Source Project
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

#include <keymaster/android_keymaster.h>

#include <assert.h>
#include <string.h>

#include <stddef.h>

#include <keymaster/UniquePtr.h>
#include <keymaster/android_keymaster_utils.h>
#include <keymaster/key.h>
#include <keymaster/key_blob_utils/ae.h>
#include <keymaster/key_factory.h>
#include <keymaster/keymaster_context.h>
#include <keymaster/km_date.h>
#include <keymaster/km_openssl/openssl_err.h>
#include <keymaster/operation.h>
#include <keymaster/operation_table.h>

namespace keymaster {

namespace {

keymaster_error_t CheckVersionInfo(const AuthorizationSet& tee_enforced,
                                   const AuthorizationSet& sw_enforced,
                                   const KeymasterContext& context) {
    uint32_t os_version;
    uint32_t os_patchlevel;
    context.GetSystemVersion(&os_version, &os_patchlevel);

    uint32_t key_os_patchlevel;
    if (tee_enforced.GetTagValue(TAG_OS_PATCHLEVEL, &key_os_patchlevel) ||
        sw_enforced.GetTagValue(TAG_OS_PATCHLEVEL, &key_os_patchlevel)) {
        if (key_os_patchlevel < os_patchlevel)
            return KM_ERROR_KEY_REQUIRES_UPGRADE;
        else if (key_os_patchlevel > os_patchlevel)
            return KM_ERROR_INVALID_KEY_BLOB;
    }

    return KM_ERROR_OK;
}

}  // anonymous namespace

AndroidKeymaster::AndroidKeymaster(KeymasterContext* context, size_t operation_table_size)
    : context_(context), operation_table_(new (std::nothrow) OperationTable(operation_table_size)) {
}

AndroidKeymaster::~AndroidKeymaster() {}

AndroidKeymaster::AndroidKeymaster(AndroidKeymaster&& other)
    : context_(move(other.context_)), operation_table_(move(other.operation_table_)) {}

// TODO(swillden): Unify support analysis.  Right now, we have per-keytype methods that determine if
// specific modes, padding, etc. are supported for that key type, and AndroidKeymaster also has
// methods that return the same information.  They'll get out of sync.  Best to put the knowledge in
// the keytypes and provide some mechanism for AndroidKeymaster to query the keytypes for the
// information.

template <typename T>
bool check_supported(const KeymasterContext& context, keymaster_algorithm_t algorithm,
                     SupportedResponse<T>* response) {
    if (context.GetKeyFactory(algorithm) == nullptr) {
        response->error = KM_ERROR_UNSUPPORTED_ALGORITHM;
        return false;
    }
    return true;
}

void AndroidKeymaster::GetVersion(const GetVersionRequest&, GetVersionResponse* rsp) {
    if (rsp == nullptr) return;

    rsp->major_ver = 2;
    rsp->minor_ver = 0;
    rsp->subminor_ver = 0;
    rsp->error = KM_ERROR_OK;
}

GetVersion2Response AndroidKeymaster::GetVersion2(const GetVersion2Request& req) {
    GetVersion2Response rsp;
    rsp.km_version = context_->GetKmVersion();
    rsp.km_date = kKmDate;
    rsp.max_message_version = MessageVersion(rsp.km_version, rsp.km_date);

    // Determine what message version we should use.
    message_version_ = NegotiateMessageVersion(req, rsp);
    return rsp;
}

void AndroidKeymaster::SupportedAlgorithms(const SupportedAlgorithmsRequest& /* request */,
                                           SupportedAlgorithmsResponse* response) {
    if (response == nullptr) return;

    response->error = KM_ERROR_OK;

    size_t algorithm_count = 0;
    const keymaster_algorithm_t* algorithms = context_->GetSupportedAlgorithms(&algorithm_count);
    if (algorithm_count == 0) return;
    response->results_length = algorithm_count;
    response->results = dup_array(algorithms, algorithm_count);
    if (!response->results) response->error = KM_ERROR_MEMORY_ALLOCATION_FAILED;
}

template <typename T>
void GetSupported(const KeymasterContext& context, keymaster_algorithm_t algorithm,
                  keymaster_purpose_t purpose,
                  const T* (OperationFactory::*get_supported_method)(size_t* count) const,
                  SupportedResponse<T>* response) {
    if (response == nullptr || !check_supported(context, algorithm, response)) return;

    const OperationFactory* factory = context.GetOperationFactory(algorithm, purpose);
    if (!factory) {
        response->error = KM_ERROR_UNSUPPORTED_PURPOSE;
        return;
    }

    size_t count;
    const T* supported = (factory->*get_supported_method)(&count);
    response->SetResults(supported, count);
}

void AndroidKeymaster::SupportedBlockModes(const SupportedBlockModesRequest& request,
                                           SupportedBlockModesResponse* response) {
    GetSupported(*context_, request.algorithm, request.purpose,
                 &OperationFactory::SupportedBlockModes, response);
}

void AndroidKeymaster::SupportedPaddingModes(const SupportedPaddingModesRequest& request,
                                             SupportedPaddingModesResponse* response) {
    GetSupported(*context_, request.algorithm, request.purpose,
                 &OperationFactory::SupportedPaddingModes, response);
}

void AndroidKeymaster::SupportedDigests(const SupportedDigestsRequest& request,
                                        SupportedDigestsResponse* response) {
    GetSupported(*context_, request.algorithm, request.purpose, &OperationFactory::SupportedDigests,
                 response);
}

void AndroidKeymaster::SupportedImportFormats(const SupportedImportFormatsRequest& request,
                                              SupportedImportFormatsResponse* response) {
    if (response == nullptr || !check_supported(*context_, request.algorithm, response)) return;

    size_t count;
    const keymaster_key_format_t* formats =
        context_->GetKeyFactory(request.algorithm)->SupportedImportFormats(&count);
    response->SetResults(formats, count);
}

void AndroidKeymaster::SupportedExportFormats(const SupportedExportFormatsRequest& request,
                                              SupportedExportFormatsResponse* response) {
    if (response == nullptr || !check_supported(*context_, request.algorithm, response)) return;

    size_t count;
    const keymaster_key_format_t* formats =
        context_->GetKeyFactory(request.algorithm)->SupportedExportFormats(&count);
    response->SetResults(formats, count);
}

GetHmacSharingParametersResponse AndroidKeymaster::GetHmacSharingParameters() {
    GetHmacSharingParametersResponse response(message_version());
    KeymasterEnforcement* policy = context_->enforcement_policy();
    if (!policy) {
        response.error = KM_ERROR_UNIMPLEMENTED;
        return response;
    }

    response.error = policy->GetHmacSharingParameters(&response.params);
    return response;
}

ComputeSharedHmacResponse
AndroidKeymaster::ComputeSharedHmac(const ComputeSharedHmacRequest& request) {
    ComputeSharedHmacResponse response(message_version());
    KeymasterEnforcement* policy = context_->enforcement_policy();
    if (!policy) {
        response.error = KM_ERROR_UNIMPLEMENTED;
        return response;
    }
    response.error = policy->ComputeSharedHmac(request.params_array, &response.sharing_check);

    return response;
}

VerifyAuthorizationResponse
AndroidKeymaster::VerifyAuthorization(const VerifyAuthorizationRequest& request) {
    KeymasterEnforcement* policy = context_->enforcement_policy();
    if (!policy) {
        VerifyAuthorizationResponse response(message_version());
        response.error = KM_ERROR_UNIMPLEMENTED;
        return response;
    }

    return policy->VerifyAuthorization(request);
}

void AndroidKeymaster::GenerateTimestampToken(GenerateTimestampTokenRequest& request,
                                              GenerateTimestampTokenResponse* response) {
    KeymasterEnforcement* policy = context_->enforcement_policy();
    if (!policy) {
        response->error = KM_ERROR_UNIMPLEMENTED;
    } else {
        response->token.challenge = request.challenge;
        response->error = policy->GenerateTimestampToken(&response->token);
    }
}

void AndroidKeymaster::AddRngEntropy(const AddEntropyRequest& request,
                                     AddEntropyResponse* response) {
    response->error = context_->AddRngEntropy(request.random_data.peek_read(),
                                              request.random_data.available_read());
}

const KeyFactory* get_key_factory(const AuthorizationSet& key_description,
                                  const KeymasterContext& context,  //
                                  keymaster_error_t* error) {
    keymaster_algorithm_t algorithm;
    const KeyFactory* factory{};
    if (!key_description.GetTagValue(TAG_ALGORITHM, &algorithm) ||
        !(factory = context.GetKeyFactory(algorithm))) {
        *error = KM_ERROR_UNSUPPORTED_ALGORITHM;
    }
    return factory;
}

void AndroidKeymaster::GenerateKey(const GenerateKeyRequest& request,
                                   GenerateKeyResponse* response) {
    if (response == nullptr) return;

    const KeyFactory* factory =
        get_key_factory(request.key_description, *context_, &response->error);
    if (!factory) return;

    if (context_->enforcement_policy() &&
        request.key_description.GetTagValue(TAG_EARLY_BOOT_ONLY) &&
        !context_->enforcement_policy()->in_early_boot()) {
        response->error = KM_ERROR_EARLY_BOOT_ENDED;
        return;
    }

    UniquePtr<Key> attest_key;
    if (request.attestation_signing_key_blob.key_material_size) {
        attest_key = LoadKey(request.attestation_signing_key_blob, request.attest_key_params,
                             &response->error);
        if (response->error != KM_ERROR_OK) return;
    }

    response->enforced.Clear();
    response->unenforced.Clear();
    response->error = factory->GenerateKey(request.key_description,
                                           move(attest_key),  //
                                           request.issuer_subject,
                                           &response->key_blob,  //
                                           &response->enforced,
                                           &response->unenforced,  //
                                           &response->certificate_chain);
}

void AndroidKeymaster::GetKeyCharacteristics(const GetKeyCharacteristicsRequest& request,
                                             GetKeyCharacteristicsResponse* response) {
    if (response == nullptr) return;

    UniquePtr<Key> key;
    response->error =
        context_->ParseKeyBlob(KeymasterKeyBlob(request.key_blob), request.additional_params, &key);
    if (response->error != KM_ERROR_OK) return;

    // scavenge the key object for the auth lists
    response->enforced = move(key->hw_enforced());
    response->unenforced = move(key->sw_enforced());

    response->error = CheckVersionInfo(response->enforced, response->unenforced, *context_);
}

void AndroidKeymaster::BeginOperation(const BeginOperationRequest& request,
                                      BeginOperationResponse* response) {
    if (response == nullptr) return;
    response->op_handle = 0;

    UniquePtr<Key> key = LoadKey(request.key_blob, request.additional_params, &response->error);
    if (!key) return;

    response->error = KM_ERROR_UNKNOWN_ERROR;
    keymaster_algorithm_t key_algorithm;
    if (!key->authorizations().GetTagValue(TAG_ALGORITHM, &key_algorithm)) return;

    response->error = KM_ERROR_UNSUPPORTED_PURPOSE;
    OperationFactory* factory = key->key_factory()->GetOperationFactory(request.purpose);
    if (!factory) return;

    OperationPtr operation(
        factory->CreateOperation(move(*key), request.additional_params, &response->error));
    if (operation.get() == nullptr) return;

    if (context_->enforcement_policy()) {
        km_id_t key_id;
        response->error = KM_ERROR_UNKNOWN_ERROR;
        if (!context_->enforcement_policy()->CreateKeyId(request.key_blob, &key_id)) return;
        operation->set_key_id(key_id);
        response->error = context_->enforcement_policy()->AuthorizeOperation(
            request.purpose, key_id, operation->authorizations(), request.additional_params,
            0 /* op_handle */, true /* is_begin_operation */);
        if (response->error != KM_ERROR_OK) return;
    }

    response->output_params.Clear();
    response->error = operation->Begin(request.additional_params, &response->output_params);
    if (response->error != KM_ERROR_OK) return;

    response->op_handle = operation->operation_handle();
    response->error = operation_table_->Add(move(operation));
}

void AndroidKeymaster::UpdateOperation(const UpdateOperationRequest& request,
                                       UpdateOperationResponse* response) {
    if (response == nullptr) return;

    response->error = KM_ERROR_INVALID_OPERATION_HANDLE;
    Operation* operation = operation_table_->Find(request.op_handle);
    if (operation == nullptr) return;

    if (context_->enforcement_policy()) {
        response->error = context_->enforcement_policy()->AuthorizeOperation(
            operation->purpose(), operation->key_id(), operation->authorizations(),
            request.additional_params, request.op_handle, false /* is_begin_operation */);
        if (response->error != KM_ERROR_OK) {
            operation_table_->Delete(request.op_handle);
            return;
        }
    }

    response->error =
        operation->Update(request.additional_params, request.input, &response->output_params,
                          &response->output, &response->input_consumed);
    if (response->error != KM_ERROR_OK) {
        // Any error invalidates the operation.
        operation_table_->Delete(request.op_handle);
    }
}

void AndroidKeymaster::FinishOperation(const FinishOperationRequest& request,
                                       FinishOperationResponse* response) {
    if (response == nullptr) return;

    response->error = KM_ERROR_INVALID_OPERATION_HANDLE;
    Operation* operation = operation_table_->Find(request.op_handle);
    if (operation == nullptr) return;

    if (context_->enforcement_policy()) {
        response->error = context_->enforcement_policy()->AuthorizeOperation(
            operation->purpose(), operation->key_id(), operation->authorizations(),
            request.additional_params, request.op_handle, false /* is_begin_operation */);
        if (response->error != KM_ERROR_OK) {
            operation_table_->Delete(request.op_handle);
            return;
        }
    }

    response->error = operation->Finish(request.additional_params, request.input, request.signature,
                                        &response->output_params, &response->output);
    if (response->error != KM_ERROR_OK) {
        operation_table_->Delete(request.op_handle);
        return;
    }

    // Invalidate the single use key from secure storage after finish.
    if (operation->hw_enforced().Contains(TAG_USAGE_COUNT_LIMIT, 1) &&
        context_->secure_key_storage() != nullptr) {
        response->error = context_->secure_key_storage()->DeleteKey(operation->key_id());
    }
    operation_table_->Delete(request.op_handle);
}

void AndroidKeymaster::AbortOperation(const AbortOperationRequest& request,
                                      AbortOperationResponse* response) {
    if (!response) return;

    Operation* operation = operation_table_->Find(request.op_handle);
    if (!operation) {
        response->error = KM_ERROR_INVALID_OPERATION_HANDLE;
        return;
    }

    response->error = operation->Abort();
    operation_table_->Delete(request.op_handle);
}

void AndroidKeymaster::ExportKey(const ExportKeyRequest& request, ExportKeyResponse* response) {
    if (response == nullptr) return;

    UniquePtr<Key> key;
    response->error =
        context_->ParseKeyBlob(KeymasterKeyBlob(request.key_blob), request.additional_params, &key);
    if (response->error != KM_ERROR_OK) return;

    UniquePtr<uint8_t[]> out_key;
    size_t size;
    response->error = key->formatted_key_material(request.key_format, &out_key, &size);
    if (response->error == KM_ERROR_OK) {
        response->key_data = out_key.release();
        response->key_data_length = size;
    }
}

void AndroidKeymaster::AttestKey(const AttestKeyRequest& request, AttestKeyResponse* response) {
    if (!response) return;

    UniquePtr<Key> key = LoadKey(request.key_blob, request.attest_params, &response->error);
    if (!key) return;

    keymaster_blob_t attestation_application_id;
    if (request.attest_params.GetTagValue(TAG_ATTESTATION_APPLICATION_ID,
                                          &attestation_application_id)) {
        key->sw_enforced().push_back(TAG_ATTESTATION_APPLICATION_ID, attestation_application_id);
    }

    response->certificate_chain =
        context_->GenerateAttestation(*key, request.attest_params, {} /* attestation_signing_key */,
                                      {} /* issuer_subject */, &response->error);
}

void AndroidKeymaster::UpgradeKey(const UpgradeKeyRequest& request, UpgradeKeyResponse* response) {
    if (!response) return;

    KeymasterKeyBlob upgraded_key;
    response->error = context_->UpgradeKeyBlob(KeymasterKeyBlob(request.key_blob),
                                               request.upgrade_params, &upgraded_key);
    if (response->error != KM_ERROR_OK) return;
    response->upgraded_key = upgraded_key.release();
}

void AndroidKeymaster::ImportKey(const ImportKeyRequest& request, ImportKeyResponse* response) {
    if (response == nullptr) return;

    const KeyFactory* factory =
        get_key_factory(request.key_description, *context_, &response->error);
    if (!factory) return;

    UniquePtr<Key> attest_key;
    if (request.attestation_signing_key_blob.key_material_size) {

        attest_key =
            LoadKey(request.attestation_signing_key_blob, {} /* params */, &response->error);
        if (response->error != KM_ERROR_OK) return;
    }

    response->error = factory->ImportKey(request.key_description,  //
                                         request.key_format,       //
                                         request.key_data,         //
                                         move(attest_key),         //
                                         request.issuer_subject,   //
                                         &response->key_blob,      //
                                         &response->enforced,      //
                                         &response->unenforced,    //
                                         &response->certificate_chain);
}

void AndroidKeymaster::DeleteKey(const DeleteKeyRequest& request, DeleteKeyResponse* response) {
    if (!response) return;
    response->error = context_->DeleteKey(KeymasterKeyBlob(request.key_blob));
}

void AndroidKeymaster::DeleteAllKeys(const DeleteAllKeysRequest&, DeleteAllKeysResponse* response) {
    if (!response) return;
    response->error = context_->DeleteAllKeys();
}

void AndroidKeymaster::Configure(const ConfigureRequest& request, ConfigureResponse* response) {
    if (!response) return;
    response->error = context_->SetSystemVersion(request.os_version, request.os_patchlevel);
}

bool AndroidKeymaster::has_operation(keymaster_operation_handle_t op_handle) const {
    return operation_table_->Find(op_handle) != nullptr;
}

UniquePtr<Key> AndroidKeymaster::LoadKey(const keymaster_key_blob_t& key_blob,
                                         const AuthorizationSet& additional_params,
                                         keymaster_error_t* error) {
    if (!error) return {};

    UniquePtr<Key> key;
    KeymasterKeyBlob key_material;
    *error = context_->ParseKeyBlob(KeymasterKeyBlob(key_blob), additional_params, &key);
    if (*error != KM_ERROR_OK) return {};

    *error = CheckVersionInfo(key->hw_enforced(), key->sw_enforced(), *context_);
    if (*error != KM_ERROR_OK) return {};

    return key;
}

void AndroidKeymaster::ImportWrappedKey(const ImportWrappedKeyRequest& request,
                                        ImportWrappedKeyResponse* response) {
    if (!response) return;

    KeymasterKeyBlob secret_key;
    AuthorizationSet key_description;
    keymaster_key_format_t key_format;

    response->error =
        context_->UnwrapKey(request.wrapped_key, request.wrapping_key, request.additional_params,
                            request.masking_key, &key_description, &key_format, &secret_key);

    if (response->error != KM_ERROR_OK) {
        return;
    }

    int sid_idx = key_description.find(TAG_USER_SECURE_ID);
    if (sid_idx != -1) {
        uint8_t sids = key_description[sid_idx].long_integer;
        if (!key_description.erase(sid_idx)) {
            response->error = KM_ERROR_UNKNOWN_ERROR;
            return;
        }
        if (sids & HW_AUTH_PASSWORD) {
            key_description.push_back(TAG_USER_SECURE_ID, request.password_sid);
        }
        if (sids & HW_AUTH_FINGERPRINT) {
            key_description.push_back(TAG_USER_SECURE_ID, request.biometric_sid);
        }

        if (context_->GetKmVersion() >= KmVersion::KEYMINT_1) {
            key_description.push_back(TAG_CERTIFICATE_NOT_BEFORE, 0);
            key_description.push_back(TAG_CERTIFICATE_NOT_AFTER, kUndefinedExpirationDateTime);
        }
    }

    const KeyFactory* factory = get_key_factory(key_description, *context_, &response->error);
    if (!factory) return;

    response->error = factory->ImportKey(key_description,          //
                                         key_format,               //
                                         secret_key,               //
                                         {} /* attest_key */,      //
                                         {} /* issuer_subject */,  //
                                         &response->key_blob,      //
                                         &response->enforced,      //
                                         &response->unenforced,    //
                                         &response->certificate_chain);
}

EarlyBootEndedResponse AndroidKeymaster::EarlyBootEnded() {
    if (context_->enforcement_policy()) {
        context_->enforcement_policy()->early_boot_ended();
    }
    return EarlyBootEndedResponse(message_version());
}

DeviceLockedResponse AndroidKeymaster::DeviceLocked(const DeviceLockedRequest& request) {
    if (context_->enforcement_policy()) {
        context_->enforcement_policy()->device_locked(request.passwordOnly);
    }
    return DeviceLockedResponse(message_version());
}

}  // namespace keymaster
