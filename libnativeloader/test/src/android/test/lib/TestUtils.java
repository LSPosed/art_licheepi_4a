/*
 * Copyright (C) 2022 The Android Open Source Project
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

package android.test.lib;

import static com.google.common.truth.Truth.assertThat;
import static org.junit.Assert.assertThrows;

import androidx.test.platform.app.InstrumentationRegistry;
import org.junit.function.ThrowingRunnable;

public final class TestUtils {
    public static void assertLibraryNotFound(ThrowingRunnable loadLibrary) {
        Throwable t = assertThrows(UnsatisfiedLinkError.class, loadLibrary);
        assertThat(t.getMessage()).containsMatch("dlopen failed: library .* not found");
    }

    public static void assertLinkerNamespaceError(ThrowingRunnable loadLibrary) {
        Throwable t = assertThrows(UnsatisfiedLinkError.class, loadLibrary);
        assertThat(t.getMessage())
                .containsMatch("dlopen failed: .* is not accessible for the namespace");
    }

    public static String libPath(String dir, String libName) {
        String libDirName = InstrumentationRegistry.getArguments().getString("libDirName");
        return dir + "/" + libDirName + "/lib" + libName + ".so";
    }
}
