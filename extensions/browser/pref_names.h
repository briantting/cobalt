// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef EXTENSIONS_BROWSER_PREF_NAMES_H_
#define EXTENSIONS_BROWSER_PREF_NAMES_H_

#include <string>

#include "build/build_config.h"
#include "extensions/browser/extension_prefs_scope.h"

// Preference keys which are needed by both the ExtensionPrefs and by external
// clients, such as APIs.

namespace extensions {
namespace pref_names {

// If the given |scope| is persisted, return true and populate |result| with the
// appropriate property (i.e. one of kPref*) within a kExtensions dictionary. If
// |scope| is not persisted, return false, and leave |result| unchanged.
bool ScopeToPrefName(ExtensionPrefsScope scope, std::string* result);

// Browser-level preferences ---------------------------------------------------

// Whether we have run the extension-alert system (see ExtensionGlobalError)
// at least once for this profile.
extern const char kAlertsInitialized[];

// The sites that are allowed to install extensions. These sites should be
// allowed to install extensions without the scary dangerous downloads bar.
// Also, when off-store-extension installs are disabled, these sites are exempt.
extern const char kAllowedInstallSites[];

// A list of allowed extension types. Extensions can only be installed if their
// type is on this allowlist or alternatively on kInstallAllowList or
// kInstallForceList.
extern const char kAllowedTypes[];

// A boolean that tracks whether apps are allowed to enter fullscreen mode.
extern const char kAppFullscreenAllowed[];

// A boolean indicating if external extensions are blocked from installing.
extern const char kBlockExternalExtensions[];

// A preference for a list of Component extensions that have been
// uninstalled/removed and should not be reloaded.
extern const char kDeletedComponentExtensions[];

// A list of app origins that will grant a long-lived background lifetime to
// the connecting extension, if connected to them via persistent messaging
// ports. The value is controlled by the
// `ExtensionExtendedBackgroundLifetimeForPortConnectionsToUrls` policy.
extern const char kExtendedBackgroundLifetimeForPortConnectionsToUrls[];

// Dictionary pref that keeps track of per-extension settings. The keys are
// extension ids.
extern const char kExtensions[];

// Dictionary pref that manages extensions, controlled by policy.
// Values are expected to conform to the schema of the ExtensionManagement
// policy.
extern const char kExtensionManagement[];

// An integer that indicates the availability of extensions that are unpublished
// on the Chrome Web Store. More details can be found at
// ExtensionUnpublishedAvailability.yaml
extern const char kExtensionUnpublishedAvailability[];

// Policy that indicates whether CRX2 extension updates are allowed.
extern const char kInsecureExtensionUpdatesEnabled[];

// A allowlist of extension ids the user can install: exceptions from the
// following denylist.
extern const char kInstallAllowList[];

// A denylist, containing extensions the user cannot install. This list can
// contain "*" meaning all extensions. This list should not be confused with the
// extension blocklist, which is Google controlled.
extern const char kInstallDenyList[];

// A list containing extensions that Chrome will silently install
// at startup time. It is a list of strings, each string contains
// an extension ID and an update URL, delimited by a semicolon.
// This preference is set by an admin policy, and meant to be only
// accessed through extensions::ExternalPolicyProvider.
extern const char kInstallForceList[];

// String pref for what version chrome was last time the extension prefs were
// loaded.
extern const char kLastChromeVersion[];

// Blocklist and allowlist for Native Messaging Hosts.
extern const char kNativeMessagingBlocklist[];
extern const char kNativeMessagingAllowlist[];

// Flag allowing usage of Native Messaging hosts installed on user level.
extern const char kNativeMessagingUserLevelHosts[];

// Time of the next scheduled extensions auto-update checks.
extern const char kNextUpdateCheck[];

// An integer indicates the availability of manifest v2 extensions. The value is
// controlled by the ExtensionManifestV2Availability policy. More details can
// be found at ExtensionManifestV2Availability.yaml.
extern const char kManifestV2Availability[];

// A preference that tracks extensions pinned to the toolbar. This is a list
// object stored in the Preferences file. The extensions are stored by ID.
extern const char kPinnedExtensions[];

// Indicates on-disk data might have skeletal data that needs to be cleaned
// on the next start of the browser.
extern const char kStorageGarbageCollect[];

// A preference for an enterprise policy which is a temporary workaround for
// behaviour changes to <webview>.
extern const char kChromeAppsWebViewPermissiveBehaviorAllowed[];

// Properties in kExtensions dictionaries --------------------------------------

// Extension-controlled preferences.
extern const char kPrefPreferences[];

// Extension-controlled incognito preferences.
extern const char kPrefIncognitoPreferences[];

// Extension-controlled regular-only preferences.
extern const char kPrefRegularOnlyPreferences[];

// Extension-set content settings.
extern const char kPrefContentSettings[];

// Extension-set incognito content settings.
extern const char kPrefIncognitoContentSettings[];

}  // namespace pref_names
}  // namespace extensions

#endif  // EXTENSIONS_BROWSER_PREF_NAMES_H_
