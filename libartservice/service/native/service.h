/*
 * Copyright (C) 2021 The Android Open Source Project
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

#ifndef ART_LIBARTSERVICE_SERVICE_NATIVE_SERVICE_H_
#define ART_LIBARTSERVICE_SERVICE_NATIVE_SERVICE_H_

#include <string>

#include "android-base/result.h"

namespace art {
namespace service {

android::base::Result<void> ValidateAbsoluteNormalPath(const std::string& path_str);

android::base::Result<void> ValidatePathElementSubstring(const std::string& path_element_substring,
                                                         const std::string& name);

android::base::Result<void> ValidatePathElement(const std::string& path_element,
                                                const std::string& name);

android::base::Result<void> ValidateDexPath(const std::string& dex_path);

}  // namespace service
}  // namespace art

#endif  // ART_LIBARTSERVICE_SERVICE_NATIVE_SERVICE_H_
