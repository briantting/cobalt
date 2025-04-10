// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/cert_verifier/cert_verifier_creation.h"

#include "base/types/optional_util.h"
#include "build/build_config.h"
#include "build/chromeos_buildflags.h"
#include "net/base/features.h"
#include "net/cert/cert_verify_proc.h"
#include "net/cert/crl_set.h"
#include "net/cert/multi_threaded_cert_verifier.h"
#include "net/cert_net/cert_net_fetcher_url_request.h"
#include "net/net_buildflags.h"

#if BUILDFLAG(IS_FUCHSIA) || BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)
#include "net/cert/cert_verify_proc_builtin.h"
#include "net/cert/internal/system_trust_store.h"
#endif

#if BUILDFLAG(IS_CHROMEOS)
#include "crypto/nss_util_internal.h"
#include "net/cert/internal/system_trust_store_nss.h"
#endif

#if BUILDFLAG(CHROME_ROOT_STORE_SUPPORTED)
#include "net/cert/cert_verify_proc_builtin.h"
#include "net/cert/internal/system_trust_store.h"
#include "net/cert/internal/trust_store_chrome.h"
#endif

#if BUILDFLAG(TRIAL_COMPARISON_CERT_VERIFIER_SUPPORTED)
#include "services/cert_verifier/trial_comparison_cert_verifier_mojo.h"
#endif

namespace cert_verifier {

namespace {

#if BUILDFLAG(IS_CHROMEOS)
crypto::ScopedPK11Slot GetUserSlotRestrictionForChromeOSParams(
    mojom::CertVerifierCreationParams* creation_params) {
  crypto::ScopedPK11Slot public_slot;
#if BUILDFLAG(IS_CHROMEOS_LACROS)
  if (creation_params && creation_params->nss_full_path.has_value()) {
    public_slot =
        crypto::OpenSoftwareNSSDB(creation_params->nss_full_path.value(),
                                  /*description=*/"cert_db");
    // `public_slot` can contain important security related settings. Crash if
    // failed to load it.
    CHECK(public_slot);
  }
#elif BUILDFLAG(IS_CHROMEOS_ASH)
  if (creation_params && !creation_params->username_hash.empty()) {
    // Make sure NSS is initialized for the user.
    crypto::InitializeNSSForChromeOSUser(creation_params->username_hash,
                                         creation_params->nss_path.value());
    public_slot =
        crypto::GetPublicSlotForChromeOSUser(creation_params->username_hash);
  }
#else
#error IS_CHROMEOS set without IS_CHROMEOS_LACROS or IS_CHROMEOS_ASH
#endif
  return public_slot;
}
#endif  // BUILDFLAG(IS_CHROMEOS)

class CertVerifyProcFactoryImpl : public net::CertVerifyProcFactory {
 public:
  explicit CertVerifyProcFactoryImpl(
      mojom::CertVerifierCreationParams* creation_params) {
#if BUILDFLAG(IS_CHROMEOS)
    user_slot_restriction_ =
        GetUserSlotRestrictionForChromeOSParams(creation_params);
#endif
  }

  scoped_refptr<net::CertVerifyProc> CreateCertVerifyProc(
      scoped_refptr<net::CertNetFetcher> cert_net_fetcher,
      const CertVerifyProcFactory::ImplParams& impl_params) override {
#if BUILDFLAG(CHROME_ROOT_STORE_ONLY)
    return CreateNewCertVerifyProc(
        cert_net_fetcher, impl_params.crl_set,
        base::OptionalToPtr(impl_params.root_store_data));
#else
#if BUILDFLAG(CHROME_ROOT_STORE_OPTIONAL)
    if (impl_params.use_chrome_root_store) {
      return CreateNewCertVerifyProc(
          cert_net_fetcher, impl_params.crl_set,
          base::OptionalToPtr(impl_params.root_store_data));
    }
#endif
    return CreateOldCertVerifyProc(cert_net_fetcher, impl_params.crl_set);
#endif
  }

