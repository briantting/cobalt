// Copyright 2023 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ASH_APP_LIST_SEARCH_SYSTEM_INFO_SYSTEM_INFO_ANSWER_RESULT_H_
#define CHROME_BROWSER_ASH_APP_LIST_SEARCH_SYSTEM_INFO_SYSTEM_INFO_ANSWER_RESULT_H_

#include <string>

#include "base/memory/raw_ptr.h"
#include "chrome/browser/ash/app_list/search/chrome_search_result.h"
#include "chrome/browser/profiles/profile.h"

namespace app_list {

class SystemInfoAnswerResult : public ChromeSearchResult {
 public:
  enum class SystemInfoCategory { kSettings, kDiagnostics };

  SystemInfoAnswerResult(Profile* profile,
                         const std::u16string& query,
                         const std::string& url_path,
                         const gfx::ImageSkia& icon,
                         double relevance_score,
                         const std::u16string& title,
                         const std::u16string& description,
                         SystemInfoCategory system_info_category,
                         const ash::SystemInfoAnswerCardData& answer_card_info);
  SystemInfoAnswerResult(const SystemInfoAnswerResult&) = delete;
  SystemInfoAnswerResult& operator=(const SystemInfoAnswerResult&) = delete;

  ~SystemInfoAnswerResult() override;

  void Open(int event_flags) override;

  void UpdateTitle(const std::u16string& title);

  void UpdateTitleAndDetails(const std::u16string& title,
                             const std::u16string& description);

  void UpdateBarChartPercentage(const double bar_chart_percentage);

 private:
  SystemInfoCategory const system_info_category_;
  const raw_ptr<Profile, ExperimentalAsh> profile_;
  const std::u16string query_;

  const std::string url_path_;
};

}  // namespace app_list

#endif  // CHROME_BROWSER_ASH_APP_LIST_SEARCH_SYSTEM_INFO_SYSTEM_INFO_ANSWER_RESULT_H_