 protected:
  ~CertVerifyProcFactoryImpl() override = default;

#if !BUILDFLAG(CHROME_ROOT_STORE_ONLY)
  // Factory function that returns a CertVerifyProc that supports the old
  // configuration for platforms where we are transitioning from one cert
  // configuration to another. If the platform only supports one configuration,
  // return a CertVerifyProc that supports that configuration.
  scoped_refptr<net::CertVerifyProc> CreateOldCertVerifyProc(
      scoped_refptr<net::CertNetFetcher> cert_net_fetcher,
      scoped_refptr<net::CRLSet> crl_set) {
    scoped_refptr<net::CertVerifyProc> verify_proc;
#if BUILDFLAG(IS_CHROMEOS)
    verify_proc = net::CreateCertVerifyProcBuiltin(
        std::move(cert_net_fetcher), std::move(crl_set),
        net::CreateSslSystemTrustStoreNSSWithUserSlotRestriction(
            user_slot_restriction_ ? crypto::ScopedPK11Slot(PK11_ReferenceSlot(
                                         user_slot_restriction_.get()))
                                   : nullptr));
#elif BUILDFLAG(IS_FUCHSIA) || BUILDFLAG(IS_LINUX)
    verify_proc = net::CreateCertVerifyProcBuiltin(
        std::move(cert_net_fetcher), std::move(crl_set),
        net::CreateSslSystemTrustStore());
#else
    verify_proc = net::CertVerifyProc::CreateSystemVerifyProc(
        std::move(cert_net_fetcher), std::move(crl_set));
#endif
    return verify_proc;
  }
#endif  // !BUILDFLAG(CHROME_ROOT_STORE_ONLY)

#if BUILDFLAG(CHROME_ROOT_STORE_SUPPORTED)
  // CertVerifyProcFactory that returns a CertVerifyProc that uses the
  // Chrome Cert Verifier with the Chrome Root Store.
  scoped_refptr<net::CertVerifyProc> CreateNewCertVerifyProc(
      scoped_refptr<net::CertNetFetcher> cert_net_fetcher,
      scoped_refptr<net::CRLSet> crl_set,
      const net::ChromeRootStoreData* root_store_data) {
    std::unique_ptr<net::TrustStoreChrome> chrome_root;
    if (!root_store_data) {
      chrome_root = std::make_unique<net::TrustStoreChrome>();
    } else {
      chrome_root = std::make_unique<net::TrustStoreChrome>(*root_store_data);
    }
    std::unique_ptr<net::SystemTrustStore> trust_store;
#if BUILDFLAG(IS_CHROMEOS)
    trust_store =
        net::CreateSslSystemTrustStoreChromeRootWithUserSlotRestriction(
            std::move(chrome_root),
            user_slot_restriction_ ? crypto::ScopedPK11Slot(PK11_ReferenceSlot(
                                         user_slot_restriction_.get()))
                                   : nullptr);
#else
    trust_store =
        net::CreateSslSystemTrustStoreChromeRoot(std::move(chrome_root));
#endif
#if BUILDFLAG(IS_WIN)
    // Start initialization of TrustStoreWin on a separate thread if it hasn't
    // been done already. We do this here instead of in the TrustStoreWin
    // constructor to avoid any unnecessary threading in unit tests that don't
    // use threads otherwise.
    net::InitializeTrustStoreWinSystem();
#endif
#if BUILDFLAG(IS_ANDROID)
    // Start initialization of TrustStoreAndroid on a separate thread if it
    // hasn't been done already. We do this here instead of in the
    // TrustStoreAndroid constructor to avoid any unnecessary threading in unit
    // tests that don't use threads otherwise.
    net::InitializeTrustStoreAndroid();
#endif
    return net::CreateCertVerifyProcBuiltin(std::move(cert_net_fetcher),
                                            std::move(crl_set),
                                            std::move(trust_store));
  }
#endif  // BUILDFLAG(CHROME_ROOT_STORE_SUPPORTED)

#if BUILDFLAG(IS_CHROMEOS)
  crypto::ScopedPK11Slot user_slot_restriction_;
#endif
};

#if BUILDFLAG(TRIAL_COMPARISON_CERT_VERIFIER_SUPPORTED)
// Returns true if creation_params are requesting the creation of a
// TrialComparisonCertVerifier.
bool IsTrialVerificationOn(
    const mojom::CertVerifierCreationParams* creation_params) {
  // Check to see if we have trial comparison cert verifier params.
  return creation_params &&
         creation_params->trial_comparison_cert_verifier_params;
}

// Should only be called if IsTrialVerificationOn(creation_params) == true.
std::unique_ptr<net::CertVerifierWithUpdatableProc> CreateTrialCertVerifier(
    mojom::CertVerifierCreationParams* creation_params,
    scoped_refptr<net::CertNetFetcher> cert_net_fetcher,
    const net::CertVerifyProcFactory::ImplParams& impl_params) {
  DCHECK(IsTrialVerificationOn(creation_params));

  scoped_refptr<net::CertVerifyProcFactory> proc_factory =
      base::MakeRefCounted<CertVerifyProcFactoryImpl>(creation_params);

#if !BUILDFLAG(CHROME_ROOT_STORE_OPTIONAL)
#error "CHROME_ROOT_STORE_OPTIONAL must be true"
#endif

  return std::make_unique<TrialComparisonCertVerifierMojo>(
      creation_params->trial_comparison_cert_verifier_params->initial_allowed,
      std::move(creation_params->trial_comparison_cert_verifier_params
                    ->config_client_receiver),
      std::move(creation_params->trial_comparison_cert_verifier_params
                    ->report_client),
      std::move(proc_factory), cert_net_fetcher, impl_params);
}
#endif  // BUILDFLAG(TRIAL_COMPARISON_CERT_VERIFIER_SUPPORTED)

}  // namespace

bool IsUsingCertNetFetcher() {
#if BUILDFLAG(IS_ANDROID) || BUILDFLAG(IS_FUCHSIA) ||      \
    BUILDFLAG(IS_CHROMEOS) || BUILDFLAG(IS_LINUX) ||       \
    BUILDFLAG(TRIAL_COMPARISON_CERT_VERIFIER_SUPPORTED) || \
    BUILDFLAG(CHROME_ROOT_STORE_SUPPORTED)
  return true;
#else
  return false;
#endif
}

std::unique_ptr<net::CertVerifierWithUpdatableProc> CreateCertVerifier(
    mojom::CertVerifierCreationParams* creation_params,
    scoped_refptr<net::CertNetFetcher> cert_net_fetcher,
    const net::CertVerifyProcFactory::ImplParams& impl_params) {
  DCHECK(cert_net_fetcher || !IsUsingCertNetFetcher());

#if BUILDFLAG(TRIAL_COMPARISON_CERT_VERIFIER_SUPPORTED)
  if (IsTrialVerificationOn(creation_params)) {
    return CreateTrialCertVerifier(creation_params, cert_net_fetcher,
                                   impl_params);
  }
#endif

  scoped_refptr<net::CertVerifyProcFactory> proc_factory =
      base::MakeRefCounted<CertVerifyProcFactoryImpl>(creation_params);
  return std::make_unique<net::MultiThreadedCertVerifier>(
      proc_factory->CreateCertVerifyProc(cert_net_fetcher, impl_params),
      proc_factory);
}

}  // namespace cert_verifier
